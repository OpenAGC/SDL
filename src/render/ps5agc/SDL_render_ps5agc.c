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
#include "../software/SDL_render_sw_c.h"
#include "../../video/SDL_sysvideo.h"
#include "../../video/ps5/SDL_ps5video.h"

#include <agc_cb.h>
#include <agc_error.h>
#include <agc_graphics.h>
#include <agc_memory.h>
#include <agc_videoout.h>
#include <agcdriver.h>

#define PS5AGC_BUFFER_COUNT 3u
#define PS5AGC_COMMAND_BYTES (64u * 1024u)
#define PS5AGC_GPU_TIMEOUT_US 1000000u
#define PS5AGC_PRESENT_TIMEOUT_US 1000000u
#define PS5AGC_DIRECT_ALIGNMENT (2u * 1024u * 1024u)

typedef struct PS5AGC_RenderData
{
    SDL_VideoDevice *device;
    SDL_Surface *surface;
    AgcVideoOut *video_out;
    AgcVideoOutMode mode;
    AgcGpuMemory render_memory;
    AgcGpuMemory command_memory;
    AgcGpuMemory fence_memory;
    AgcGpuMemory display_memory[PS5AGC_BUFFER_COUNT];
    void *display_buffers[PS5AGC_BUFFER_COUNT];
    Uint64 frame_id;
    Uint32 submitted_fence[PS5AGC_BUFFER_COUNT];
    size_t frame_bytes;
} PS5AGC_RenderData;

static int PS5AGC_SetError(const char *operation, int32_t error)
{
    return SDL_SetError("%s failed: %s (0x%08x)", operation,
                        agcErrorString(error), (unsigned int)error);
}

static void PS5AGC_DestroyData(PS5AGC_RenderData *data)
{
    Uint32 i;

    if (!data) {
        return;
    }
    if (data->video_out) {
        agcVideoOutClose(data->video_out);
        data->video_out = NULL;
    }
    SDL_FreeSurface(data->surface);
    data->surface = NULL;
    for (i = 0; i < PS5AGC_BUFFER_COUNT; ++i) {
        agcGpuMemoryFreeDirect(&data->display_memory[i]);
    }
    agcGpuMemoryFreeFlexible(&data->fence_memory);
    agcGpuMemoryFreeFlexible(&data->command_memory);
    agcGpuMemoryFreeFlexible(&data->render_memory);
    PS5_ReleasePresentation(data->device, PS5_PRESENTATION_OPENAGC);
    SDL_free(data);
}

static void PS5AGC_DestroyRenderer(SDL_Renderer *renderer, void *userdata)
{
    (void)renderer;
    PS5AGC_DestroyData((PS5AGC_RenderData *)userdata);
}

static int PS5AGC_SetVSync(SDL_Renderer *renderer, int vsync, void *userdata)
{
    (void)renderer;
    (void)userdata;
    if (vsync == 1) {
        return 0;
    }
    return SDL_SetError("ps5agc supports FIFO/VSYNC presentation only");
}

static int PS5AGC_Present(SDL_Renderer *renderer, void *userdata)
{
    PS5AGC_RenderData *data = (PS5AGC_RenderData *)userdata;
    const Uint32 index = (Uint32)(data->frame_id % PS5AGC_BUFFER_COUNT);
    const Uint32 fence_value = (Uint32)(data->frame_id + 1u);
    const size_t fence_offset = (size_t)index * sizeof(Uint32);
    volatile Uint32 *fences =
        (volatile Uint32 *)data->fence_memory.cpu_address;
    AgcGfx1013ResourceTransition transition;
    AgcCommandBufferSubmit submit;
    SceAgcCb cb;
    int32_t error;

    (void)renderer;
    if (data->submitted_fence[index]) {
        error = agcGpuMemoryWait32(&data->fence_memory, fence_offset,
                                   data->submitted_fence[index],
                                   PS5AGC_GPU_TIMEOUT_US);
        if (error != AGC_OK) {
            return PS5AGC_SetError("waiting for an OpenAGC frame slot", error);
        }
    }
    error = agcGpuMemoryFlush(&data->render_memory, 0, data->frame_bytes);
    if (error != AGC_OK) {
        return PS5AGC_SetError("publishing the SDL render target", error);
    }
    fences[index] = 0;
    error = agcGpuMemoryFlush(&data->fence_memory, fence_offset,
                              sizeof(fences[index]));
    if (error != AGC_OK) {
        return PS5AGC_SetError("resetting the OpenAGC fence", error);
    }

    agcCbInit(&cb, data->command_memory.cpu_address, PS5AGC_COMMAND_BYTES);
    SDL_zero(transition);
    transition.before = AGC_GFX1013_RESOURCE_USAGE_HOST_READ;
    transition.after = AGC_GFX1013_RESOURCE_USAGE_COPY_SOURCE;
    error = agcGfx1013TransitionResource(&cb, &transition);
    if (error == AGC_OK) {
        transition.before = data->frame_id < PS5AGC_BUFFER_COUNT ?
                            AGC_GFX1013_RESOURCE_USAGE_UNDEFINED :
                            AGC_GFX1013_RESOURCE_USAGE_PRESENT;
        transition.after = AGC_GFX1013_RESOURCE_USAGE_COPY_DESTINATION;
        error = agcGfx1013TransitionResource(&cb, &transition);
    }
    if (error == AGC_OK) {
        error = agcGfx1013CopyBuffer(&cb, data->render_memory.gpu_address,
                                     data->display_memory[index].gpu_address,
                                     data->frame_bytes);
    }
    if (error == AGC_OK) {
        transition.before = AGC_GFX1013_RESOURCE_USAGE_COPY_DESTINATION;
        transition.after = AGC_GFX1013_RESOURCE_USAGE_PRESENT;
        transition.completion_address = data->fence_memory.gpu_address +
                                        fence_offset;
        transition.completion_value = fence_value;
        error = agcGfx1013TransitionResource(&cb, &transition);
    }
    if (error != AGC_OK) {
        return PS5AGC_SetError("recording the OpenAGC presentation copy", error);
    }
    error = agcGpuMemoryFlush(&data->command_memory, 0,
                              (size_t)agcCbUsedDwords(&cb) * sizeof(Uint32));
    if (error != AGC_OK) {
        return PS5AGC_SetError("publishing the OpenAGC command buffer", error);
    }

    submit.command_address = data->command_memory.gpu_address;
    submit.dword_count = agcCbUsedDwords(&cb);
    submit.reserved = 0;
    error = sceAgcDriverSubmitDcb(&submit);
    if (error != AGC_OK) {
        return PS5AGC_SetError("submitting the OpenAGC command buffer", error);
    }
    data->submitted_fence[index] = fence_value;
    error = agcGpuMemoryWait32(&data->fence_memory, fence_offset, fence_value,
                               PS5AGC_GPU_TIMEOUT_US);
    if (error != AGC_OK) {
        return PS5AGC_SetError("waiting for the OpenAGC presentation copy", error);
    }
    error = agcVideoOutPresent(data->video_out, index, data->frame_id,
                               PS5AGC_PRESENT_TIMEOUT_US);
    if (error != AGC_OK) {
        return PS5AGC_SetError("agcVideoOutPresent", error);
    }
    ++data->frame_id;
    return 0;
}

static SDL_Renderer *PS5AGC_CreateRenderer(SDL_Window *window, Uint32 flags)
{
    SDL_VideoDevice *device = SDL_GetVideoDevice();
    PS5AGC_RenderData *data = NULL;
    SDL_Renderer *renderer = NULL;
    AgcVideoOutCreateInfo video_info;
    Uint32 i;
    int32_t error;

    (void)window;
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
    data = (PS5AGC_RenderData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        SDL_OutOfMemory();
        PS5_ReleasePresentation(device, PS5_PRESENTATION_OPENAGC);
        return NULL;
    }
    data->device = device;

    error = agcVideoOutGetDefaultMode(&data->mode);
    if (error != AGC_OK) {
        PS5AGC_SetError("agcVideoOutGetDefaultMode", error);
        goto fail;
    }
    if (!data->mode.width || !data->mode.height) {
        SDL_SetError("OpenAGC returned an invalid default display mode");
        goto fail;
    }
    data->frame_bytes = (size_t)data->mode.width * data->mode.height * 4u;
    if (data->frame_bytes / 4u / data->mode.width != data->mode.height) {
        SDL_SetError("OpenAGC display mode is too large");
        goto fail;
    }

    error = sce_agc_initialize();
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
    error = agcGpuMemoryAllocateFlexible(&data->render_memory,
                                         data->frame_bytes, 256,
                                         "SDL ps5agc render target");
    if (error == AGC_OK) {
        error = agcGpuMemoryAllocateFlexible(&data->command_memory,
                                             PS5AGC_COMMAND_BYTES, 256,
                                             "SDL ps5agc command buffer");
    }
    if (error == AGC_OK) {
        error = agcGpuMemoryAllocateFlexible(&data->fence_memory,
                                             PS5AGC_BUFFER_COUNT * sizeof(Uint32),
                                             4,
                                             "SDL ps5agc fence");
    }
    if (error != AGC_OK) {
        PS5AGC_SetError("allocating OpenAGC flexible memory", error);
        goto fail;
    }
    for (i = 0; i < PS5AGC_BUFFER_COUNT; ++i) {
        error = agcGpuMemoryAllocateDirectWriteCombined(
            &data->display_memory[i], data->frame_bytes,
            PS5AGC_DIRECT_ALIGNMENT);
        if (error != AGC_OK) {
            PS5AGC_SetError("allocating an OpenAGC VideoOut buffer", error);
            goto fail;
        }
        data->display_buffers[i] = data->display_memory[i].cpu_address;
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

    data->surface = SDL_CreateRGBSurfaceWithFormatFrom(
        data->render_memory.cpu_address, data->mode.width, data->mode.height,
        32, (int)data->mode.width * 4, SDL_PIXELFORMAT_ABGR8888);
    if (!data->surface) {
        goto fail;
    }
    renderer = SW_CreateRendererForSurface(data->surface);
    if (!renderer) {
        goto fail;
    }
    SW_SetRendererCallbacks(renderer, PS5AGC_Present,
                            PS5AGC_DestroyRenderer, PS5AGC_SetVSync, data);
    renderer->WindowEvent = NULL;
    renderer->info = PS5AGC_RenderDriver.info;
    return renderer;

fail:
    PS5AGC_DestroyData(data);
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
        0,
        0
    }
};

#endif /* SDL_VIDEO_RENDER_PS5AGC */
