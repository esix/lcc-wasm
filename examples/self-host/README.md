# self-host (work in progress)

**LCC compiling *itself* to WebAssembly with the lcc-wasm back end.**

`build-rcc.sh` amalgamates LCC's front end + the wasm back end + the wasm libc
into one translation unit and compiles it with our own `rcc -target=wasm`,
producing **`rcc.wasm`** — the LCC C compiler, as a ~260 KB WebAssembly module.

```sh
# prereqs: (cd ../.. && make BUILDDIR=build rcc) ; brew install wabt
./build-rcc.sh          # -> rcc.wasm  (the compiled compiler)
node run-node.js        # feed it C on stdin, get .wat on stdout
```

The host (`run-node.js`) is the model from the discussion: **3 syscalls**
(`__read`/`__write`/`__exit`) over a tiny in-memory FS — `stdin` is the source,
`stdout` collects the `.wat`. A browser version would back the same 3 calls with
a JS mock filesystem; the wasm is identical.

## Status: builds and runs partway

`rcc.wasm` **builds** and **runs** — it initializes its type system, reads the
source through the host syscalls, and **tokenizes** — but currently **traps** on
a scale-dependent codegen bug in the tokenizer before it emits output. So this
is **not yet a working self-host**.

Getting here required fixing several real back-end bugs that only the
self-hosting stress test surfaced (void-function returns, shadow-stack restore on
fall-through, and data-segment layout for incomplete arrays and BSS). The
remaining work is flushing out the rest of the runtime codegen bugs that a
9,000-line input exercises — open-ended but tractable.

**For a fully working end-to-end demo today, see [`../hello-world`](../hello-world)** —
real C compiled by lcc-wasm and run in both node and the browser.

## What's verified to work in `rcc.wasm`

- amalgamation of all of LCC + libc compiles through our back end with zero C errors
- the module instantiates and `main()` runs
- type-system init, symbol/string interning, the arg buffer, the shadow stack
- input is read via `__read` and the lexer runs

## Files

| file | what |
|------|------|
| `build-rcc.sh`  | amalgamate LCC + compile it with `rcc -target=wasm` → `rcc.wasm` |
| `rcc.wasm`      | the compiled compiler (committed; rebuild with `build-rcc.sh`) |
| `rcc-amalg.c`   | the generated single-TU amalgamation (intermediate) |
| `run-node.js`   | host: syscalls + in-memory FS; feeds C in, collects `.wat` |
