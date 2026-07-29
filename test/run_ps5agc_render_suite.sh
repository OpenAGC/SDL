#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

: "${SDL_PS5AGC_PROBE_RENDERER:=ps5agc}"
: "${SDL_PS5AGC_EXPECT_RENDERER:=ps5agc}"
: "${SDL_PS5AGC_SUITE_FRAMES:=3}"
: "${SDL_PS5AGC_SUITE_STANDALONE_TARGETS:=testgeometry testrendertarget testscale testsprite2 testrendercopyex}"
export SDL_PS5AGC_PROBE_RENDERER SDL_PS5AGC_EXPECT_RENDERER

case "$SDL_PS5AGC_SUITE_FRAMES" in
    ''|*[!0-9]*|0*)
        echo "SDL_PS5AGC_SUITE_FRAMES must be a positive integer" >&2
        exit 2
        ;;
esac
for standalone_target in $SDL_PS5AGC_SUITE_STANDALONE_TARGETS; do
    case "$standalone_target" in
        testgeometry|testrendercopyex|testrendertarget|testscale|testsprite2) ;;
        *)
            echo "unsupported standalone renderer test: $standalone_target" >&2
            exit 2
            ;;
    esac
done

SDL_PS5AGC_PROBE_KIND=automation \
    "$script_dir/run_ps5agc_display_probe.sh"

for standalone_target in $SDL_PS5AGC_SUITE_STANDALONE_TARGETS; do
    SDL_PS5AGC_PROBE_KIND=standalone \
    SDL_PS5AGC_PROBE_FRAMES=$SDL_PS5AGC_SUITE_FRAMES \
    SDL_PS5AGC_STANDALONE_TARGET=$standalone_target \
        "$script_dir/run_ps5agc_display_probe.sh"
done
