#!/bin/sh
# build-rcc.sh - compile LCC itself to WebAssembly using lcc-wasm (self-host).
#
# Amalgamates LCC's front end + the wasm back end + the wasm libc into ONE
# translation unit (our back end emits a self-contained module per run, so
# "linking" = a single TU), then compiles that with our own rcc -> rcc.wasm.
#
# Prereqs: build rcc first  ->  (cd ../.. && make BUILDDIR=build rcc) ; brew install wabt
#
# STATUS: working self-host. rcc.wasm (~260 KB) builds, runs, and compiles C to
# WebAssembly -- see ./compile-and-run.js for the full compile-and-execute loop.
set -e
cd "$(dirname "$0")/../.."          # repo root (lcc/)
RCC="${RCC:-build/rcc}"
OUT=examples/self-host/rcc-amalg.c

# front end + null/wasm back ends + main (no register back ends, no profiling)
MIN="alloc dag decl enode error event expr init inits input lex list output simp stmt string sym trace tree types null wasm wasmbin main"

# per-file renames to resolve file-scope name collisions in one TU
rename() {
  f=$1
  perl -pe "s/\\brcsid\\b/rcsid_$f/g" < src/$f.c | case $f in
    string) perl -pe 's/\bbuckets\b/buckets_str/g; s/\bentry\b/entry_str/g' ;;
    sym)    perl -pe 's/\bbuckets\b/buckets_sym/g; s/\bentry\b/entry_sym/g; s/\bids\b/ids_sym/g' ;;
    types)  perl -pe 's/\bbuckets\b/buckets_typ/g; s/\bentry\b/entry_typ/g' ;;
    dag)    perl -pe 's/\bbuckets\b/buckets_dag/g; s/\bentry\b/entry_dag/g; s/\btypestab\b/typestab_dag/g' ;;
    tree)   perl -pe 's/\bids\b/ids_tree/g' ;;
    main)   perl -pe 's/\btypestab\b/typestab_main/g' ;;
    *)      cat ;;
  esac
}
{
  echo '#include "c.h"'
  echo 'extern Interface wasmIR, nullIR;'
  for f in $MIN; do
    [ "$f" = main ] && echo 'Binding bindings[4] = { {"wasm", &wasmIR}, {"wasm-bin", &wasmIR}, {"null", &nullIR}, {0, 0} };'
    echo "/**** $f.c ****/"; rename $f
  done
  echo '/**** libc.c ****/'; cat lib/wasm/libc.c
} > "$OUT"

clang -E -nostdinc -Iinclude/wasm -Isrc "$OUT" -o examples/self-host/rcc-amalg.i
"$RCC" -target=wasm examples/self-host/rcc-amalg.i > examples/self-host/rcc.wat
wat2wasm examples/self-host/rcc.wat -o examples/self-host/rcc.wasm --debug-names
echo "built examples/self-host/rcc.wasm ($(wc -c < examples/self-host/rcc.wasm | tr -d ' ') bytes)"
