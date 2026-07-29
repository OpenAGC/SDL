#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
workspace_dir=$(CDPATH= cd -- "$script_dir/../../../../.." && pwd)
psbc=${PSBC:-$workspace_dir/openagc-psbc/psbc}
out=$(mktemp -d "${TMPDIR:-/tmp}/sdl-ps5agc-shaders.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM

command -v glslangValidator >/dev/null 2>&1
command -v xxd >/dev/null 2>&1
test -x "$psbc"

glslangValidator -V --target-env vulkan1.2 "$script_dir/ps5agc.vert" \
    -o "$out/ps5agc.vert.spv"
glslangValidator -V --target-env vulkan1.2 "$script_dir/ps5agc.geom" \
    -o "$out/ps5agc.geom.spv"

"$psbc" -f "$out/ps5agc.geom.spv" -s geometry \
    --pre-vertex "$out/ps5agc.vert.spv" --wave32 \
    --vertex-attribute 0:0:0:32:r32g32_float \
    --vertex-attribute 1:0:8:32:r32g32b32a32_float \
    --vertex-attribute 2:0:24:32:r32g32_float \
    --ngg-front "$out/ps5agc_ngg_front.sb" \
    -o "$out/ps5agc_ngg_back.sb"

for shader in \
    ps5agc_solid \
    ps5agc \
    ps5agc_yuv_planar_jpeg \
    ps5agc_yuv_planar_bt601 \
    ps5agc_yuv_planar_bt709 \
    ps5agc_yuv_nv_jpeg \
    ps5agc_yuv_nv_bt601 \
    ps5agc_yuv_nv_bt709
do
    glslangValidator -V --target-env vulkan1.2 "$script_dir/$shader.frag" \
        -o "$out/$shader.frag.spv"
    "$psbc" -f "$out/$shader.frag.spv" -s fragment \
        -o "$out/$shader.sb"
done

for shader in \
    ps5agc_ngg_front \
    ps5agc_ngg_back \
    ps5agc_solid \
    ps5agc_frag \
    ps5agc_yuv_planar_jpeg \
    ps5agc_yuv_planar_bt601 \
    ps5agc_yuv_planar_bt709 \
    ps5agc_yuv_nv_jpeg \
    ps5agc_yuv_nv_bt601 \
    ps5agc_yuv_nv_bt709
do
    source=$shader
    if [ "$shader" = ps5agc_frag ]; then
        source=ps5agc
    fi
    cp "$out/$source.sb" "$script_dir/$shader.sb"
    xxd -i -n "${shader}_sb" "$out/$source.sb" "$out/$shader.h"
    sed \
        -e 's/^unsigned char /static const unsigned char /' \
        -e 's/^unsigned int /static const unsigned int /' \
        "$out/$shader.h" >"$script_dir/$shader.h"
done
