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
#include "shaders/ps5agc_frag.h"

#define PS5AGC_BUFFER_COUNT 3u
#define PS5AGC_COMMAND_BYTES (256u * 1024u)
#define PS5AGC_UPLOAD_BYTES (4u * 1024u * 1024u)
#define PS5AGC_SHADER_BYTES (64u * 1024u)
#define PS5AGC_GPU_TIMEOUT_US 1000000u
#define PS5AGC_PRESENT_TIMEOUT_US 1000000u
#define PS5AGC_DIRECT_ALIGNMENT (2u * 1024u * 1024u)
#define PS5AGC_DRAW_MODIFIER 0x40000000u

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
    size_t pixel_bytes;
    int pitch;
    SDL_ScaleMode scale_mode;
    AgcGfx1013ResourceUsage usage;
} PS5AGC_TextureData;

typedef struct PS5AGC_RenderData
{
    SDL_VideoDevice *device;
    AgcVideoOut *video_out;
    AgcVideoOutMode mode;
    AgcGpuMemory command_memory;
    AgcGpuMemory upload_memory;
    AgcGpuMemory shader_memory;
    AgcGpuMemory fence_memory;
    AgcGpuMemory white_memory;
    AgcGpuMemory display_memory[PS5AGC_BUFFER_COUNT];
    void *display_buffers[PS5AGC_BUFFER_COUNT];
    AgcGfx1013CombinedImageSamplerDescriptor *white_descriptor;
    AgcShaderRecord front_record;
    AgcShaderRecord back_record;
    AgcShaderRecord pixel_record;
    AgcShaderRecord fused_record;
    AgcRegisterValue fused_registers[32];
    AgcGfx1013Wave32VsPsState shaders;
    AgcGfx1013ResourceUsage display_usage[PS5AGC_BUFFER_COUNT];
    Uint64 frame_id;
    Uint32 fence_value;
    SDL_Rect viewport;
    SDL_Rect cliprect;
    SDL_bool clip_enabled;
    SDL_bool screen_dirty;
} PS5AGC_RenderData;

static size_t PS5AGC_Align(size_t value, size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static int PS5AGC_SetError(const char *operation, int32_t error)
{
    return SDL_SetError("%s failed: %s (0x%08x)", operation,
                        agcErrorString(error), (unsigned int)error);
}

static int PS5AGC_Flush(const AgcGpuMemory *memory, size_t offset,
                        size_t size, const char *operation)
{
    int32_t error = agcGpuMemoryFlush(memory, offset, size);
    return error == AGC_OK ? 0 : PS5AGC_SetError(operation, error);
}

static int PS5AGC_SubmitAndWait(PS5AGC_RenderData *data, SceAgcCb *cb,
                                const char *operation)
{
    AgcCommandBufferSubmit submit;
    AgcGfx1013EopFenceState fence;
    volatile Uint32 *value = (volatile Uint32 *)data->fence_memory.cpu_address;
    int32_t error;

    ++data->fence_value;
    if (data->fence_value == 0u) {
        ++data->fence_value;
    }
    *value = 0u;
    error = agcGpuMemoryFlush(&data->fence_memory, 0, sizeof(*value));
    if (error != AGC_OK) {
        return PS5AGC_SetError("resetting an OpenAGC fence", error);
    }
    SDL_zero(fence);
    fence.address = data->fence_memory.gpu_address;
    fence.value = data->fence_value;
    error = agcGfx1013SignalEopFence(cb, &fence);
    if (error != AGC_OK) {
        return PS5AGC_SetError("recording an OpenAGC EOP fence", error);
    }
    error = agcGpuMemoryFlush(&data->command_memory, 0,
                              (size_t)agcCbUsedDwords(cb) * sizeof(Uint32));
    if (error != AGC_OK) {
        return PS5AGC_SetError("publishing an OpenAGC command buffer", error);
    }
    submit.command_address = data->command_memory.gpu_address;
    submit.dword_count = agcCbUsedDwords(cb);
    submit.reserved = 0;
    error = sceAgcDriverSubmitDcb(&submit);
    if (error != AGC_OK) {
        return PS5AGC_SetError(operation, error);
    }
    error = agcGpuMemoryWait32(&data->fence_memory, 0, data->fence_value,
                               PS5AGC_GPU_TIMEOUT_US);
    return error == AGC_OK ? 0 : PS5AGC_SetError("waiting for OpenAGC", error);
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

static int PS5AGC_InitDescriptor(PS5AGC_TextureData *texture,
                                 int width, int height)
{
    AgcGfx1013Image2DState image;
    AgcSamplerDescriptor sampler;
    int32_t error;

    SDL_zero(image);
    image.address = texture->memory.gpu_address;
    image.width = (Uint32)width;
    image.height = (Uint32)height;
    image.format = AGC_GFX1013_IMAGE_FORMAT_RGBA8_UNORM;
    image.image_type = AGC_GFX1013_IMAGE_TYPE_2D;
    image.dst_sel_x = 4u;
    image.dst_sel_y = 5u;
    image.dst_sel_z = 6u;
    image.dst_sel_w = 7u;
    agcSamplerDescriptorInit(&sampler);
    agcSamplerDescriptorSetClampMode(&sampler, kAgcClampClamp,
                                     kAgcClampClamp, kAgcClampClamp);
    agcSamplerDescriptorSetFilterMode(
        &sampler,
        texture->scale_mode == SDL_ScaleModeNearest ? kAgcFilterPoint : kAgcFilterBilinear,
        texture->scale_mode == SDL_ScaleModeNearest ? kAgcFilterPoint : kAgcFilterBilinear,
        kAgcMipFilterNone);
    error = agcGfx1013CombinedImageSamplerDescriptorEncode(
        texture->descriptor, &image, &sampler);
    if (error != AGC_OK) {
        return PS5AGC_SetError("encoding an OpenAGC texture descriptor", error);
    }
    return PS5AGC_Flush(&texture->memory, texture->descriptor_offset,
                        sizeof(*texture->descriptor), "publishing a texture descriptor");
}

static int PS5AGC_InitShaders(PS5AGC_RenderData *data)
{
    size_t front_offset = 0u;
    size_t back_offset;
    size_t pixel_offset;
    const AgcShaderRecord *file_record;
    size_t code_offset;
    size_t code_size;
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
        error = agcShaderRecordRelocateBinary(&data->pixel_record,
                                              ps5agc_frag_sb,
                                              ps5agc_frag_sb_len);
    }
    if (error != AGC_OK) {
        return PS5AGC_SetError("relocating checked-in PSBC shaders", error);
    }

#define PS5AGC_UPLOAD_SHADER(blob, record, offset)                                      \
    do {                                                                                 \
        file_record = (const AgcShaderRecord *)(blob);                                   \
        code_offset = (size_t)file_record->code;                                         \
        if (code_offset > sizeof(blob)) {                                                 \
            return SDL_SetError("checked-in PSBC shader has an invalid code offset");   \
        }                                                                                \
        code_size = sizeof(blob) - code_offset;                                          \
        SDL_memcpy((Uint8 *)data->shader_memory.cpu_address + (offset),                  \
                   (const Uint8 *)(blob) + code_offset, code_size);                       \
        (record).code = data->shader_memory.gpu_address + (offset);                       \
        (offset) = PS5AGC_Align((offset) + code_size, 256u);                              \
    } while (0)

    PS5AGC_UPLOAD_SHADER(ps5agc_ngg_front_sb, data->front_record, front_offset);
    back_offset = front_offset;
    PS5AGC_UPLOAD_SHADER(ps5agc_ngg_back_sb, data->back_record, back_offset);
    pixel_offset = back_offset;
    PS5AGC_UPLOAD_SHADER(ps5agc_frag_sb, data->pixel_record, pixel_offset);
#undef PS5AGC_UPLOAD_SHADER

    if (pixel_offset > data->shader_memory.size) {
        return SDL_SetError("checked-in PSBC shaders exceed the OpenAGC shader pool");
    }
    if (PS5AGC_Flush(&data->shader_memory, 0, pixel_offset,
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
    data->shaders.primitive.record = &data->fused_record;
    data->shaders.primitive.sh_registers = data->fused_registers;
    data->shaders.primitive.num_sh_registers = data->fused_record.num_sh_registers;
    data->shaders.primitive.cx_registers = (const AgcRegisterValue *)(uintptr_t)data->back_record.cx_registers;
    data->shaders.primitive.num_cx_registers = data->back_record.num_cx_registers;
    data->shaders.primitive.code_address = data->back_record.code;
    data->shaders.pixel.record = &data->pixel_record;
    data->shaders.pixel.sh_registers = (const AgcRegisterValue *)(uintptr_t)data->pixel_record.sh_registers;
    data->shaders.pixel.num_sh_registers = data->pixel_record.num_sh_registers;
    data->shaders.pixel.cx_registers = (const AgcRegisterValue *)(uintptr_t)data->pixel_record.cx_registers;
    data->shaders.pixel.num_cx_registers = data->pixel_record.num_cx_registers;
    data->shaders.pixel.code_address = data->pixel_record.code;
    data->shaders.primitive_back_code_address = data->back_record.code;
    data->shaders.primitive_type = 4u; /* triangle list */
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
    size_t pixels;
    size_t allocation;
    int32_t error;
    (void)renderer;

    if (texture->format != SDL_PIXELFORMAT_ABGR8888) {
        return SDL_SetError("ps5agc accepts ABGR8888 textures after SDL conversion");
    }
    if (texture->w <= 0 || texture->h <= 0 ||
        (size_t)texture->w > SIZE_MAX / 4u / (size_t)texture->h) {
        return SDL_SetError("invalid ps5agc texture dimensions");
    }
    data = (PS5AGC_TextureData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }
    data->pitch = texture->w * 4;
    pixels = (size_t)data->pitch * texture->h;
    data->pixel_bytes = pixels;
    data->descriptor_offset = PS5AGC_Align(pixels, 256u);
    allocation = data->descriptor_offset + sizeof(*data->descriptor);
    error = agcGpuMemoryAllocateFlexible(&data->memory, allocation, 256u,
                                         "SDL ps5agc texture");
    if (error != AGC_OK) {
        SDL_free(data);
        return PS5AGC_SetError("allocating an OpenAGC texture", error);
    }
    data->descriptor = (AgcGfx1013CombinedImageSamplerDescriptor *)
        ((Uint8 *)data->memory.cpu_address + data->descriptor_offset);
    data->scale_mode = texture->scaleMode;
    data->usage = AGC_GFX1013_RESOURCE_USAGE_UNDEFINED;
    texture->driverdata = data;
    if (PS5AGC_InitDescriptor(data, texture->w, texture->h) < 0) {
        agcGpuMemoryFreeFlexible(&data->memory);
        SDL_free(data);
        texture->driverdata = NULL;
        return -1;
    }
    return 0;
}

static int PS5AGC_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                                const SDL_Rect *rect, const void *pixels,
                                int pitch)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    Uint8 *dst = (Uint8 *)data->memory.cpu_address + rect->y * data->pitch + rect->x * 4;
    const Uint8 *src = (const Uint8 *)pixels;
    int row;
    (void)renderer;

    for (row = 0; row < rect->h; ++row) {
        SDL_memcpy(dst, src, (size_t)rect->w * 4u);
        dst += data->pitch;
        src += pitch;
    }
    data->usage = AGC_GFX1013_RESOURCE_USAGE_HOST_READ;
    return PS5AGC_Flush(&data->memory,
                        (size_t)rect->y * data->pitch,
                        (size_t)rect->h * data->pitch,
                        "publishing an OpenAGC texture update");
}

static int PS5AGC_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                              const SDL_Rect *rect, void **pixels, int *pitch)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    (void)renderer;
    *pixels = (Uint8 *)data->memory.cpu_address + rect->y * data->pitch + rect->x * 4;
    *pitch = data->pitch;
    return 0;
}

static void PS5AGC_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    (void)renderer;
    data->usage = AGC_GFX1013_RESOURCE_USAGE_HOST_READ;
    if (PS5AGC_Flush(&data->memory, 0, data->pixel_bytes,
                     "publishing a locked OpenAGC texture") < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "%s", SDL_GetError());
    }
}

static void PS5AGC_SetTextureScaleMode(SDL_Renderer *renderer,
                                       SDL_Texture *texture,
                                       SDL_ScaleMode scale_mode)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    (void)renderer;
    data->scale_mode = scale_mode;
    if (PS5AGC_InitDescriptor(data, texture->w, texture->h) < 0) {
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
    PS5AGC_Vertex *vertices;
    int i;
    (void)texture;

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
        vertices[i].u = texcoord ? texcoord[0] : 0.5f;
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
    size_t descriptor_offset;
    size_t vertex_offset;
    int32_t error;

    descriptor_offset = PS5AGC_Align(*upload_offset, 16u);
    vertex_offset = PS5AGC_Align(descriptor_offset + sizeof(*descriptor), 32u);
    if (count > UINT32_MAX || vertex_offset > data->upload_memory.size ||
        count > (data->upload_memory.size - vertex_offset) / sizeof(*vertices)) {
        return SDL_SetError("ps5agc per-submit vertex upload pool exhausted");
    }
    descriptor = (AgcGfx1013BufferDescriptor *)
        ((Uint8 *)data->upload_memory.cpu_address + descriptor_offset);
    vertices = (PS5AGC_Vertex *)
        ((Uint8 *)data->upload_memory.cpu_address + vertex_offset);
    SDL_memcpy(vertices, source, count * sizeof(*vertices));
    PS5AGC_TransformVertices(vertices, count, &data->viewport,
                            (int)frame->viewport.width, (int)frame->viewport.height);
    error = agcGfx1013BufferDescriptorEncode(
        descriptor, data->upload_memory.gpu_address + vertex_offset,
        sizeof(*vertices), (Uint32)count);
    if (error != AGC_OK) {
        return PS5AGC_SetError("encoding an OpenAGC vertex descriptor", error);
    }
    *upload_offset = vertex_offset + count * sizeof(*vertices);
    if (PS5AGC_SetBlend(cb, blend) < 0) {
        return -1;
    }
    primitive_table.placeholder = OPENAGC_VERTEX_BUFFER_TABLE_PLACEHOLDER;
    primitive_table.address = data->upload_memory.gpu_address + descriptor_offset;
    pixel_table.placeholder = OPENAGC_DESCRIPTOR_SET_PLACEHOLDER(0u);
    pixel_table.address = texture_data ?
        texture_data->memory.gpu_address + texture_data->descriptor_offset :
        data->white_memory.gpu_address + 256u;
    SDL_zero(draw);
    draw.shaders = data->shaders;
    draw.frame = frame;
    draw.primitive_resource_tables = &primitive_table;
    draw.num_primitive_resource_tables = 1u;
    draw.pixel_resource_tables = &pixel_table;
    draw.num_pixel_resource_tables = 1u;
    draw.index_type = kAgcIndexSize16;
    draw.instance_count = 1u;
    draw.vertex_count = (Uint32)count;
    draw.draw_modifier = PS5AGC_DRAW_MODIFIER;
    error = agcGfx1013DrawBaselineIndexAuto(cb, &draw);
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

static int PS5AGC_RunCommandQueue(SDL_Renderer *renderer,
                                  SDL_RenderCommand *cmd,
                                  void *queued_vertices, size_t vertsize)
{
    PS5AGC_RenderData *data = (PS5AGC_RenderData *)renderer->driverdata;
    PS5AGC_TextureData *target_texture = renderer->target ?
        (PS5AGC_TextureData *)renderer->target->driverdata : NULL;
    const Uint32 display_index = (Uint32)(data->frame_id % PS5AGC_BUFFER_COUNT);
    AgcGfx1013ResourceUsage *target_usage = target_texture ?
        &target_texture->usage : &data->display_usage[display_index];
    const Uint64 target_address = target_texture ?
        target_texture->memory.gpu_address : data->display_memory[display_index].gpu_address;
    const int width = target_texture ? renderer->target->w : (int)data->mode.width;
    const int height = target_texture ? renderer->target->h : (int)data->mode.height;
    const AgcGfx1013ColorTargetFormat format = target_texture ?
        AGC_GFX1013_RT_FORMAT_RGBA8_UNORM : AGC_GFX1013_RT_FORMAT_BGRA8_SRGB;
    AgcGfx1013FrameState frame;
    AgcGfx1013GraphicsDefaultStats stats;
    SceAgcCb cb;
    size_t upload_offset = 0u;
    int32_t error;
    (void)vertsize;

    if (!cmd) {
        return 0;
    }
    agcCbInit(&cb, data->command_memory.cpu_address, PS5AGC_COMMAND_BYTES);
    if (PS5AGC_Transition(&cb, *target_usage,
                          AGC_GFX1013_RESOURCE_USAGE_RENDER_TARGET) < 0) {
        return -1;
    }
    *target_usage = AGC_GFX1013_RESOURCE_USAGE_RENDER_TARGET;
    SDL_zero(frame);
    error = agcGfx1013InitColorTarget(&frame.color_target, target_address,
                                      (Uint32)width, (Uint32)height, format);
    if (error != AGC_OK) {
        return PS5AGC_SetError("initializing an OpenAGC color target", error);
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
        return PS5AGC_SetError("recording the OpenAGC frame prologue", error);
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
                return PS5AGC_SetError("recording an OpenAGC viewport scissor", error);
            }
            break;
        case SDL_RENDERCMD_SETCLIPRECT:
            data->clip_enabled = cmd->data.cliprect.enabled;
            data->cliprect = cmd->data.cliprect.rect;
            PS5AGC_UpdateScissor(data, &frame);
            error = agcGfx1013SetScissor(&cb, &frame.scissor);
            if (error != AGC_OK) {
                return PS5AGC_SetError("recording an OpenAGC clip scissor", error);
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
                return PS5AGC_SetError("recording an OpenAGC clear scissor", error);
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
                return -1;
            }
            data->viewport = saved_viewport;
            frame.scissor = saved_scissor;
            error = agcGfx1013SetScissor(&cb, &frame.scissor);
            if (error != AGC_OK) {
                return PS5AGC_SetError("restoring an OpenAGC scissor", error);
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
            if (sampled && PS5AGC_Transition(&cb, sampled->usage,
                                             AGC_GFX1013_RESOURCE_USAGE_SHADER_READ) < 0) {
                return -1;
            }
            if (sampled) {
                sampled->usage = AGC_GFX1013_RESOURCE_USAGE_SHADER_READ;
            }
            if (PS5AGC_RecordDraw(data, &cb, vertices, cmd->data.draw.count,
                                  cmd->data.draw.texture, cmd->data.draw.blend,
                                  &upload_offset, &frame) < 0) {
                return -1;
            }
            break;
        }
        default:
            break;
        }
        cmd = cmd->next;
    }
    if (PS5AGC_Flush(&data->upload_memory, 0, upload_offset,
                     "publishing OpenAGC draw vertices") < 0) {
        return -1;
    }
    if (PS5AGC_SubmitAndWait(data, &cb, "submitting OpenAGC draws") < 0) {
        return -1;
    }
    if (!target_texture) {
        data->screen_dirty = SDL_TRUE;
    }
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
    AgcGfx1013ResourceUsage *usage = texture ?
        &texture->usage : &data->display_usage[index];
    AgcGpuMemory *memory = texture ? &texture->memory : &data->display_memory[index];
    const int source_pitch = texture ? texture->pitch : (int)data->mode.width * 4;
    Uint8 *source = (Uint8 *)memory->cpu_address + rect->y * source_pitch + rect->x * 4;
    SceAgcCb cb;
    int32_t error;

    agcCbInit(&cb, data->command_memory.cpu_address, PS5AGC_COMMAND_BYTES);
    if (PS5AGC_Transition(&cb, *usage,
                          AGC_GFX1013_RESOURCE_USAGE_HOST_READ) < 0) {
        return -1;
    }
    *usage = AGC_GFX1013_RESOURCE_USAGE_HOST_READ;
    if (PS5AGC_SubmitAndWait(data, &cb,
                             "submitting an OpenAGC readback transition") < 0) {
        return -1;
    }
    error = agcGpuMemoryInvalidate(memory,
        (size_t)rect->y * source_pitch, (size_t)rect->h * source_pitch);
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

    if (!data->screen_dirty) {
        return 0;
    }
    agcCbInit(&cb, data->command_memory.cpu_address, PS5AGC_COMMAND_BYTES);
    if (PS5AGC_Transition(&cb, data->display_usage[index],
                          AGC_GFX1013_RESOURCE_USAGE_PRESENT) < 0) {
        return -1;
    }
    data->display_usage[index] = AGC_GFX1013_RESOURCE_USAGE_PRESENT;
    if (PS5AGC_SubmitAndWait(data, &cb, "submitting OpenAGC presentation transition") < 0) {
        return -1;
    }
    error = agcVideoOutPresent(data->video_out, index, data->frame_id,
                               PS5AGC_PRESENT_TIMEOUT_US);
    if (error != AGC_OK) {
        return PS5AGC_SetError("agcVideoOutPresent", error);
    }
    ++data->frame_id;
    data->screen_dirty = SDL_FALSE;
    return 0;
}

static void PS5AGC_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    PS5AGC_TextureData *data = (PS5AGC_TextureData *)texture->driverdata;
    (void)renderer;
    if (data) {
        agcGpuMemoryFreeFlexible(&data->memory);
        SDL_free(data);
        texture->driverdata = NULL;
    }
}

static void PS5AGC_DestroyData(PS5AGC_RenderData *data)
{
    Uint32 i;
    if (!data) {
        return;
    }
    if (data->video_out) {
        agcVideoOutClose(data->video_out);
    }
    for (i = 0; i < PS5AGC_BUFFER_COUNT; ++i) {
        agcGpuMemoryFreeDirect(&data->display_memory[i]);
    }
    agcGpuMemoryFreeFlexible(&data->white_memory);
    agcGpuMemoryFreeFlexible(&data->fence_memory);
    agcGpuMemoryFreeFlexible(&data->shader_memory);
    agcGpuMemoryFreeFlexible(&data->upload_memory);
    agcGpuMemoryFreeFlexible(&data->command_memory);
    PS5_ReleasePresentation(data->device, PS5_PRESENTATION_OPENAGC);
    SDL_free(data);
}

static void PS5AGC_DestroyRenderer(SDL_Renderer *renderer)
{
    PS5AGC_DestroyData((PS5AGC_RenderData *)renderer->driverdata);
    SDL_free(renderer);
}

static int PS5AGC_InitWhiteTexture(PS5AGC_RenderData *data)
{
    PS5AGC_TextureData white;
    Uint32 *pixel;
    const size_t descriptor_offset = 256u;
    int32_t error;
    SDL_zero(white);
    error = agcGpuMemoryAllocateFlexible(
        &data->white_memory,
        descriptor_offset + sizeof(AgcGfx1013CombinedImageSamplerDescriptor),
        256u, "SDL ps5agc white texture");
    if (error != AGC_OK) {
        return PS5AGC_SetError("allocating the OpenAGC white texture", error);
    }
    pixel = (Uint32 *)data->white_memory.cpu_address;
    *pixel = 0xffffffffu;
    data->white_descriptor = (AgcGfx1013CombinedImageSamplerDescriptor *)
        ((Uint8 *)data->white_memory.cpu_address + descriptor_offset);
    white.memory = data->white_memory;
    white.descriptor = data->white_descriptor;
    white.descriptor_offset = descriptor_offset;
    white.scale_mode = SDL_ScaleModeNearest;
    if (PS5AGC_InitDescriptor(&white, 1, 1) < 0) {
        return -1;
    }
    return PS5AGC_Flush(&data->white_memory, 0, data->white_memory.size,
                        "publishing the OpenAGC white texture");
}

static SDL_Renderer *PS5AGC_CreateRenderer(SDL_Window *window, Uint32 flags)
{
    SDL_VideoDevice *device = SDL_GetVideoDevice();
    PS5AGC_RenderData *data = NULL;
    SDL_Renderer *renderer = NULL;
    AgcVideoOutCreateInfo video_info;
    size_t frame_bytes;
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
    error = agcVideoOutGetDefaultMode(&data->mode);
    if (error != AGC_OK) {
        PS5AGC_SetError("agcVideoOutGetDefaultMode", error);
        goto fail;
    }
    if (!data->mode.width || !data->mode.height ||
        (size_t)data->mode.width > SIZE_MAX / 4u / data->mode.height) {
        SDL_SetError("OpenAGC returned an invalid default display mode");
        goto fail;
    }
    frame_bytes = (size_t)data->mode.width * data->mode.height * 4u;
    error = sce_agc_initialize();
    if (error == AGC_OK) error = sce_agc_initialize_internal_memory();
    if (error == AGC_OK) error = sceAgcDriverNotifyDefaultStates(0);
    if (error == AGC_OK) error = sceAgcDriverSetupAsyncGraphics(1);
    if (error != AGC_OK) {
        PS5AGC_SetError("OpenAGC initialization", error);
        goto fail;
    }
    error = agcGpuMemoryAllocateFlexible(&data->command_memory,
        PS5AGC_COMMAND_BYTES, 256u, "SDL ps5agc command buffer");
    if (error == AGC_OK) error = agcGpuMemoryAllocateFlexible(&data->upload_memory,
        PS5AGC_UPLOAD_BYTES, 256u, "SDL ps5agc vertex uploads");
    if (error == AGC_OK) error = agcGpuMemoryAllocateFlexible(&data->shader_memory,
        PS5AGC_SHADER_BYTES, 256u, "SDL ps5agc shaders");
    if (error == AGC_OK) error = agcGpuMemoryAllocateFlexible(&data->fence_memory,
        sizeof(Uint32), 4u, "SDL ps5agc fence");
    if (error != AGC_OK) {
        PS5AGC_SetError("allocating OpenAGC renderer memory", error);
        goto fail;
    }
    if (PS5AGC_InitShaders(data) < 0 || PS5AGC_InitWhiteTexture(data) < 0) {
        goto fail;
    }
    for (i = 0; i < PS5AGC_BUFFER_COUNT; ++i) {
        error = agcGpuMemoryAllocateDirectWriteCombined(
            &data->display_memory[i], frame_bytes, PS5AGC_DIRECT_ALIGNMENT);
        if (error != AGC_OK) {
            PS5AGC_SetError("allocating an OpenAGC VideoOut buffer", error);
            goto fail;
        }
        data->display_buffers[i] = data->display_memory[i].cpu_address;
        data->display_usage[i] = AGC_GFX1013_RESOURCE_USAGE_UNDEFINED;
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

    renderer->GetOutputSize = PS5AGC_GetOutputSize;
    renderer->SupportsBlendMode = PS5AGC_SupportsBlendMode;
    renderer->CreateTexture = PS5AGC_CreateTexture;
    renderer->UpdateTexture = PS5AGC_UpdateTexture;
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
        1,
        { SDL_PIXELFORMAT_ABGR8888 },
        16384,
        16384
    }
};

#endif /* SDL_VIDEO_RENDER_PS5AGC */
