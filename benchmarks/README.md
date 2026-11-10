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

Windows x64, gcc 13.1 (no clang installed), median of 7 runs, in ms (lower is better). Expect a variation of about ±20% between sessions on the same machine.

| Workload | sb native | sb `--llvm` | C | Go | Rust |
|---|---:|---:|---:|---:|---:|
| 1. Inlining + strength reduction (50M iters) | 211.7 | 21.9 | 85.6 | 91.2 | 16.4 |
| 2. Float Mandelbrot (1000×1000, 100 iters) | 172.6 | 99.1 | 98.6 | 107.2 | 100.7 |
| 3. Slice sum in range loop (51.2M loads) | 69.4 | 9.2 | 7.5 | 33.1 | 4.1 |
| 4. Quicksort (200,000 i64) | 20.5 | 16.6 | 13.4 | 16.3 | 15.3 |
| **Total** | **474.2** | **146.8** | **205.1** | **247.8** | **136.5** |

What the numbers show:
- **The optimizing backend is in the same range as C and Rust.** It is fastest on no workload, but within about 1.2× of the best on workloads 2 and 4.
- **Rust won workloads 1 and 3.** The other workloads were close across all compiled targets.
- **Workload 1 is an outlier for C.** Hand-written C (85.6 ms) is about 4× slower than Antimony's generated C (21.9 ms), even though both use gcc. The cause has not been investigated.
- **The native backend trades runtime for compile time.** It runs about 2–4× slower than the optimizing targets, but it compiles in milliseconds with no external tools.
