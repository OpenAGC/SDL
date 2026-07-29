#!/bin/sh
set -eu

: "${PS5_HOST:?set PS5_HOST to the FW 5.50 console address}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runner=$script_dir/run_ps5agc_display_probe.sh

for failure_point in mode-query initialization allocation submission presentation; do
    echo "ps5agc failure qualification: $failure_point"
    SDL_PS5AGC_PROBE_KIND=failure \
    SDL_PS5AGC_FAILURE_POINT=$failure_point \
        "$runner"
done

echo "ps5agc failure qualification matrix: PASS cases=5"
