/*
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "SDL.h"
#include "SDL_test_font.h"
#include "testyuv_cvt.h"

/* 422 (YUY2, etc) formats are the largest */
#define MAX_YUV_SURFACE_SIZE(W, H, P) (H * 4 * (W + P + 1) / 2)

/* Return true if the YUV format is packed pixels */
static SDL_bool is_packed_yuv_format(Uint32 format)
{
    return format == SDL_PIXELFORMAT_YUY2 || format == SDL_PIXELFORMAT_UYVY || format == SDL_PIXELFORMAT_YVYU;
}

/* Create a surface with a good pattern for verifying YUV conversion */
static SDL_Surface *generate_test_pattern(int pattern_size)
{
    SDL_Surface *pattern = SDL_CreateRGBSurfaceWithFormat(0, pattern_size, pattern_size, 0, SDL_PIXELFORMAT_RGB24);

    if (pattern) {
        int i, x, y;
        Uint8 *p, c;
        const int thickness = 2; /* Important so 2x2 blocks of color are the same, to avoid Cr/Cb interpolation over pixels */

        /* R, G, B in alternating horizontal bands */
        for (y = 0; y < pattern->h; y += thickness) {
            for (i = 0; i < thickness; ++i) {
                p = (Uint8 *)pattern->pixels + (y + i) * pattern->pitch + ((y / thickness) % 3);
                for (x = 0; x < pattern->w; ++x) {
                    *p = 0xFF;
                    p += 3;
                }
            }
        }

        /* Black and white in alternating vertical bands */
        c = 0xFF;
        for (x = 1 * thickness; x < pattern->w; x += 2 * thickness) {
            for (i = 0; i < thickness; ++i) {
                p = (Uint8 *)pattern->pixels + (x + i) * 3;
                for (y = 0; y < pattern->h; ++y) {
                    SDL_memset(p, c, 3);
                    p += pattern->pitch;
                }
            }
            if (c) {
                c = 0x00;
            } else {
                c = 0xFF;
            }
        }
    }
    return pattern;
}

static SDL_bool verify_yuv_data(Uint32 format, const Uint8 *yuv, int yuv_pitch, SDL_Surface *surface)
{
    const int tolerance = 20;
    const int size = (surface->h * surface->pitch);
    Uint8 *rgb;
    SDL_bool result = SDL_FALSE;

    rgb = (Uint8 *)SDL_malloc(size);
    if (!rgb) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Out of memory");
        return SDL_FALSE;
    }

    if (SDL_ConvertPixels(surface->w, surface->h, format, yuv, yuv_pitch, surface->format->format, rgb, surface->pitch) == 0) {
        int x, y;
        result = SDL_TRUE;
        for (y = 0; y < surface->h; ++y) {
            const Uint8 *actual = rgb + y * surface->pitch;
            const Uint8 *expected = (const Uint8 *)surface->pixels + y * surface->pitch;
            for (x = 0; x < surface->w; ++x) {
                int deltaR = (int)actual[0] - expected[0];
                int deltaG = (int)actual[1] - expected[1];
                int deltaB = (int)actual[2] - expected[2];
                int distance = (deltaR * deltaR + deltaG * deltaG + deltaB * deltaB);
                if (distance > tolerance) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Pixel at %d,%d was 0x%.2x,0x%.2x,0x%.2x, expected 0x%.2x,0x%.2x,0x%.2x, distance = %d\n", x, y, actual[0], actual[1], actual[2], expected[0], expected[1], expected[2], distance);
                    result = SDL_FALSE;
                }
                actual += 3;
                expected += 3;
            }
        }
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't convert %s to %s: %s\n", SDL_GetPixelFormatName(format), SDL_GetPixelFormatName(surface->format->format), SDL_GetError());
    }
    SDL_free(rgb);

    return result;
}

static int run_automated_tests(int pattern_size, int extra_pitch)
{
    const Uint32 formats[] = {
        SDL_PIXELFORMAT_YV12,
        SDL_PIXELFORMAT_IYUV,
        SDL_PIXELFORMAT_NV12,
        SDL_PIXELFORMAT_NV21,
        SDL_PIXELFORMAT_YUY2,
        SDL_PIXELFORMAT_UYVY,
        SDL_PIXELFORMAT_YVYU
    };
    int i, j;
    SDL_Surface *pattern = generate_test_pattern(pattern_size);
    const int yuv_len = MAX_YUV_SURFACE_SIZE(pattern->w, pattern->h, extra_pitch);
    Uint8 *yuv1 = (Uint8 *)SDL_malloc(yuv_len);
    Uint8 *yuv2 = (Uint8 *)SDL_malloc(yuv_len);
    int yuv1_pitch, yuv2_pitch;
    int result = -1;

    if (!pattern || !yuv1 || !yuv2) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't allocate test surfaces");
        goto done;
    }

    /* Verify conversion from YUV formats */
    for (i = 0; i < SDL_arraysize(formats); ++i) {
        if (!ConvertRGBtoYUV(formats[i], pattern->pixels, pattern->pitch, yuv1, pattern->w, pattern->h, SDL_GetYUVConversionModeForResolution(pattern->w, pattern->h), 0, 100)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ConvertRGBtoYUV() doesn't support converting to %s\n", SDL_GetPixelFormatName(formats[i]));
            goto done;
        }
        yuv1_pitch = CalculateYUVPitch(formats[i], pattern->w);
        if (!verify_yuv_data(formats[i], yuv1, yuv1_pitch, pattern)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed conversion from %s to RGB\n", SDL_GetPixelFormatName(formats[i]));
            goto done;
        }
    }

    /* Verify conversion to YUV formats */
    for (i = 0; i < SDL_arraysize(formats); ++i) {
        yuv1_pitch = CalculateYUVPitch(formats[i], pattern->w) + extra_pitch;
        if (SDL_ConvertPixels(pattern->w, pattern->h, pattern->format->format, pattern->pixels, pattern->pitch, formats[i], yuv1, yuv1_pitch) < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't convert %s to %s: %s\n", SDL_GetPixelFormatName(pattern->format->format), SDL_GetPixelFormatName(formats[i]), SDL_GetError());
            goto done;
        }
        if (!verify_yuv_data(formats[i], yuv1, yuv1_pitch, pattern)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed conversion from RGB to %s\n", SDL_GetPixelFormatName(formats[i]));
            goto done;
        }
    }

    /* Verify conversion between YUV formats */
    for (i = 0; i < SDL_arraysize(formats); ++i) {
        for (j = 0; j < SDL_arraysize(formats); ++j) {
            yuv1_pitch = CalculateYUVPitch(formats[i], pattern->w) + extra_pitch;
            yuv2_pitch = CalculateYUVPitch(formats[j], pattern->w) + extra_pitch;
            if (SDL_ConvertPixels(pattern->w, pattern->h, pattern->format->format, pattern->pixels, pattern->pitch, formats[i], yuv1, yuv1_pitch) < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't convert %s to %s: %s\n", SDL_GetPixelFormatName(pattern->format->format), SDL_GetPixelFormatName(formats[i]), SDL_GetError());
                goto done;
            }
            if (SDL_ConvertPixels(pattern->w, pattern->h, formats[i], yuv1, yuv1_pitch, formats[j], yuv2, yuv2_pitch) < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't convert %s to %s: %s\n", SDL_GetPixelFormatName(formats[i]), SDL_GetPixelFormatName(formats[j]), SDL_GetError());
                goto done;
            }
            if (!verify_yuv_data(formats[j], yuv2, yuv2_pitch, pattern)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed conversion from %s to %s\n", SDL_GetPixelFormatName(formats[i]), SDL_GetPixelFormatName(formats[j]));
                goto done;
            }
        }
    }

    /* Verify conversion between YUV formats in-place */
    for (i = 0; i < SDL_arraysize(formats); ++i) {
        for (j = 0; j < SDL_arraysize(formats); ++j) {
            if (is_packed_yuv_format(formats[i]) != is_packed_yuv_format(formats[j])) {
                /* Can't change plane vs packed pixel layout in-place */
                continue;
            }

            yuv1_pitch = CalculateYUVPitch(formats[i], pattern->w) + extra_pitch;
            yuv2_pitch = CalculateYUVPitch(formats[j], pattern->w) + extra_pitch;
            if (SDL_ConvertPixels(pattern->w, pattern->h, pattern->format->format, pattern->pixels, pattern->pitch, formats[i], yuv1, yuv1_pitch) < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't convert %s to %s: %s\n", SDL_GetPixelFormatName(pattern->format->format), SDL_GetPixelFormatName(formats[i]), SDL_GetError());
                goto done;
            }
            if (SDL_ConvertPixels(pattern->w, pattern->h, formats[i], yuv1, yuv1_pitch, formats[j], yuv1, yuv2_pitch) < 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't convert %s to %s: %s\n", SDL_GetPixelFormatName(formats[i]), SDL_GetPixelFormatName(formats[j]), SDL_GetError());
                goto done;
            }
            if (!verify_yuv_data(formats[j], yuv1, yuv2_pitch, pattern)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed conversion from %s to %s\n", SDL_GetPixelFormatName(formats[i]), SDL_GetPixelFormatName(formats[j]));
                goto done;
            }
        }
    }

    result = 0;

done:
    SDL_free(yuv1);
    SDL_free(yuv2);
    SDL_FreeSurface(pattern);
    return result;
}

static int UpdateTextureWithOddPitches(SDL_Texture *texture, Uint32 format,
                                       const Uint8 *raw_yuv, int width, int height)
{
    const SDL_Rect rect = { 1, 1, width - 2, height - 2 };
    const int full_chroma_width = (width + 1) / 2;
    const int full_chroma_height = (height + 1) / 2;
    const int chroma_width = (rect.w + 1) / 2;
    const int chroma_height = (rect.h + 1) / 2;
    const int y_pitch = rect.w + 7;
    const int chroma_pitch = chroma_width + 5;
    const int uv_pitch = chroma_width * 2 + 6;
    const Uint8 *first_chroma;
    Uint8 *y_plane = NULL;
    Uint8 *u_plane = NULL;
    Uint8 *v_plane = NULL;
    Uint8 *uv_plane = NULL;
    int result = -1;
    int row;

    if (width < 3 || height < 3) {
        return SDL_SetError("YUV update probe requires at least a 3x3 image");
    }
    first_chroma = raw_yuv + (size_t)width * height;
    y_plane = (Uint8 *)SDL_malloc((size_t)y_pitch * rect.h);
    if (!y_plane) {
        return SDL_OutOfMemory();
    }
    SDL_memset(y_plane, 0xa5, (size_t)y_pitch * rect.h);
    for (row = 0; row < rect.h; ++row) {
        SDL_memcpy(y_plane + (size_t)row * y_pitch,
                   raw_yuv + (size_t)(rect.y + row) * width + rect.x,
                   (size_t)rect.w);
    }

    if (format == SDL_PIXELFORMAT_NV12 || format == SDL_PIXELFORMAT_NV21) {
        const int full_uv_pitch = full_chroma_width * 2;
        uv_plane = (Uint8 *)SDL_malloc((size_t)uv_pitch * chroma_height);
        if (!uv_plane) {
            SDL_OutOfMemory();
            goto done;
        }
        SDL_memset(uv_plane, 0x5a, (size_t)uv_pitch * chroma_height);
        for (row = 0; row < chroma_height; ++row) {
            SDL_memcpy(uv_plane + (size_t)row * uv_pitch,
                       first_chroma +
                           (size_t)(rect.y / 2 + row) * full_uv_pitch +
                           (size_t)(rect.x / 2) * 2,
                       (size_t)chroma_width * 2);
        }
        result = SDL_UpdateNVTexture(texture, &rect, y_plane, y_pitch,
                                     uv_plane, uv_pitch);
    } else {
        const Uint8 *source_u;
        const Uint8 *source_v;
        const Uint8 *second_chroma = first_chroma +
            (size_t)full_chroma_width * full_chroma_height;
        if (format == SDL_PIXELFORMAT_YV12) {
            source_v = first_chroma;
            source_u = second_chroma;
        } else {
            source_u = first_chroma;
            source_v = second_chroma;
        }
        u_plane = (Uint8 *)SDL_malloc((size_t)chroma_pitch * chroma_height);
        v_plane = (Uint8 *)SDL_malloc((size_t)chroma_pitch * chroma_height);
        if (!u_plane || !v_plane) {
            SDL_OutOfMemory();
            goto done;
        }
        SDL_memset(u_plane, 0x5a, (size_t)chroma_pitch * chroma_height);
        SDL_memset(v_plane, 0xa5, (size_t)chroma_pitch * chroma_height);
        for (row = 0; row < chroma_height; ++row) {
            const size_t source_offset =
                (size_t)(rect.y / 2 + row) * full_chroma_width +
                (size_t)(rect.x / 2);
            SDL_memcpy(u_plane + (size_t)row * chroma_pitch,
                       source_u + source_offset, (size_t)chroma_width);
            SDL_memcpy(v_plane + (size_t)row * chroma_pitch,
                       source_v + source_offset, (size_t)chroma_width);
        }
        result = SDL_UpdateYUVTexture(texture, &rect, y_plane, y_pitch,
                                      u_plane, chroma_pitch,
                                      v_plane, chroma_pitch);
    }

    if (result == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "YUV odd update rect=%d,%d %dx%d pitches=%d/%d\n",
                    rect.x, rect.y, rect.w, rect.h, y_pitch,
                    (format == SDL_PIXELFORMAT_NV12 ||
                     format == SDL_PIXELFORMAT_NV21) ? uv_pitch : chroma_pitch);
    }

done:
    SDL_free(uv_plane);
    SDL_free(v_plane);
    SDL_free(u_plane);
    SDL_free(y_plane);
    return result;
}

static SDL_bool PixelsWithinTolerance(Uint32 actual, Uint32 expected, Uint8 tolerance)
{
    int shift;
    for (shift = 0; shift < 32; shift += 8) {
        const int a = (int)((actual >> shift) & 0xffu);
        const int e = (int)((expected >> shift) & 0xffu);
        if (SDL_abs(a - e) > tolerance) {
            return SDL_FALSE;
        }
    }
    return SDL_TRUE;
}

int main(int argc, char **argv)
{
    struct
    {
        SDL_bool enable_intrinsics;
        int pattern_size;
        int extra_pitch;
    } automated_test_params[] = {
        /* Test: even width and height */
        { SDL_FALSE, 2, 0 },
        { SDL_FALSE, 4, 0 },
        /* Test: odd width and height */
        { SDL_FALSE, 1, 0 },
        { SDL_FALSE, 3, 0 },
        /* Test: even width and height, extra pitch */
        { SDL_FALSE, 2, 3 },
        { SDL_FALSE, 4, 3 },
        /* Test: odd width and height, extra pitch */
        { SDL_FALSE, 1, 3 },
        { SDL_FALSE, 3, 3 },
        /* Test: even width and height with intrinsics */
        { SDL_TRUE, 32, 0 },
        /* Test: odd width and height with intrinsics */
        { SDL_TRUE, 33, 0 },
        { SDL_TRUE, 37, 0 },
        /* Test: even width and height with intrinsics, extra pitch */
        { SDL_TRUE, 32, 3 },
        /* Test: odd width and height with intrinsics, extra pitch */
        { SDL_TRUE, 33, 3 },
        { SDL_TRUE, 37, 3 },
    };
    int arg = 1;
    const char *filename;
    SDL_Surface *original;
    SDL_Surface *converted;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *output[3];
    const char *titles[3] = { "ORIGINAL", "SOFTWARE", "HARDWARE" };
    char title[128];
    const char *yuv_name;
    const char *yuv_mode;
    Uint32 rgb_format = SDL_PIXELFORMAT_RGBX8888;
    Uint32 yuv_format = SDL_PIXELFORMAT_YV12;
    int current = 0;
    int max_frames = 0;
    SDL_bool bare_frame = SDL_FALSE;
    SDL_bool clear_only = SDL_FALSE;
    SDL_bool display_probe = SDL_FALSE;
    SDL_bool target_probe = SDL_FALSE;
    SDL_bool yuv_update_probe = SDL_FALSE;
    SDL_bool probe_failed = SDL_FALSE;
    Uint32 renderer_flags = 0;
    const char *requested_renderer = NULL;
    int pitch;
    Uint8 *raw_yuv;
    Uint32 then, now, i, iterations = 100;
    SDL_bool should_run_automated_tests = SDL_FALSE;

    while (argv[arg] && *argv[arg] == '-') {
        if (SDL_strcmp(argv[arg], "--jpeg") == 0) {
            SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_JPEG);
        } else if (SDL_strcmp(argv[arg], "--bt601") == 0) {
            SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT601);
        } else if (SDL_strcmp(argv[arg], "--bt709") == 0) {
            SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
        } else if (SDL_strcmp(argv[arg], "--auto") == 0) {
            SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_AUTOMATIC);
        } else if (SDL_strcmp(argv[arg], "--yv12") == 0) {
            yuv_format = SDL_PIXELFORMAT_YV12;
        } else if (SDL_strcmp(argv[arg], "--iyuv") == 0) {
            yuv_format = SDL_PIXELFORMAT_IYUV;
        } else if (SDL_strcmp(argv[arg], "--yuy2") == 0) {
            yuv_format = SDL_PIXELFORMAT_YUY2;
        } else if (SDL_strcmp(argv[arg], "--uyvy") == 0) {
            yuv_format = SDL_PIXELFORMAT_UYVY;
        } else if (SDL_strcmp(argv[arg], "--yvyu") == 0) {
            yuv_format = SDL_PIXELFORMAT_YVYU;
        } else if (SDL_strcmp(argv[arg], "--nv12") == 0) {
            yuv_format = SDL_PIXELFORMAT_NV12;
        } else if (SDL_strcmp(argv[arg], "--nv21") == 0) {
            yuv_format = SDL_PIXELFORMAT_NV21;
        } else if (SDL_strcmp(argv[arg], "--rgb555") == 0) {
            rgb_format = SDL_PIXELFORMAT_RGB555;
        } else if (SDL_strcmp(argv[arg], "--rgb565") == 0) {
            rgb_format = SDL_PIXELFORMAT_RGB565;
        } else if (SDL_strcmp(argv[arg], "--rgb24") == 0) {
            rgb_format = SDL_PIXELFORMAT_RGB24;
        } else if (SDL_strcmp(argv[arg], "--argb") == 0) {
            rgb_format = SDL_PIXELFORMAT_ARGB8888;
        } else if (SDL_strcmp(argv[arg], "--abgr") == 0) {
            rgb_format = SDL_PIXELFORMAT_ABGR8888;
        } else if (SDL_strcmp(argv[arg], "--rgba") == 0) {
            rgb_format = SDL_PIXELFORMAT_RGBA8888;
        } else if (SDL_strcmp(argv[arg], "--bgra") == 0) {
            rgb_format = SDL_PIXELFORMAT_BGRA8888;
        } else if (SDL_strcmp(argv[arg], "--automated") == 0) {
            should_run_automated_tests = SDL_TRUE;
        } else if (SDL_strcmp(argv[arg], "--hardware") == 0) {
            current = 2;
        } else if (SDL_strcmp(argv[arg], "--bare") == 0) {
            bare_frame = SDL_TRUE;
        } else if (SDL_strcmp(argv[arg], "--clear-only") == 0) {
            clear_only = SDL_TRUE;
        } else if (SDL_strcmp(argv[arg], "--display-probe") == 0) {
            clear_only = SDL_TRUE;
            display_probe = SDL_TRUE;
        } else if (SDL_strcmp(argv[arg], "--target-probe") == 0) {
            clear_only = SDL_TRUE;
            target_probe = SDL_TRUE;
        } else if (SDL_strcmp(argv[arg], "--target-texture-probe") == 0) {
            bare_frame = SDL_TRUE;
            target_probe = SDL_TRUE;
        } else if (SDL_strcmp(argv[arg], "--yuv-update-probe") == 0) {
            bare_frame = SDL_TRUE;
            target_probe = SDL_TRUE;
            yuv_update_probe = SDL_TRUE;
            current = 2;
        } else if (SDL_strcmp(argv[arg], "--renderer") == 0) {
            if (!argv[arg + 1] || !*argv[arg + 1]) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "--renderer requires a driver name\n");
                return 1;
            }
            requested_renderer = argv[++arg];
        } else if (SDL_strcmp(argv[arg], "--accelerated") == 0) {
            renderer_flags |= SDL_RENDERER_ACCELERATED;
        } else if (SDL_strcmp(argv[arg], "--frames") == 0) {
            if (!argv[arg + 1] || SDL_atoi(argv[arg + 1]) <= 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "--frames requires a positive frame count\n");
                return 1;
            }
            max_frames = SDL_atoi(argv[++arg]);
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Usage: %s [--jpeg|--bt601|-bt709|--auto] [--yv12|--iyuv|--yuy2|--uyvy|--yvyu|--nv12|--nv21] [--rgb555|--rgb565|--rgb24|--argb|--abgr|--rgba|--bgra] [--hardware] [--bare|--clear-only|--display-probe|--target-probe|--target-texture-probe|--yuv-update-probe] [--renderer name] [--accelerated] [--frames count] [image_filename]\n", argv[0]);
            return 1;
        }
        ++arg;
    }
    if (display_probe && max_frames == 0) {
        max_frames = 1;
    }

    /* Run automated tests */
    if (should_run_automated_tests) {
        for (i = 0; i < SDL_arraysize(automated_test_params); ++i) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Running automated test, pattern size %d, extra pitch %d, intrinsics %s\n",
                        automated_test_params[i].pattern_size,
                        automated_test_params[i].extra_pitch,
                        automated_test_params[i].enable_intrinsics ? "enabled" : "disabled");
            if (run_automated_tests(automated_test_params[i].pattern_size, automated_test_params[i].extra_pitch) < 0) {
                return 2;
            }
        }
        return 0;
    }

    if (argv[arg]) {
        filename = argv[arg];
    } else {
        filename = "testyuv.bmp";
    }
    original = SDL_ConvertSurfaceFormat(SDL_LoadBMP(filename), SDL_PIXELFORMAT_RGB24, 0);
    if (!original) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't load %s: %s\n", filename, SDL_GetError());
        return 3;
    }

    raw_yuv = SDL_calloc(1, MAX_YUV_SURFACE_SIZE(original->w, original->h, 0));
    ConvertRGBtoYUV(yuv_format, original->pixels, original->pitch, raw_yuv, original->w, original->h,
                    SDL_GetYUVConversionModeForResolution(original->w, original->h),
                    0, 100);
    pitch = CalculateYUVPitch(yuv_format, original->w);

    converted = SDL_CreateRGBSurfaceWithFormat(0, original->w, original->h, 0, rgb_format);
    if (!converted) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create converted surface: %s\n", SDL_GetError());
        return 3;
    }

    then = SDL_GetTicks();
    for (i = 0; i < iterations; ++i) {
        SDL_ConvertPixels(original->w, original->h, yuv_format, raw_yuv, pitch, rgb_format, converted->pixels, converted->pitch);
    }
    now = SDL_GetTicks();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%" SDL_PRIu32 " iterations in %" SDL_PRIu32 " ms, %.2fms each\n", iterations, (now - then), (float)(now - then) / iterations);

    window = SDL_CreateWindow("YUV test",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              original->w, original->h,
                              0);
    if (!window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create window: %s\n", SDL_GetError());
        return 4;
    }

    if (requested_renderer &&
        !SDL_SetHint(SDL_HINT_RENDER_DRIVER, requested_renderer)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Couldn't request renderer %s\n", requested_renderer);
        return 4;
    }
    renderer = SDL_CreateRenderer(window, -1, renderer_flags);
    if (!renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't create renderer: %s\n", SDL_GetError());
        return 4;
    }
    {
        SDL_RendererInfo renderer_info;
        if (SDL_GetRendererInfo(renderer, &renderer_info) < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Couldn't query renderer: %s\n", SDL_GetError());
            return 4;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Renderer selected: %s flags=0x%08" SDL_PRIx32 "\n",
                    renderer_info.name, renderer_info.flags);
        if (requested_renderer &&
            SDL_strcmp(renderer_info.name, requested_renderer) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Renderer mismatch: requested %s, selected %s\n",
                         requested_renderer, renderer_info.name);
            return 4;
        }
    }
    if (target_probe) {
        SDL_Texture *target;
        Uint32 *seed;
        size_t seed_count;
        size_t seed_index;
        target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                   SDL_TEXTUREACCESS_TARGET, original->w, original->h);
        seed = (Uint32 *)SDL_malloc((size_t)original->w * original->h * sizeof(*seed));
        seed_count = (size_t)original->w * original->h;
        if (seed) {
            for (seed_index = 0; seed_index < seed_count; ++seed_index) {
                seed[seed_index] = 0xffff00ffu;
            }
        }
        if (!target || !seed ||
            SDL_UpdateTexture(target, NULL, seed, original->w * (int)sizeof(*seed)) < 0 ||
            SDL_SetRenderTarget(renderer, target) < 0) {
            SDL_free(seed);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Couldn't create target probe: %s\n", SDL_GetError());
            return 5;
        }
        SDL_free(seed);
    }

    output[0] = SDL_CreateTextureFromSurface(renderer, original);
    output[1] = SDL_CreateTextureFromSurface(renderer, converted);
    output[2] = SDL_CreateTexture(renderer, yuv_format, SDL_TEXTUREACCESS_STREAMING, original->w, original->h);
    if (!output[0] || !output[1] || !output[2]) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't set create texture: %s\n", SDL_GetError());
        return 5;
    }
    if (yuv_update_probe) {
        if (UpdateTextureWithOddPitches(output[2], yuv_format, raw_yuv,
                                        original->w, original->h) < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Couldn't perform odd pitched YUV update: %s\n",
                         SDL_GetError());
            return 5;
        }
    } else if (SDL_UpdateTexture(output[2], NULL, raw_yuv, pitch) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Couldn't update YUV texture: %s\n", SDL_GetError());
        return 5;
    }

    yuv_name = SDL_GetPixelFormatName(yuv_format);
    if (SDL_strncmp(yuv_name, "SDL_PIXELFORMAT_", 16) == 0) {
        yuv_name += 16;
    }

    switch (SDL_GetYUVConversionModeForResolution(original->w, original->h)) {
    case SDL_YUV_CONVERSION_JPEG:
        yuv_mode = "JPEG";
        break;
    case SDL_YUV_CONVERSION_BT601:
        yuv_mode = "BT.601";
        break;
    case SDL_YUV_CONVERSION_BT709:
        yuv_mode = "BT.709";
        break;
    default:
        yuv_mode = "UNKNOWN";
        break;
    }

    {
        int done = 0;
        int frames = 0;
        while (!done) {
            SDL_Event event;
            while (SDL_PollEvent(&event) > 0) {
                if (event.type == SDL_QUIT) {
                    done = 1;
                }
                if (event.type == SDL_KEYDOWN) {
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        done = 1;
                    } else if (event.key.keysym.sym == SDLK_LEFT) {
                        --current;
                    } else if (event.key.keysym.sym == SDLK_RIGHT) {
                        ++current;
                    }
                }
                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    if (event.button.x < (original->w / 2)) {
                        --current;
                    } else {
                        ++current;
                    }
                }
            }

            /* Handle wrapping */
            if (current < 0) {
                current += SDL_arraysize(output);
            }
            if (current >= SDL_arraysize(output)) {
                current -= SDL_arraysize(output);
            }

            if (clear_only) {
                SDL_SetRenderDrawColor(renderer, 0xFF, 0x00, 0x00, 0xFF);
                SDL_RenderClear(renderer);
            } else if (!bare_frame) {
                SDL_RenderClear(renderer);
            }
            if (!clear_only) {
                SDL_RenderCopy(renderer, output[current], NULL, NULL);
            }
            if (!bare_frame && !clear_only) {
                SDL_SetRenderDrawColor(renderer, 0xFF, 0xFF, 0xFF, 0xFF);
                if (current == 0) {
                    SDLTest_DrawString(renderer, 4, 4, titles[current]);
                } else {
                    (void)SDL_snprintf(title, sizeof(title), "%s %s %s", titles[current], yuv_name, yuv_mode);
                    SDLTest_DrawString(renderer, 4, 4, title);
                }
            }
            if (max_frames > 0 && frames == 0) {
                const SDL_Rect probe_rect = { original->w / 2, original->h / 2, 1, 1 };
                Uint32 probe = 0;
                Uint32 expected_probe = 0;
                if (SDL_RenderReadPixels(renderer, &probe_rect,
                                         SDL_PIXELFORMAT_ABGR8888,
                                         &probe, sizeof(probe)) == 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "GPU center pixel: 0x%08" SDL_PRIx32 "\n", probe);
                    if (display_probe && probe != 0xff0000ffu) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                     "VideoOut readback mismatch: expected 0xff0000ff\n");
                        probe_failed = SDL_TRUE;
                    }
                    if (yuv_update_probe) {
                        const Uint8 *expected_source =
                            (const Uint8 *)converted->pixels +
                            (size_t)(original->h / 2) * converted->pitch +
                            (size_t)(original->w / 2) * converted->format->BytesPerPixel;
                        if (SDL_ConvertPixels(1, 1,
                                             converted->format->format,
                                             expected_source, converted->pitch,
                                             SDL_PIXELFORMAT_ABGR8888,
                                             &expected_probe,
                                             (int)sizeof(expected_probe)) < 0) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                         "CPU center conversion failed: %s\n",
                                         SDL_GetError());
                            probe_failed = SDL_TRUE;
                        } else if (!PixelsWithinTolerance(probe, expected_probe, 3)) {
                            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                         "YUV update probe mismatch: expected 0x%08" SDL_PRIx32
                                         " actual 0x%08" SDL_PRIx32 " tolerance=3\n",
                                         expected_probe, probe);
                            probe_failed = SDL_TRUE;
                        } else {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "YUV update probe: PASS expected=0x%08" SDL_PRIx32
                                        " actual=0x%08" SDL_PRIx32 " tolerance=3\n",
                                        expected_probe, probe);
                        }
                    }
                } else {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "GPU center readback failed: %s\n", SDL_GetError());
                    probe_failed = display_probe || yuv_update_probe;
                }
            }
            SDL_RenderPresent(renderer);
            if (max_frames > 0 && ++frames >= max_frames) {
                done = 1;
            }
            SDL_Delay(10);
        }
    }
    SDL_Quit();
    return probe_failed ? 6 : 0;
}

/* vi: set ts=4 sw=4 expandtab: */
