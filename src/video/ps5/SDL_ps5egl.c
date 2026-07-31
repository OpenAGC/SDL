/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.
*/
#include "../../SDL_internal.h"

#if defined(SDL_VIDEO_DRIVER_PS5) && defined(SDL_PS5_ZINK)

#include "../SDL_egl_c.h"
#include "SDL_ps5egl.h"
#include "SDL_ps5video.h"

static int PS5_EGL_LoadLibrary(_THIS, const char *path)
{
    return SDL_EGL_LoadLibrary(_this, path, EGL_DEFAULT_DISPLAY,
                               EGL_PLATFORM_SURFACELESS_MESA);
}

static SDL_GLContext PS5_EGL_CreateContext(_THIS, SDL_Window *window)
{
    PS5_WindowData *window_data = (PS5_WindowData *)window->driverdata;
    return SDL_EGL_CreateContext(_this, window_data->egl_surface);
}

static int PS5_EGL_MakeCurrent(_THIS, SDL_Window *window,
                               SDL_GLContext context)
{
    EGLSurface surface = EGL_NO_SURFACE;
    if (window) {
        PS5_WindowData *window_data = (PS5_WindowData *)window->driverdata;
        surface = window_data->egl_surface;
    }
    return SDL_EGL_MakeCurrent(_this, surface, context);
}

static int PS5_EGL_SwapWindow(_THIS, SDL_Window *window)
{
    PS5_WindowData *window_data = (PS5_WindowData *)window->driverdata;
    return SDL_EGL_SwapBuffers(_this, window_data->egl_surface);
}

int PS5_EGL_InitDevice(SDL_VideoDevice *device)
{
    device->GL_LoadLibrary = PS5_EGL_LoadLibrary;
    device->GL_GetProcAddress = SDL_EGL_GetProcAddress;
    device->GL_UnloadLibrary = SDL_EGL_UnloadLibrary;
    device->GL_CreateContext = PS5_EGL_CreateContext;
    device->GL_MakeCurrent = PS5_EGL_MakeCurrent;
    device->GL_SetSwapInterval = SDL_EGL_SetSwapInterval;
    device->GL_GetSwapInterval = SDL_EGL_GetSwapInterval;
    device->GL_SwapWindow = PS5_EGL_SwapWindow;
    device->GL_DeleteContext = SDL_EGL_DeleteContext;
    return 0;
}

#endif /* SDL_VIDEO_DRIVER_PS5 && SDL_PS5_ZINK */
