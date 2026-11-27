const vscode = require("vscode");
const fs = require("fs");
const path = require("path");
const { execFile, spawn } = require("child_process");

const BUILTIN_DOCS = {
  // Scalar & built-in types
  i8: "**`i8`** — 8-bit signed two's-complement integer (`-128` to `127`).",
  i16: "**`i16`** — 16-bit signed two's-complement integer (`-32768` to `32767`).",
  i32: "**`i32`** — 32-bit signed two's-complement integer (default integer literal type).",
  i64: "**`i64`** — 64-bit signed two's-complement integer.",
  isize: "**`isize`** — Pointer-sized signed integer (64-bit on x64).",
  u8: "**`u8`** — 8-bit unsigned integer (`0` to `255`).",
  u16: "**`u16`** — 16-bit unsigned integer (`0` to `65535`).",
  u32: "**`u32`** — 32-bit unsigned integer.",
  u64: "**`u64`** — 64-bit unsigned integer.",
  usize: "**`usize`** — Pointer-sized unsigned integer (sizes, byte counts, indices).",
  f32: "**`f32`** — 32-bit IEEE-754 floating-point number.",
  f64: "**`f64`** — 64-bit IEEE-754 floating-point number (default float literal type).",
  bool: "**`bool`** — Boolean value (`true` or `false`). No implicit integer conversions.",
  str: "**`str`** — Immutable UTF-8 string view (`{ptr, len}`), supports `len(s)` and slicing `s[lo..hi]`.",

  // Built-in functions
  print: "**`print(x)` / `print(fmt, args...)`** — Built-in output function. Prints scalars, pointers, `bool`, or `str` followed by a newline; supports `{}` format placeholders (`{{` and `}}` escape literal braces).",
  exit: "**`exit(code: i32)`** — Built-in that terminates the process immediately with `code`.",
  rev: "**`rev(range)`** — Reverses a range expression inside a `loop` header (e.g. `loop i in rev(0..10)`).",
  len: "**`len(x)`** — Returns the element count of a fixed-size array (untyped constant), slice, or `str` (`i64`).",
  sizeof: "**`sizeof(T)`** — Compile-time size in bytes of type `T`.",
  alignof: "**`alignof(T)`** — Compile-time alignment in bytes of type `T`.",
  ptr_cast: "**`ptr_cast(TargetPtrType, ptr_expr)`** — Explicit pointer type cast (e.g. `ptr_cast(*Vec2, raw_ptr)`).",
  atomic_load: "**`atomic_load(ptr: *i64): i64`** — Hardware atomic load.",
  atomic_store: "**`atomic_store(ptr: *i64, val: i64)`** — Hardware atomic store.",
  atomic_add: "**`atomic_add(ptr: *i64, delta: i64): i64`** — Hardware atomic fetch-and-add.",
  atomic_cas: "**`atomic_cas(ptr: *i64, expected: i64, desired: i64): i64`** — Hardware atomic compare-and-swap (returns the previous value).",

  // Keywords
  fn: "**`fn`** — Declares a function, method (`fn (self: *T) name(...)`), function pointer type (`fn(T): R`), or stateless anonymous function.",
  var: "**`var`** — Declares a mutable variable (`var x: i32 = 0` or `var x = 0`).",
  const: "**`const`** — Declares a compile-time constant (`const N = 10`) or a read-only pointer/slice qualifier (`*const T`, `[]const T`).",
  struct: "**`struct`** — Declares a composite value type with naturally aligned fields.",
  enum: "**`enum`** — Declares an `i32` enum or tagged union (`enum Option[T] { Some(T), None }`).",
  type: "**`type`** — Declares a transparent type alias (`type Byte = u8`).",
  match: "**`match`** — Pattern-matches values or tagged union variants (`match x { .Some(v) { ... } else { ... } }`).",
  defer: "**`defer`** — Schedules a statement to execute in LIFO order when the enclosing scope exits.",
  loop: "**`loop`** — The unified loop keyword: infinite (`loop { }`), conditional (`loop cond { }`), range (`loop i in 0..n { }`), or C-style (`loop var i = 0; i < n; i += 1 { }`).",
  in: "**`in`** — Introduces the iterable range, array, or slice in a `loop` header.",
  by: "**`by`** — Specifies a positive step size in a range loop (`loop i in 0..10 by 2`).",
  import: "**`import`** — Imports another `.sb` source module (`import \"std/io.sb\"`).",
  extern: "**`extern`** — Declares foreign DLL functions (`extern \"kernel32.dll\" fn ...` or `extern \"dll\" { ... }`).",
  pub: "**`pub`** — Marks a top-level declaration as exported from the module (enables opt-in module encapsulation).",
  priv: "**`priv`** — Restricts a top-level declaration or method to the current `.sb` file.",
  null: "**`null`** — Zero pointer value for pointer (`*T`, `*const T`) and function-pointer types."
};

/**
 * Strips comments and string/char literals while preserving line & column offsets,
 * so top-level symbol parsing never gets fooled by braces or keywords in strings/comments.
 */
function maskCommentsAndStrings(text) {
  const chars = text.split("");
  const n = chars.length;
  let i = 0;
  while (i < n) {
    const c = chars[i];
    if (c === "/" && chars[i + 1] === "/") {
      while (i < n && chars[i] !== "\n") {
        chars[i] = " ";
        i++;
      }
      continue;
    }
    if (c === '"') {
      chars[i++] = " ";
      while (i < n && chars[i] !== "\n") {
        if (chars[i] === "\\" && i + 1 < n && chars[i + 1] !== "\n") {
          chars[i++] = " ";
          chars[i++] = " ";
          continue;
        }
        if (chars[i] === '"') {
          chars[i++] = " ";
          break;
        }
        chars[i++] = " ";
      }
      continue;
    }
    if (c === "`") {
      chars[i++] = " ";
      while (i < n) {
        const ch = chars[i];
        if (ch !== "\n" && ch !== "\r") {
          chars[i] = " ";
        }
        i++;
        if (ch === "`") break;
      }
      continue;
    }
    if (c === "'") {
      chars[i++] = " ";
      while (i < n && chars[i] !== "\n") {
        if (chars[i] === "\\" && i + 1 < n && chars[i + 1] !== "\n") {
          chars[i++] = " ";
          chars[i++] = " ";
          continue;
        }
        if (chars[i] === "'") {
          chars[i++] = " ";
          break;
        }
        chars[i++] = " ";
      }
      continue;
    }
    i++;
  }
  return chars.join("");
}

function findBlockEndLine(lines, startLine) {
  let depth = 0;
  let seenOpen = false;
  for (let ln = startLine; ln < lines.length; ln++) {
    const line = lines[ln];
    for (let j = 0; j < line.length; j++) {
      if (line[j] === "{") {
        depth++;
        seenOpen = true;
      } else if (line[j] === "}") {
        depth--;
        if (seenOpen && depth <= 0) {
          return ln;
        }
      }
    }
  }
  return startLine;
}

class AntimonyDocumentSymbolProvider {
  provideDocumentSymbols(document) {
    const rawText = document.getText();
    const masked = maskCommentsAndStrings(rawText);
    const lines = masked.split(/\r?\n/);
    const rawLines = rawText.split(/\r?\n/);
    const symbols = [];

    let depth = 0;
    let inExternBlock = false;
    let externBlockDepth = 0;

    for (let ln = 0; ln < lines.length; ln++) {
      const line = lines[ln];
      const trimmed = line.trim();

      if (trimmed.length > 0) {
        if (depth === 0) {
          // Strip optional pub/priv modifier
          const declText = trimmed.replace(/^(pub|priv)\s+/, "");

          // extern "dll" { ... }
          if (/^extern\b/.test(declText) && /\{/.test(declText) && !/\bfn\b/.test(declText)) {
            inExternBlock = true;
            externBlockDepth = depth + 1;
          } else {
            const sym = this.parseTopDecl(declText, rawLines[ln], lines, ln);
            if (sym) {
              symbols.push(sym);
            }
          }
        } else if (inExternBlock && depth === externBlockDepth) {
          const sym = this.parseExternFn(trimmed, rawLines[ln], ln);
          if (sym) {
            symbols.push(sym);
          }
        }
      }

      for (let j = 0; j < line.length; j++) {
        if (line[j] === "{") depth++;
        else if (line[j] === "}") {
          depth = Math.max(0, depth - 1);
          if (inExternBlock && depth < externBlockDepth) {
            inExternBlock = false;
          }
        }
      }
    }

    return symbols;
  }

  makeRange(startLine, endLine, lineLength) {
    return new vscode.Range(
      new vscode.Position(startLine, 0),
      new vscode.Position(endLine, Math.max(0, lineLength))
    );
  }

  parseExternFn(trimmed, rawLine, ln) {
    const m = trimmed.match(/^fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(/);
    if (!m) return null;
    const name = m[1];
    const col = Math.max(0, rawLine.indexOf(name));
    const fullRange = this.makeRange(ln, ln, rawLine.length);
    const selRange = new vscode.Range(ln, col, ln, col + name.length);
    return new vscode.DocumentSymbol(name, "extern fn", vscode.SymbolKind.Function, fullRange, selRange);
  }

  parseTopDecl(declText, rawLine, lines, ln) {
    // extern "dll" fn name(...)
    let m = declText.match(/^extern\b.*\bfn\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(/);
    if (m) {
      const name = m[1];
      const col = Math.max(0, rawLine.indexOf(name));
      const fullRange = this.makeRange(ln, ln, rawLine.length);
      const selRange = new vscode.Range(ln, col, ln, col + name.length);
      return new vscode.DocumentSymbol(name, "extern fn", vscode.SymbolKind.Function, fullRange, selRange);
    }

    // Method: fn (recv: Type) name(...)
    m = declText.match(/^fn\s*\(([^)]*)\)\s*([A-Za-z_][A-Za-z0-9_]*)/);
    if (m) {
      const recv = m[1].trim();
      const name = m[2];
      const endLine = findBlockEndLine(lines, ln);
      const col = Math.max(0, rawLine.indexOf(name));
      const fullRange = this.makeRange(ln, endLine, lines[endLine].length);
      const selRange = new vscode.Range(ln, col, ln, col + name.length);
      return new vscode.DocumentSymbol(name, `(${recv})`, vscode.SymbolKind.Method, fullRange, selRange);
    }

    // Function: fn name[T](...)
    m = declText.match(/^fn\s+([A-Za-z_][A-Za-z0-9_]*)/);
    if (m) {
      const name = m[1];
      const endLine = findBlockEndLine(lines, ln);
      const col = Math.max(0, rawLine.indexOf(name));
      const fullRange = this.makeRange(ln, endLine, lines[endLine].length);
      const selRange = new vscode.Range(ln, col, ln, col + name.length);
      return new vscode.DocumentSymbol(name, "fn", vscode.SymbolKind.Function, fullRange, selRange);
    }

    // Struct: struct Name
    m = declText.match(/^struct\s+([A-Za-z_][A-Za-z0-9_]*)/);
    if (m) {
      const name = m[1];
      const endLine = findBlockEndLine(lines, ln);
      const col = Math.max(0, rawLine.indexOf(name));
      const fullRange = this.makeRange(ln, endLine, lines[endLine].length);
      const selRange = new vscode.Range(ln, col, ln, col + name.length);
      const sym = new vscode.DocumentSymbol(name, "struct", vscode.SymbolKind.Struct, fullRange, selRange);

      for (let fl = ln + 1; fl < endLine; fl++) {
        const fTrim = lines[fl].trim();
        const fm = fTrim.match(/^([A-Za-z_][A-Za-z0-9_]*(?:\[[^\]]*\])?)\s*:\s*(.+)$/);
        if (fm) {
          const fname = fm[1];
          const fcol = Math.max(0, lines[fl].indexOf(fname));
          const fRange = this.makeRange(fl, fl, lines[fl].length);
          const fSel = new vscode.Range(fl, fcol, fl, fcol + fname.length);
          sym.children.push(
            new vscode.DocumentSymbol(fname, fm[2].trim(), vscode.SymbolKind.Field, fRange, fSel)
          );
        }
      }
      return sym;
    }

    // Enum: enum Name
    m = declText.match(/^enum\s+([A-Za-z_][A-Za-z0-9_]*)/);
    if (m) {
      const name = m[1];
      const endLine = findBlockEndLine(lines, ln);
      const col = Math.max(0, rawLine.indexOf(name));
      const fullRange = this.makeRange(ln, endLine, lines[endLine].length);
      const selRange = new vscode.Range(ln, col, ln, col + name.length);
      const sym = new vscode.DocumentSymbol(name, "enum", vscode.SymbolKind.Enum, fullRange, selRange);

      for (let vl = ln + 1; vl < endLine; vl++) {
        const vTrim = lines[vl].trim();
        const vm = vTrim.match(/^([A-Za-z_][A-Za-z0-9_]*)/);
        if (vm) {
          const vname = vm[1];
          const vcol = Math.max(0, lines[vl].indexOf(vname));
          const vRange = this.makeRange(vl, vl, lines[vl].length);
          const vSel = new vscode.Range(vl, vcol, vl, vcol + vname.length);
          sym.children.push(
            new vscode.DocumentSymbol(vname, "", vscode.SymbolKind.EnumMember, vRange, vSel)
          );
        }
      }
      return sym;
    }

    // Type alias: type Name = ...
    m = declText.match(/^type\s+([A-Za-z_][A-Za-z0-9_]*)/);
    if (m) {
      const name = m[1];
      const col = Math.max(0, rawLine.indexOf(name));
      const fullRange = this.makeRange(ln, ln, rawLine.length);
      const selRange = new vscode.Range(ln, col, ln, col + name.length);
      return new vscode.DocumentSymbol(name, "type", vscode.SymbolKind.Interface, fullRange, selRange);
    }

    // Const: const NAME ...
    m = declText.match(/^const\s+([A-Za-z_][A-Za-z0-9_]*)/);
    if (m) {
      const name = m[1];
      const col = Math.max(0, rawLine.indexOf(name));
      const fullRange = this.makeRange(ln, ln, rawLine.length);
      const selRange = new vscode.Range(ln, col, ln, col + name.length);
      return new vscode.DocumentSymbol(name, "const", vscode.SymbolKind.Constant, fullRange, selRange);
    }

    // Top-level var: var name ...
    m = declText.match(/^var\s+([A-Za-z_][A-Za-z0-9_]*)/);
    if (m) {
      const name = m[1];
      const col = Math.max(0, rawLine.indexOf(name));
      const fullRange = this.makeRange(ln, ln, rawLine.length);
      const selRange = new vscode.Range(ln, col, ln, col + name.length);
      return new vscode.DocumentSymbol(name, "var", vscode.SymbolKind.Variable, fullRange, selRange);
    }

    return null;
  }
}

class AntimonyHoverProvider {
  provideHover(document, position) {
    const range = document.getWordRangeAtPosition(position, /[A-Za-z_][A-Za-z0-9_]*/);
    if (!range) return null;

    const lineText = document.lineAt(position.line).text;
    const commentIdx = lineText.indexOf("//");
    if (commentIdx !== -1 && position.character >= commentIdx) {
      return null;
    }

    const word = document.getText(range);
    const doc = BUILTIN_DOCS[word];
    if (!doc) return null;

    return new vscode.Hover(new vscode.MarkdownString(doc), range);
  }
}

function resolveCompiler(fileUri) {
  const config = vscode.workspace.getConfiguration("antimony", fileUri);
  const configured = (config.get("compilerPath") || "").trim();
  if (configured) {
    return configured;
  }

  const wsFolder = vscode.workspace.getWorkspaceFolder(fileUri) ||
    (vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0]);
  if (wsFolder) {
    const exeName = process.platform === "win32" ? "sbc.exe" : "sbc";
    const candidate = path.join(wsFolder.uri.fsPath, exeName);
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }

  return process.platform === "win32" ? "sbc.exe" : "sbc";
}

function formatTextWithSbc(text, fileUri) {
  return new Promise((resolve, reject) => {
    const compiler = resolveCompiler(fileUri);
    const wsFolder = vscode.workspace.getWorkspaceFolder(fileUri);
    const cwd = wsFolder ? wsFolder.uri.fsPath : (fileUri && fileUri.fsPath ? path.dirname(fileUri.fsPath) : undefined);

    const child = spawn(compiler, ["fmt", "--stdin"], { cwd, windowsHide: true });
    let stdout = "";
    let stderr = "";

    child.stdout.on("data", (chunk) => {
      stdout += chunk.toString("utf8");
    });
    child.stderr.on("data", (chunk) => {
      stderr += chunk.toString("utf8");
    });
    child.on("error", (err) => {
      reject(err);
    });
    child.on("close", (code) => {
      if (code === 0) {
        resolve(stdout);
      } else {
        reject(new Error(stderr.trim() || `sbc fmt exited with code ${code}`));
      }
    });

    child.stdin.write(text, "utf8");
    child.stdin.end();
  });
}

class AntimonyDocumentFormattingEditProvider {
  async provideDocumentFormattingEdits(document) {
    const original = document.getText();
    try {
      const formatted = await formatTextWithSbc(original, document.uri);
      if (formatted === original || formatted.replace(/\r\n/g, "\n") === original.replace(/\r\n/g, "\n")) {
        return [];
      }
      const lastLine = document.lineCount - 1;
      const fullRange = new vscode.Range(
        new vscode.Position(0, 0),
        document.lineAt(lastLine).range.end
      );
      return [vscode.TextEdit.replace(fullRange, formatted)];
    } catch (err) {
      vscode.window.setStatusBarMessage(`Antimony fmt: ${err.message.split(/\r?\n/)[0]}`, 4000);
      return [];
    }
  }
}

function parseCompilerDiagnostics(output, fallbackFilePath) {
  const byFile = new Map();
  const lines = output.split(/\r?\n/);
  const locRe = /^(.+?):(\d+):(\d+):\s*(error|warning|hint):\s*(.+)$/;
  const plainErrRe = /^error:\s*(.+)$/;

  const pushDiag = (filePath, lineNum, colNum, severityStr, message) => {
    const resolved = path.resolve(filePath);
    const ln = Math.max(0, lineNum - 1);
    const col = Math.max(0, colNum - 1);
    const range = new vscode.Range(ln, col, ln, col + 1);
    let severity = vscode.DiagnosticSeverity.Error;
    if (severityStr === "warning") severity = vscode.DiagnosticSeverity.Warning;
    else if (severityStr === "hint") severity = vscode.DiagnosticSeverity.Hint;

    const diag = new vscode.Diagnostic(range, message, severity);
    diag.source = "sbc";
    if (!byFile.has(resolved)) {
      byFile.set(resolved, []);
    }
    byFile.get(resolved).push(diag);
  };

  for (const raw of lines) {
    const m = raw.match(locRe);
    if (m) {
      const filePart = m[1] === "<input>" ? fallbackFilePath : m[1];
      pushDiag(filePart, parseInt(m[2], 10), parseInt(m[3], 10), m[4], m[5]);
      continue;
    }
    const pm = raw.match(plainErrRe);
    if (pm) {
      pushDiag(fallbackFilePath, 1, 1, "error", pm[1]);
    }
  }
  return byFile;
}

function runDiagnostics(document, collection) {
  if (!document || document.languageId !== "antimony" || document.uri.scheme !== "file") {
    return;
  }
  const config = vscode.workspace.getConfiguration("antimony", document.uri);
  if (!config.get("enableDiagnostics", true)) {
    collection.delete(document.uri);
    return;
  }

  const filePath = document.uri.fsPath;
  const compiler = resolveCompiler(document.uri);
  const wsFolder = vscode.workspace.getWorkspaceFolder(document.uri);
  const cwd = wsFolder ? wsFolder.uri.fsPath : path.dirname(filePath);

  execFile(compiler, ["check", "--lib", filePath], { cwd, windowsHide: true }, (_err, stdout, stderr) => {
    const combined = `${stdout || ""}\n${stderr || ""}`;
    const byFile = parseCompilerDiagnostics(combined, filePath);
    collection.delete(document.uri);
    for (const [diagPath, diags] of byFile.entries()) {
      collection.set(vscode.Uri.file(diagPath), diags);
    }
  });
}

async function runCompilerTask(mode) {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== "antimony") {
    vscode.window.showWarningMessage("Open an Antimony (.sb) file to build, run, or test.");
    return;
  }

  if (editor.document.isUntitled) {
    vscode.window.showWarningMessage("Please save the Antimony file first.");
    return;
  }

  if (editor.document.isDirty) {
    const saved = await editor.document.save();
    if (!saved) return;
  }

  const filePath = editor.document.uri.fsPath;
  const compiler = resolveCompiler(editor.document.uri);
  const args = [mode, filePath];
  const label = mode === "run" ? "Run Antimony File" : (mode === "test" ? "Test Antimony File" : "Build Antimony File");
  const wsFolder = vscode.workspace.getWorkspaceFolder(editor.document.uri);
  const cwd = wsFolder ? wsFolder.uri.fsPath : path.dirname(filePath);

  const task = new vscode.Task(
    { type: "antimony", task: mode },
    wsFolder || vscode.TaskScope.Workspace,
    label,
    "antimony",
    new vscode.ProcessExecution(compiler, args, { cwd })
  );
  task.presentationOptions = {
    reveal: vscode.TaskRevealKind.Always,
    panel: vscode.TaskPanelKind.Shared,
    clear: true
  };

  await vscode.tasks.executeTask(task);
}

function activate(context) {
  const selector = { language: "antimony", scheme: "file" };
  const untitledSelector = { language: "antimony", scheme: "untitled" };
  const diagnostics = vscode.languages.createDiagnosticCollection("antimony");

  context.subscriptions.push(
    diagnostics,
    vscode.languages.registerDocumentSymbolProvider([selector, untitledSelector], new AntimonyDocumentSymbolProvider()),
    vscode.languages.registerHoverProvider([selector, untitledSelector], new AntimonyHoverProvider()),
    vscode.languages.registerDocumentFormattingEditProvider(
      [selector, untitledSelector],
      new AntimonyDocumentFormattingEditProvider()
    ),
    vscode.commands.registerCommand("antimony.runFile", () => runCompilerTask("run")),
    vscode.commands.registerCommand("antimony.buildFile", () => runCompilerTask("build")),
    vscode.commands.registerCommand("antimony.testFile", () => runCompilerTask("test")),
    vscode.commands.registerCommand("antimony.checkFile", () => {
      const editor = vscode.window.activeTextEditor;
      if (editor && editor.document.languageId === "antimony") {
        runDiagnostics(editor.document, diagnostics);
      }
    }),
    vscode.commands.registerCommand("antimony.formatFile", async () => {
      await vscode.commands.executeCommand("editor.action.formatDocument");
    }),
    vscode.workspace.onDidOpenTextDocument((doc) => runDiagnostics(doc, diagnostics)),
    vscode.workspace.onDidSaveTextDocument((doc) => runDiagnostics(doc, diagnostics)),
    vscode.workspace.onDidCloseTextDocument((doc) => diagnostics.delete(doc.uri)),
    vscode.window.onDidChangeActiveTextEditor((editor) => {
      if (editor) runDiagnostics(editor.document, diagnostics);
    })
  );

  if (vscode.window.activeTextEditor) {
    runDiagnostics(vscode.window.activeTextEditor.document, diagnostics);
  }
}

function deactivate() {}

module.exports = {
  activate,
  deactivate
};
