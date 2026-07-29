#!/bin/sh
set -eu

: "${PS5_HOST:?set PS5_HOST to the FW 5.50 console address}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runner=$script_dir/run_ps5agc_display_probe.sh

for format in iyuv yv12 nv12 nv21; do
    for mode in jpeg bt601 bt709; do
        echo "ps5agc YUV qualification: format=$format mode=$mode"
        SDL_PS5AGC_PROBE_KIND=yuv \
        SDL_PS5AGC_YUV_FORMAT=$format \
        SDL_PS5AGC_YUV_MODE=$mode \
        SDL_PS5AGC_PROBE_RENDERER=ps5agc \
        SDL_PS5AGC_EXPECT_RENDERER=ps5agc \
        "$runner"
    done
done

echo "ps5agc YUV qualification matrix: PASS formats=4 modes=3 cases=12"
