# Local build settings for lcc-wasm.
# Modern clang treats K&R implicit declarations / int<->ptr as hard errors; relax them.
CFLAGS=-g -O0 -Wno-implicit-function-declaration -Wno-int-conversion -Wno-implicit-int -Wno-error -Wno-parentheses -Wno-return-type
