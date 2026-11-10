// benchmarks/bench.go - Go port of benchmarks/bench.sb (same algorithms, same inputs).
// Output: for each workload, one line with elapsed microseconds, one line with the checksum.
package main

import (
	"fmt"
	"time"
)

func clampI64(x, lo, hi int64) int64 {
	if x < lo {
		return lo
	}
	if x > hi {
		return hi
	}
	return x
}

func mixStep(x, i int64) int64 {
	v := clampI64(i&1023, 10, 900)
	return (x + v*16) - (i / 8) + (i % 64)
}

//go:noinline
func benchInlineAndStrength(iters int64) int64 {
	var acc int64
	for i := int64(0); i < iters; i++ {
		acc = mixStep(acc, i)
	}
	return acc
}

func mandelbrotPoint(cr, ci float64, maxIter int32) int32 {
	zr, zi := 0.0, 0.0
	var it int32
	for it < maxIter {
		zr2, zi2 := zr*zr, zi*zi
		if zr2+zi2 > 4.0 {
			return it
		}
		zi = 2.0*zr*zi + ci
		zr = zr2 - zi2 + cr
		it++
	}
	return maxIter
}

//go:noinline
func benchMandelbrot(size, maxIter int32) int64 {
	var total int64
	inv := 1.0 / float64(size)
	for y := int32(0); y < size; y++ {
		ci := (float64(y)*inv)*2.0 - 1.0
		for x := int32(0); x < size; x++ {
			cr := (float64(x)*inv)*3.5 - 2.5
			total += int64(mandelbrotPoint(cr, ci, maxIter))
		}
	}
	return total
}

//go:noinline
func sumSlice(s []int64, rounds int32) int64 {
	var total int64
	for r := int32(0); r < rounds; r++ {
		for i := 0; i < len(s); i++ {
			total += s[i]*4 + int64(r&7)
		}
	}
	return total
}

func sortI64(s []int64) {
	n := int64(len(s))
	if n <= 1 {
		return
	}
	var stackLo, stackHi [64]int64
	sp := 1
	stackHi[0] = n - 1
	for sp > 0 {
		sp--
		lo, hi := stackLo[sp], stackHi[sp]
		for hi-lo >= 16 {
			mid := lo + ((hi - lo) >> 1)
			if s[mid] < s[lo] {
				s[lo], s[mid] = s[mid], s[lo]
			}
			if s[hi] < s[lo] {
				s[lo], s[hi] = s[hi], s[lo]
			}
			if s[hi] < s[mid] {
				s[mid], s[hi] = s[hi], s[mid]
			}
			pivot, i, j := s[mid], lo, hi
			for {
				for s[i] < pivot {
					i++
				}
				for pivot < s[j] {
					j--
				}
				if i >= j {
					break
				}
				s[i], s[j] = s[j], s[i]
				i++
				j--
			}
			if (j - lo) < (hi - i) {
				if i < hi {
					stackLo[sp], stackHi[sp] = i, hi
					sp++
				}
				hi = j
			} else {
				if lo < j {
					stackLo[sp], stackHi[sp] = lo, j
					sp++
				}
				lo = i
			}
		}
		for i := lo + 1; i <= hi; i++ {
			key, j := s[i], i-1
			for j >= lo && key < s[j] {
				s[j+1] = s[j]
				j--
			}
			s[j+1] = key
		}
	}
}

//go:noinline
func benchQuicksort(n int64) int64 {
	v := make([]int64, n)
	var state uint64 = 0x123456789ABCDEF0
	for i := int64(0); i < n; i++ {
		state = state*6364136223846793005 + 1442695040888963407
		v[i] = int64(state >> 16)
	}
	sortI64(v)
	return v[0] ^ v[n/2] ^ v[n-1]
}

func main() {
	t := time.Now()
	r1 := benchInlineAndStrength(50_000_000)
	fmt.Printf("%d\n%d\n", time.Since(t).Microseconds(), r1)

	t = time.Now()
	r2 := benchMandelbrot(1000, 100)
	fmt.Printf("%d\n%d\n", time.Since(t).Microseconds(), r2)

	buf := make([]int64, 1024)
	for i := range buf {
		buf[i] = int64(i*3 + 1)
	}
	t = time.Now()
	r3 := sumSlice(buf, 50_000)
	fmt.Printf("%d\n%d\n", time.Since(t).Microseconds(), r3)

	t = time.Now()
	r4 := benchQuicksort(200_000)
	fmt.Printf("%d\n%d\n", time.Since(t).Microseconds(), r4)
}
