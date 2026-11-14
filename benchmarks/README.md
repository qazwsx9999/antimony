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
| Antimony optimizing | `sbc bench.sb --llvm --release` (emits LLVM IR `.ll` directly, then `clang -O3 -march=native`) |
| C | `gcc -O3 -march=native` |
| Go | `go build` |
| Rust | `rustc -C opt-level=3 -C target-cpu=native` |

## Sample result

Windows x64, Antimony `--llvm` generating LLVM IR directly for **LLVM Clang 23.1.5** (`-O3 -march=native`), C via `gcc 13.1 -O3 -march=native`, Go (`go build`), and Rust (`rustc -C opt-level=3 -C target-cpu=native`), median of 5 runs, in ms (lower is better). Expect a variation of about ±20% between sessions on the same machine.

| Workload | sb native | sb `--llvm` | C | Go | Rust |
|---|---:|---:|---:|---:|---:|
| 1. Inlining + strength reduction (50M iters) | 172.5 | 13.7 | 68.3 | 69.4 | 14.3 |
| 2. Float Mandelbrot (1000×1000, 100 iters) | 158.4 | 85.6 | 84.8 | 86.1 | 84.4 |
| 3. Slice sum in range loop (51.2M loads) | 55.2 | 3.5 | 8.4 | 28.1 | 3.5 |
| 4. Quicksort (200,000 i64) | 16.6 | 11.5 | 12.1 | 12.6 | 13.5 |
| **Total** | **402.7** | **114.2** | **173.6** | **196.2** | **115.8** |

What the numbers show:
- **Direct LLVM IR emission gives Antimony `--llvm` parity with (and on this run a slight edge over) Rust (`114.2 ms` total vs `115.8 ms` Rust, `173.6 ms` C, and `196.2 ms` Go).**
- **On Workload 1 and Workload 3, LLVM (`sb --llvm` and Rust) vectorizes/reduces the loops tightly**, finishing well ahead of GCC and Go.
- **Workload 1 is an outlier for GCC (`bench.c` at 68.3 ms vs Clang/LLVM at 13.7–14.3 ms).**
- **The native backend trades runtime for compile time.** It runs about 3–4× slower than `--llvm` overall, but compiles in ~3.6 ms with zero external dependencies.
