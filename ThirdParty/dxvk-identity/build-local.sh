#!/bin/sh
# Local, isolated dependency build; performs no install step.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
destination=${1:?usage: build-local.sh /absolute/isolated/output}
case "$destination" in /*) ;; *) echo 'absolute output path required' >&2; exit 1;; esac
[ ! -e "$destination/source" ] || { echo 'source already exists; use a fresh output' >&2; exit 1; }
mkdir -p "$destination"
git clone --depth 1 --branch v3.1.1 --no-checkout https://github.com/doitsujin/dxvk.git "$destination/source"
git -C "$destination/source" checkout --detach b1a1c99ab52b687cf950d62c88bc2fa316b41663
[ "$(git -C "$destination/source" rev-parse HEAD)" = b1a1c99ab52b687cf950d62c88bc2fa316b41663 ]
git -C "$destination/source" submodule update --init --recursive --depth 1 --jobs 2
git -C "$destination/source" apply --check "$root/ThirdParty/dxvk-identity/dxvk-v3.1.1-cpu-identity.patch"
git -C "$destination/source" apply "$root/ThirdParty/dxvk-identity/dxvk-v3.1.1-cpu-identity.patch"
# Explicit compilers avoid implicit compiler-cache wrappers.
CC=${CC:-gcc} CXX=${CXX:-g++} meson setup "$destination/build" "$destination/source" \
    --buildtype=release \
    -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false \
    -Dnative_sdl2=enabled -Dnative_sdl3=disabled -Dnative_glfw=disabled
meson compile -C "$destination/build" -j2
sha256sum "$destination/build/src/d3d9/"libdxvk_d3d9.so* \
    "$destination/source/include/native/directx/d3d9_cpu_identity.h"
