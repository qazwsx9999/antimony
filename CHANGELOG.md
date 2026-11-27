# Changelog

All notable changes to **Antimony (`sbc`)** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-11-27

First stable core language and toolchain release for Windows x64.

### Language & Specification
- Finalized **Antimony Language Specification v1.0** (`docs/spec.md`) and full guided tutorial (`docs/tutorial.md`).
- Static typing with local forward-only type inference, untyped numeric constants, no implicit conversions (`T(x)`), and 6 operator precedence levels.
- Control flow: `if`/`else`, unified `loop` (infinite, conditional, half-open range `a..b [by s]` + `rev(...)`, collection iteration, restricted C-style), loop labels, exhaustive `match`, `defer`, and `?` error/option propagation.
- Type system: fixed arrays `[N]T`, bounds-checked slices `[]T` / `[]const T`, pointers `*T` / `*const T`, structs, methods with auto-ref/auto-deref, enums & tagged unions, transparent `type` aliases, first-class function pointers, stateless anonymous functions, and compile-time monomorphized generics `[T]` with recursive nested type-argument inference.
- Safety: unbounded definite-assignment analysis (no implicit zero-init), checked overflow & narrowing conversions in debug mode, explicit wrapping operators (`+%`, `-%`, `*%`), null checks in debug mode, and bounds checks in all modes.
- Multi-file `import` modules with opt-in `pub`/`priv` encapsulation, direct Windows DLL FFI (`extern`), and hardware atomic built-ins (`atomic_load`, `atomic_store`, `atomic_add`, `atomic_cas`).

### Dual-Backend Compiler (`sbc`)
- **Native x64 Backend:** Direct in-memory PE32+ executable emission with zero external dependencies (register allocation, AST function inlining, strength reduction, SSE2 XMM register-tree evaluation, SIB addressing, and range-loop bounds-check elimination).
- **Optimizing LLVM Backend (`--llvm` / `-O3` / `--emit-llvm`):** Direct AST-to-LLVM-IR emission with Rust-parity runtime performance on the cross-language benchmark suite (`benchmarks/`).
- **CLI Toolchain:** `sbc build`, `sbc run`, `sbc test` (automatic `fn test_*()` discovery + `--filter`), `sbc check [--lib]`, `sbc fmt` (canonical lossless 4-space formatter with `-w`, `--check`, `--stdin`), and `sbc --version`.
- Portable `build.bat` (auto-detects `gcc`, `clang`, MinGW-w64, MSYS2, or `%CC%`) and clean zero-warning C11 build under `-Wall -Wextra -Werror`.

### Standard Library (25 Modules)
- **Core & Memory:** `std/core.sb` (`Option[T]`, `Result[T, E]`, `Ordering`, `panic`, `assert`, `swap`), `std/mem.sb` (process heap, `alloc_typed`, `Arena`), `std/testing.sb`.
- **Collections & Algorithms:** `std/vec.sb` (`Vec[T]`), `std/map.sb` (`HashMap[K, V]`), `std/set.sb` (`HashSet[T]`), `std/deque.sb` (`Deque[T]`), `std/heap.sb` (`BinaryHeap[T]`, `MinHeap[T]`), `std/sort.sb`.
- **Text, Data & Crypto:** `std/str.sb` (`StringBuilder`, search, strict `Result` parsers), `std/encoding.sb` (RFC 4648 Hex & Base64), `std/hash.sb` (FNV-1a, IEEE CRC32, FIPS 180-4 SHA-256), `std/json.sb` (RFC 8259 arena-backed parser & serializer).
- **System, I/O & Networking:** `std/io.sb` (`File`, `IoError`, `SeekFrom`), `std/bufio.sb` (`BufReader`, `BufWriter`), `std/fs.sb` (filesystem + `fs_read_dir`), `std/path.sb`, `std/os.sb`, `std/time.sb` (`Stopwatch`), `std/rand.sb` (xoshiro256++ + Windows CSPRNG), `std/thread.sb` (`Thread`, `Mutex`, `RwLock`, `CondVar`, `AtomicI64`), `std/process.sb` (`Child`, `process_capture`), `std/net.sb` (Winsock2 TCP/UDP + DNS), `std/http.sb` (HTTP/1.1 client & server), `std/math.sb`.

### Examples, Editor & CI
- Flagship examples: `examples/demo.sb`, `examples/sbwc.sb` (CLI word-count & search utility), and `examples/sbhttpd.sb` (multithreaded HTTP/1.1 static-file & JSON API server).
- VS Code extension v0.2.0 (`editors/vscode/`) with syntax highlighting, live `sbc check --lib` diagnostics, `sbc fmt` document formatting, outline symbols, hover docs, snippets, and Build/Run/Test commands.
- GitHub Actions CI (`.github/workflows/ci.yml`), automated tag-based release workflow (`.github/workflows/release.yml`), 193 compiler/backend regression tests (`tests/run_tests.ps1`), and self-hosted stdlib test suite (`tests/stdlib_test.sb`).
