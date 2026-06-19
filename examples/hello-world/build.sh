#!/bin/sh
# Compile hello.c to WebAssembly with the lcc-wasm back end, then assemble
# the .wat text to a .wasm binary with wat2wasm (WABT).
#
# Prerequisites:
#   1. build rcc:   (cd ../.. && make BUILDDIR=build rcc)
#   2. install WABT (provides wat2wasm), e.g.  brew install wabt
set -e
cd "$(dirname "$0")"
RCC="${RCC:-../../build/rcc}"

"$RCC" -target=wasm hello.c > hello.wat        # C  -> WebAssembly text
wat2wasm hello.wat -o hello.wasm               # .wat -> .wasm binary
echo "built hello.wasm ($(wc -c < hello.wasm | tr -d ' ') bytes)"
