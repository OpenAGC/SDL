#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

: "${SDL_PS5AGC_PROBE_KIND:=automation}"
: "${SDL_PS5AGC_PROBE_RENDERER:=ps5agc}"
: "${SDL_PS5AGC_EXPECT_RENDERER:=ps5agc}"
export SDL_PS5AGC_PROBE_KIND SDL_PS5AGC_PROBE_RENDERER SDL_PS5AGC_EXPECT_RENDERER

exec "$script_dir/run_ps5agc_display_probe.sh"
