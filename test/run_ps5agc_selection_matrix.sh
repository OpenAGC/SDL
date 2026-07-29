#!/bin/sh
set -eu

: "${PS5_HOST:?set PS5_HOST to the FW 5.50 console address}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runner=$script_dir/run_ps5agc_display_probe.sh
disabled_elf=${SDL_PS5AGC_DISABLED_ELF:-}

case "$disabled_elf" in
    '') ;;
    *)
        if [ ! -f "$disabled_elf" ]; then
            echo "SDL_PS5AGC_DISABLED_ELF does not exist: $disabled_elf" >&2
            exit 2
        fi
        ;;
esac

echo "ps5agc selection qualification: explicit hardware"
SDL_PS5AGC_PROBE_KIND=display \
SDL_PS5AGC_PROBE_RENDERER=ps5agc \
SDL_PS5AGC_EXPECT_RENDERER=ps5agc \
"$runner"

echo "ps5agc selection qualification: automatic hardware"
SDL_PS5AGC_PROBE_KIND=display \
SDL_PS5AGC_PROBE_RENDERER=auto \
SDL_PS5AGC_EXPECT_RENDERER=ps5agc \
"$runner"

echo "ps5agc selection qualification: unnamed accelerated hardware"
SDL_PS5AGC_PROBE_KIND=display \
SDL_PS5AGC_PROBE_RENDERER=auto \
SDL_PS5AGC_EXPECT_RENDERER=ps5agc \
SDL_PS5AGC_PROBE_ACCELERATED=1 \
"$runner"

echo "ps5agc selection qualification: explicit software"
SDL_PS5AGC_PROBE_KIND=display \
SDL_PS5AGC_PROBE_RENDERER=software \
SDL_PS5AGC_EXPECT_RENDERER=software \
"$runner"

case "$disabled_elf" in
    '') ;;
    *)
        echo "ps5agc selection qualification: backend-absent automatic software"
        SDL_PS5AGC_PROBE_KIND=display \
        SDL_PS5AGC_PROBE_ELF="$disabled_elf" \
        SDL_PS5AGC_SKIP_BUILD=1 \
        SDL_PS5AGC_PROBE_RENDERER=auto \
        SDL_PS5AGC_EXPECT_RENDERER=software \
        "$runner"

        echo "ps5agc selection qualification: backend-absent explicit failure"
        SDL_PS5AGC_PROBE_KIND=display \
        SDL_PS5AGC_PROBE_ELF="$disabled_elf" \
        SDL_PS5AGC_SKIP_BUILD=1 \
        SDL_PS5AGC_PROBE_RENDERER=ps5agc \
        SDL_PS5AGC_EXPECT_FAILURE=1 \
        SDL_PS5AGC_EXPECT_ERROR='ps5agc renderer is not available' \
        "$runner"
        ;;
esac

if [ -n "$disabled_elf" ]; then
    echo "ps5agc selection qualification matrix: PASS supported=4 backend-absent=2 cases=6"
else
    echo "ps5agc selection qualification matrix: PASS supported=4 cases=4"
fi
