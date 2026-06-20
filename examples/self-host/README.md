# self-host

**LCC compiling *itself* to WebAssembly with the lcc-wasm back end.**

`build-rcc.sh` amalgamates LCC's front end + the wasm back end + the wasm libc
into one translation unit and compiles it with our own `rcc -target=wasm`,
producing **`rcc.wasm`** — the LCC C compiler, as a ~260 KB WebAssembly module.

```sh
# prereqs: (cd ../.. && make BUILDDIR=build rcc) ; brew install wabt
./build-rcc.sh          # -> rcc.wasm  (the compiled compiler)
node compile-and-run.js # rcc.wasm compiles a hello-world, then we run the result
node run-node.js        # just print the .wat rcc.wasm emits for the hello-world

# in the browser: edit C, Compile & Run, all client-side
node serve.js           # then open http://localhost:8080/
```

The host (`run-node.js`) is the model from the discussion: **3 syscalls**
(`__read`/`__write`/`__exit`) over a tiny in-memory FS — `stdin` is the source,
`stdout` collects the `.wat`. The browser demo (`index.html`) backs the same 3
calls with a JS mock filesystem; the wasm is identical.

## Browser demo (`index.html`)

A live, fully client-side C compiler: type C, hit **Compile & Run**, and the page
runs `rcc.wasm` (the LCC compiler, as wasm) to emit `.wat`, assembles it with
**wabt** (`wabt.js`, vendored from the `wabt` npm package — the one thing the
browser can't do natively is wat→wasm), instantiates the result, and runs it —
capturing `putchar` output. A fresh `rcc.wasm` instance per compile keeps the
compiler's globals clean. No server-side compilation, no native toolchain.

## Status: working self-host

`rcc.wasm` **builds, runs, and compiles C to WebAssembly.** `compile-and-run.js`
drives the whole loop:

```
--- rcc.wasm compiled the C source to .wat (932 bytes) ---
--- running the result: Hello from self-hosted lcc-wasm!
--- main() returned 0 ---
```

That is the LCC C compiler — itself running as WebAssembly — reading C source,
emitting a `.wat` module, which `wat2wasm` assembles and node runs. A real
compile-and-execute pipeline with no native toolchain involved.

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
| `build-rcc.sh`       | amalgamate LCC + compile it with `rcc -target=wasm` → `rcc.wasm` |
| `rcc.wasm`           | the compiled compiler (committed; rebuild with `build-rcc.sh`) |
| `rcc-amalg.c`        | the generated single-TU amalgamation (intermediate) |
| `run-node.js`        | host: syscalls + in-memory FS; prints the `.wat` for a hello-world |
| `compile-and-run.js` | node full loop: compile with `rcc.wasm`, assemble, run, print greeting |
| `index.html`         | browser demo: edit C, Compile & Run, all client-side |
| `wabt.js`            | vendored WABT (wat→wasm in the browser; from the `wabt` npm package) |
| `serve.js`           | static server for the browser demo (`node serve.js` → localhost:8080) |
