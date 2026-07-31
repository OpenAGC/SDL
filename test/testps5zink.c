/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.
*/
#include <SDL.h>
#include <SDL_opengl.h>

typedef const GLubyte *(APIENTRY *PS5_PFNGLGETSTRINGPROC)(GLenum name);
typedef void (APIENTRY *PS5_PFNGLVIEWPORTPROC)(GLint x, GLint y,
                                               GLsizei width, GLsizei height);
typedef void (APIENTRY *PS5_PFNGLCLEARCOLORPROC)(GLfloat red, GLfloat green,
                                                 GLfloat blue, GLfloat alpha);
typedef void (APIENTRY *PS5_PFNGLCLEARPROC)(GLbitfield mask);
typedef void (APIENTRY *PS5_PFNGLFINISHPROC)(void);
typedef void (APIENTRY *PS5_PFNGLREADPIXELSPROC)(GLint x, GLint y,
                                                 GLsizei width, GLsizei height,
                                                 GLenum format, GLenum type,
                                                 void *pixels);
typedef GLenum (APIENTRY *PS5_PFNGLGETERRORPROC)(void);

static int channel_matches(Uint8 actual, Uint8 expected)
{
    return actual >= expected - 1 && actual <= expected + 1;
}

int main(int argc, char **argv)
{
    SDL_Window *window = NULL;
    SDL_GLContext context = NULL;
    const GLubyte *renderer;
    PS5_PFNGLGETSTRINGPROC p_glGetString;
    PS5_PFNGLVIEWPORTPROC p_glViewport;
    PS5_PFNGLCLEARCOLORPROC p_glClearColor;
    PS5_PFNGLCLEARPROC p_glClear;
    PS5_PFNGLFINISHPROC p_glFinish;
    PS5_PFNGLREADPIXELSPROC p_glReadPixels;
    PS5_PFNGLGETERRORPROC p_glGetError;
    Uint8 pixel[4] = { 0, 0, 0, 0 };
    int result = 1;

    if (argc == 2 && SDL_strncmp(argv[1], "--egl=", 6) == 0 && argv[1][6]) {
        if (SDL_setenv("SDL_VIDEO_EGL_DRIVER", argv[1] + 6, 1) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ps5-zink: could not set EGL path");
            goto done;
        }
    } else if (argc != 1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "usage: testps5zink [--egl=/absolute/libEGL.so.1]");
        goto done;
    }
    if (SDL_SetHint(SDL_HINT_PS5_OPENGL_DRIVER, "zink") != SDL_TRUE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ps5-zink: could not select zink");
        goto done;
    }
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ps5-zink: SDL_Init failed: %s", SDL_GetError());
        goto done;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    window = SDL_CreateWindow("PS5 Zink qualification", 0, 0, 64, 64,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ps5-zink: window failed: %s", SDL_GetError());
        goto done;
    }
    context = SDL_GL_CreateContext(window);
    if (!context) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ps5-zink: context failed: %s", SDL_GetError());
        goto done;
    }
    p_glGetString = (PS5_PFNGLGETSTRINGPROC)SDL_GL_GetProcAddress("glGetString");
    p_glViewport = (PS5_PFNGLVIEWPORTPROC)SDL_GL_GetProcAddress("glViewport");
    p_glClearColor = (PS5_PFNGLCLEARCOLORPROC)SDL_GL_GetProcAddress("glClearColor");
    p_glClear = (PS5_PFNGLCLEARPROC)SDL_GL_GetProcAddress("glClear");
    p_glFinish = (PS5_PFNGLFINISHPROC)SDL_GL_GetProcAddress("glFinish");
    p_glReadPixels = (PS5_PFNGLREADPIXELSPROC)SDL_GL_GetProcAddress("glReadPixels");
    p_glGetError = (PS5_PFNGLGETERRORPROC)SDL_GL_GetProcAddress("glGetError");
    if (!p_glGetString || !p_glViewport || !p_glClearColor || !p_glClear ||
        !p_glFinish || !p_glReadPixels || !p_glGetError) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ps5-zink: required GL entry point is missing");
        goto done;
    }
    renderer = p_glGetString(GL_RENDERER);
    if (!renderer || !SDL_strstr((const char *)renderer, "zink")) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ps5-zink: unexpected renderer: %s",
                     renderer ? (const char *)renderer : "(null)");
        goto done;
    }

    p_glViewport(0, 0, 64, 64);
    p_glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glFinish();
    p_glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    if (p_glGetError() != GL_NO_ERROR ||
        !channel_matches(pixel[0], 64) ||
        !channel_matches(pixel[1], 128) ||
        !channel_matches(pixel[2], 191) || pixel[3] != 255) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ps5-zink: readback mismatch rgba=%u,%u,%u,%u",
                     pixel[0], pixel[1], pixel[2], pixel[3]);
        goto done;
    }
    SDL_ClearError();
    SDL_GL_SwapWindow(window);
    if (*SDL_GetError() != '\0') {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ps5-zink: pbuffer swap failed: %s", SDL_GetError());
        goto done;
    }

    SDL_Log("ps5-zink: PASS renderer=%s rgba=%u,%u,%u,%u",
            (const char *)renderer, pixel[0], pixel[1], pixel[2], pixel[3]);
    result = 0;

done:
    if (context) {
        SDL_GL_DeleteContext(context);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    return result;
}
