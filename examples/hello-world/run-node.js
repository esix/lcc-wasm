// Run hello.wasm under node. The host supplies putchar(); the wasm module
// writes "Hello, world..." one character at a time by calling it.
//
//   node run-node.js
const fs = require("fs");
const path = require("path");

const bytes = fs.readFileSync(path.join(__dirname, "hello.wasm"));
const env = {
  putchar: (c) => { process.stdout.write(String.fromCharCode(c & 0xff)); return c; },
};

WebAssembly.instantiate(bytes, { env }).then(({ instance }) => {
  process.exitCode = instance.exports.main();
});
