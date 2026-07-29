#!/bin/sh
set -eu

: "${PS5_HOST:?set PS5_HOST to the FW 5.50 console address}"
: "${SDL_PS5AGC_STRESS_FRAMES:=180}"
: "${SDL_PS5AGC_STRESS_CHURN:=128}"
: "${SDL_PS5AGC_STRESS_RECREATE:=16}"
: "${SDL_PS5AGC_STRESS_SUITE_FRAMES:=30}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runner=$script_dir/run_ps5agc_display_probe.sh
render_suite=$script_dir/run_ps5agc_render_suite.sh

validate_positive() {
    name=$1
    value=$2
    case "$value" in
        ''|*[!0-9]*|0*)
            echo "$name must be a positive integer" >&2
            exit 2
            ;;
    esac
}

validate_positive SDL_PS5AGC_STRESS_FRAMES "$SDL_PS5AGC_STRESS_FRAMES"
validate_positive SDL_PS5AGC_STRESS_CHURN "$SDL_PS5AGC_STRESS_CHURN"
validate_positive SDL_PS5AGC_STRESS_RECREATE "$SDL_PS5AGC_STRESS_RECREATE"
validate_positive SDL_PS5AGC_STRESS_SUITE_FRAMES "$SDL_PS5AGC_STRESS_SUITE_FRAMES"
if [ "$SDL_PS5AGC_STRESS_CHURN" -gt 1000 ]; then
    echo "SDL_PS5AGC_STRESS_CHURN must not exceed 1000" >&2
    exit 2
fi

echo "ps5agc stress: triple-buffer presentation frames=$SDL_PS5AGC_STRESS_FRAMES"
SDL_PS5AGC_PROBE_KIND=display \
SDL_PS5AGC_PROBE_RENDERER=ps5agc \
SDL_PS5AGC_EXPECT_RENDERER=ps5agc \
SDL_PS5AGC_PROBE_FRAMES=$SDL_PS5AGC_STRESS_FRAMES \
    "$runner"

echo "ps5agc stress: texture churn count=$SDL_PS5AGC_STRESS_CHURN"
SDL_PS5AGC_PROBE_KIND=churn \
SDL_PS5AGC_PROBE_RENDERER=ps5agc \
SDL_PS5AGC_EXPECT_RENDERER=ps5agc \
SDL_PS5AGC_TEXTURE_CHURN_COUNT=$SDL_PS5AGC_STRESS_CHURN \
SDL_PS5AGC_PROBE_FRAMES=3 \
    "$runner"

echo "ps5agc stress: renderer recreation count=$SDL_PS5AGC_STRESS_RECREATE"
SDL_PS5AGC_PROBE_KIND=recreate \
SDL_PS5AGC_PROBE_RENDERER=ps5agc \
SDL_PS5AGC_EXPECT_RENDERER=ps5agc \
SDL_PS5AGC_RECREATE_COUNT=$SDL_PS5AGC_STRESS_RECREATE \
SDL_PS5AGC_PROBE_FRAMES=3 \
    "$runner"

echo "ps5agc stress: bounded renderer suite frames=$SDL_PS5AGC_STRESS_SUITE_FRAMES"
SDL_PS5AGC_PROBE_RENDERER=ps5agc \
SDL_PS5AGC_EXPECT_RENDERER=ps5agc \
SDL_PS5AGC_SUITE_FRAMES=$SDL_PS5AGC_STRESS_SUITE_FRAMES \
    "$render_suite"

echo "ps5agc stress matrix: PASS presentation=$SDL_PS5AGC_STRESS_FRAMES churn=$SDL_PS5AGC_STRESS_CHURN recreate=$SDL_PS5AGC_STRESS_RECREATE suite_frames=$SDL_PS5AGC_STRESS_SUITE_FRAMES"
