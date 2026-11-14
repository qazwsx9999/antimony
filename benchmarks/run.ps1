# benchmarks/run.ps1 - Reproducible cross-language runtime benchmark.
#
# Builds benchmarks/bench.{sb,c,go,rs} with each available compiler, runs every
# binary -Runs times, verifies that all checksums agree, and reports the MEDIAN
# runtime per workload in milliseconds.
#
# Usage:  powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1 [-Runs 5]
#
# Notes:
#   * Only runtime is compared. Compile times are not reported here because they
#     depend heavily on caches (e.g. Go's build cache) and are easy to misread.
#   * Toolchains that are not installed are skipped.
#   * `sbc --llvm` emits LLVM IR (.ll) directly from the Antimony AST and compiles it
#     via LLVM `clang -O3 -march=native`. C and Rust get matching native-CPU flags
#     so that comparison is fair. Go has no equivalent flag.
param([int]$Runs = 5)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$bench = $PSScriptRoot
$out   = Join-Path $bench "out"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$sbc = Join-Path $root "sbc.exe"
$gcc = "C:\Qt\Tools\mingw1310_64\bin\gcc.exe"
if (!(Test-Path $gcc)) { $c = Get-Command gcc -ErrorAction SilentlyContinue; $gcc = if ($c) { $c.Source } else { $null } }
$go  = (Get-Command go    -ErrorAction SilentlyContinue).Source
$rs  = (Get-Command rustc -ErrorAction SilentlyContinue).Source

$workloads = @(
    "1. Inlining + strength reduction (50M iters)",
    "2. Float Mandelbrot (1000x1000, 100 iters)",
    "3. Slice sum in range loop (51.2M loads)",
    "4. Quicksort (200,000 i64)"
)

# name -> build script block producing the exe
$targets = [ordered]@{}
$targets["Antimony native (--release)"] = { & $sbc "$bench\bench.sb" --release -o "$out\sb_native.exe" | Out-Null; "$out\sb_native.exe" }
$targets["Antimony --llvm --release"]   = { & $sbc "$bench\bench.sb" --llvm --release -o "$out\sb_llvm.exe" | Out-Null; "$out\sb_llvm.exe" }
if ($gcc) { $targets["C (gcc -O3 -march=native)"] = { & $gcc -O3 -march=native -std=c11 "$bench\bench.c" -o "$out\c_o3.exe"; "$out\c_o3.exe" } }
if ($go)  { $targets["Go (go build)"]           = { & $go build -o "$out\go.exe" "$bench\bench.go"; "$out\go.exe" } }
if ($rs)  { $targets["Rust (rustc -C opt-level=3 -C target-cpu=native)"] = { & $rs -C opt-level=3 -C target-cpu=native "$bench\bench.rs" -o "$out\rs.exe" 2>&1 | Out-Null; "$out\rs.exe" } }

function Get-Median([double[]]$xs) {
    $s = $xs | Sort-Object
    $n = $s.Count
    if ($n % 2 -eq 1) { return $s[($n - 1) / 2] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}

$results   = [ordered]@{}   # target -> double[4] medians (ms)
$checksums = [ordered]@{}   # target -> string[4]

foreach ($name in $targets.Keys) {
    Write-Host "Building $name ..."
    $exe = & $targets[$name]
    if ($LASTEXITCODE -ne 0 -or !(Test-Path $exe)) { Write-Host "  build failed, skipping"; continue }

    $times = @(@(), @(), @(), @())
    $ck = $null
    for ($r = 0; $r -lt $Runs; $r++) {
        # Keep only pure integer lines: alternating (microseconds, checksum) per workload.
        $nums = @(& $exe | Where-Object { $_ -match '^\s*-?\d+\s*$' } | ForEach-Object { $_.Trim() })
        if ($nums.Count -ne 8) { throw "$name produced $($nums.Count) numeric lines, expected 8" }
        for ($w = 0; $w -lt 4; $w++) { $times[$w] += [double]$nums[2 * $w] / 1000.0 }
        $runCk = @($nums[1], $nums[3], $nums[5], $nums[7])
        if ($null -eq $ck) { $ck = $runCk }
    }
    $results[$name]   = @(0..3 | ForEach-Object { Get-Median $times[$_] })
    $checksums[$name] = $ck
}

# Verify every target computed the same answers.
$ref = $null
$allMatch = $true
foreach ($name in $checksums.Keys) {
    if ($null -eq $ref) { $ref = $checksums[$name]; continue }
    for ($w = 0; $w -lt 4; $w++) {
        if ($checksums[$name][$w] -ne $ref[$w]) {
            Write-Host "CHECKSUM MISMATCH: $name workload $($w + 1): $($checksums[$name][$w]) vs $($ref[$w])" -ForegroundColor Red
            $allMatch = $false
        }
    }
}

Write-Host ""
Write-Host "Median runtime in ms over $Runs runs (lower is better)"
$header = "{0,-46}" -f "Workload"
foreach ($name in $results.Keys) { $header += " | {0,12}" -f ($name -replace ' \(.*\)', '' -replace 'Antimony ', 'sb ') }
Write-Host $header
for ($w = 0; $w -lt 4; $w++) {
    $line = "{0,-46}" -f $workloads[$w]
    foreach ($name in $results.Keys) { $line += " | {0,12:F1}" -f $results[$name][$w] }
    Write-Host $line
}
$line = "{0,-46}" -f "Total"
foreach ($name in $results.Keys) { $line += " | {0,12:F1}" -f (($results[$name] | Measure-Object -Sum).Sum) }
Write-Host $line
Write-Host ""
Write-Host ("Columns: " + (($results.Keys) -join ", "))
if ($allMatch) { Write-Host "All checksums match." } else { exit 1 }
