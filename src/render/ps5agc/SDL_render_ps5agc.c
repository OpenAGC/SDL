/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.
*/

#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_RENDER_PS5AGC

#include "../SDL_sysrender.h"
#include "../../video/SDL_sysvideo.h"
#include "../../video/ps5/SDL_ps5video.h"

#include <agc_cb.h>
#include <agc_error.h>
#include <agc_graphics.h>
#include <agc_memory.h>
#include <agc_shader.h>
#include <agc_texture.h>
#include <agc_videoout.h>
#include <agcdriver.h>

#include "shaders/ps5agc_ngg_front.h"
#include "shaders/ps5agc_ngg_back.h"
#include "shaders/ps5agc_solid.h"
#include "shaders/ps5agc_frag.h"
#include "shaders/ps5agc_yuv_planar_jpeg.h"
#include "shaders/ps5agc_yuv_planar_bt601.h"
#include "shaders/ps5agc_yuv_planar_bt709.h"
#include "shaders/ps5agc_yuv_nv_jpeg.h"
#include "shaders/ps5agc_yuv_nv_bt601.h"
#include "shaders/ps5agc_yuv_nv_bt709.h"

#define PS5AGC_BUFFER_COUNT 3u
#define PS5AGC_COMMAND_BYTES (256u * 1024u)
#define PS5AGC_UPLOAD_BYTES (4u * 1024u * 1024u)
#define PS5AGC_SHADER_BYTES (64u * 1024u)
#define PS5AGC_GPU_TIMEOUT_US 1000000u
#define PS5AGC_PRESENT_TIMEOUT_US 1000000u
#define PS5AGC_DIRECT_ALIGNMENT (2u * 1024u * 1024u)
#define PS5AGC_RENDER_ALIGNMENT (64u * 1024u)
#define PS5AGC_MIN_RENDERER_POOL_BYTES (25u * 1024u * 1024u)
#define PS5AGC_DRAW_MODIFIER 0x40000000u
#define PS5AGC_TEST_FAILURE_ENV "SDL_PS5AGC_TEST_FAILURE"

typedef enum PS5AGC_ShaderKind
{
    PS5AGC_SHADER_SOLID,
    PS5AGC_SHADER_RGBA,
    PS5AGC_SHADER_PLANAR_JPEG,
    PS5AGC_SHADER_PLANAR_BT601,
    PS5AGC_SHADER_PLANAR_BT709,
    PS5AGC_SHADER_NV_JPEG,
    PS5AGC_SHADER_NV_BT601,
    PS5AGC_SHADER_NV_BT709,
    PS5AGC_SHADER_COUNT
} PS5AGC_ShaderKind;

typedef struct PS5AGC_Vertex
{
    float x, y;
    float r, g, b, a;
    float u, v;
} PS5AGC_Vertex;

typedef struct PS5AGC_TextureData
{
    AgcGpuMemory memory;
    AgcGfx1013CombinedImageSamplerDescriptor *descriptor;
    size_t descriptor_offset;
    size_t plane_offset[3];
    size_t pixel_bytes;
    size_t lock_bytes;
    void *lock_buffer;
    int pitch;
    int plane_pitch[3];
    Uint32 plane_width[3];
    Uint32 plane_height[3];
    Uint32 format;
    Uint32 plane_count;
    SDL_ScaleMode scale_mode;
    AgcGfx1013ResourceUsage usage;
} PS5AGC_TextureData;

typedef struct PS5AGC_RenderData
{
    SDL_VideoDevice *device;
    AgcVideoOut *video_out;
    AgcVideoOutMode mode;
    AgcGpuMemory renderer_memory;
    AgcGpuMemory command_memory[PS5AGC_BUFFER_COUNT];
    AgcGpuMemory upload_memory;
    AgcGpuMemory shader_memory;
    AgcGpuMemory fence_memory[PS5AGC_BUFFER_COUNT];
    AgcGpuMemory render_memory[PS5AGC_BUFFER_COUNT];
    AgcGpuMemory display_memory;
    void *display_buffers[PS5AGC_BUFFER_COUNT];
    size_t display_stride;
    AgcShaderRecord front_record;
    AgcShaderRecord back_record;
    AgcShaderRecord pixel_records[PS5AGC_SHADER_COUNT];
    AgcShaderRecord fused_record;
    AgcRegisterValue fused_registers[32];
    AgcGfx1013Wave32VsPsState shaders[PS5AGC_SHADER_COUNT];
    AgcGfx1013ResourceUsage render_usage[PS5AGC_BUFFER_COUNT];
    AgcGfx1013ResourceUsage display_usage[PS5AGC_BUFFER_COUNT];
    Uint64 frame_id;
    Uint32 fence_value[PS5AGC_BUFFER_COUNT];
    SDL_Rect viewport;
    SDL_Rect cliprect;
    SDL_bool clip_enabled;
    SDL_bool screen_dirty;
    SDL_bool screen_synced;
    SDL_bool submission_failed;
} PS5AGC_RenderData;

static size_t PS5AGC_Align(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static int PS5AGC_AppendMemoryRegion(size_t *total, size_t size,
                                     size_t alignment, size_t *offset)
{
    size_t aligned;

    if (!total || !offset || !size || !alignment ||
        (alignment & (alignment - 1u)) != 0u) {
        return SDL_InvalidParamError("ps5agc memory region");
    }
    if (*total > SIZE_MAX - (alignment - 1u)) {
        return SDL_OutOfMemory();
    }
    aligned = PS5AGC_Align(*total, alignment);
    if (size > SIZE_MAX - aligned) {
        return SDL_OutOfMemory();
    }
    *offset = aligned;
    *total = aligned + size;
    return 0;
}

static void PS5AGC_InitMemorySlice(AgcGpuMemory *slice,
                                   const AgcGpuMemory *pool,
                                   size_t offset, size_t size)
{
    SDL_memset(slice, 0, sizeof(*slice));
    slice->cpu_address = (Uint8 *)pool->cpu_address + offset;
    slice->gpu_address = pool->gpu_address + offset;
    slice->size = size;
    slice->type = pool->type;
}

static int PS5AGC_SetError(const char *operation, int32_t error)
{
    return SDL_SetError("%s failed: %s (0x%08x)", operation,
                        agcErrorString(error), (unsigned int)error);
}

static int PS5AGC_InjectFailure(const char *point)
{
#ifdef SDL_PS5_OPENAGC_TEST_HOOKS
    const char *requested = SDL_getenv(PS5AGC_TEST_FAILURE_ENV);

    if (requested && SDL_strcmp(requested, point) == 0) {
        return SDL_SetError("ps5agc test failure injected at %s", point);
    }
#else
    (void)point;
#endif
    return 0;
}

static int PS5AGC_Flush(const AgcGpuMemory *memory, size_t offset,
                        size_t size, const char *operation)
{
    int32_t error = agcGpuMemoryFlush(memory, offset, size);
    return error == AGC_OK ? 0 : PS5AGC_SetError(operation, error);
}

static int PS5AGC_SubmitAndWait(PS5AGC_RenderData *data, Uint32 slot,
                                SceAgcCb *cb, const char *operation)
{
    AgcCommandBufferSubmit submit;
    AgcGfx1013EopFenceState fence;
    AgcGpuMemory *command_memory;
    AgcGpuMemory *fence_memory;
    volatile Uint32 *value;
    int32_t error;

    if (data->submission_failed || slot >= PS5AGC_BUFFER_COUNT) {
        return SDL_SetError("ps5agc is unavailable after a submission failure");
    }
    command_memory = &data->command_memory[slot];
    fence_memory = &data->fence_memory[slot];
    value = (volatile Uint32 *)fence_memory->cpu_address;

    ++data->fence_value[slot];
    if (data->fence_value[slot] == 0u) {
        ++data->fence_value[slot];
    }
    *value = 0u;
    error = agcGpuMemoryFlush(fence_memory, 0, sizeof(*value));
    if (error != AGC_OK) {
        data->submission_failed = SDL_TRUE;
        return PS5AGC_SetError("resetting an OpenAGC fence", error);
    }
    SDL_zero(fence);
    fence.address = fence_memory->gpu_address;
    fence.value = data->fence_value[slot];
    error = agcGfx1013SignalEopFence(cb, &fence);
    if (error != AGC_OK) {
        data->submission_failed = SDL_TRUE;
        return PS5AGC_SetError("recording an OpenAGC EOP fence", error);
    }
    error = agcGpuMemoryFlush(command_memory, 0,
                              (size_t)agcCbUsedDwords(cb) * sizeof(Uint32));
    if (error != AGC_OK) {
        data->submission_failed = SDL_TRUE;
        return PS5AGC_SetError("publishing an OpenAGC command buffer", error);
    }
    submit.command_address = command_memory->gpu_address;
    submit.dword_count = agcCbUsedDwords(cb);
    submit.reserved = 0;
    if (PS5AGC_InjectFailure("submission") < 0) {
        data->submission_failed = SDL_TRUE;
        return -1;
    }
    error = sceAgcDriverSubmitDcb(&submit);
    if (error != AGC_OK) {
        data->submission_failed = SDL_TRUE;
        return PS5AGC_SetError(operation, error);
    }
    error = agcGpuMemoryWait32(fence_memory, 0, data->fence_value[slot],
                               PS5AGC_GPU_TIMEOUT_US);
    if (error != AGC_OK) {
        data->submission_failed = SDL_TRUE;
        return PS5AGC_SetError("waiting for OpenAGC", error);
    }
    return 0;
}

static int PS5AGC_Transition(SceAgcCb *cb, AgcGfx1013ResourceUsage before,
                             AgcGfx1013ResourceUsage after)
{
    AgcGfx1013ResourceTransition transition;
    int32_t error;

    if (before == after) {
        return 0;
    }
    SDL_zero(transition);
    transition.before = before;
    transition.after = after;
    error = agcGfx1013TransitionResource(cb, &transition);
    return error == AGC_OK ? 0 : PS5AGC_SetError("recording a resource transition", error);
}

static int PS5AGC_InitDescriptor(PS5AGC_TextureData *texture)
{
    AgcGfx1013Image2DState image;
    AgcSamplerDescriptor sampler;
    Uint32 plane;
    int32_t error;

    agcSamplerDescriptorInit(&sampler);
    agcSamplerDescriptorSetClampMode(&sampler, kAgcClampClamp,
                                     kAgcClampClamp, kAgcClampClamp);
    agcSamplerDescriptorSetFilterMode(
        &sampler,
        texture->scale_mode == SDL_ScaleModeNearest ? kAgcFilterPoint : kAgcFilterBilinear,
        texture->scale_mode == SDL_ScaleModeNearest ? kAgcFilterPoint : kAgcFilterBilinear,
        kAgcMipFilterNone);

    for (plane = 0; plane < texture->plane_count; ++plane) {
        SDL_zero(image);
        image.address = texture->memory.gpu_address + texture->plane_offset[plane];
        image.width = texture->plane_width[plane];
        image.height = texture->plane_height[plane];
        image.image_type = AGC_GFX1013_IMAGE_TYPE_2D;
        image.dst_sel_x = 4u;
        image.dst_sel_y = 5u;
        image.dst_sel_z = 6u;
        image.dst_sel_w = 7u;
        if (texture->format == SDL_PIXELFORMAT_ABGR8888) {
            image.format = AGC_GFX1013_IMAGE_FORMAT_RGBA8_UNORM;
        } else if (texture->format == SDL_PIXELFORMAT_NV12 ||
                   texture->format == SDL_PIXELFORMAT_NV21) {
            image.format = plane == 0 ?
                agcTextureFormatEncode(kAgcDataFormat8, kAgcNumberUnorm) :
                agcTextureFormatEncode(kAgcDataFormat8_8, kAgcNumberUnorm);
            if (plane == 0) {
                image.dst_sel_y = image.dst_sel_z = 4u;
            } else if (texture->format == SDL_PIXELFORMAT_NV21) {
                image.dst_sel_x = 5u;
                image.dst_sel_y = 4u;
            }
        } else {
            image.format = agcTextureFormatEncode(kAgcDataFormat8, kAgcNumberUnorm);
            image.dst_sel_y = image.dst_sel_z = 4u;
        }
        error = agcGfx1013CombinedImageSamplerDescriptorEncode(
            &texture->descriptor[plane], &image, &sampler);
        if (error != AGC_OK) {
            return PS5AGC_SetError("encoding an OpenAGC texture descriptor", error);
        }
    }
    return PS5AGC_Flush(&texture->memory, texture->descriptor_offset,
                        texture->plane_count * sizeof(*texture->descriptor),
                        "publishing texture descriptors");
}

static int PS5AGC_InitShaders(PS5AGC_RenderData *data)
{
    size_t front_offset = 0u;
    size_t back_offset;
    size_t shader_offset;
    const AgcShaderRecord *file_record;
    size_t code_offset;
    size_t code_size;
    Uint32 shader;
    int32_t error;

    error = agcShaderRecordRelocateBinary(&data->front_record,
                                          ps5agc_ngg_front_sb,
                                          ps5agc_ngg_front_sb_len);
    if (error == AGC_OK) {
        error = agcShaderRecordRelocateBinary(&data->back_record,
                                              ps5agc_ngg_back_sb,
                                              ps5agc_ngg_back_sb_len);
    }
    if (error == AGC_OK) {
        error = agcShaderRecordRelocateBinary(&data->pixel_records[PS5AGC_SHADER_SOLID],
                                              ps5agc_solid_sb,
                                              ps5agc_solid_sb_len);
    }
    if (error == AGC_OK) {
        error = agcShaderRecordRelocateBinary(&data->pixel_records[PS5AGC_SHADER_RGBA],
                                              ps5agc_frag_sb,
                                              ps5agc_frag_sb_len);
    }
#define PS5AGC_RELOCATE_PIXEL(kind, blob)                                      \
    if (error == AGC_OK) {                                                     \
        error = agcShaderRecordRelocateBinary(&data->pixel_records[(kind)],    \
                                               (blob), sizeof(blob));           \
    }
    PS5AGC_RELOCATE_PIXEL(PS5AGC_SHADER_PLANAR_JPEG, ps5agc_yuv_planar_jpeg_sb);
    PS5AGC_RELOCATE_PIXEL(PS5AGC_SHADER_PLANAR_BT601, ps5agc_yuv_planar_bt601_sb);
    PS5AGC_RELOCATE_PIXEL(PS5AGC_SHADER_PLANAR_BT709, ps5agc_yuv_planar_bt709_sb);
    PS5AGC_RELOCATE_PIXEL(PS5AGC_SHADER_NV_JPEG, ps5agc_yuv_nv_jpeg_sb);
    PS5AGC_RELOCATE_PIXEL(PS5AGC_SHADER_NV_BT601, ps5agc_yuv_nv_bt601_sb);
    PS5AGC_RELOCATE_PIXEL(PS5AGC_SHADER_NV_BT709, ps5agc_yuv_nv_bt709_sb);
#undef PS5AGC_RELOCATE_PIXEL
    if (error != AGC_OK) {
        return PS5AGC_SetError("relocating checked-in PSBC shaders", error);
    }

#define PS5AGC_UPLOAD_SHADER(blob, record, offset)                                      \
    do {                                                                                 \
        file_record = (const AgcShaderRecord *)(blob);                                   \
        code_offset = (size_t)file_record->code;                                         \
        if (code_offset >= sizeof(blob)) {                                                \
            return SDL_SetError("checked-in PSBC shader has an invalid code offset");   \
        }                                                                                \
        code_size = sizeof(blob) - code_offset;                                          \
        if (code_size < 16u) {                                                           \
            return SDL_SetError("checked-in PSBC shader code is truncated");           \
        }                                                                                \
        if ((offset) > data->shader_memory.size ||                                       \
            code_size > data->shader_memory.size - (offset)) {                           \
            return SDL_SetError("checked-in PSBC shaders exceed the shader pool");      \
        }                                                                                \
        SDL_memcpy((Uint8 *)data->shader_memory.cpu_address + (offset),                  \
                   (const Uint8 *)(blob) + code_offset, code_size);                       \
        (record).code = data->shader_memory.gpu_address + (offset);                       \
        (offset) = PS5AGC_Align((offset) + code_size, 256u);                              \
    } while (0)

    PS5AGC_UPLOAD_SHADER(ps5agc_ngg_front_sb, data->front_record, front_offset);
    back_offset = front_offset;
    PS5AGC_UPLOAD_SHADER(ps5agc_ngg_back_sb, data->back_record, back_offset);
    shader_offset = back_offset;
    PS5AGC_UPLOAD_SHADER(ps5agc_solid_sb,
        data->pixel_records[PS5AGC_SHADER_SOLID], shader_offset);
    PS5AGC_UPLOAD_SHADER(ps5agc_frag_sb,
        data->pixel_records[PS5AGC_SHADER_RGBA], shader_offset);
    PS5AGC_UPLOAD_SHADER(ps5agc_yuv_planar_jpeg_sb,
        data->pixel_records[PS5AGC_SHADER_PLANAR_JPEG], shader_offset);
    PS5AGC_UPLOAD_SHADER(ps5agc_yuv_planar_bt601_sb,
        data->pixel_records[PS5AGC_SHADER_PLANAR_BT601], shader_offset);
    PS5AGC_UPLOAD_SHADER(ps5agc_yuv_planar_bt709_sb,
        data->pixel_records[PS5AGC_SHADER_PLANAR_BT709], shader_offset);
    PS5AGC_UPLOAD_SHADER(ps5agc_yuv_nv_jpeg_sb,
        data->pixel_records[PS5AGC_SHADER_NV_JPEG], shader_offset);
    PS5AGC_UPLOAD_SHADER(ps5agc_yuv_nv_bt601_sb,
        data->pixel_records[PS5AGC_SHADER_NV_BT601], shader_offset);
    PS5AGC_UPLOAD_SHADER(ps5agc_yuv_nv_bt709_sb,
        data->pixel_records[PS5AGC_SHADER_NV_BT709], shader_offset);
#undef PS5AGC_UPLOAD_SHADER

    if (shader_offset > data->shader_memory.size) {
        return SDL_SetError("checked-in PSBC shaders exceed the OpenAGC shader pool");
    }
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache(
        (char *)data->shader_memory.cpu_address,
        (char *)data->shader_memory.cpu_address + shader_offset);
#endif
    if (PS5AGC_Flush(&data->shader_memory, 0, shader_offset,
                     "publishing checked-in PSBC shaders") < 0) {
        return -1;
    }
    error = sceAgcFuseShaderHalves_0200(
        &data->fused_record, &data->front_record, &data->back_record,
        data->fused_registers);
    if (error != AGC_OK) {
        return PS5AGC_SetError("fusing checked-in PSBC shader halves", error);
    }
    SDL_zero(data->shaders);
    for (shader = 0; shader < PS5AGC_SHADER_COUNT; ++shader) {
        AgcShaderRecord *pixel = &data->pixel_records[shader];
        data->shaders[shader].primitive.record = &data->fused_record;
        data->shaders[shader].primitive.sh_registers = data->fused_registers;
        data->shaders[shader].primitive.num_sh_registers = data->fused_record.num_sh_registers;
        data->shaders[shader].primitive.cx_registers =
            (const AgcRegisterValue *)(uintptr_t)data->back_record.cx_registers;
        data->shaders[shader].primitive.num_cx_registers = data->back_record.num_cx_registers;
        data->shaders[shader].primitive.code_address = data->back_record.code;
        data->shaders[shader].pixel.record = pixel;
        data->shaders[shader].pixel.sh_registers =
            (const AgcRegisterValue *)(uintptr_t)pixel->sh_registers;
        data->shaders[shader].pixel.num_sh_registers = pixel->num_sh_registers;
        data->shaders[shader].pixel.cx_registers =
            (const AgcRegisterValue *)(uintptr_t)pixel->cx_registers;
        data->shaders[shader].pixel.num_cx_registers = pixel->num_cx_registers;
        data->shaders[shader].pixel.code_address = pixel->code;
        data->shaders[shader].primitive_back_code_address = data->back_record.code;
        data->shaders[shader].primitive_type = 4u; /* triangle list */
        error = agcGfx1013ValidateWave32VsPs(&data->shaders[shader]);
        if (error != AGC_OK) {
            return PS5AGC_SetError("validating checked-in PSBC shader state", error);
        }
    }
    return 0;
}

static int PS5AGC_GetOutputSize(SDL_Renderer *renderer, int *w, int *h)
{
    PS5AGC_RenderData *data = (PS5AGC_RenderData *)renderer->driverdata;
    *w = (int)data->mode.width;
    *h = (int)data->mode.height;
    return 0;
}

static SDL_bool PS5AGC_ConvertBlendFactor(SDL_BlendFactor input,
                                          AgcGfx1013BlendFactor *output)
{
    switch (input) {
    case SDL_BLENDFACTOR_ZERO: *output = AGC_GFX1013_BLEND_ZERO; break;
    case SDL_BLENDFACTOR_ONE: *output = AGC_GFX1013_BLEND_ONE; break;
    case SDL_BLENDFACTOR_SRC_COLOR: *output = AGC_GFX1013_BLEND_SRC_COLOR; break;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR: *output = AGC_GFX1013_BLEND_ONE_MINUS_SRC_COLOR; break;
    case SDL_BLENDFACTOR_SRC_ALPHA: *output = AGC_GFX1013_BLEND_SRC_ALPHA; break;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA: *output = AGC_GFX1013_BLEND_ONE_MINUS_SRC_ALPHA; break;
    case SDL_BLENDFACTOR_DST_COLOR: *output = AGC_GFX1013_BLEND_DST_COLOR; break;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR: *output = AGC_GFX1013_BLEND_ONE_MINUS_DST_COLOR; break;
    case SDL_BLENDFACTOR_DST_ALPHA: *output = AGC_GFX1013_BLEND_DST_ALPHA; break;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA: *output = AGC_GFX1013_BLEND_ONE_MINUS_DST_ALPHA; break;
    default: return SDL_FALSE;
    }
    return SDL_TRUE;
}

static SDL_bool PS5AGC_ConvertBlendOp(SDL_BlendOperation input,
                                      AgcGfx1013BlendOp *output)
{
    switch (input) {
    case SDL_BLENDOPERATION_ADD: *output = AGC_GFX1013_BLEND_OP_ADD; break;
    case SDL_BLENDOPERATION_SUBTRACT: *output = AGC_GFX1013_BLEND_OP_SUBTRACT; break;
    case SDL_BLENDOPERATION_REV_SUBTRACT: *output = AGC_GFX1013_BLEND_OP_REVERSE_SUBTRACT; break;
    case SDL_BLENDOPERATION_MINIMUM: *output = AGC_GFX1013_BLEND_OP_MIN; break;
    case SDL_BLENDOPERATION_MAXIMUM: *output = AGC_GFX1013_BLEND_OP_MAX; break;
    default: return SDL_FALSE;
    }
    return SDL_TRUE;
}

static SDL_bool PS5AGC_SupportsBlendMode(SDL_Renderer *renderer,
                                         SDL_BlendMode blend)
{
    AgcGfx1013BlendFactor factor;
    AgcGfx1013BlendOp operation;
    (void)renderer;
    return PS5AGC_ConvertBlendFactor(SDL_GetBlendModeSrcColorFactor(blend), &factor) &&
           PS5AGC_ConvertBlendFactor(SDL_GetBlendModeDstColorFactor(blend), &factor) &&
           PS5AGC_ConvertBlendOp(SDL_GetBlendModeColorOperation(blend), &operation) &&
           PS5AGC_ConvertBlendFactor(SDL_GetBlendModeSrcAlphaFactor(blend), &factor) &&
           PS5AGC_ConvertBlendFactor(SDL_GetBlendModeDstAlphaFactor(blend), &factor) &&
           PS5AGC_ConvertBlendOp(SDL_GetBlendModeAlphaOperation(blend), &operation);
}

static int PS5AGC_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    PS5AGC_TextureData *data;
    size_t y_bytes;
    size_t chroma_bytes;
    size_t v_offset;
    size_t allocation;
    int chroma_width;
    int chroma_height;
    int32_t error;
    (void)renderer;

    if (texture->format != SDL_PIXELFORMAT_ABGR8888 &&
        texture->format != SDL_PIXELFORMAT_IYUV &&
        texture->format != SDL_PIXELFORMAT_YV12 &&
        texture->format != SDL_PIXELFORMAT_NV12 &&
        texture->format != SDL_PIXELFORMAT_NV21) {
        return SDL_SetError("unsupported ps5agc texture format");
    }
    if (texture->access == SDL_TEXTUREACCESS_TARGET &&
        texture->format != SDL_PIXELFORMAT_ABGR8888) {
        return SDL_SetError("ps5agc render targets must use ABGR8888");
    }
    if (texture->w <= 0 || texture->h <= 0 ||
        (size_t)texture->w > SIZE_MAX / 4u / (size_t)texture->h) {
        return SDL_SetError("invalid ps5agc texture dimensions");
    }
    data = (PS5AGC_TextureData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }
    data->format = texture->format;
    data->scale_mode = texture->scaleMode;
    if (texture->format == SDL_PIXELFORMAT_ABGR8888) {
        data->pitch = (int)PS5AGC_Align((size_t)texture->w * 4u, 256u);
        data->plane_pitch[0] = data->pitch;
        data->plane_width[0] = texture->access == SDL_TEXTUREACCESS_TARGET ?
            (Uint32)(data->pitch / 4) : (Uint32)texture->w;
        data->plane_height[0] = (Uint32)texture->h;
        data->plane_count = 1u;
        data->pixel_bytes = (size_t)data->pitch * texture->h;
    } else {
        chroma_width = (texture->w + 1) / 2;
        chroma_height = (texture->h + 1) / 2;
        data->pitch = texture->w;
        data->plane_pitch[0] = (int)PS5AGC_Align((size_t)texture->w, 256u);
        data->plane_width[0] = (Uint32)texture->w;
        data->plane_height[0] = (Uint32)texture->h;
        y_bytes = (size_t)data->plane_pitch[0] * texture->h;
        if (texture->format == SDL_PIXELFORMAT_NV12 ||
            texture->format == SDL_PIXELFORMAT_NV21) {
            data->plane_count = 2u;
            data->plane_pitch[1] = (int)PS5AGC_Align(
                (size_t)chroma_width * 2u, 256u);
            data->plane_width[1] = (Uint32)chroma_width;
            data->plane_height[1] = (Uint32)chroma_height;
            data->plane_offset[1] = PS5AGC_Align(y_bytes, 256u);
            chroma_bytes = (size_t)data->plane_pitch[1] * chroma_height;
            data->lock_bytes = (size_t)texture->w * texture->h +
                (size_t)chroma_width * 2u * chroma_height;
            data->pixel_bytes = data->plane_offset[1] + chroma_bytes;
        } else {
            data->plane_count = 3u;
            data->plane_pitch[1] = (int)PS5AGC_Align(
                (size_t)chroma_width, 256u);
            data->plane_pitch[2] = data->plane_pitch[1];
            data->plane_width[1] = data->plane_width[2] = (Uint32)chroma_width;
            data->plane_height[1] = data->plane_height[2] = (Uint32)chroma_height;
            chroma_bytes = (size_t)data->plane_pitch[1] * chroma_height;
            data->lock_bytes = (size_t)texture->w * texture->h +
                (size_t)chroma_width * chroma_height * 2u;
            if (texture->format == SDL_PIXELFORMAT_IYUV) {
                data->plane_offset[1] = PS5AGC_Align(y_bytes, 256u);
                data->plane_offset[2] = PS5AGC_Align(
                    data->plane_offset[1] + chroma_bytes, 256u);
                data->pixel_bytes = data->plane_offset[2] + chroma_bytes;
            } else {
                v_offset = PS5AGC_Align(y_bytes, 256u);
                data->plane_offset[1] = PS5AGC_Align(v_offset + chroma_bytes, 256u);
                data->plane_offset[2] = v_offset;
                data->pixel_bytes = data->plane_offset[1] + chroma_bytes;
            }
        }
    }
    data->descriptor_offset = PS5AGC_Align(data->pixel_bytes, 256u);
    allocation = data->descriptor_offset +
        data->plane_count * sizeof(*data->descriptor);
    error = agcGpuMemoryAllocateFlexible(&data->memory, allocation, 256u,
                                         "SDL ps5agc texture");
    if (error != AGC_OK) {
        SDL_free(data);
        return PS5AGC_SetError("allocating an OpenAGC texture", error);
    }
    data->descriptor = (AgcGfx1013CombinedImageSamplerDescriptor *)
        ((Uint8 *)data->memory.cpu_address + data->descriptor_offset);
    data->usage = AGC_GFX1013_RESOURCE_USAGE_UNDEFINED;
    texture->driverdata = data;
    if (PS5AGC_InitDescriptor(data) < 0) {
        agcGpuMemoryFreeFlexible(&data->memory);
        SDL_free(data);
        texture->driverdata = NULL;
        return -1;
    }
    return 0;
}

static void PS5AGC_CopyPlane(PS5AGC_TextureData *data, Uint32 plane,
                             int x, int y, int width, int height,
                             int bytes_per_sample, const Uint8 *pixels,
                             int pitch)
{
    Uint8 *dst = (Uint8 *)data->memory.cpu_address + data->plane_offset[plane] +
        (size_t)y * data->plane_pitch[plane] + (size_t)x * bytes_per_sample;
    int row;

    for (row = 0; row < height; ++row) {
        SDL_memcpy(dst, pixels, (size_t)width * bytes_per_sample);
        dst += data->plane_pitch[plane];
        pixels += pitch;
    }
}

static int PS5AGC_FinishTextureUpdate(PS5AGC_TextureData *data)
{
    if (PS5AGC_Flush(&data->memory, 0, data->pixel_bytes,
                     "publishing an OpenAGC texture update") < 0) {
        return -1;
    }
    data->usage = AGC_GFX1013_RESOURCE_USAGE_HOST_READ;
    return 0;
}

static int PS5AGC_UpdateTextureYUV(SDL_Renderer *renderer, SDL_Texture *texture,
                                   const SDL_Rect *rect,
                                   const Uint8 *Yplane, int Ypitch,
                                   const Uint8 *Uplane, int Upitch,
                                   const Uint8 *Vplane, int Vpitch)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    const int chroma_width = (rect->w + 1) / 2;
    const int chroma_height = (rect->h + 1) / 2;
    (void)renderer;

    PS5AGC_CopyPlane(data, 0u, rect->x, rect->y, rect->w, rect->h,
                     1, Yplane, Ypitch);
    PS5AGC_CopyPlane(data, 1u, rect->x / 2, rect->y / 2,
                     chroma_width, chroma_height, 1, Uplane, Upitch);
    PS5AGC_CopyPlane(data, 2u, rect->x / 2, rect->y / 2,
                     chroma_width, chroma_height, 1, Vplane, Vpitch);
    return PS5AGC_FinishTextureUpdate(data);
}

static int PS5AGC_UpdateTextureNV(SDL_Renderer *renderer, SDL_Texture *texture,
                                  const SDL_Rect *rect,
                                  const Uint8 *Yplane, int Ypitch,
                                  const Uint8 *UVplane, int UVpitch)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    const int chroma_width = (rect->w + 1) / 2;
    const int chroma_height = (rect->h + 1) / 2;
    (void)renderer;

    PS5AGC_CopyPlane(data, 0u, rect->x, rect->y, rect->w, rect->h,
                     1, Yplane, Ypitch);
    PS5AGC_CopyPlane(data, 1u, rect->x / 2, rect->y / 2,
                     chroma_width, chroma_height, 2, UVplane, UVpitch);
    return PS5AGC_FinishTextureUpdate(data);
}

static int PS5AGC_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                                const SDL_Rect *rect, const void *pixels,
                                int pitch)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    const Uint8 *Yplane = (const Uint8 *)pixels;
    const Uint8 *first_chroma;
    const Uint8 *second_chroma;
    const int chroma_height = (rect->h + 1) / 2;
    const int chroma_pitch = (pitch + 1) / 2;

    if (texture->format == SDL_PIXELFORMAT_ABGR8888) {
        PS5AGC_CopyPlane(data, 0u, rect->x, rect->y, rect->w, rect->h,
                         4, Yplane, pitch);
        return PS5AGC_FinishTextureUpdate(data);
    }
    first_chroma = Yplane + (size_t)pitch * rect->h;
    if (texture->format == SDL_PIXELFORMAT_NV12 ||
        texture->format == SDL_PIXELFORMAT_NV21) {
        return PS5AGC_UpdateTextureNV(renderer, texture, rect, Yplane, pitch,
                                      first_chroma, chroma_pitch * 2);
    }
    second_chroma = first_chroma + (size_t)chroma_pitch * chroma_height;
    if (texture->format == SDL_PIXELFORMAT_YV12) {
        return PS5AGC_UpdateTextureYUV(renderer, texture, rect, Yplane, pitch,
                                       second_chroma, chroma_pitch,
                                       first_chroma, chroma_pitch);
    }
    return PS5AGC_UpdateTextureYUV(renderer, texture, rect, Yplane, pitch,
                                   first_chroma, chroma_pitch,
                                   second_chroma, chroma_pitch);
}

static int PS5AGC_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                              const SDL_Rect *rect, void **pixels, int *pitch)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    (void)renderer;

    if (texture->format != SDL_PIXELFORMAT_ABGR8888) {
        if (rect->x != 0 || rect->y != 0 ||
            rect->w != texture->w || rect->h != texture->h) {
            return SDL_SetError("planar ps5agc textures only support full locks");
        }
        if (!data->lock_buffer) {
            data->lock_buffer = SDL_malloc(data->lock_bytes);
            if (!data->lock_buffer) {
                return SDL_OutOfMemory();
            }
        }
        *pixels = data->lock_buffer;
        *pitch = data->pitch;
        return 0;
    }
    *pixels = (Uint8 *)data->memory.cpu_address + data->plane_offset[0] +
        (size_t)rect->y * data->pitch + (size_t)rect->x * 4u;
    *pitch = data->pitch;
    return 0;
}

static void PS5AGC_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    (void)renderer;
    if (data->lock_buffer) {
        SDL_Rect rect = { 0, 0, texture->w, texture->h };
        if (PS5AGC_UpdateTexture(renderer, texture, &rect,
                                 data->lock_buffer, data->pitch) < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "%s", SDL_GetError());
        }
        return;
    }
    if (PS5AGC_Flush(&data->memory, 0, data->pixel_bytes,
                     "publishing a locked OpenAGC texture") < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "%s", SDL_GetError());
        return;
    }
    data->usage = AGC_GFX1013_RESOURCE_USAGE_HOST_READ;
}

static void PS5AGC_SetTextureScaleMode(SDL_Renderer *renderer,
                                       SDL_Texture *texture,
                                       SDL_ScaleMode scale_mode)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    (void)renderer;
    data->scale_mode = scale_mode;
    if (PS5AGC_InitDescriptor(data) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "%s", SDL_GetError());
    }
}

static int PS5AGC_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    (void)renderer;
    (void)texture;
    return 0;
}

static int PS5AGC_QueueNoOp(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    (void)renderer;
    (void)cmd;
    return 0;
}

static int PS5AGC_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd,
                                SDL_Texture *texture, const float *xy,
                                int xy_stride, const SDL_Color *color,
                                int color_stride, const float *uv,
                                int uv_stride, int num_vertices,
                                const void *indices, int num_indices,
                                int size_indices, float scale_x, float scale_y)
{
    const int count = indices ? num_indices : num_vertices;
    const PS5AGC_TextureData *texture_data = texture ?
        (const PS5AGC_TextureData *)texture->driverdata : NULL;
    const float u_scale = texture_data ?
        (float)texture->w / (float)texture_data->plane_width[0] : 1.0f;
    PS5AGC_Vertex *vertices;
    int i;

    vertices = (PS5AGC_Vertex *)SDL_AllocateRenderVertices(
        renderer, (size_t)count * sizeof(*vertices), 0, &cmd->data.draw.first);
    if (!vertices) {
        return -1;
    }
    cmd->data.draw.count = (size_t)count;
    for (i = 0; i < count; ++i) {
        int source = i;
        const float *position;
        const SDL_Color *vertex_color;
        const float *texcoord = NULL;
        if (indices) {
            source = size_indices == 4 ? (int)((const Uint32 *)indices)[i] :
                     size_indices == 2 ? (int)((const Uint16 *)indices)[i] :
                                         (int)((const Uint8 *)indices)[i];
        }
        position = (const float *)((const Uint8 *)xy + source * xy_stride);
        vertex_color = (const SDL_Color *)((const Uint8 *)color + source * color_stride);
        if (uv) {
            texcoord = (const float *)((const Uint8 *)uv + source * uv_stride);
        }
        vertices[i].x = position[0] * scale_x;
        vertices[i].y = position[1] * scale_y;
        vertices[i].r = vertex_color->r / 255.0f;
        vertices[i].g = vertex_color->g / 255.0f;
        vertices[i].b = vertex_color->b / 255.0f;
        vertices[i].a = vertex_color->a / 255.0f;
        vertices[i].u = texcoord ? texcoord[0] * u_scale : 0.5f;
        vertices[i].v = texcoord ? texcoord[1] : 0.5f;
    }
    return 0;
}

static int PS5AGC_QueueDrawPoints(SDL_Renderer *renderer,
                                  SDL_RenderCommand *cmd,
                                  const SDL_FPoint *points, int count)
{
    PS5AGC_Vertex *vertices;
    int i;
    const float r = cmd->data.draw.r / 255.0f;
    const float g = cmd->data.draw.g / 255.0f;
    const float b = cmd->data.draw.b / 255.0f;
    const float a = cmd->data.draw.a / 255.0f;

    vertices = (PS5AGC_Vertex *)SDL_AllocateRenderVertices(
        renderer, (size_t)count * 6u * sizeof(*vertices), 0,
        &cmd->data.draw.first);
    if (!vertices) {
        return -1;
    }
    cmd->data.draw.count = (size_t)count * 6u;
    for (i = 0; i < count; ++i) {
        const float x = points[i].x;
        const float y = points[i].y;
        static const Uint8 corners[12] = { 0,0, 1,0, 1,1, 0,0, 1,1, 0,1 };
        int j;
        for (j = 0; j < 6; ++j) {
            PS5AGC_Vertex *v = &vertices[i * 6 + j];
            v->x = x + corners[j * 2];
            v->y = y + corners[j * 2 + 1];
            v->r = r;
            v->g = g;
            v->b = b;
            v->a = a;
            v->u = v->v = 0.5f;
        }
    }
    return 0;
}

static int PS5AGC_SetBlend(SceAgcCb *cb, SDL_BlendMode blend)
{
    AgcGfx1013ColorBlendState state;
    AgcGfx1013ColorBlendTargetState *target;
    int32_t error;

    SDL_zero(state);
    state.target_count = 1u;
    target = &state.targets[0];
    target->write_mask = 0xfu;
    if (blend != SDL_BLENDMODE_NONE) {
        target->enable = 1u;
        target->separate_alpha = 1u;
        if (!PS5AGC_ConvertBlendFactor(SDL_GetBlendModeSrcColorFactor(blend), &target->color_source) ||
            !PS5AGC_ConvertBlendFactor(SDL_GetBlendModeDstColorFactor(blend), &target->color_destination) ||
            !PS5AGC_ConvertBlendOp(SDL_GetBlendModeColorOperation(blend), &target->color_operation) ||
            !PS5AGC_ConvertBlendFactor(SDL_GetBlendModeSrcAlphaFactor(blend), &target->alpha_source) ||
            !PS5AGC_ConvertBlendFactor(SDL_GetBlendModeDstAlphaFactor(blend), &target->alpha_destination) ||
            !PS5AGC_ConvertBlendOp(SDL_GetBlendModeAlphaOperation(blend), &target->alpha_operation)) {
            return SDL_SetError("unsupported ps5agc blend mode");
        }
    }
    error = agcGfx1013SetColorBlendState(cb, &state);
    return error == AGC_OK ? 0 : PS5AGC_SetError("recording OpenAGC blend state", error);
}

static void PS5AGC_TransformVertices(PS5AGC_Vertex *vertices, size_t count,
                                     const SDL_Rect *viewport,
                                     int width, int height)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        const float x = viewport->x + vertices[i].x;
        const float y = viewport->y + vertices[i].y;
        vertices[i].x = (x * 2.0f / width) - 1.0f;
        vertices[i].y = 1.0f - (y * 2.0f / height);
    }
}

static PS5AGC_ShaderKind PS5AGC_SelectShader(SDL_Texture *texture)
{
    SDL_YUV_CONVERSION_MODE mode;
    SDL_bool nv;

    if (!texture) {
        return PS5AGC_SHADER_SOLID;
    }
    if (texture->format == SDL_PIXELFORMAT_ABGR8888) {
        return PS5AGC_SHADER_RGBA;
    }
    nv = texture->format == SDL_PIXELFORMAT_NV12 ||
         texture->format == SDL_PIXELFORMAT_NV21;
    mode = SDL_GetYUVConversionModeForResolution(texture->w, texture->h);
    switch (mode) {
    case SDL_YUV_CONVERSION_JPEG:
        return nv ? PS5AGC_SHADER_NV_JPEG : PS5AGC_SHADER_PLANAR_JPEG;
    case SDL_YUV_CONVERSION_BT709:
        return nv ? PS5AGC_SHADER_NV_BT709 : PS5AGC_SHADER_PLANAR_BT709;
    case SDL_YUV_CONVERSION_BT601:
    default:
        return nv ? PS5AGC_SHADER_NV_BT601 : PS5AGC_SHADER_PLANAR_BT601;
    }
}

static int PS5AGC_RecordDraw(PS5AGC_RenderData *data, SceAgcCb *cb,
                              const PS5AGC_Vertex *source, size_t count,
                              SDL_Texture *texture, SDL_BlendMode blend,
                              size_t *upload_offset,
                              const AgcGfx1013FrameState *frame)
{
    PS5AGC_TextureData *texture_data = texture ?
        (PS5AGC_TextureData *)texture->driverdata : NULL;
    AgcGfx1013BufferDescriptor *descriptor;
    PS5AGC_Vertex *vertices;
    AgcGfx1013ResourceTableBinding primitive_table;
    AgcGfx1013ResourceTableBinding pixel_table;
    AgcGfx1013BaselineDrawState draw;
    AgcGfx1013IndexedDrawState indexed;
    Uint32 *indices;
    size_t descriptor_offset;
    size_t vertex_offset;
    size_t index_offset;
    size_t i;
    int32_t error;

    descriptor_offset = PS5AGC_Align(*upload_offset, 16u);
    vertex_offset = PS5AGC_Align(descriptor_offset + sizeof(*descriptor), 32u);
    if (count > UINT32_MAX || vertex_offset > data->upload_memory.size ||
        count > (data->upload_memory.size - vertex_offset) / sizeof(*vertices)) {
        return SDL_SetError("ps5agc per-submit vertex upload pool exhausted");
    }
    index_offset = PS5AGC_Align(vertex_offset + count * sizeof(*vertices),
                                sizeof(*indices));
    if (index_offset > data->upload_memory.size ||
        count > (data->upload_memory.size - index_offset) / sizeof(*indices)) {
        return SDL_SetError("ps5agc per-submit index upload pool exhausted");
    }
    descriptor = (AgcGfx1013BufferDescriptor *)
        ((Uint8 *)data->upload_memory.cpu_address + descriptor_offset);
    vertices = (PS5AGC_Vertex *)
        ((Uint8 *)data->upload_memory.cpu_address + vertex_offset);
    indices = (Uint32 *)
        ((Uint8 *)data->upload_memory.cpu_address + index_offset);
    SDL_memcpy(vertices, source, count * sizeof(*vertices));
    for (i = 0; i < count; ++i) {
        indices[i] = (Uint32)i;
    }
    PS5AGC_TransformVertices(vertices, count, &data->viewport,
                            (int)frame->viewport.width, (int)frame->viewport.height);
    error = agcGfx1013BufferDescriptorEncode(
        descriptor, data->upload_memory.gpu_address + vertex_offset,
        sizeof(*vertices), (Uint32)count);
    if (error != AGC_OK) {
        return PS5AGC_SetError("encoding an OpenAGC vertex descriptor", error);
    }
    *upload_offset = index_offset + count * sizeof(*indices);
    if (PS5AGC_SetBlend(cb, blend) < 0) {
        return -1;
    }
    primitive_table.placeholder = OPENAGC_VERTEX_BUFFER_TABLE_PLACEHOLDER;
    primitive_table.address = data->upload_memory.gpu_address + descriptor_offset;
    pixel_table.placeholder = OPENAGC_DESCRIPTOR_SET_PLACEHOLDER(0u);
    pixel_table.address = texture_data ?
        texture_data->memory.gpu_address + texture_data->descriptor_offset : 0u;
    SDL_zero(draw);
    draw.shaders = data->shaders[PS5AGC_SelectShader(texture)];
    draw.frame = frame;
    draw.primitive_resource_tables = &primitive_table;
    draw.num_primitive_resource_tables = 1u;
    draw.pixel_resource_tables = texture_data ? &pixel_table : NULL;
    draw.num_pixel_resource_tables = texture_data ? 1u : 0u;
    draw.index_type = kAgcIndexSize32;
    draw.instance_count = 1u;
    draw.vertex_count = (Uint32)count;
    draw.draw_modifier = PS5AGC_DRAW_MODIFIER;
    SDL_zero(indexed);
    indexed.draw = draw;
    indexed.index_buffer_address = data->upload_memory.gpu_address + index_offset;
    indexed.index_buffer_count = (Uint32)count;
    indexed.index_count = (Uint32)count;
    error = agcGfx1013DrawBaselineIndexed(cb, &indexed);
    return error == AGC_OK ? 0 : PS5AGC_SetError("recording an OpenAGC draw", error);
}

static void PS5AGC_UpdateScissor(PS5AGC_RenderData *data,
                                 AgcGfx1013FrameState *frame)
{
    SDL_Rect bounds = { 0, 0, (int)frame->viewport.width, (int)frame->viewport.height };
    SDL_Rect result = data->viewport;
    if (data->clip_enabled) {
        SDL_Rect clip = { data->viewport.x + data->cliprect.x,
                          data->viewport.y + data->cliprect.y,
                          data->cliprect.w, data->cliprect.h };
        SDL_IntersectRect(&result, &clip, &result);
    }
    SDL_IntersectRect(&result, &bounds, &result);
    frame->scissor.left = (Uint32)SDL_max(result.x, 0);
    frame->scissor.top = (Uint32)SDL_max(result.y, 0);
    frame->scissor.right = (Uint32)SDL_max(result.x + result.w, result.x);
    frame->scissor.bottom = (Uint32)SDL_max(result.y + result.h, result.y);
}

static int PS5AGC_RecordSampledTransition(
    SceAgcCb *cb, PS5AGC_TextureData *texture,
    PS5AGC_TextureData **pending_textures, size_t pending_capacity,
    size_t *pending_count)
{
    size_t i;

    for (i = 0; i < *pending_count; ++i) {
        if (pending_textures[i] == texture) {
            return 0;
        }
    }
    if (*pending_count >= pending_capacity) {
        return SDL_SetError("ps5agc sampled-texture transition table overflow");
    }
    if (PS5AGC_Transition(cb, texture->usage,
                          AGC_GFX1013_RESOURCE_USAGE_SHADER_READ) < 0) {
        return -1;
    }
    pending_textures[*pending_count] = texture;
    ++*pending_count;
    return 0;
}

static int PS5AGC_RunCommandQueue(SDL_Renderer *renderer,
                                  SDL_RenderCommand *cmd,
                                  void *queued_vertices, size_t vertsize)
{
    PS5AGC_RenderData *data = (PS5AGC_RenderData *)renderer->driverdata;
    PS5AGC_TextureData *target_texture = renderer->target ?
        (PS5AGC_TextureData *)renderer->target->driverdata : NULL;
    const Uint32 display_index = (Uint32)(data->frame_id % PS5AGC_BUFFER_COUNT);
    AgcGfx1013ResourceUsage *target_usage = target_texture ?
        &target_texture->usage : &data->render_usage[display_index];
    const Uint64 target_address = target_texture ?
        target_texture->memory.gpu_address :
        data->render_memory[display_index].gpu_address;
    const int width = target_texture ? renderer->target->w : (int)data->mode.width;
    const int height = target_texture ? renderer->target->h : (int)data->mode.height;
    const Uint32 surface_width = target_texture ?
        (Uint32)(target_texture->pitch / 4) : (Uint32)width;
    const AgcGfx1013ColorTargetFormat format = target_texture ?
        AGC_GFX1013_RT_FORMAT_RGBA8_UNORM : AGC_GFX1013_RT_FORMAT_BGRA8_UNORM;
    AgcGfx1013FrameState frame;
    AgcGfx1013GraphicsDefaultStats stats;
    SceAgcCb cb;
    SDL_RenderCommand *scan;
    PS5AGC_TextureData **pending_textures = NULL;
    size_t pending_capacity = 0u;
    size_t pending_count = 0u;
    size_t upload_offset = 0u;
    size_t commit_index;
    SDL_bool pending_isstack = SDL_FALSE;
    int result = -1;
    int32_t error;
    (void)vertsize;

    if (!cmd) {
        return 0;
    }
    for (scan = cmd; scan; scan = scan->next) {
        if ((scan->command == SDL_RENDERCMD_DRAW_POINTS ||
             scan->command == SDL_RENDERCMD_GEOMETRY) &&
            scan->data.draw.texture) {
            ++pending_capacity;
        }
    }
    if (pending_capacity > 0u) {
        if (pending_capacity > SIZE_MAX / sizeof(*pending_textures)) {
            return SDL_OutOfMemory();
        }
        pending_textures = SDL_small_alloc(
            PS5AGC_TextureData *, pending_capacity, &pending_isstack);
        if (!pending_textures) {
            return SDL_OutOfMemory();
        }
    }
    agcCbInit(&cb, data->command_memory[display_index].cpu_address,
              PS5AGC_COMMAND_BYTES);
    if (PS5AGC_Transition(&cb, *target_usage,
                          AGC_GFX1013_RESOURCE_USAGE_RENDER_TARGET) < 0) {
        goto done;
    }
    SDL_zero(frame);
    error = agcGfx1013InitColorTarget(&frame.color_target, target_address,
                                      surface_width, (Uint32)height, format);
    if (error != AGC_OK) {
        PS5AGC_SetError("initializing an OpenAGC color target", error);
        goto done;
    }
    frame.color_target_count = 1u;
    frame.viewport.width = (Uint32)width;
    frame.viewport.height = (Uint32)height;
    frame.scissor.right = (Uint32)width;
    frame.scissor.bottom = (Uint32)height;
    frame.target_mask = AGC_GFX1013_TARGET_MASK_RGBA0;
    frame.context_load_control = AGC_GFX1013_CONTEXT_CONTROL_ENABLE;
    frame.context_shadow_control = AGC_GFX1013_CONTEXT_CONTROL_ENABLE;
    frame.max_vertex_index = 0xffffffffu;
    frame.ngg_mode_control = AGC_GFX1013_NGG_MODE_CONTROL;
    frame.vertex_reuse_block_control = AGC_GFX1013_VERTEX_REUSE_BLOCK;
    frame.instance_step_rate = 1u;
    error = agcGfx1013BuildFramePrologue(&cb, &frame, &stats);
    if (error != AGC_OK) {
        PS5AGC_SetError("recording the OpenAGC frame prologue", error);
        goto done;
    }
    data->viewport.x = 0;
    data->viewport.y = 0;
    data->viewport.w = width;
    data->viewport.h = height;
    data->clip_enabled = SDL_FALSE;

    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETVIEWPORT:
            data->viewport = cmd->data.viewport.rect;
            PS5AGC_UpdateScissor(data, &frame);
            error = agcGfx1013SetScissor(&cb, &frame.scissor);
            if (error != AGC_OK) {
                PS5AGC_SetError("recording an OpenAGC viewport scissor", error);
                goto done;
            }
            break;
        case SDL_RENDERCMD_SETCLIPRECT:
            data->clip_enabled = cmd->data.cliprect.enabled;
            data->cliprect = cmd->data.cliprect.rect;
            PS5AGC_UpdateScissor(data, &frame);
            error = agcGfx1013SetScissor(&cb, &frame.scissor);
            if (error != AGC_OK) {
                PS5AGC_SetError("recording an OpenAGC clip scissor", error);
                goto done;
            }
            break;
        case SDL_RENDERCMD_CLEAR:
        {
            PS5AGC_Vertex clear_vertices[6];
            int i;
            static const float corners[12] = { 0,0, 1,0, 1,1, 0,0, 1,1, 0,1 };
            SDL_Rect saved_viewport = data->viewport;
            AgcGfx1013ScissorState saved_scissor = frame.scissor;
            data->viewport = (SDL_Rect){ 0, 0, width, height };
            frame.scissor = (AgcGfx1013ScissorState){ 0, 0, (Uint32)width, (Uint32)height };
            error = agcGfx1013SetScissor(&cb, &frame.scissor);
            if (error != AGC_OK) {
                PS5AGC_SetError("recording an OpenAGC clear scissor", error);
                goto done;
            }
            for (i = 0; i < 6; ++i) {
                clear_vertices[i].x = corners[i * 2] * width;
                clear_vertices[i].y = corners[i * 2 + 1] * height;
                clear_vertices[i].r = cmd->data.color.r / 255.0f;
                clear_vertices[i].g = cmd->data.color.g / 255.0f;
                clear_vertices[i].b = cmd->data.color.b / 255.0f;
                clear_vertices[i].a = cmd->data.color.a / 255.0f;
                clear_vertices[i].u = clear_vertices[i].v = 0.5f;
            }
            if (PS5AGC_RecordDraw(data, &cb, clear_vertices, 6u, NULL,
                                  SDL_BLENDMODE_NONE, &upload_offset, &frame) < 0) {
                goto done;
            }
            data->viewport = saved_viewport;
            frame.scissor = saved_scissor;
            error = agcGfx1013SetScissor(&cb, &frame.scissor);
            if (error != AGC_OK) {
                PS5AGC_SetError("restoring an OpenAGC scissor", error);
                goto done;
            }
            break;
        }
        case SDL_RENDERCMD_DRAW_POINTS:
        case SDL_RENDERCMD_GEOMETRY:
        {
            PS5AGC_TextureData *sampled = cmd->data.draw.texture ?
                (PS5AGC_TextureData *)cmd->data.draw.texture->driverdata : NULL;
            const PS5AGC_Vertex *vertices = (const PS5AGC_Vertex *)
                ((const Uint8 *)queued_vertices + cmd->data.draw.first);
            if (sampled) {
                if (sampled == target_texture) {
                    SDL_SetError("ps5agc cannot sample the active render target");
                    goto done;
                }
                if (PS5AGC_RecordSampledTransition(
                        &cb, sampled, pending_textures, pending_capacity,
                        &pending_count) < 0) {
                    goto done;
                }
            }
            if (PS5AGC_RecordDraw(data, &cb, vertices, cmd->data.draw.count,
                                  cmd->data.draw.texture, cmd->data.draw.blend,
                                  &upload_offset, &frame) < 0) {
                goto done;
            }
            break;
        }
        default:
            break;
        }
        cmd = cmd->next;
    }
    if (upload_offset != 0u &&
        PS5AGC_Flush(&data->upload_memory, 0, upload_offset,
                     "publishing OpenAGC draw vertices") < 0) {
        goto done;
    }
    if (PS5AGC_SubmitAndWait(data, display_index, &cb,
                             "submitting OpenAGC draws") < 0) {
        goto done;
    }
    *target_usage = AGC_GFX1013_RESOURCE_USAGE_RENDER_TARGET;
    for (commit_index = 0; commit_index < pending_count; ++commit_index) {
        pending_textures[commit_index]->usage = AGC_GFX1013_RESOURCE_USAGE_SHADER_READ;
    }
    if (!target_texture) {
        data->screen_dirty = SDL_TRUE;
        data->screen_synced = SDL_FALSE;
    }
    result = 0;

done:
    if (pending_textures) {
        SDL_small_free(pending_textures, pending_isstack);
    }
    return result;
}

static int PS5AGC_SyncDisplayBuffer(PS5AGC_RenderData *data, Uint32 index)
{
    const size_t frame_bytes = (size_t)data->mode.width * data->mode.height * 4u;
    const size_t display_offset = (size_t)index * data->display_stride;
    SceAgcCb cb;
    int32_t error;

    if (data->screen_synced) {
        return 0;
    }
    agcCbInit(&cb, data->command_memory[index].cpu_address,
              PS5AGC_COMMAND_BYTES);
    if (PS5AGC_Transition(&cb, data->render_usage[index],
                          AGC_GFX1013_RESOURCE_USAGE_COPY_SOURCE) < 0 ||
        PS5AGC_Transition(&cb, data->display_usage[index],
                          AGC_GFX1013_RESOURCE_USAGE_COPY_DESTINATION) < 0) {
        return -1;
    }
    error = agcGfx1013CopyBuffer(
        &cb, data->render_memory[index].gpu_address,
        data->display_memory.gpu_address + display_offset, frame_bytes);
    if (error != AGC_OK) {
        return PS5AGC_SetError("recording the OpenAGC scanout copy", error);
    }
    if (PS5AGC_Transition(&cb, AGC_GFX1013_RESOURCE_USAGE_COPY_DESTINATION,
                          AGC_GFX1013_RESOURCE_USAGE_HOST_READ) < 0) {
        return -1;
    }
    if (PS5AGC_SubmitAndWait(data, index, &cb,
                             "submitting the OpenAGC scanout copy") < 0) {
        return -1;
    }
    data->render_usage[index] = AGC_GFX1013_RESOURCE_USAGE_COPY_SOURCE;
    data->display_usage[index] = AGC_GFX1013_RESOURCE_USAGE_HOST_READ;
    data->screen_synced = SDL_TRUE;
    return 0;
}

static int PS5AGC_RenderReadPixels(SDL_Renderer *renderer,
                                   const SDL_Rect *rect, Uint32 format,
                                   void *pixels, int pitch)
{
    PS5AGC_RenderData *data = (PS5AGC_RenderData *)renderer->driverdata;
    PS5AGC_TextureData *texture = renderer->target ?
        (PS5AGC_TextureData *)renderer->target->driverdata : NULL;
    const Uint32 index = (Uint32)(data->frame_id % PS5AGC_BUFFER_COUNT);
    AgcGpuMemory *memory = texture ? &texture->memory : &data->render_memory[index];
    const size_t display_offset = 0u;
    const int source_pitch = texture ? texture->pitch : (int)data->mode.width * 4;
    Uint8 *source = (Uint8 *)memory->cpu_address + display_offset +
        rect->y * source_pitch + rect->x * 4;
    SceAgcCb cb;
    int32_t error;

    if (texture) {
        agcCbInit(&cb, data->command_memory[index].cpu_address,
                  PS5AGC_COMMAND_BYTES);
        if (PS5AGC_Transition(&cb, texture->usage,
                              AGC_GFX1013_RESOURCE_USAGE_HOST_READ) < 0) {
            return -1;
        }
        if (PS5AGC_SubmitAndWait(data, index, &cb,
                                 "submitting an OpenAGC readback transition") < 0) {
            return -1;
        }
        texture->usage = AGC_GFX1013_RESOURCE_USAGE_HOST_READ;
    } else {
        agcCbInit(&cb, data->command_memory[index].cpu_address,
                  PS5AGC_COMMAND_BYTES);
        if (PS5AGC_Transition(&cb, data->render_usage[index],
                              AGC_GFX1013_RESOURCE_USAGE_HOST_READ) < 0) {
            return -1;
        }
        if (PS5AGC_SubmitAndWait(data, index, &cb,
                                 "submitting an OpenAGC screen readback transition") < 0) {
            return -1;
        }
        data->render_usage[index] = AGC_GFX1013_RESOURCE_USAGE_HOST_READ;
    }
    error = agcGpuMemoryInvalidate(memory,
        display_offset + (size_t)rect->y * source_pitch,
        (size_t)rect->h * source_pitch);
    if (error != AGC_OK) {
        return PS5AGC_SetError("invalidating an OpenAGC readback rectangle", error);
    }
    return SDL_ConvertPixels(rect->w, rect->h,
                             texture ? SDL_PIXELFORMAT_ABGR8888 : SDL_PIXELFORMAT_ARGB8888,
                             source, source_pitch, format, pixels, pitch);
}

static int PS5AGC_SetVSync(SDL_Renderer *renderer, int vsync)
{
    (void)renderer;
    return vsync == 1 ? 0 : SDL_SetError("ps5agc supports FIFO/VSYNC presentation only");
}

static int PS5AGC_Present(SDL_Renderer *renderer)
{
    PS5AGC_RenderData *data = (PS5AGC_RenderData *)renderer->driverdata;
    const Uint32 index = (Uint32)(data->frame_id % PS5AGC_BUFFER_COUNT);
    SceAgcCb cb;
    int32_t error;

    if (data->submission_failed) {
        return SDL_SetError("ps5agc is unavailable after a GPU or presentation failure");
    }
    if (!data->screen_dirty) {
        return 0;
    }
    if (PS5AGC_SyncDisplayBuffer(data, index) < 0) {
        return -1;
    }
    agcCbInit(&cb, data->command_memory[index].cpu_address,
              PS5AGC_COMMAND_BYTES);
    if (PS5AGC_Transition(&cb, data->display_usage[index],
                          AGC_GFX1013_RESOURCE_USAGE_PRESENT) < 0) {
        return -1;
    }
    if (PS5AGC_SubmitAndWait(data, index, &cb,
                             "submitting OpenAGC presentation transition") < 0) {
        return -1;
    }
    data->display_usage[index] = AGC_GFX1013_RESOURCE_USAGE_PRESENT;
    if (PS5AGC_InjectFailure("presentation") < 0) {
        data->submission_failed = SDL_TRUE;
        return -1;
    }
    error = agcVideoOutPresent(data->video_out, index, data->frame_id,
                               PS5AGC_PRESENT_TIMEOUT_US);
    if (error != AGC_OK) {
        data->submission_failed = SDL_TRUE;
        return PS5AGC_SetError("agcVideoOutPresent", error);
    }
    ++data->frame_id;
    data->screen_dirty = SDL_FALSE;
    data->screen_synced = SDL_FALSE;
    return 0;
}

static void PS5AGC_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    (void)renderer;
    if (data) {
        SDL_free(data->lock_buffer);
        agcGpuMemoryFreeFlexible(&data->memory);
        SDL_free(data);
        texture->driverdata = NULL;
    }
}

static void PS5AGC_DestroyData(PS5AGC_RenderData *data)
{
    if (!data) {
        return;
    }
    if (data->video_out) {
        agcVideoOutClose(data->video_out);
    }
    (void)agcDriverShutdown();
    agcGpuMemoryFreeDirect(&data->display_memory);
    agcGpuMemoryFreeDirect(&data->upload_memory);
    agcGpuMemoryFreeFlexible(&data->renderer_memory);
    PS5_ReleasePresentation(data->device, PS5_PRESENTATION_OPENAGC);
    SDL_free(data);
}

static void PS5AGC_DestroyRenderer(SDL_Renderer *renderer)
{
    PS5AGC_DestroyData((PS5AGC_RenderData *)renderer->driverdata);
    SDL_free(renderer);
}

static int PS5AGC_AllocateVideoMemory(AgcGpuMemory *memory, size_t size)
{
    const int32_t error = agcGpuMemoryAllocateDirectWriteCombined(
        memory, size, PS5AGC_DIRECT_ALIGNMENT);
    return error == AGC_OK ? 0 :
        PS5AGC_SetError("allocating OpenAGC VideoOut memory", error);
}

static SDL_Renderer *PS5AGC_CreateRenderer(SDL_Window *window, Uint32 flags)
{
    SDL_VideoDevice *device = SDL_GetVideoDevice();
    PS5AGC_RenderData *data = NULL;
    SDL_Renderer *renderer = NULL;
    AgcVideoOutCreateInfo video_info;
    size_t frame_bytes;
    size_t renderer_bytes = 0u;
    size_t command_offset[PS5AGC_BUFFER_COUNT];
    size_t shader_offset;
    size_t fence_offset[PS5AGC_BUFFER_COUNT];
    size_t render_offset[PS5AGC_BUFFER_COUNT];
    Uint32 i;
    int32_t error;

    if (!device || !device->name || SDL_strcmp(device->name, "ps5") != 0) {
        SDL_SetError("ps5agc requires the PS5 video driver");
        return NULL;
    }
    if (flags & SDL_RENDERER_SOFTWARE) {
        SDL_SetError("ps5agc cannot satisfy SDL_RENDERER_SOFTWARE");
        return NULL;
    }
    if (PS5_AcquirePresentation(device, PS5_PRESENTATION_OPENAGC) < 0) {
        return NULL;
    }
    renderer = (SDL_Renderer *)SDL_calloc(1, sizeof(*renderer));
    data = (PS5AGC_RenderData *)SDL_calloc(1, sizeof(*data));
    if (!renderer || !data) {
        SDL_OutOfMemory();
        SDL_free(renderer);
        SDL_free(data);
        PS5_ReleasePresentation(device, PS5_PRESENTATION_OPENAGC);
        return NULL;
    }
    data->device = device;
    if (PS5AGC_InjectFailure("mode-query") < 0) {
        goto fail;
    }
    error = agcVideoOutGetDefaultMode(&data->mode);
    if (error != AGC_OK) {
        PS5AGC_SetError("agcVideoOutGetDefaultMode", error);
        goto fail;
    }
    if (!data->mode.width || !data->mode.height ||
        data->mode.width > (Uint32)(SDL_MAX_SINT32 / 4) ||
        data->mode.height > (Uint32)SDL_MAX_SINT32 ||
        (size_t)data->mode.width > SIZE_MAX / 4u / data->mode.height) {
        SDL_SetError("OpenAGC returned an invalid default display mode");
        goto fail;
    }
    frame_bytes = (size_t)data->mode.width * data->mode.height * 4u;
    data->display_stride = PS5AGC_Align(frame_bytes, PS5AGC_DIRECT_ALIGNMENT);
    if (data->display_stride > SIZE_MAX / PS5AGC_BUFFER_COUNT) {
        SDL_SetError("OpenAGC display allocation is too large");
        goto fail;
    }
    if (PS5AGC_AllocateVideoMemory(
            &data->display_memory,
            data->display_stride * PS5AGC_BUFFER_COUNT) < 0) {
        goto fail;
    }
    for (i = 0; i < PS5AGC_BUFFER_COUNT; ++i) {
        data->display_buffers[i] = (Uint8 *)data->display_memory.cpu_address +
                                   (size_t)i * data->display_stride;
    }
    SDL_zero(video_info);
    video_info.width = data->mode.width;
    video_info.height = data->mode.height;
    video_info.pitch_pixels = data->mode.width;
    video_info.buffer_count = PS5AGC_BUFFER_COUNT;
    video_info.buffers = data->display_buffers;
    video_info.format = AGC_VIDEO_OUT_FORMAT_BGRA8_SRGB;
    error = agcVideoOutOpen(&video_info, &data->video_out);
    if (error != AGC_OK) {
        PS5AGC_SetError("agcVideoOutOpen", error);
        goto fail;
    }

    error = sce_agc_initialize();
    if (error == AGC_OK && PS5AGC_InjectFailure("initialization") < 0) {
        goto fail;
    }
    if (error == AGC_OK) {
        error = sce_agc_initialize_internal_memory();
    }
    if (error == AGC_OK) {
        error = sceAgcDriverNotifyDefaultStates(0);
    }
    if (error == AGC_OK) {
        error = sceAgcDriverSetupAsyncGraphics(1);
    }
    if (error != AGC_OK) {
        PS5AGC_SetError("OpenAGC initialization", error);
        goto fail;
    }
    if (PS5AGC_AppendMemoryRegion(&renderer_bytes, PS5AGC_SHADER_BYTES,
                                  PS5AGC_RENDER_ALIGNMENT,
                                  &shader_offset) < 0) {
        goto fail;
    }
    for (i = 0; i < PS5AGC_BUFFER_COUNT; ++i) {
        if (PS5AGC_AppendMemoryRegion(&renderer_bytes, frame_bytes,
                                      PS5AGC_RENDER_ALIGNMENT,
                                      &render_offset[i]) < 0 ||
            PS5AGC_AppendMemoryRegion(&renderer_bytes,
                                      PS5AGC_COMMAND_BYTES, 256u,
                                      &command_offset[i]) < 0 ||
            PS5AGC_AppendMemoryRegion(&renderer_bytes, sizeof(Uint32),
                                      256u, &fence_offset[i]) < 0) {
            goto fail;
        }
    }
    if (renderer_bytes > SIZE_MAX - (PS5AGC_RENDER_ALIGNMENT - 1u)) {
        SDL_OutOfMemory();
        goto fail;
    }
    renderer_bytes = PS5AGC_Align(renderer_bytes, PS5AGC_RENDER_ALIGNMENT);
    if (renderer_bytes < PS5AGC_MIN_RENDERER_POOL_BYTES) {
        renderer_bytes = PS5AGC_MIN_RENDERER_POOL_BYTES;
    }
    if (PS5AGC_InjectFailure("allocation") < 0) {
        goto fail;
    }
    error = agcGpuMemoryAllocateFlexible(&data->renderer_memory,
        renderer_bytes, 256u, "SDL ps5agc renderer pool");
    if (error != AGC_OK) {
        PS5AGC_SetError("allocating OpenAGC renderer memory", error);
        goto fail;
    }
    PS5AGC_InitMemorySlice(&data->shader_memory, &data->renderer_memory,
                           shader_offset, PS5AGC_SHADER_BYTES);
    for (i = 0; i < PS5AGC_BUFFER_COUNT; ++i) {
        PS5AGC_InitMemorySlice(&data->render_memory[i],
                               &data->renderer_memory, render_offset[i],
                               frame_bytes);
        PS5AGC_InitMemorySlice(&data->command_memory[i],
                               &data->renderer_memory, command_offset[i],
                               PS5AGC_COMMAND_BYTES);
        PS5AGC_InitMemorySlice(&data->fence_memory[i],
                               &data->renderer_memory, fence_offset[i],
                               sizeof(Uint32));
    }
    error = agcGpuMemoryAllocateDirectWriteCombined(&data->upload_memory,
        PS5AGC_UPLOAD_BYTES, PS5AGC_DIRECT_ALIGNMENT);
    if (error != AGC_OK) {
        PS5AGC_SetError("allocating OpenAGC vertex upload memory", error);
        goto fail;
    }
    if (PS5AGC_InitShaders(data) < 0) {
        goto fail;
    }
    for (i = 0; i < PS5AGC_BUFFER_COUNT; ++i) {
        data->render_usage[i] = AGC_GFX1013_RESOURCE_USAGE_UNDEFINED;
        data->display_usage[i] = AGC_GFX1013_RESOURCE_USAGE_UNDEFINED;
    }
    SDL_Log("ps5agc: native renderer ready at %ux%u",
            data->mode.width, data->mode.height);

    renderer->GetOutputSize = PS5AGC_GetOutputSize;
    renderer->SupportsBlendMode = PS5AGC_SupportsBlendMode;
    renderer->CreateTexture = PS5AGC_CreateTexture;
    renderer->UpdateTexture = PS5AGC_UpdateTexture;
#if SDL_HAVE_YUV
    renderer->UpdateTextureYUV = PS5AGC_UpdateTextureYUV;
    renderer->UpdateTextureNV = PS5AGC_UpdateTextureNV;
#endif
    renderer->LockTexture = PS5AGC_LockTexture;
    renderer->UnlockTexture = PS5AGC_UnlockTexture;
    renderer->SetTextureScaleMode = PS5AGC_SetTextureScaleMode;
    renderer->SetRenderTarget = PS5AGC_SetRenderTarget;
    renderer->QueueSetViewport = PS5AGC_QueueNoOp;
    renderer->QueueSetDrawColor = PS5AGC_QueueNoOp;
    renderer->QueueDrawPoints = PS5AGC_QueueDrawPoints;
    renderer->QueueGeometry = PS5AGC_QueueGeometry;
    renderer->RunCommandQueue = PS5AGC_RunCommandQueue;
    renderer->RenderReadPixels = PS5AGC_RenderReadPixels;
    renderer->RenderPresent = PS5AGC_Present;
    renderer->DestroyTexture = PS5AGC_DestroyTexture;
    renderer->DestroyRenderer = PS5AGC_DestroyRenderer;
    renderer->SetVSync = PS5AGC_SetVSync;
    renderer->info = PS5AGC_RenderDriver.info;
    renderer->driverdata = data;
    renderer->window = window;
    return renderer;

fail:
    PS5AGC_DestroyData(data);
    SDL_free(renderer);
    return NULL;
}

SDL_RenderDriver PS5AGC_RenderDriver = {
    PS5AGC_CreateRenderer,
    {
        "ps5agc",
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE |
            SDL_RENDERER_PRESENTVSYNC,
        5,
        { SDL_PIXELFORMAT_ABGR8888, SDL_PIXELFORMAT_IYUV,
          SDL_PIXELFORMAT_YV12, SDL_PIXELFORMAT_NV12,
          SDL_PIXELFORMAT_NV21 },
        16384,
        16384
    }
};

#endif /* SDL_VIDEO_RENDER_PS5AGC */
