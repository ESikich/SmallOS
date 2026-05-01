#!/bin/sh
set -eu

ROOT_DIR=${1:-$(pwd)}
BUILD_DIR=${2:-"$ROOT_DIR/build/tinycc-host"}
SRC_DIR=${3:-"$ROOT_DIR/third_party/tinycc"}
HOST_CC=${CC:-gcc}
HOST_AR=${AR:-ar}

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ ! -f config.mak ] || [ ! -f config.h ]; then
    "$SRC_DIR/configure" \
        --cc="$HOST_CC" \
        --ar="$HOST_AR" \
        --enable-static \
        --disable-rpath \
        --source-path="$SRC_DIR"
fi

make -j2
