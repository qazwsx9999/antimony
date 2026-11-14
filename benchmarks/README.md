# Benchmarks

The same four workloads are written in Antimony ([bench.sb](bench.sb)), C ([bench.c](bench.c)), Go ([bench.go](bench.go)), and Rust ([bench.rs](bench.rs)), using the same algorithms and inputs.

```powershell
powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1 -Runs 7
```

The script builds every available target, runs each one `-Runs` times, checks that all checksums agree, and prints the **median** runtime per workload. It compares runtime only; compile times depend too much on caches to compare fairly.

## Compiler flags

| Target | Command |
|---|---|
| Antimony native | `sbc bench.sb --release` |
| Antimony optimizing | `sbc bench.sb --llvm --release` (translates to C, then `clang`/`gcc -O3 -march=native`) |
| C | `gcc -O3 -march=native` |
| Go | `go build` |
| Rust | `rustc -C opt-level=3 -C target-cpu=native` |

## Sample result

Windows x64, Antimony `--llvm` via **LLVM Clang 23.1.5** (`-O3 -march=native`), C via `gcc 13.1 -O3 -march=native`, Go (`go build`), and Rust (`rustc -C opt-level=3 -C target-cpu=native`), median of 7 runs, in ms (lower is better). Expect a variation of about ±20% between sessions on the same machine.

| Workload | sb native | sb `--llvm` | C | Go | Rust |
|---|---:|---:|---:|---:|---:|
| 1. Inlining + strength reduction (50M iters) | 188.1 | 11.8 | 70.7 | 78.7 | 15.7 |
| 2. Float Mandelbrot (1000×1000, 100 iters) | 167.6 | 79.3 | 85.0 | 100.1 | 97.6 |
| 3. Slice sum in range loop (51.2M loads) | 58.5 | 3.8 | 7.9 | 34.2 | 4.6 |
| 4. Quicksort (200,000 i64) | 18.0 | 12.2 | 12.6 | 16.6 | 17.2 |
| **Total** | **432.2** | **107.1** | **176.3** | **229.6** | **135.1** |

What the numbers show:
- **With LLVM Clang installed, Antimony `--llvm` matches or edges out Rust and C across all four workloads (`107.1 ms` total vs `135.1 ms` Rust and `176.3 ms` C).** Note that C here was compiled with GCC 13.1 rather than Clang.
- **On Workload 1 and Workload 3, LLVM (`sb --llvm` and Rust) vectorizes/reduces the loops tightly**, finishing well ahead of GCC and Go.
- **Workload 1 is an outlier for GCC (`bench.c` at 70.7 ms vs Clang/LLVM at 11.8–15.7 ms).**
- **The native backend trades runtime for compile time.** It runs about 3–4× slower than `--llvm` overall, but compiles in milliseconds with no external tools.
