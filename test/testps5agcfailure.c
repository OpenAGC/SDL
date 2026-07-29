/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.
*/

#include "SDL.h"

#define PS5AGC_TEST_FAILURE_ENV "SDL_PS5AGC_TEST_FAILURE"

static int SetFailurePoint(const char *point)
{
    if (SDL_setenv(PS5AGC_TEST_FAILURE_ENV, point, 1) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "could not set ps5agc failure point: %s", SDL_GetError());
        return -1;
    }
    return 0;
}

static int VerifySoftwarePresentation(SDL_Window *window)
{
    SDL_Renderer *renderer;
    SDL_RendererInfo info;
    int result = -1;

    if (!SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software")) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "could not request the software renderer");
        return -1;
    }
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "software renderer could not acquire presentation: %s",
                     SDL_GetError());
        return -1;
    }
    if (SDL_GetRendererInfo(renderer, &info) < 0 ||
        SDL_strcmp(info.name, "software") != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "expected software renderer after ps5agc failure");
        goto done;
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    if (SDL_RenderClear(renderer) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "software clear failed: %s", SDL_GetError());
        goto done;
    }
    SDL_ClearError();
    SDL_RenderPresent(renderer);
    if (*SDL_GetError() != '\0') {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "software presentation failed after ps5agc cleanup: %s",
                     SDL_GetError());
        goto done;
    }
    result = 0;

done:
    SDL_DestroyRenderer(renderer);
    return result;
}

static int VerifyCreationFailure(SDL_Window *window, const char *point)
{
    SDL_Renderer *renderer;
    SDL_RendererInfo info;
    char expected[128];
    int result = -1;

    if (SetFailurePoint(point) < 0 ||
        !SDL_SetHint(SDL_HINT_RENDER_DRIVER, "ps5agc")) {
        return -1;
    }
    SDL_snprintf(expected, sizeof(expected),
                 "ps5agc test failure injected at %s", point);
    SDL_ClearError();
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "explicit ps5agc unexpectedly survived %s", point);
        SDL_DestroyRenderer(renderer);
        return -1;
    }
    if (SDL_strcmp(SDL_GetError(), expected) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "wrong explicit %s failure: %s", point, SDL_GetError());
        return -1;
    }

    SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
    SDL_ClearError();
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "accelerated request unexpectedly survived %s", point);
        SDL_DestroyRenderer(renderer);
        return -1;
    }
    if (SDL_strcmp(SDL_GetError(), expected) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "wrong accelerated %s failure: %s", point, SDL_GetError());
        return -1;
    }

    SDL_ClearError();
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "automatic fallback failed after %s: %s",
                     point, SDL_GetError());
        return -1;
    }
    if (SDL_GetRendererInfo(renderer, &info) < 0 ||
        SDL_strcmp(info.name, "software") != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "automatic %s failure did not select software", point);
        goto done;
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    if (SDL_RenderClear(renderer) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "fallback clear failed after %s: %s", point, SDL_GetError());
        goto done;
    }
    SDL_ClearError();
    SDL_RenderPresent(renderer);
    if (*SDL_GetError() != '\0') {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "fallback presentation failed after %s: %s",
                     point, SDL_GetError());
        goto done;
    }
    result = 0;

done:
    SDL_DestroyRenderer(renderer);
    return result;
}

static int VerifyRuntimeFailure(SDL_Window *window, const char *point)
{
    SDL_Renderer *renderer;
    SDL_RendererInfo info;
    char expected[128];
    int result = -1;

    if (SetFailurePoint("") < 0 ||
        !SDL_SetHint(SDL_HINT_RENDER_DRIVER, "ps5agc")) {
        return -1;
    }
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer || SDL_GetRendererInfo(renderer, &info) < 0 ||
        SDL_strcmp(info.name, "ps5agc") != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "could not create ps5agc for %s injection: %s",
                     point, SDL_GetError());
        if (renderer) {
            SDL_DestroyRenderer(renderer);
        }
        return -1;
    }
    if (SetFailurePoint(point) < 0) {
        goto done;
    }
    SDL_snprintf(expected, sizeof(expected),
                 "ps5agc test failure injected at %s", point);
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    if (SDL_RenderClear(renderer) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "could not queue %s probe: %s", point, SDL_GetError());
        goto done;
    }
    SDL_ClearError();
    if (SDL_strcmp(point, "submission") == 0) {
        if (SDL_RenderFlush(renderer) == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "submission failure unexpectedly succeeded");
            goto done;
        }
    } else {
        SDL_RenderPresent(renderer);
    }
    if (SDL_strcmp(SDL_GetError(), expected) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "wrong runtime %s failure: %s", point, SDL_GetError());
        goto done;
    }

    SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255);
    if (SDL_RenderClear(renderer) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "could not queue fail-stop check: %s", SDL_GetError());
        goto done;
    }
    SDL_ClearError();
    if (SDL_RenderFlush(renderer) == 0 ||
        SDL_strcmp(SDL_GetError(),
                   "ps5agc is unavailable after a submission failure") != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "runtime %s failure was not fail-stop: %s",
                     point, SDL_GetError());
        goto done;
    }
    result = 0;

done:
    SDL_DestroyRenderer(renderer);
    if (SetFailurePoint("") < 0) {
        return -1;
    }
    if (result == 0) {
        result = VerifySoftwarePresentation(window);
    }
    return result;
}

int main(int argc, char **argv)
{
    const char *point;
    SDL_Window *window = NULL;
    int result = 1;

    if (argc != 3 || SDL_strcmp(argv[1], "--failure") != 0) {
        SDL_Log("USAGE: %s --failure mode-query|initialization|allocation|submission|presentation", argv[0]);
        return 2;
    }
    point = argv[2];
    if (SDL_strcmp(point, "mode-query") != 0 &&
        SDL_strcmp(point, "initialization") != 0 &&
        SDL_strcmp(point, "allocation") != 0 &&
        SDL_strcmp(point, "submission") != 0 &&
        SDL_strcmp(point, "presentation") != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "unsupported ps5agc failure point: %s", point);
        return 2;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_Init failed: %s", SDL_GetError());
        goto done;
    }
    window = SDL_CreateWindow("ps5agc failure injection",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, 64, 64, 0);
    if (!window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_CreateWindow failed: %s", SDL_GetError());
        goto done;
    }

    if (SDL_strcmp(point, "submission") == 0 ||
        SDL_strcmp(point, "presentation") == 0) {
        if (VerifyRuntimeFailure(window, point) < 0) {
            goto done;
        }
    } else if (VerifyCreationFailure(window, point) < 0) {
        goto done;
    }

    SDL_Log("ps5agc failure injection: PASS point=%s", point);
    result = 0;

done:
    SetFailurePoint("");
    SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    return result;
}

/* vi: set ts=4 sw=4 expandtab: */
