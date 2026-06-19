# hello-world

A "hello world" compiled from C to WebAssembly by **lcc-wasm**, runnable in
**node** and the **browser**. The whole module is 199 bytes.

There is no libc: the program reaches the outside world by calling `putchar`,
which is **imported from the host** (node/browser JS). This is the same model a
winweb app uses to call its runtime.

```c
int putchar(int c);              /* provided by the host */
int main(void) {
    char *p;
    for (p = "Hello, world from lcc-wasm!\n"; *p; p++)
        putchar(*p);
    return 0;
}
```

## Prerequisites

- `rcc` built: from the repo root, `make BUILDDIR=build rcc`
- `wat2wasm` (WABT): `brew install wabt`

## Build

```sh
./build.sh        # hello.c --rcc--> hello.wat --wat2wasm--> hello.wasm
```

`hello.wat` and `hello.wasm` are committed, so you can skip the build and just
run the demos below.

## Run in node

```sh
node run-node.js
# -> Hello, world from lcc-wasm!
```

## Run in the browser

Browsers won't `fetch` a `.wasm` over `file://`, so serve the directory:

```sh
node serve.js     # then open http://localhost:8080/
```

## Files

| file | what |
|------|------|
| `hello.c`     | the C source |
| `build.sh`    | compile `hello.c` → `hello.wat` → `hello.wasm` |
| `hello.wat`   | generated WebAssembly text (human-readable) |
| `hello.wasm`  | generated binary module |
| `run-node.js` | node runner (supplies `putchar`) |
| `index.html`  | browser page (supplies `putchar`, prints to the page) |
| `serve.js`    | tiny static server for the browser demo |

## How it works

`rcc -target=wasm` turns the C into WebAssembly text: the string literal becomes
a `(data ...)` segment in linear memory, the loop becomes a `br_table` dispatch,
`*p` becomes an `i32.load8_s`, and `putchar(*p)` becomes a `call $putchar` to an
imported function. `wat2wasm` assembles that to the `.wasm` binary. The host then
instantiates the module, providing `putchar`, and calls the exported `main`.
