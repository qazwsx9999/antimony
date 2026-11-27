# Antimony (`.sb`) VS Code Extension

Language support for the **Antimony** programming language in Visual Studio Code.

## Features

- **Syntax Highlighting**: Full TextMate grammar (`source.sb`) covering:
  - Comments (`// ...`)
  - Escaped strings (`"..."`), multiline raw backtick strings (`` `...` ``), and character literals (`'a'`, `'\n'`, `'\x41'`)
  - Decimal, hex (`0x`), binary (`0b`), octal (`0o`), underscore-separated integers, and floating-point literals (while keeping `0..10` ranges intact)
  - Keywords (`fn`, `var`, `const`, `struct`, `enum`, `type`, `match`, `defer`, `loop`, `in`, `by`, `if`, `else`, `return`, `break`, `continue`, `import`, `extern`, `pub`, `priv`, `true`, `false`, `null`)
  - Built-in scalar types (`i8`–`i64`, `u8`–`u64`, `isize`, `usize`, `f32`, `f64`, `bool`, `str`) and user-defined/generic types (`Vec[T]`, `Option[T]`)
  - Built-in functions (`print`, `exit`, `rev`, `len`, `sizeof`, `alignof`, `ptr_cast`, `atomic_load`, `atomic_store`, `atomic_add`, `atomic_cas`)
  - Wrapping operators (`+%`, `-%`, `*%`, `+%=`, `-%=`, `*%=`), range operator (`..`), error propagation (`?`), enum variant shorthands (`.Some`, `.None`), and loop labels
- **Editor Configuration**: Auto-closing brackets/quotes/backticks, `//` line comment toggling, smart indentation, and `.sb` file icons.
- **Snippets**: Quick templates for `main`, `fn`, `fnm`, `struct`, `enum`, `type`, `match`, `loop`, `loopr`, `loopfor`, `if`, `ife`, `var`, `const`, `defer`, `import`, `extern`, `print`.
- **Outline & Breadcrumbs (`Ctrl+Shift+O`)**: Symbol navigation for functions, methods, structs (with fields), enums (with variants), type aliases, constants, variables, and `extern` declarations.
- **Hover Documentation**: Quick descriptions for Antimony keywords, scalar types, and built-in functions.
- **Live Compiler Diagnostics**: Automatically runs `sbc check --lib` on open and save to highlight compile errors and hints inline (toggle with `antimony.enableDiagnostics`).
- **Canonical Formatting (`Shift+Alt+F` / Format on Save)**: Uses `sbc fmt --stdin` to format Antimony source files cleanly without modifying disk until saved.
- **Compile, Check, Test & Run Commands**: Use the editor title ▶ button or Command Palette (`Antimony: Compile & Run Current File`, `Antimony: Compile Current File`, `Antimony: Type-Check Current File`, `Antimony: Run Tests in Current File (sbc test)`, `Antimony: Format Current File (sbc fmt)`). Uses `sbc.exe` in the workspace root, `antimony.compilerPath`, or `sbc` on `PATH`.

## Local Installation

Copy (or symlink) this directory to your VS Code extensions folder:

```powershell
Copy-Item -Recurse -Force editors\vscode "$env:USERPROFILE\.vscode\extensions\antimony-lang.antimony-0.2.0"
```

Then reload VS Code (`Developer: Reload Window`).
