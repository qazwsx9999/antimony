# Antimony

[![CI](https://github.com/qazwsx9999/antimony/actions/workflows/ci.yml/badge.svg)](https://github.com/qazwsx9999/antimony/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**Antimony** (`.sb`) is a statically typed systems programming language with a compiler, `sbc`, written from scratch in C. It produces standalone Windows x64 executables directly — no external assembler, linker, or C toolchain required — and offers an optional LLVM backend for maximum runtime performance.

```
import "std/vec.sb"

enum Shape {
    Circle(f64)
    Rect(f64)
}

fn (s: Shape) area(): f64 {
    match s {
        .Circle(r) { return 3.14159 * r * r }
        .Rect(w)   { return w * w }
    }
}

fn main() {
    var shapes: Vec[Shape]
    shapes.init(4)
    defer shapes.free()

    shapes.push(.Circle(1.5))
    shapes.push(.Rect(2.0))

    var total = 0.0
    loop s in shapes.as_slice() {
        total += s.area()
    }
    print("total area = {}", total)
}
```

## Highlights

- **Fast compilation.** The native backend type-checks and emits a PE32+ executable in milliseconds, with no dependencies beyond `sbc.exe`.
- **Two backends.** A built-in x64 code generator (register allocation, inlining, strength reduction, bounds-check elimination) for fast builds, and an LLVM IR backend (`--llvm`) for optimized release builds.
- **Safety without a runtime.** Definite-assignment analysis instead of implicit initialization, no implicit numeric conversions, overflow and narrowing checks in debug builds, and bounds checks in all builds — with no garbage collector and no hidden allocations.
- **Modern type system.** Structs and methods, tagged unions with exhaustive `match`, `?` error propagation, compile-time generics, first-class function pointers, read-only pointers and slices, and type aliases.
- **Practical systems features.** `defer`, multi-file modules with `pub`/`priv` visibility, direct Windows DLL calls via `extern`, OS threads, locks, and hardware atomics.
- **Small, consistent syntax.** One loop keyword, one brace style, newline-terminated statements, and six precedence levels.

## Getting started

### Requirements

- **Windows x64** (native PE32+ backend + optional LLVM `-O3` backend) or **Linux x64** (LLVM x86-64 ELF backend + Windows PE32+ cross-compilation) — with **all 25 `std/` modules** supported on both platforms via `std/pal/`.
- A C11 compiler to build `sbc` — tested with **MinGW-w64 GCC 13** (Windows) and **GCC / Clang** (Linux).
- *Optional on Windows, required on Linux:* **LLVM/Clang** (`clang`). On Windows, `sbc` looks for `clang.exe` in `C:\Program Files\LLVM\bin`, `C:\msys64\clang64\bin`, `C:\msys64\mingw64\bin`, and on `PATH`.

### Build

**Windows:**
```powershell
.\build.bat
```

`build.bat` automatically finds `gcc` or `clang` on `PATH` (or common MinGW-w64/MSYS2 locations, or `%CC%`). You can also invoke `gcc` directly:

```powershell
gcc -std=c11 -O2 -Wall -Wextra src\util.c src\lexer.c src\parser.c src\check.c `
    src\x64.c src\codegen.c src\pe.c src\llvm_c.c src\fmt.c src\main.c -o sbc.exe
```

**Linux x64:**
```bash
make
./sbc --version
```

Keep `std/` next to `sbc.exe`: imports are resolved relative to the importing file first, then relative to the compiler's directory.

### Hello, world

```
fn main() {
    print("Hello, Antimony!")
}
```

```powershell
.\sbc.exe hello.sb --run
```

## Usage

```
sbc [build] <file.sb> [-o <out>] [--target windows-x64|linux-x64] [--release] [--llvm | -O3] [--emit-llvm] [--run]
sbc run <file.sb> [--release] [--llvm | -O3]
sbc test <file.sb> [--filter <pat> | -f <pat>] [--release] [--llvm | -O3]
sbc check [--lib] [--target windows-x64|linux-x64] <file.sb>
sbc fmt [-w | --write] [--check] [--stdin | <file.sb>...]
sbc --version | --help
```

| Command / Option | Effect |
|---|---|
| `sbc [build] <file.sb>` | Debug build (native PE32+ on Windows; LLVM ELF on Linux): overflow, narrowing, null, and bounds checks enabled. |
| `--target <os>` | Select target platform: `windows-x64` or `linux-x64` (supports cross-compiling Windows PE32+ `.exe` directly from Linux, or emitting Linux LLVM IR from Windows). |
| `sbc run <file.sb>` | Compile and execute immediately (equivalent to `--run` / `-r`). |
| `sbc test <file.sb>` | Discover and run all `fn test_*()` unit tests in a file (supports `--filter`, `--release`, `--llvm`). |
| `sbc check [--lib] [--target windows-x64|linux-x64] <file.sb>` | Fast type-check only (no codegen, no output file). Pass `--lib` for library modules without `fn main()`. |
| `sbc fmt <file.sb>...` | Canonical 4-space source formatter (lossless, comment-preserving). Use `-w` to format in place, `--check` for CI, or `--stdin` for editors. |
| `--release` | Release build: arithmetic wraps; bounds checks, division by zero, and `MIN / -1` still trap. |
| `--llvm`, `-O3` | Compile through LLVM (`clang -O3 -march=native`); implies `--release`. |
| `--emit-llvm` | Write textual LLVM IR (`.ll`) to the output path instead of an executable. |
| `-o <path>` | Output path (default: input name with `.exe`). |
| `--version`, `-V` | Print compiler version information. |

## Performance

The same four workloads were written in Antimony, C, Go, and Rust using identical algorithms (median of 5 runs, lower is better):

| | sb native | sb `--llvm` | C (gcc -O3) | Go | Rust |
|---|---:|---:|---:|---:|---:|
| **Total (ms)** | 402.7 | 114.2 | 173.6 | 196.2 | 115.8 |

With `--llvm`, Antimony performs on par with Rust. The native backend is about 3–4× slower at runtime in exchange for near-instant builds. Workload details, compiler flags, and a reproducible script are in [benchmarks/](benchmarks/README.md).

## Standard library

| Module | Contents |
|---|---|
| `std/core.sb` | `Option[T]`, `Result[T, E]`, `Ordering`, `panic`, `assert`, `swap[T]` |
| `std/mem.sb` | Heap allocation, `alloc_typed[T]`, memory utilities (`copy_mem`, `move_mem`, `cmp_mem`, `eq_mem`), bump-pointer `Arena` |
| `std/vec.sb` | Generic growable array `Vec[T]` (bounds-checked `get`/`set`/`pop`, `try_get`, `try_pop`, `insert`, `remove`, `swap_remove`, `extend_slice`) |
| `std/map.sb` | Generic open-addressing hash table `HashMap[K, V]` (`put`, `get`, `get_ptr`, `take`, `remove`, `reserve`) |
| `std/set.sb` | Generic hash set `HashSet[T]` |
| `std/deque.sb` | Generic ring-buffer double-ended queue `Deque[T]` |
| `std/heap.sb` | Generic priority queues `BinaryHeap[T]` (max-heap) and `MinHeap[T]` (min-heap) |
| `std/sort.sb` | Generic sorting (`sort`, `sort_desc`, `sort_by`), `binary_search`, `lower_bound`, `upper_bound`, `reverse` |
| `std/str.sb` | String search (`str_find`, `str_contains`, `str_count`, `str_split_once`), strict `Result` parsers (`try_parse_i64`/`u64`/`f64`), C-string interop, `StringBuilder` |
| `std/math.sb` | Constants, transcendental and rounding functions, integer and bit utilities |
| `std/io.sb` | `File` handle (`file_open`, `file_create`, `read`, `write_all`, `seek`, `flush`), `IoError`, `read_file_all`, `write_file_all` |
| `std/bufio.sb` | Buffered file I/O (`BufReader` with `read_line`/`read_byte`, `BufWriter` with batched `write`/`flush`) |
| `std/fs.sb` | Filesystem operations (`fs_exists`, `fs_is_file`, `fs_is_dir`, `fs_mkdir`, `fs_rmdir`, `fs_remove_file`, `fs_rename`, `fs_copy`, `fs_read_dir`) |
| `std/path.sb` | Path utilities (`path_basename`, `path_dirname`, `path_extension`, `path_stem`, `path_is_abs`, `path_join`) |
| `std/os.sb` | Console streams (`stdin`/`stdout`/`stderr`), command-line arguments, environment variables, `cwd_into` |
| `std/time.sb` | High-resolution timers, `Stopwatch`, `sleep_ms` |
| `std/rand.sb` | Deterministic PRNG (`Rng`, xoshiro256++), `rng_shuffle`, OS CSPRNG (`os_random_bytes` via `BCryptGenRandom` / `getrandom`) |
| `std/process.sb` | Child process spawning, `wait`/`try_wait`/`kill`, stdout/stderr pipe capture (`process_capture`), `current_pid` |
| `std/net.sb` | Cross-platform IPv4 TCP & UDP networking (`TcpListener`, `TcpStream`, `UdpSocket`, `Ipv4Addr`, `SocketAddr`, `dns_resolve`, `tcp_connect_host`, `NetError`) |
| `std/http.sb` | HTTP/1.1 client & server (`http_get`, `http_post`, `http_request`, `HttpRequest`, `HttpResponse`, `http_parse_url`, `http_decode_chunked`, `http_mime_type`) |
| `std/encoding.sb` | RFC 4648 Hexadecimal and Base64 (standard & URL-safe) encoding and decoding |
| `std/hash.sb` | FNV-1a 32/64, IEEE CRC32, and FIPS 180-4 SHA-256 (streaming & one-shot) |
| `std/json.sb` | RFC 8259 arena-backed JSON parser, DOM builder, and compact/pretty serializer (`JsonDoc`) |
| `std/testing.sb` | Unit-test assertions (`assert_eq`, `assert_ne`, `assert_approx`, `assert_ok`, `assert_some`, `assert_str_contains`) for `sbc test` |
| `std/thread.sb` | Threads, `Mutex`, `RwLock`, `CondVar`, `AtomicI64` |

## Documentation

- [Tutorial](docs/tutorial.md) — a guided tour of the language and standard library.
- [Language specification](docs/spec.md) — the normative definition of the language, including the grammar and decision log.
- [Examples](examples/) — [`demo.sb`](examples/demo.sb) (feature tour), [`sbwc.sb`](examples/sbwc.sb) (word-count & search CLI), and [`sbhttpd.sb`](examples/sbhttpd.sb) (multithreaded HTTP/1.1 static-file & JSON API server).
- [Changelog](CHANGELOG.md) & [Contributing](CONTRIBUTING.md) — release history and contributor guidelines.

## Testing

```powershell
# Self-hosted standard library unit tests (`sbc test`)
.\sbc.exe test tests\stdlib_test.sb
.\sbc.exe test tests\stdlib_test.sb --llvm

# Full compiler & backend suite (native x64 + LLVM -O3 + compile-error + runtime-trap)
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1

# Fast native-only run (~10 seconds) or filtered subset
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -Quick
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1 -Filter phase25
```

The suite compiles and runs test programs with both backends and checks their output, verifies that runtime errors (overflow, bounds, null) trap with the expected message, and verifies that invalid programs are rejected with the expected compile error. The `--llvm` tests require Clang.

## Project layout

```
src/          compiler sources (lexer, parser, checker, x64 and LLVM backends, PE writer)
std/          standard library modules
docs/         tutorial and language specification
examples/     sample programs
tests/        test runner and test programs
benchmarks/   cross-language benchmark suite
editors/      VS Code extension (highlighting, live diagnostics, sbc fmt, outline, hover, snippets)
```

## Status

The core language is complete and specified in [docs/spec.md](docs/spec.md). Both **Windows x64** (native PE32+ + LLVM `-O3`) and **Linux x64** (LLVM x86-64 System V ELF + POSIX FFI + Windows cross-compilation) have **100% parity across all 25 `std/` modules** (including threads, processes, sockets, HTTP/1.1, filesystem, and JSON) and are verified in CI. Planned work — including macOS support, an LSP server, and self-hosting — is tracked in [§13 of the specification](docs/spec.md#13-open-items).

## License

Released under the [MIT License](LICENSE).
