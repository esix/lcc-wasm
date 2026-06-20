// compile-and-run.js - the full self-host loop in one command.
//
// 1. rcc.wasm (LCC compiled to wasm by lcc-wasm) compiles a C hello-world to .wat
// 2. wat2wasm assembles that .wat to a .wasm
// 3. node instantiates and runs it -> prints the greeting
//
//   node compile-and-run.js          (needs wat2wasm on PATH: brew install wabt)
const fs = require("fs");
const path = require("path");
const { execFileSync } = require("child_process");

const source = [
  "int putchar(int c);",
  "int main(void){",
  '  char *s = "Hello from self-hosted lcc-wasm!\\n";',
  "  int i = 0;",
  "  while (s[i]) { putchar(s[i]); i = i + 1; }",
  "  return 0;",
  "}",
  "",
].join("\n");

// ---- step 1: run rcc.wasm to compile `source` -> .wat ----
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
put(A, "rcc"); put(A + 8, "-target=wasm");
dv.setUint32(A + 32, A, true); dv.setUint32(A + 36, A + 8, true);
try { rcc.exports.main(2, A + 32); } catch (e) { if (e.__exit === undefined) throw e; }
const wat = Buffer.concat(out).toString();
console.log("--- rcc.wasm compiled the C source to .wat (" + wat.length + " bytes) ---");

// ---- step 2: wat2wasm assembles the .wat ----
const watPath = path.join(require("os").tmpdir(), "selfhost-hello.wat");
const wasmPath = path.join(require("os").tmpdir(), "selfhost-hello.wasm");
fs.writeFileSync(watPath, wat);
execFileSync("wat2wasm", [watPath, "-o", wasmPath]);

// ---- step 3: run the produced module ----
let greeting = "";
const hello = new WebAssembly.Instance(
  new WebAssembly.Module(fs.readFileSync(wasmPath)),
  { env: { putchar: (c) => { greeting += String.fromCharCode(c); return c; } } });
const rc = hello.exports.main();
process.stdout.write("--- running the result: " + greeting);
console.log("--- main() returned " + rc + " ---");
