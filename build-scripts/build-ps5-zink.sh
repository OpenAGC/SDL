#!/bin/sh

set -eu

MESA_REVISION=7cbd56d5d7dece6da6e95368db448903bfcc68dc
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SDL_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)
MESA_SOURCE=${MESA_SOURCE:-"${SDL_ROOT}/../mesa"}
MESA_SOURCE=$(CDPATH= cd -- "${MESA_SOURCE}" && pwd)
MESA_BUILD_DIR=${MESA_BUILD_DIR:-"${MESA_SOURCE}/build-prospero-zink"}
PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-"${HOME}/ps5-payload-sdk"}

if [ ! -x "${PS5_PAYLOAD_SDK}/bin/prospero-clang" ]; then
    echo "build-ps5-zink: invalid PS5_PAYLOAD_SDK: ${PS5_PAYLOAD_SDK}" >&2
    exit 1
fi

actual_revision=$(git -C "${MESA_SOURCE}" rev-parse HEAD)
if [ "${actual_revision}" != "${MESA_REVISION}" ]; then
    echo "build-ps5-zink: Mesa revision ${actual_revision}, expected ${MESA_REVISION}" >&2
    exit 1
fi

BISON_BINDIR=${BISON_BINDIR:-/opt/homebrew/opt/bison/bin}
PATH="${BISON_BINDIR}:${PS5_PAYLOAD_SDK}/bin:${PATH}"
export PATH
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS PKG_CONFIG_PATH

set -- \
    --cross-file="${SCRIPT_DIR}/ps5-mesa-cross.ini" \
    -Dgallium-drivers=zink \
    '-Dvulkan-drivers=[]' \
    '-Dplatforms=[]' \
    -Dglx=disabled \
    -Degl=enabled \
    -Dgbm=disabled \
    -Dllvm=disabled \
    -Dshared-glapi=disabled \
    -Dopengl=true \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dbuild-tests=false \
    -Dvalgrind=disabled \
    -Dlibunwind=disabled \
    -Dzlib=disabled \
    -Dzstd=disabled \
    -Dshader-cache=disabled \
    '-Dvideo-codecs=[]' \
    '-Dtools=[]'

cd "${MESA_SOURCE}"
if [ -f "${MESA_BUILD_DIR}/meson-private/coredata.dat" ]; then
    meson setup --reconfigure "${MESA_BUILD_DIR}" "$@"
else
    meson setup "${MESA_BUILD_DIR}" "${MESA_SOURCE}" "$@"
fi
meson compile -C "${MESA_BUILD_DIR}" "${MESA_BUILD_JOBS:--j4}"

if [ -n "${MESA_DESTDIR:-}" ]; then
    DESTDIR=${MESA_DESTDIR} meson install -C "${MESA_BUILD_DIR}"
fi

echo "build-ps5-zink: PASS mesa=${MESA_REVISION} build=${MESA_BUILD_DIR}"
