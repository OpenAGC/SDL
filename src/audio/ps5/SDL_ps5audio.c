/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2015 Sam Lantinga <slouken@libsdl.org>

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
#include "../../SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_PS5

#include "SDL_audio.h"
#include "SDL_ps5audio.h"


inline static Uint16 PS5AUDIO_SampleSize(Uint16 size)
{
    if (size >= 2048) return 2048;
    if (size >= 1792) return 1792;
    if (size >= 1536) return 1536;
    if (size >= 1280) return 1280;
    if (size >= 1024) return 1024;
    if (size >= 768) return 768;
    if (size >= 512) return 512;
    return 256;
}

static int PS5AUDIO_OpenDevice(_THIS, const char *devname)
{
    SDL_AudioFormat test_format;
    Uint8 fmt;

    this->hidden = (struct SDL_PrivateAudioData *) SDL_calloc(1, sizeof(*this->hidden));
    if (this->hidden == NULL) {
        return SDL_OutOfMemory();
    }

    for (test_format = SDL_FirstAudioFormat(this->spec.format); test_format; test_format = SDL_NextAudioFormat()) {
        if (test_format == AUDIO_S16LSB) {
            fmt = (this->spec.channels == 1) ? PROSPERO_AUDIO_OUT_PARAM_FORMAT_S16_MONO
                                             : PROSPERO_AUDIO_OUT_PARAM_FORMAT_S16_STEREO;
        } else if (test_format == AUDIO_F32LSB) {
            fmt = (this->spec.channels == 1) ? PROSPERO_AUDIO_OUT_PARAM_FORMAT_FLOAT_MONO
                                             : PROSPERO_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO;
        } else {
            continue;
        }
        break;
    }

    if (!test_format) {
        return SDL_SetError("%s: Unsupported audio format", "ps5");
    }

    this->spec.format = test_format;
    this->spec.samples = PS5AUDIO_SampleSize(this->spec.samples);
    this->spec.freq = 48000;
    this->spec.channels = this->spec.channels == 1 ? 1 : 2;

    /* Update the fragment size as size in bytes. */
    SDL_CalculateAudioSpec(&this->spec);

    this->hidden->handle = sceAudioOutOpen(PROSPERO_USER_SERVICE_USER_ID_SYSTEM,
					   PROSPERO_AUDIO_OUT_PORT_TYPE_MAIN,
					   0, this->spec.samples, 48000, fmt);
    if (this->hidden->handle < 1) {
        return SDL_SetError("sceAudioOutOpen: %s", strerror(this->hidden->handle));
    }

    this->hidden->mixlen = this->spec.size;
    this->hidden->mixbuf = (Uint8 *)SDL_calloc(1, this->hidden->mixlen);

    return 0;
}

static void PS5AUDIO_PlayDevice(_THIS)
{
    sceAudioOutOutput(this->hidden->handle, this->hidden->mixbuf);
}

static void PS5AUDIO_WaitDevice(_THIS)
{
  // NOP, sceAudioOutOutput() is blocking
}

static Uint8 *PS5AUDIO_GetDeviceBuf(_THIS)
{
    return this->hidden->mixbuf;
}

static void PS5AUDIO_CloseDevice(_THIS)
{
    int res;
    if (this->hidden->handle > 0) {
        res = sceAudioOutClose(this->hidden->handle);
        if (res != 0) {
            SDL_SetError("sceAudioOutClose: %s", strerror(res));
        }
        this->hidden->handle = -1;
    }

    if (this->hidden->mixbuf) {
        free(this->hidden->mixbuf);
        this->hidden->mixbuf = 0;
    }
}

static void PS5AUDIO_ThreadInit(_THIS)
{
}

static void PS5AUDIO_Deinitialize(void)
{
}

static SDL_bool PS5AUDIO_Init(SDL_AudioDriverImpl *impl)
{
    static SDL_bool need_init = SDL_TRUE;

    if (need_init && sceAudioOutInit()) {
        return SDL_FALSE;
    }

    need_init = SDL_FALSE;

    impl->ThreadInit = PS5AUDIO_ThreadInit;
    impl->OpenDevice = PS5AUDIO_OpenDevice;
    impl->PlayDevice = PS5AUDIO_PlayDevice;
    impl->WaitDevice = PS5AUDIO_WaitDevice;
    impl->GetDeviceBuf = PS5AUDIO_GetDeviceBuf;
    impl->CloseDevice = PS5AUDIO_CloseDevice;
    impl->Deinitialize = PS5AUDIO_Deinitialize;

    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;

    return SDL_TRUE;
}

AudioBootStrap PS5AUDIO_bootstrap = { "ps5", "PS5 audio driver", PS5AUDIO_Init, SDL_FALSE };

#endif /* SDL_AUDIO_DRIVER_PS5 */

/* vi: set ts=4 sw=4 expandtab: */
