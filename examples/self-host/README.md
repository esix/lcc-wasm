# self-host

**LCC compiling *itself* to WebAssembly with the lcc-wasm back end.**

`build-rcc.sh` amalgamates LCC's front end + the wasm back end + the wasm libc
into one translation unit and compiles it with our own `rcc`, producing
**`rcc.wasm`** — the LCC C compiler, as a ~280 KB WebAssembly module.

```sh
# prereqs: (cd ../.. && make BUILDDIR=build rcc) ; brew install wabt (only to BUILD rcc.wasm)
./build-rcc.sh          # -> rcc.wasm  (the compiled compiler)
node compile-and-run.js # rcc.wasm compiles a hello-world to binary .wasm, then runs it
node run-node.js        # print the .wat rcc.wasm emits for the hello-world (for reading)

# in the browser: edit C, Compile & Run, all client-side
node serve.js           # then open http://localhost:8080/
```

`rcc` has two wasm targets: **`-target=wasm`** emits `.wat` text, while
**`-target=wasm-bin`** emits a binary `.wasm` directly (an in-process assembler,
`src/wasmbin.c`). The demos use `-target=wasm-bin`, so **no `wat2wasm`/`wabt` is
needed to run** — `rcc` (native or as `rcc.wasm`) produces a runnable module on
its own. `wabt` is only used to *build* `rcc.wasm` from the amalgam. (Both targets
refuse to write output on a compile error and exit non-zero, so a pipeline can't
ship a module that traps at runtime.)

The host is the model from the discussion: **3 syscalls** (`__read`/`__write`/`__exit`)
over a tiny in-memory FS — `stdin` is the source, `stdout` is the module bytes. The
browser demo (`index.html`) backs the same 3 calls with a JS mock filesystem; the
wasm is identical.

## Browser demo (`index.html`)

A live, fully client-side C compiler: type C, hit **Compile & Run**, and the page
runs `rcc.wasm` (the LCC compiler, as wasm) with `-target=wasm-bin` to emit a
binary `.wasm`, then `WebAssembly.instantiate`s and runs it — capturing `putchar`
output. A fresh `rcc.wasm` instance per compile keeps the compiler's globals
clean. **No assembler, no server, no native toolchain — just the ~280 KB
compiler** (the `.wat` pane shows the readable form `rcc` can also emit).

## Status: working self-host

`rcc.wasm` **builds, runs, and compiles C to WebAssembly.** `compile-and-run.js`
drives the whole loop:

```
--- rcc.wasm compiled the C source to a binary .wasm (242 bytes) ---
--- running the result: Hello from self-hosted lcc-wasm!
--- main() returned 0 ---
```

That is the LCC C compiler — itself running as WebAssembly — reading C source,
emitting a runnable binary `.wasm` module that node instantiates and runs. A real
compile-and-execute pipeline with **no native toolchain and no assembler** in the
loop.

For non-trivial programs (structs, recursion, `switch`, pointer post-increment,
globals, arrays), the wasm-hosted `rcc.wasm` produces **byte-for-byte the same
`.wat` as the native `rcc`** — the self-host is faithful, not a reduced subset.

Getting here required fixing several real back-end bugs that only the
self-hosting stress test surfaced:

- **jump-table layout** — lcc sizes a `switch` jump-table symbol by case *count*
  but emits one entry per value in the *range* (gaps → default), so its
  `type->size` understates the data. Eagerly reserving its address by
  `type->size` under-reserved and let the table overflow into the next literal,
  corrupting the tokenizer's keyword dispatch. Fixed by deferring code→data
  address resolution (and derived `base+offset` aliases) until each symbol is
  byte-counted by its own definition.
- **address-taken parameters** — a scalar parameter whose address is taken
  (`f(T x){ ... &x ... }`) was left as a wasm local, so `&x` had no valid
  address. LCC's own de-dag/spill pass does exactly this (`prune`/`undag` take
  `&forest` of their pointer parameter), so the miscompiled compiler then
  dropped the temp-spill for `*p++` and similar. Fixed by spilling such params
  to the shadow-stack frame and copying the incoming arg into the slot.
- **`printf` length/width** — the wasm back end prints constants with `%ld`/`%lu`
  and data bytes with `%02x`; the hand-written libc `vformat` didn't handle the
  `l` length modifier or `0`/width flags. Fixed in `lib/wasm/libc.c`.

## Files

| file | what |
|------|------|
| `build-rcc.sh`       | amalgamate LCC (incl. `wasmbin.c`) + compile it with `rcc` → `rcc.wasm` |
| `rcc.wasm`           | the compiled compiler (committed; rebuild with `build-rcc.sh`) |
| `rcc-amalg.c`        | the generated single-TU amalgamation (intermediate) |
| `run-node.js`        | host: syscalls + in-memory FS; prints the `.wat` for a hello-world |
| `compile-and-run.js` | node full loop: `rcc.wasm` emits binary `.wasm`, node runs it, prints greeting |
| `index.html`         | browser demo: edit C, Compile & Run, all client-side (no wabt) |
| `serve.js`           | static server for the browser demo (`node serve.js` → localhost:8080) |
