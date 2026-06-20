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
```

The host (`run-node.js`) is the model from the discussion: **3 syscalls**
(`__read`/`__write`/`__exit`) over a tiny in-memory FS — `stdin` is the source,
`stdout` collects the `.wat`. A browser version would back the same 3 calls with
a JS mock filesystem; the wasm is identical.

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

Getting here required fixing several real back-end bugs that only the
self-hosting stress test surfaced:

- **jump-table layout** — lcc sizes a `switch` jump-table symbol by case *count*
  but emits one entry per value in the *range* (gaps → default), so its
  `type->size` understates the data. Eagerly reserving its address by
  `type->size` under-reserved and let the table overflow into the next literal,
  corrupting the tokenizer's keyword dispatch. Fixed by deferring code→data
  address resolution (and derived `base+offset` aliases) until each symbol is
  byte-counted by its own definition.
- **`printf` length/width** — the wasm back end prints constants with `%ld`/`%lu`
  and data bytes with `%02x`; the hand-written libc `vformat` didn't handle the
  `l` length modifier or `0`/width flags. Fixed in `lib/wasm/libc.c`.

## Known limitation

A few front-end constructs in the de-dag/spill pass are still miscompiled when
LCC runs as wasm; the most visible is post-increment through a pointer (`*p++`),
which emits an `;; UNSUPPORTED` marker. Write the equivalent with explicit
indexing (`s[i]; i = i + 1;`) and it compiles cleanly — that is what the demo
programs do. Native `rcc` compiles `*p++` correctly, so this is a self-host
codegen bug, not a language gap.

## Files

| file | what |
|------|------|
| `build-rcc.sh`       | amalgamate LCC + compile it with `rcc -target=wasm` → `rcc.wasm` |
| `rcc.wasm`           | the compiled compiler (committed; rebuild with `build-rcc.sh`) |
| `rcc-amalg.c`        | the generated single-TU amalgamation (intermediate) |
| `run-node.js`        | host: syscalls + in-memory FS; prints the `.wat` for a hello-world |
| `compile-and-run.js` | full loop: compile with `rcc.wasm`, assemble, run, print greeting |
