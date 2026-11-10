// benchmarks/bench.c - C port of benchmarks/bench.sb (same algorithms, same inputs).
// Output: for each workload, one line with elapsed microseconds, one line with the checksum.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static int64_t perf_counter(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)c.QuadPart;
}

static int64_t elapsed_us(int64_t start, int64_t end) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return ((end - start) * 1000000LL) / (int64_t)f.QuadPart;
}

static inline int64_t clamp_i64(int64_t x, int64_t lo, int64_t hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static inline int64_t mix_step(int64_t x, int64_t i) {
    int64_t v = clamp_i64(i & 1023, 10, 900);
    return (x + v * 16) - (i / 8) + (i % 64);
}

__attribute__((noinline)) int64_t bench_inline_and_strength(int64_t iters) {
    int64_t acc = 0;
    for (int64_t i = 0; i < iters; i++) acc = mix_step(acc, i);
    return acc;
}

static int32_t mandelbrot_point(double cr, double ci, int32_t max_iter) {
    double zr = 0.0, zi = 0.0;
    int32_t it = 0;
    while (it < max_iter) {
        double zr2 = zr * zr, zi2 = zi * zi;
        if (zr2 + zi2 > 4.0) return it;
        zi = 2.0 * zr * zi + ci;
        zr = zr2 - zi2 + cr;
        it += 1;
    }
    return max_iter;
}

__attribute__((noinline)) int64_t bench_mandelbrot(int32_t size, int32_t max_iter) {
    int64_t total = 0;
    double inv = 1.0 / (double)size;
    for (int32_t y = 0; y < size; y++) {
        double ci = ((double)y * inv) * 2.0 - 1.0;
        for (int32_t x = 0; x < size; x++) {
            double cr = ((double)x * inv) * 3.5 - 2.5;
            total += (int64_t)mandelbrot_point(cr, ci, max_iter);
        }
    }
    return total;
}

__attribute__((noinline)) int64_t sum_slice(const int64_t *s, int64_t len, int32_t rounds) {
    int64_t total = 0;
    for (int32_t r = 0; r < rounds; r++)
        for (int64_t i = 0; i < len; i++) total += s[i] * 4 + (int64_t)(r & 7);
    return total;
}

static void sort_i64(int64_t *s, int64_t n) {
    if (n <= 1) return;
    int64_t stack_lo[64], stack_hi[64];
    int sp = 1;
    stack_lo[0] = 0;
    stack_hi[0] = n - 1;
    while (sp > 0) {
        sp -= 1;
        int64_t lo = stack_lo[sp], hi = stack_hi[sp];
        while (hi - lo >= 16) {
            int64_t mid = lo + ((hi - lo) >> 1), t;
            if (s[mid] < s[lo]) { t = s[lo]; s[lo] = s[mid]; s[mid] = t; }
            if (s[hi] < s[lo])  { t = s[lo]; s[lo] = s[hi];  s[hi] = t; }
            if (s[hi] < s[mid]) { t = s[mid]; s[mid] = s[hi]; s[hi] = t; }
            int64_t pivot = s[mid], i = lo, j = hi;
            for (;;) {
                while (s[i] < pivot) i += 1;
                while (pivot < s[j]) j -= 1;
                if (i >= j) break;
                t = s[i]; s[i] = s[j]; s[j] = t;
                i += 1;
                j -= 1;
            }
            if ((j - lo) < (hi - i)) {
                if (i < hi) { stack_lo[sp] = i; stack_hi[sp] = hi; sp += 1; }
                hi = j;
            } else {
                if (lo < j) { stack_lo[sp] = lo; stack_hi[sp] = j; sp += 1; }
                lo = i;
            }
        }
        for (int64_t i = lo + 1; i <= hi; i++) {
            int64_t key = s[i], j = i - 1;
            while (j >= lo && key < s[j]) { s[j + 1] = s[j]; j -= 1; }
            s[j + 1] = key;
        }
    }
}

__attribute__((noinline)) int64_t bench_quicksort(int64_t n) {
    int64_t *v = (int64_t *)malloc((size_t)n * sizeof(int64_t));
    uint64_t state = 0x123456789ABCDEF0ULL;
    for (int64_t i = 0; i < n; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        v[i] = (int64_t)(state >> 16);
    }
    sort_i64(v, n);
    int64_t res = v[0] ^ v[n / 2] ^ v[n - 1];
    free(v);
    return res;
}

int main(void) {
    int64_t t0 = perf_counter();
    int64_t r1 = bench_inline_and_strength(50000000LL);
    int64_t t1 = perf_counter();
    printf("%lld\n%lld\n", (long long)elapsed_us(t0, t1), (long long)r1);

    t0 = perf_counter();
    int64_t r2 = bench_mandelbrot(1000, 100);
    t1 = perf_counter();
    printf("%lld\n%lld\n", (long long)elapsed_us(t0, t1), (long long)r2);

    int64_t *buf = (int64_t *)malloc(1024 * sizeof(int64_t));
    for (int i = 0; i < 1024; i++) buf[i] = (int64_t)(i * 3 + 1);
    t0 = perf_counter();
    int64_t r3 = sum_slice(buf, 1024, 50000);
    t1 = perf_counter();
    free(buf);
    printf("%lld\n%lld\n", (long long)elapsed_us(t0, t1), (long long)r3);

    t0 = perf_counter();
    int64_t r4 = bench_quicksort(200000LL);
    t1 = perf_counter();
    printf("%lld\n%lld\n", (long long)elapsed_us(t0, t1), (long long)r4);
    return 0;
}
