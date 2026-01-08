/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "SDL_ps5osmesa.h"

#ifdef SDL_VIDEO_OPENGL_OSMESA

#include <SDL2/SDL_opengl.h>
#include <dlfcn.h>

#ifndef OSMESA_Y_UP
#define OSMESA_Y_UP 0x11
#endif

#define DEFAULT_OSMESA "libOSMesa.so.8"

struct SDL_GLDriverData
{
    void* (*OSMesaGetProcAddress)(const char*);
    void* (*OSMesaCreateContext)(GLenum, void*);
    void (*OSMesaPixelStore)(GLint, GLint);
    void (*OSMesaPostprocess)(void*, const char*, unsigned);
    void (*OSMesaDestroyContext)(void*);
    GLboolean (*OSMesaMakeCurrent)(void* , void*, GLenum, GLsizei, GLsizei);
    GLboolean (*OSMesaGetColorBuffer)(void*, GLint*, GLint*, GLint*, void**);
    void (*OSMesaFlush)(void);
};

static int OSMesa_LoadLibrary(_THIS, const char *path)
{
    void* handle;

    if (_this->gl_data) {
        return SDL_SetError("OSMesa library already loaded");
    }

    path = DEFAULT_OSMESA;
    handle = _this->gl_config.dll_handle = dlopen(path, RTLD_LAZY);
    if (!handle) {
      return SDL_SetError("%s: %s", path, dlerror());
    }

    SDL_strlcpy(_this->gl_config.driver_path, path,
                SDL_arraysize(_this->gl_config.driver_path));

    _this->gl_data = SDL_calloc(1, sizeof(struct SDL_GLDriverData));
    if (!_this->gl_data) {
        return SDL_OutOfMemory();
    }

    _this->gl_data->OSMesaGetProcAddress = dlsym(handle, "OSMesaGetProcAddress");
    if (!_this->gl_data->OSMesaGetProcAddress) {
        return SDL_SetError("%s", dlerror());
    }

    _this->gl_data->OSMesaCreateContext = dlsym(handle, "OSMesaCreateContext");
    if (!_this->gl_data->OSMesaCreateContext) {
        return SDL_SetError("%s", dlerror());
    }

    _this->gl_data->OSMesaDestroyContext = dlsym(handle, "OSMesaDestroyContext");
    if (!_this->gl_data->OSMesaDestroyContext) {
        return SDL_SetError("%s", dlerror());
    }

    _this->gl_data->OSMesaGetColorBuffer = dlsym(handle, "OSMesaGetColorBuffer");
    if (!_this->gl_data->OSMesaGetColorBuffer) {
        return SDL_SetError("%s", dlerror());
    }

    _this->gl_data->OSMesaMakeCurrent = dlsym(handle, "OSMesaMakeCurrent");
    if (!_this->gl_data->OSMesaMakeCurrent) {
        return SDL_SetError("%s", dlerror());
    }

    _this->gl_data->OSMesaPixelStore = dlsym(handle, "OSMesaPixelStore");
    if (!_this->gl_data->OSMesaPixelStore) {
        return SDL_SetError("%s", dlerror());
    }

    _this->gl_data->OSMesaFlush = _this->gl_data->OSMesaGetProcAddress("glFlush");
    if (!_this->gl_data->OSMesaFlush) {
        return SDL_SetError("%s", dlerror());
    }

    return 0;
}

static void OSMesa_UnloadLibrary(_THIS)
{
    if (_this->gl_config.dll_handle) {
        dlclose(_this->gl_config.dll_handle);
	_this->gl_config.dll_handle = NULL;
    }

    if (_this->gl_data) {
      SDL_free(_this->gl_data);
      _this->gl_data = NULL;
    }
}

static void* OSMesa_GetProcAddress(_THIS, const char *proc)
{
    if (_this->gl_data) {
        return _this->gl_data->OSMesaGetProcAddress(proc);
    }
    return NULL;
}

static int OSMesa_MakeCurrent(_THIS, SDL_Window * window, SDL_GLContext context)
{
    SDL_Surface* surface = SDL_GetWindowSurface(window);

    if (!_this->gl_data) {
        return SDL_SetError("OSMesa is not loaded");
    }
    if (!surface) {
        return SDL_SetError("Couldn't find surface for window");
    }

    if (!_this->gl_data->OSMesaMakeCurrent(context, surface->pixels, GL_UNSIGNED_BYTE,
					   surface->w, surface->h)) {
        return SDL_SetError("Failed to make context current");
    }

    _this->gl_data->OSMesaPixelStore(OSMESA_Y_UP, 0);

    return 0;
}

static void OSMesa_DeleteContext(_THIS, SDL_GLContext ctx)
{
    if (_this->gl_data && ctx) {
        _this->gl_data->OSMesaDestroyContext(ctx);
    }
}

static SDL_GLContext OSMesa_CreateContext(_THIS, SDL_Window * window)
{
    void* ctx_share;
    void* ctx;

    if (!_this->gl_data) {
        SDL_SetError("OSMesa is not loaded");
	return NULL;
    }

    if (_this->gl_config.share_with_current_context) {
        ctx_share = SDL_GL_GetCurrentContext();
    } else {
        ctx_share = NULL;
    }

    ctx = _this->gl_data->OSMesaCreateContext(GL_RGBA, ctx_share);
    if (!ctx) {
        SDL_SetError("Failed to create context");
        return NULL;
    }

    if (OSMesa_MakeCurrent(_this, window, ctx) < 0) {
        OSMesa_DeleteContext(_this, ctx);
        return NULL;
    }

    return (SDL_GLContext)ctx;
}

static int OSMesa_SetSwapInterval(_THIS, int interval)
{
    return 0;
}

static int OSMesa_GetSwapInterval(_THIS)
{
    return 0;
}

static int OSMesa_SwapWindow(_THIS, SDL_Window *window)
{
    SDL_Surface *surface;

    if (!_this->gl_data) {
        return SDL_SetError("OSMesa is not loaded");
    }

    _this->gl_data->OSMesaFlush();

    surface = SDL_GetWindowSurface(window);
    if (!surface) {
        return SDL_SetError("Failed to get SDL window surface");
    }

    if (SDL_UpdateWindowSurface(window) != 0) {
        return SDL_SetError("Failed to update window surface");
    }

    return 0;
}


int PS5_OSMesa_InitDevice(SDL_VideoDevice* device)
{
    device->GL_LoadLibrary = OSMesa_LoadLibrary;
    device->GL_GetProcAddress = OSMesa_GetProcAddress;
    device->GL_UnloadLibrary = OSMesa_UnloadLibrary;
    device->GL_CreateContext = OSMesa_CreateContext;
    device->GL_MakeCurrent = OSMesa_MakeCurrent;
    device->GL_SetSwapInterval = OSMesa_SetSwapInterval;
    device->GL_GetSwapInterval = OSMesa_GetSwapInterval;
    device->GL_SwapWindow = OSMesa_SwapWindow;
    device->GL_DeleteContext = OSMesa_DeleteContext;

    return 0;
}

#endif
