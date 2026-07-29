#include "../../SDL_internal.h"

#ifdef __PROSPERO__

#include "SDL_main.h"

#ifdef main
#undef main
#endif

int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceGetAppStatus(void *status);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceKernelUsleep(unsigned int microseconds);

static SDL_NORETURN void PS5_SystemServiceExit(void)
{
    Uint32 app_status[0x100u / sizeof(Uint32)] = { 0 };
    Uint32 app_id;

    if (sceSystemServiceGetAppStatus(app_status) == 0) {
        app_id = app_status[2];
        if (app_id < 0x10u || app_id == SDL_MAX_UINT32) {
            app_id = app_status[0];
        }
        if (app_id >= 0x10u && app_id != SDL_MAX_UINT32) {
            sceSystemServiceKillApp((int)app_id, 0, 0, 0);
        }
    }

    /* SystemService termination is asynchronous. Never return into the raw
       ELF entry trampoline, even if querying or killing the app fails. */
    for (;;) {
        sceKernelUsleep(1000000u);
    }
}

int main(int argc, char *argv[])
{
    sceSystemServiceHideSplashScreen();
    SDL_main(argc, argv);
    PS5_SystemServiceExit();
}

#endif // __PROSPERO__

/* vi: set ts=4 sw=4 expandtab: */
