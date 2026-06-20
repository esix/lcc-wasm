// run-node.js - run the self-hosted rcc.wasm: feed it C on stdin, get .wat on stdout.
//
// The host implements exactly 3 syscalls (__read/__write/__exit) over a tiny
// in-memory filesystem; stdin (fd 0) is the source, stdout (fd 1) collects the
// .wat. This is the same model that would back a browser version (a mock FS).
//
//   node run-node.js
//
// rcc.wasm is LCC's own front end + our wasm back end, compiled to wasm by
// lcc-wasm itself. It reads the C below and prints a complete .wat module.
const fs = require("fs");
const path = require("path");

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
    const b = Buffer.from(new Uint8Array(mem.buffer, ptr, len));
    if (fd === 1) out.push(b); else process.stderr.write(b);
    return len;
  },
  __exit: (code) => { throw { __exit: code }; },
};
const stub = () => 0;   // dead refs (lburg/profiling we don't link) -> harmless stubs
const inst = new WebAssembly.Instance(
  new WebAssembly.Module(fs.readFileSync(path.join(__dirname, "rcc.wasm"))),
  { env: new Proxy(env, { get: (t, k) => (k in t ? t[k] : stub) }) });
mem = inst.exports.memory;

// argv = ["rcc","-target=wasm"] placed in a free region (between data and heap)
const A = 0x1f0000, dv = new DataView(mem.buffer), u8 = new Uint8Array(mem.buffer);
const put = (o, s) => { for (let i = 0; i < s.length; i++) u8[o + i] = s.charCodeAt(i); u8[o + s.length] = 0; };
put(A, "rcc"); put(A + 8, "-target=wasm");
dv.setUint32(A + 32, A, true); dv.setUint32(A + 36, A + 8, true);

try { inst.exports.main(2, A + 32); }
catch (e) { if (e.__exit === undefined) console.error("[trap: " + e.message + "]"); }

const wat = Buffer.concat(out).toString();
console.log(wat || "(no .wat emitted)");
// To run the result:  node run-node.js > hello.wat && wat2wasm hello.wat -o hello.wasm
// then instantiate hello.wasm with an `env.putchar` import -> prints the greeting.
