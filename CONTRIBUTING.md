# Contributing to Antimony

Thanks for your interest in contributing to **Antimony**! This guide explains how to build the compiler, run the test suites, format Antimony code, and submit clean pull requests.

## Prerequisites

- **Windows x64**
- A **C11 compiler** (`gcc` from MinGW-w64/MSYS2 or `clang`)
- *Optional:* **LLVM/Clang** (`clang.exe`) for running `--llvm` (`-O3`) backend tests

## Building `sbc`

```powershell
.\build.bat
```

`build.bat` automatically detects `%CC%`, `gcc` or `clang` on `PATH`, or common MinGW-w64/MSYS2 installations. Compiler sources must build cleanly with **zero warnings** under `-std=c11 -O2 -Wall -Wextra -Werror`.

## Running Tests

```powershell
# 1. Fast native-only regression suite (~10 seconds)
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -Quick

# 2. Run a specific test or phase by name filter
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -Filter phase29

# 3. Full regression suite (native x64 + LLVM -O3 + compile-error + runtime-trap)
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1

# 4. Self-hosted standard library unit tests
.\sbc.exe test tests\stdlib_test.sb
.\sbc.exe test tests\stdlib_test.sb --llvm
```

## Formatting & Code Style

### Antimony (`.sb`) files
- Format `.sb` files with the official canonical formatter before submitting:
  ```powershell
  .\sbc.exe fmt -w path\to\file.sb
  .\sbc.exe fmt --check path\to\file.sb
  ```
- Every module in `std/` must use explicit `pub` exports so internal helpers, constants, and `priv extern` FFI bindings do not leak into importing modules.
- Prefer `*const T` and `[]const T` for read-only parameters, `Result[T, E]` / `Option[T]` for fallible operations, and explicit `.free()` methods compatible with `defer`.

### Compiler (`src/*.c`, `src/sb.h`)
- Stick to clean C11 with no external library dependencies beyond the standard C library and Win32 headers (`windows.h`).
- Keep both backends (`src/codegen.c` and `src/llvm_c.c`) in feature parity when changing language semantics or adding built-ins.
- Never silently ignore limits or invalid inputs — emit a clear `fatal(pos, ...)` diagnostic with exact source location.

## Pull Request Checklist

1. `build.bat` succeeds with **zero warnings**.
2. `tests\run_tests.ps1` passes completely (both native x64 and `--llvm`).
3. `sbc test tests\stdlib_test.sb` and `sbc fmt --check tests\stdlib_test.sb` pass.
4. Any new language feature, compiler flag, or `std/` module includes regression tests and documentation updates (`docs/spec.md`, `docs/tutorial.md`, `README.md`).
