#!/usr/bin/env bash

SCRIPT_PATH="${BASH_SOURCE[0]}"
SCRIPT_DIR="$(dirname "${SCRIPT_PATH}")"


if [ -z "${PS5_PAYLOAD_SDK}" ]; then
    echo "PS5_PAYLOAD_SDK is undefined"
    exit 1
fi

SDL_PS5_OPENAGC="${SDL_PS5_OPENAGC:-OFF}"

source "${PS5_PAYLOAD_SDK}/toolchain/prospero.sh"

${CMAKE} -DCMAKE_BUILD_TYPE=Release \
	 -DSDL_OPENGL=YES \
	 -DSDL_LOADSO=YES \
	 -DSDL_PS5_OPENAGC="${SDL_PS5_OPENAGC}" \
	 -B build-ps5 \
         -S "${SCRIPT_DIR}/.."
${MAKE} -C build-ps5


${CMAKE} -DCMAKE_BUILD_TYPE=Release \
	 -DSDL_OPENGL=YES \
	 -DSDL_LOADSO=YES \
	 -B test-ps5 \
         -S "${SCRIPT_DIR}/../test"
${MAKE} -C test-ps5
