// End-to-end JSON-RPC 2.0 / LSP 3.17 integration test for `sbc lsp`.
// Runs identically on Windows x64 and Linux x64 via Node.js.
"use strict";

const { spawn } = require("child_process");
const path = require("path");
const assert = require("assert");
const { pathToFileURL } = require("url");

const root = path.resolve(__dirname, "..");
const sbcPath = process.argv[2] || path.join(root, process.platform === "win32" ? "sbc.exe" : "sbc");

const child = spawn(sbcPath, ["lsp"], { cwd: root, windowsHide: true });

let recvBuf = Buffer.alloc(0);
let nextId = 1;
const pending = new Map();
const diagWaiters = [];
const diagQueue = [];

child.stdout.on("data", (chunk) => {
  recvBuf = Buffer.concat([recvBuf, chunk]);
  for (;;) {
    const hdrEnd = recvBuf.indexOf("\r\n\r\n");
    if (hdrEnd === -1) break;
    const hdr = recvBuf.slice(0, hdrEnd).toString("ascii");
    const m = hdr.match(/Content-Length:\s*(\d+)/i);
    assert(m, `Missing Content-Length header: ${hdr}`);
    const len = parseInt(m[1], 10);
    const total = hdrEnd + 4 + len;
    if (recvBuf.length < total) break;
    const body = recvBuf.slice(hdrEnd + 4, total).toString("utf8");
    recvBuf = recvBuf.slice(total);

    const msg = JSON.parse(body);
    if (msg.id !== undefined && pending.has(msg.id)) {
      const p = pending.get(msg.id);
      pending.delete(msg.id);
      p.resolve(msg.result);
    } else if (msg.method === "textDocument/publishDiagnostics") {
      if (diagWaiters.length > 0) {
        diagWaiters.shift()(msg.params);
      } else {
        diagQueue.push(msg.params);
      }
    }
  }
});

let stderrLog = "";
child.stderr.on("data", (d) => { stderrLog += d.toString("utf8"); });

function send(obj) {
  const body = Buffer.from(JSON.stringify(obj), "utf8");
  const header = Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, "ascii");
  child.stdin.write(Buffer.concat([header, body]));
}

function request(method, params) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    const timer = setTimeout(() => reject(new Error(`Timeout waiting for ${method} (stderr: ${stderrLog})`)), 8000);
    pending.set(id, {
      resolve: (res) => { clearTimeout(timer); resolve(res); },
      reject: (err) => { clearTimeout(timer); reject(err); }
    });
    send({ jsonrpc: "2.0", id, method, params });
  });
}

function notify(method, params) {
  send({ jsonrpc: "2.0", method, params });
}

function nextDiagnostics() {
  if (diagQueue.length > 0) return Promise.resolve(diagQueue.shift());
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Timeout waiting for publishDiagnostics (stderr: ${stderrLog})`)), 8000);
    diagWaiters.push((params) => { clearTimeout(timer); resolve(params); });
  });
}

async function main() {
  // 1. Initialize
  const initRes = await request("initialize", {
    processId: process.pid,
    rootUri: pathToFileURL(root).toString(),
    capabilities: {}
  });
  assert.strictEqual(initRes.serverInfo.name, "sbc-lsp", "serverInfo.name");
  assert.strictEqual(initRes.capabilities.hoverProvider, true, "hoverProvider");
  assert.strictEqual(initRes.capabilities.definitionProvider, true, "definitionProvider");
  assert.strictEqual(initRes.capabilities.documentSymbolProvider, true, "documentSymbolProvider");
  assert.strictEqual(initRes.capabilities.documentFormattingProvider, true, "documentFormattingProvider");
  notify("initialized", {});

  // 2. Open a valid document importing std/vec.sb
  const sampleFile = path.join(root, "tests", "tmp", "lsp_probe.sb");
  const docUri = pathToFileURL(sampleFile).toString();
  const validSource = [
    'import "std/vec.sb"',               // line 0
    "",                                  // line 1
    "struct Point {",                    // line 2
    "    x: i64",                        // line 3
    "    y: i64",                        // line 4
    "}",                                 // line 5
    "",                                  // line 6
    "fn (p: *Point) scale(k: i64) {",    // line 7
    "    p.x *= k",                      // line 8
    "    p.y *= k",                      // line 9
    "}",                                 // line 10
    "",                                  // line 11
    "fn helper(n: i32): i32 {",          // line 12
    "    return n * 2",                  // line 13
    "}",                                 // line 14
    "",                                  // line 15
    "fn main() {",                       // line 16
    "    var v: Vec[i32]",               // line 17
    "    v.init(4)",                     // line 18
    "    defer v.free()",                // line 19
    "    v.push(helper(21))",            // line 20
    "    var pt = Point { x: 1, y: 2 }", // line 21
    "    pt.scale(3)",                   // line 22
    "    print(pt.x)",                   // line 23
    "}",                                 // line 24
    ""
  ].join("\n");

  const diag1P = nextDiagnostics();
  notify("textDocument/didOpen", {
    textDocument: { uri: docUri, languageId: "antimony", version: 1, text: validSource }
  });
  const diag1 = await diag1P;
  assert.strictEqual(diag1.diagnostics.length, 0, `Expected 0 diagnostics, got: ${JSON.stringify(diag1.diagnostics)}`);

  // 3. Hover: local function, imported struct, struct with fields, method, builtin
  const hoverHelper = await request("textDocument/hover", {
    textDocument: { uri: docUri },
    position: { line: 20, character: 14 } // `helper`
  });
  assert(hoverHelper && hoverHelper.contents.value.includes("fn helper(n: i32): i32"), `Unexpected hover(helper): ${JSON.stringify(hoverHelper)}`);

  const hoverVec = await request("textDocument/hover", {
    textDocument: { uri: docUri },
    position: { line: 17, character: 13 } // `Vec`
  });
  assert(hoverVec && hoverVec.contents.value.includes("struct Vec[T]") && hoverVec.contents.value.includes("vec.sb"),
    `Unexpected hover(Vec): ${JSON.stringify(hoverVec)}`);

  const hoverPush = await request("textDocument/hover", {
    textDocument: { uri: docUri },
    position: { line: 20, character: 8 } // `.push`
  });
  assert(hoverPush && hoverPush.contents.value.includes("push(val: T)"), `Unexpected hover(push): ${JSON.stringify(hoverPush)}`);

  const hoverPrint = await request("textDocument/hover", {
    textDocument: { uri: docUri },
    position: { line: 23, character: 6 } // `print`
  });
  assert(hoverPrint && hoverPrint.contents.value.includes("print"), `Unexpected hover(print): ${JSON.stringify(hoverPrint)}`);

  // 4. Go to Definition: same-file fn, cross-file std/vec.sb struct, and import line
  const defHelper = await request("textDocument/definition", {
    textDocument: { uri: docUri },
    position: { line: 20, character: 14 }
  });
  assert(defHelper && defHelper.range.start.line === 12, `Unexpected def(helper): ${JSON.stringify(defHelper)}`);

  const defVec = await request("textDocument/definition", {
    textDocument: { uri: docUri },
    position: { line: 17, character: 13 }
  });
  assert(defVec && defVec.uri.toLowerCase().endsWith("/std/vec.sb"), `Unexpected def(Vec): ${JSON.stringify(defVec)}`);

  const defImport = await request("textDocument/definition", {
    textDocument: { uri: docUri },
    position: { line: 0, character: 12 }
  });
  assert(defImport && defImport.uri.toLowerCase().endsWith("/std/vec.sb") && defImport.range.start.line === 0,
    `Unexpected def(import): ${JSON.stringify(defImport)}`);

  // 5. Document Symbols
  const syms = await request("textDocument/documentSymbol", { textDocument: { uri: docUri } });
  const symNames = syms.map((s) => s.name);
  assert(symNames.includes("Point") && symNames.includes("Point.scale") && symNames.includes("helper") && symNames.includes("main"),
    `Unexpected documentSymbols: ${JSON.stringify(symNames)}`);

  // 6. Completion (top-level prefix & dot-triggered method prefix)
  const comp1 = await request("textDocument/completion", {
    textDocument: { uri: docUri },
    position: { line: 20, character: 15 } // inside `helper`
  });
  assert(comp1.some((c) => c.label === "helper"), `Expected 'helper' in completion: ${JSON.stringify(comp1)}`);

  const compDot = await request("textDocument/completion", {
    textDocument: { uri: docUri },
    position: { line: 20, character: 9 } // inside `v.pu`
  });
  assert(compDot.some((c) => c.label === "push"), `Expected 'push' in dot completion: ${JSON.stringify(compDot)}`);

  // 7. Formatting
  const fmtFile = path.join(root, "tests", "tmp", "lsp_fmt.sb");
  const fmtUri = pathToFileURL(fmtFile).toString();
  const fmtDiagP = nextDiagnostics();
  notify("textDocument/didOpen", {
    textDocument: { uri: fmtUri, languageId: "antimony", version: 1, text: "fn main(){\nvar x=1+2\nprint(x)\n}\n" }
  });
  await fmtDiagP;
  const edits = await request("textDocument/formatting", {
    textDocument: { uri: fmtUri },
    options: { tabSize: 4, insertSpaces: true }
  });
  assert(Array.isArray(edits) && edits.length === 1 && edits[0].newText === "fn main() {\n    var x = 1 + 2\n    print(x)\n}\n",
    `Unexpected formatting edits: ${JSON.stringify(edits)}`);

  // 8. Live error recovery: introduce type error, verify diagnostic, then fix and verify cleared
  const errDiagP = nextDiagnostics();
  notify("textDocument/didChange", {
    textDocument: { uri: docUri, version: 2 },
    contentChanges: [{ text: 'fn main() {\n    var bad: i32 = "oops"\n    print(bad)\n}\n' }]
  });
  const errDiag = await errDiagP;
  assert(errDiag.diagnostics.length === 1 && errDiag.diagnostics[0].range.start.line === 1,
    `Expected type error at line 1, got: ${JSON.stringify(errDiag)}`);

  const fixDiagP = nextDiagnostics();
  notify("textDocument/didChange", {
    textDocument: { uri: docUri, version: 3 },
    contentChanges: [{ text: "fn main() {\n    var good: i32 = 42\n    print(good)\n}\n" }]
  });
  const fixDiag = await fixDiagP;
  assert.strictEqual(fixDiag.diagnostics.length, 0, `Expected cleared diagnostics, got: ${JSON.stringify(fixDiag)}`);

  // 9. Clean shutdown & exit
  const shutRes = await request("shutdown", null);
  assert.strictEqual(shutRes, null, "shutdown result");
  notify("exit", null);

  const exitCode = await new Promise((resolve) => child.on("close", resolve));
  assert.strictEqual(exitCode, 0, `Expected sbc lsp exit code 0, got ${exitCode}`);
  console.log("LSP integration tests passed (initialize, diagnostics, hover, definition, symbols, completion, formatting, recovery, shutdown).");
}

main().catch((err) => {
  console.error("LSP TEST FAILED:", err);
  try { child.kill(); } catch (_e) {}
  process.exit(1);
});
