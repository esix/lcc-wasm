// compile-and-run.js - the full self-host loop in one command, no external tools.
//
// 1. rcc.wasm (LCC compiled to wasm by lcc-wasm) compiles a C hello-world
//    straight to a binary .wasm  (rcc -target=wasm-bin -- no wat2wasm / wabt)
// 2. node instantiates and runs it -> prints the greeting
//
//   node compile-and-run.js
const fs = require("fs");
const path = require("path");

const source = [
  "int putchar(int c);",
  "int main(void){",
  '  char *s = "Hello from self-hosted lcc-wasm!\\n";',
  "  while (*s) putchar(*s++);",
  "  return 0;",
  "}",
  "",
].join("\n");

// ---- run rcc.wasm to compile `source` directly to a binary .wasm ----
let mem;
const src = Buffer.from(source), out = [];
let pos = 0;
const env = {
  __read: (fd, ptr, len) => {
    if (fd !== 0) return 0;
    const n = Math.min(len, src.length - pos); if (n <= 0) return 0;
    new Uint8Array(mem.buffer).set(src.subarray(pos, pos + n), ptr); pos += n; return n;
  },
  __write: (fd, ptr, len) => {
    if (fd === 1) out.push(Buffer.from(new Uint8Array(mem.buffer, ptr, len)));
    else process.stderr.write(Buffer.from(new Uint8Array(mem.buffer, ptr, len)));
    return len;
  },
  __exit: (code) => { throw { __exit: code }; },
};
const stub = () => 0;
const rcc = new WebAssembly.Instance(
  new WebAssembly.Module(fs.readFileSync(path.join(__dirname, "rcc.wasm"))),
  { env: new Proxy(env, { get: (t, k) => (k in t ? t[k] : stub) }) });
mem = rcc.exports.memory;
const A = 0x1f0000, dv = new DataView(mem.buffer), u8 = new Uint8Array(mem.buffer);
const put = (o, s) => { for (let i = 0; i < s.length; i++) u8[o + i] = s.charCodeAt(i); u8[o + s.length] = 0; };
put(A, "rcc"); put(A + 8, "-target=wasm-bin");
dv.setUint32(A + 32, A, true); dv.setUint32(A + 36, A + 8, true);
try { rcc.exports.main(2, A + 32); } catch (e) { if (e.__exit === undefined) throw e; }
const wasm = Buffer.concat(out);
console.log("--- rcc.wasm compiled the C source to a binary .wasm (" + wasm.length + " bytes) ---");

// ---- run the produced module (no wabt: rcc emitted the binary itself) ----
let greeting = "";
const hello = new WebAssembly.Instance(
  new WebAssembly.Module(wasm),
  { env: { putchar: (c) => { greeting += String.fromCharCode(c); return c; } } });
const rc = hello.exports.main();
process.stdout.write("--- running the result: " + greeting);
console.log("--- main() returned " + rc + " ---");
