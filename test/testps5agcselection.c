/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

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

#include "SDL.h"

int main(int argc, char **argv)
{
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_RendererInfo info;
    int result = 1;

    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_Init failed: %s", SDL_GetError());
        goto done;
    }
    window = SDL_CreateWindow("ps5agc selection test",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 64, 64, 0);
    if (!window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_CreateWindow failed: %s", SDL_GetError());
        goto done;
    }

    if (!SDL_SetHint(SDL_HINT_RENDER_DRIVER, "ps5agc")) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL could not set the ps5agc renderer hint");
        goto done;
    }
    SDL_ClearError();
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (renderer) {
        if (SDL_GetRendererInfo(renderer, &info) == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "explicit ps5agc request selected %s", info.name);
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "explicit ps5agc request unexpectedly succeeded");
        }
        goto done;
    }
    if (SDL_strcmp(SDL_GetError(), "ps5agc renderer is not available") != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "explicit ps5agc request returned the wrong error: %s",
                     SDL_GetError());
        goto done;
    }

    SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "software fallback could not acquire the window after the explicit failure: %s",
                     SDL_GetError());
        goto done;
    }
    if (SDL_GetRendererInfo(renderer, &info) < 0 ||
        SDL_strcmp(info.name, "software") != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "dummy-video fallback did not select software: %s",
                     SDL_GetError());
        goto done;
    }

    SDL_Log("ps5agc unavailable-selection contract: PASS");
    result = 0;

done:
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    return result;
}

/* vi: set ts=4 sw=4 expandtab: */
