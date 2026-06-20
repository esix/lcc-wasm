// Minimal static server for the browser demo (browsers won't fetch wasm/js over
// file://). Serves this directory.  node serve.js  then open http://localhost:8080/
const http = require("http");
const fs = require("fs");
const path = require("path");
const dir = __dirname, port = process.env.PORT || 8080;
const types = { ".html": "text/html", ".wasm": "application/wasm", ".js": "text/javascript" };
http.createServer((req, res) => {
  let f = path.join(dir, decodeURIComponent(req.url.split("?")[0]));
  if (f.endsWith(path.sep)) f = path.join(f, "index.html");
  fs.readFile(f, (err, data) => {
    if (err) { res.writeHead(404); res.end("not found"); return; }
    res.writeHead(200, { "content-type": types[path.extname(f)] || "text/plain" });
    res.end(data);
  });
}).listen(port, () => console.log("serving " + dir + "\n  open http://localhost:" + port + "/"));
