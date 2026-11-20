#!/bin/sh
# Build the fully static Linux executable (musl, mimalloc, ISA-L) in an Alpine container, from the repository root:
#
#   docker run --rm -e HOST_UID="$(id -u):$(id -g)" -v "$PWD":/src -w /src alpine:3.24 tools/ci/build-static-linux.sh
#
# The result, build/release/wikigraph, runs on any Linux distribution of the same architecture.
set -eu
apk add --no-cache build-base cmake samurai git linux-headers zlib-dev zlib-static openssl-dev openssl-libs-static
if [ "$(uname -m)" = x86_64 ]; then apk add --no-cache nasm; fi  # ISA-L's x86-64 assembly
cmake --preset release -G Ninja -DWIKIGRAPH_STATIC=ON
cmake --build build/release
if [ -n "${HOST_UID:-}" ]; then chown -R "$HOST_UID" build; fi  # the container runs as root
