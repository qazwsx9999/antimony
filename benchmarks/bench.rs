// benchmarks/bench.rs - Rust port of benchmarks/bench.sb (same algorithms, same inputs).
// Output: for each workload, one line with elapsed microseconds, one line with the checksum.
use std::time::Instant;

#[inline(always)]
fn clamp_i64(x: i64, lo: i64, hi: i64) -> i64 {
    if x < lo {
        return lo;
    }
    if x > hi {
        return hi;
    }
    x
}

#[inline(always)]
fn mix_step(x: i64, i: i64) -> i64 {
    let v = clamp_i64(i & 1023, 10, 900);
    x.wrapping_add(v.wrapping_mul(16))
        .wrapping_sub(i / 8)
        .wrapping_add(i % 64)
}

#[inline(never)]
fn bench_inline_and_strength(iters: i64) -> i64 {
    let mut acc: i64 = 0;
    for i in 0..iters {
        acc = mix_step(acc, i);
    }
    acc
}

fn mandelbrot_point(cr: f64, ci: f64, max_iter: i32) -> i32 {
    let (mut zr, mut zi) = (0.0f64, 0.0f64);
    let mut it = 0i32;
    while it < max_iter {
        let (zr2, zi2) = (zr * zr, zi * zi);
        if zr2 + zi2 > 4.0 {
            return it;
        }
        zi = 2.0 * zr * zi + ci;
        zr = zr2 - zi2 + cr;
        it += 1;
    }
    max_iter
}

#[inline(never)]
fn bench_mandelbrot(size: i32, max_iter: i32) -> i64 {
    let mut total: i64 = 0;
    let inv = 1.0 / (size as f64);
    for y in 0..size {
        let ci = ((y as f64) * inv) * 2.0 - 1.0;
        for x in 0..size {
            let cr = ((x as f64) * inv) * 3.5 - 2.5;
            total += mandelbrot_point(cr, ci, max_iter) as i64;
        }
    }
    total
}

#[inline(never)]
fn sum_slice(s: &[i64], rounds: i32) -> i64 {
    let mut total: i64 = 0;
    for r in 0..rounds {
        for i in 0..s.len() {
            total = total.wrapping_add(s[i].wrapping_mul(4).wrapping_add((r & 7) as i64));
        }
    }
    total
}

fn sort_i64(s: &mut [i64]) {
    let n = s.len() as i64;
    if n <= 1 {
        return;
    }
    let mut stack_lo = [0i64; 64];
    let mut stack_hi = [0i64; 64];
    let mut sp = 1usize;
    stack_hi[0] = n - 1;
    while sp > 0 {
        sp -= 1;
        let (mut lo, mut hi) = (stack_lo[sp], stack_hi[sp]);
        while hi - lo >= 16 {
            let mid = lo + ((hi - lo) >> 1);
            if s[mid as usize] < s[lo as usize] {
                s.swap(lo as usize, mid as usize);
            }
            if s[hi as usize] < s[lo as usize] {
                s.swap(lo as usize, hi as usize);
            }
            if s[hi as usize] < s[mid as usize] {
                s.swap(mid as usize, hi as usize);
            }
            let pivot = s[mid as usize];
            let (mut i, mut j) = (lo, hi);
            loop {
                while s[i as usize] < pivot {
                    i += 1;
                }
                while pivot < s[j as usize] {
                    j -= 1;
                }
                if i >= j {
                    break;
                }
                s.swap(i as usize, j as usize);
                i += 1;
                j -= 1;
            }
            if (j - lo) < (hi - i) {
                if i < hi {
                    stack_lo[sp] = i;
                    stack_hi[sp] = hi;
                    sp += 1;
                }
                hi = j;
            } else {
                if lo < j {
                    stack_lo[sp] = lo;
                    stack_hi[sp] = j;
                    sp += 1;
                }
                lo = i;
            }
        }
        let mut i = lo + 1;
        while i <= hi {
            let key = s[i as usize];
            let mut j = i - 1;
            while j >= lo && key < s[j as usize] {
                s[(j + 1) as usize] = s[j as usize];
                j -= 1;
            }
            s[(j + 1) as usize] = key;
            i += 1;
        }
    }
}

#[inline(never)]
fn bench_quicksort(n: i64) -> i64 {
    let mut v = Vec::with_capacity(n as usize);
    let mut state: u64 = 0x1234_5678_9ABC_DEF0;
    for _ in 0..n {
        state = state
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        v.push((state >> 16) as i64);
    }
    sort_i64(&mut v);
    v[0] ^ v[(n / 2) as usize] ^ v[(n - 1) as usize]
}

fn main() {
    let t = Instant::now();
    let r1 = bench_inline_and_strength(50_000_000);
    println!("{}\n{}", t.elapsed().as_micros(), r1);

    let t = Instant::now();
    let r2 = bench_mandelbrot(1000, 100);
    println!("{}\n{}", t.elapsed().as_micros(), r2);

    let buf: Vec<i64> = (0..1024).map(|i| (i * 3 + 1) as i64).collect();
    let t = Instant::now();
    let r3 = sum_slice(&buf, 50_000);
    println!("{}\n{}", t.elapsed().as_micros(), r3);

    let t = Instant::now();
    let r4 = bench_quicksort(200_000);
    println!("{}\n{}", t.elapsed().as_micros(), r4);
}
