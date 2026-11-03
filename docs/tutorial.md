# Antimony (`.sb`) Language Tutorial

Welcome to **Antimony**, a fast, statically-typed compiled language that compiles directly to standalone Windows x64 executables in ~1 millisecond.

---

## 1. Quick Start: Hello, World!

Create a file named `hello.sb`:

```
// hello.sb — every program starts at fn main()
fn main() {
    print("Hello, Antimony!")
}
```

Compile and run it with `sbc`:

```powershell
# Compile and run immediately
.\sbc.exe hello.sb --run

# Compile to hello.exe (debug mode: overflow checks enabled)
.\sbc.exe hello.sb -o hello.exe

# Compile in release mode (wrapping arithmetic, smaller/faster binary)
.\sbc.exe hello.sb --release -o hello.exe
```

### Core Syntax Rules
- **No semicolons at the end of lines:** A newline ends a statement. Writing `;` at the end of a normal statement is a compile error.
- **Line continuation:** If a line ends with an operator (`+`, `-`, `*`, `,`, `(`), the statement continues onto the next line.
- **Comments:** Use `//` for single-line comments.
- **One brace style:** Opening `{` must be on the same line as `fn`, `if`, `else`, or `loop`.

---

## 2. Variables (`var`) and Constants (`const`)

Antimony uses `var` for all variables. There are three ways to declare a variable:

```
fn main() {
    // 1. Type inferred from the initial value (defaults to i32 for integers)
    var a = 10

    // 2. Explicit type with an initial value
    var b: i64 = 3_000_000_000

    // 3. Explicit type without an initial value
    var c: i32
    c = a + 5       // Must be assigned on every path before it is read!

    print(a)
    print(b)
    print(c)
}
```

### Definite Assignment (No Uninitialized Reads)
If you declare `var x: i32` without a value, the compiler verifies that `x` is assigned on **every possible branch** before you read it:

```
var x: i32
if cond {
    x = 1
}
// print(x)  // <-- COMPILE ERROR: 'x' is not assigned when 'cond' is false
```

### Compile-Time Constants (`const`)
Use `const` for values known at compile time. Untyped constants adapt automatically to whatever integer type they are used with:

```
const LIMIT = 100
const MASK: u32 = 0xFFFF_FFFF

fn main() {
    var small: u8 = LIMIT      // OK: 100 fits in u8
    var large: i64 = LIMIT     // OK: same constant works as i64
    print(small)
    print(large)
}
```

### Updating Variables
Use `=` or compound assignment operators (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`):

```
var x = 10
x += 5      // 15
x *= 2      // 30
```
*(Note: There is no `++` or `--` in Antimony—there is only one way to increment a variable: `x += 1`.)*

---

## 3. Types and Literals

### Built-in Types
| Category | Types | Description |
|---|---|---|
| Signed integers | `i8`, `i16`, `i32`, `i64`, `isize` | Two's-complement signed integers (`isize` is 64-bit pointer size) |
| Unsigned integers | `u8`, `u16`, `u32`, `u64`, `usize` | Unsigned integers (`usize` is 64-bit pointer size) |
| Floating-point | `f32`, `f64` | IEEE-754 single (`f32`) and double (`f64`) precision floats |
| Booleans | `bool` | `true` or `false` |
| Strings | `str` | Immutable UTF-8 string literals, e.g. `"hello\n"` |

### Integer Literals
- **Bases:** Decimal (`42`), Hexadecimal (`0xFF`), Binary (`0b1010`), Octal (`0o755`).
- **Digit separators:** Use `_` anywhere between digits for readability: `1_000_000`, `0xDEAD_BEEF`.
- **Contextual typing:** Unsuffixed integer literals take their type from context, or default to `i32` when no type is specified. If a literal does not fit in its target type, the compiler reports an error:

```
var a = 42                      // i32 (default)
var b: i64 = 3_000_000_000      // OK: context is i64
// var c = 3_000_000_000        // COMPILE ERROR: exceeds i32
// var d: u8 = 300              // COMPILE ERROR: 300 does not fit in u8
```

### Floating-Point Literals (`f32`, `f64`)
- Unsuffixed floating-point literals (e.g. `3.14159`, `2.5`) adapt to `f32` or `f64` from context, or default to `f64` when unconstrained.
- Floating-point operations (`+`, `-`, `*`, `/`, `<`, `<=`, `>`, `>=`, `==`, `!=`) compile directly to native x86-64 SSE2 instructions.
- Use explicit conversions `f64(x)`, `f32(x)`, `i32(f)`, `i64(f)` to convert between integers and floats:

```
const PI = 3.141592653589793

fn circle_area(r: f64): f64 {
    return PI * r * r
}

fn main() {
    var r32: f32 = 2.5
    var area: f64 = circle_area(f64(r32))
    print(area)                 // 19.634954
    var trunc: i32 = i32(area)  // 19
    print(trunc)
}
```

### Explicit Type Conversions
Antimony never converts between numeric types implicitly. Use `Type(value)` to convert:

```
var x: i32 = 100
var y: i64 = i64(x) + 50
var z: u8 = u8(x)               // Checked at runtime in debug mode!
```

---

## 4. Operators and Overflow Safety

### Arithmetic, Bitwise, and Logical Operators
Precedence has only **6 levels** (highest to lowest), so bitwise operators bind tighter than comparisons (unlike C):

1. **Unary:** `-` `!` `~`
2. **Multiplicative & Shift:** `*` `/` `%` `*%` `<<` `>>` `&`
3. **Additive & Bitwise OR/XOR:** `+` `-` `+%` `-%` `|` `^`
4. **Comparison:** `==` `!=` `<` `<=` `>` `>=` *(cannot be chained)*
5. **Logical AND:** `&&` *(short-circuiting)*
6. **Logical OR:** `||` *(short-circuiting)*

Because `&` has higher precedence than `==`, `if flags & MASK == 0` works as expected without extra parentheses.

### Integer Overflow Rules
1. **Constant expressions:** Overflow is **always a compile error**:
   ```
   // var x = 2_000_000_000 * 2   // COMPILE ERROR: overflows i32
   ```
2. **Runtime in `debug` mode (default):** `+`, `-`, `*`, unary `-`, and narrowing conversions `T(x)` **trap immediately** with the exact file, line, and column if they overflow.
3. **Runtime in `--release` mode:** Arithmetic wraps using two's complement, while division by zero and `MIN / -1` still trap safely.
4. **Intentional Wrapping Operators (`+%`, `-%`, `*%`, `+%=`, `-%=`, `*%=`):** When writing hashes, RNGs, or checksums where two's-complement wrap-around is desired in **all** build modes, use the `%` suffixed operators:
   ```
   var h: u32 = 2166136261
   h = (h ^ 42) *% 16777619      // Wraps in both debug and release builds
   ```

---

## 5. Functions (`fn`)

Declare functions with `fn`. Every parameter has an explicit type.
- If the function returns a value, write `: ReturnType` before `{`.
- If the function returns nothing, omit `: ReturnType`.

```
fn add(a: i32, b: i32): i32 {
    return a + b
}

fn greet(times: i32) {
    loop i in 0..times {
        print("Hi!")
    }
}

fn main() {
    greet(2)
    print(add(10, 20))
}
```

Rules for functions:
- Functions can call other functions declared later in the file, and can be recursive.
- Parameters are **read-only** inside the function body.
- Every control-flow path in a value-returning function must end with `return expr`.

---

## 6. Conditionals (`if` / `else`)

Parentheses around the condition are not required, and the condition must be a `bool` (integers are never implicitly treated as booleans):

```
fn classify(age: i32) {
    if age >= 18 {
        print("Adult")
    } else if age >= 13 {
        print("Teen")
    } else {
        print("Child")
    }
}
```

---

## 7. Loops (`loop`)

Antimony has a single loop keyword—`loop`—with four clean forms:

### 7.1 Range Loop (Recommended for Counting)
`a..b` iterates from `a` up to **but excluding** `b`. The loop variable `i` is read-only inside the body, and its bounds are evaluated only once before the loop starts:

```
// 0, 1, 2, 3, 4
loop i in 0..5 {
    print(i)
}

// Custom step with 'by': 0, 2, 4, 6, 8
loop i in 0..10 by 2 {
    print(i)
}

// Reverse order with 'rev(...)': 4, 3, 2, 1, 0
loop i in rev(0..5) {
    print(i)
}

// Reverse with step: 8, 6, 4, 2, 0
loop i in rev(0..10 by 2) {
    print(i)
}
```

### 7.2 Conditional Loop (While-Style)
Repeats as long as the boolean condition is `true`:

```
var n = 1
loop n < 100 {
    n *= 2
}
print(n)    // 128
```

### 7.3 C-Style Loop (For Non-Linear Stepping)
Requires all three parts (`var` init, condition, and assignment step) separated by `;`. This is the **only** place in Antimony where `;` is allowed:

```
loop var i = 1; i < 1024; i *= 2 {
    print(i)
}
```
*(Tip: If you write a plain counting C-style loop like `loop var i = 0; i < n; i += 1`, the compiler will print a helpful hint suggesting `loop i in 0..n`.)*

### 7.4 Infinite Loop
Omit the header to loop forever until `break` or `return`:

```
var tries = 0
loop {
    tries += 1
    if tries == 3 {
        break
    }
}
```

### 7.5 `break`, `continue`, and Loop Labels
Use `break` to exit a loop and `continue` to skip to the next iteration. For nested loops, prefix the outer loop with `label:` and pass the label to `break` or `continue`:

```
outer: loop y in 0..10 {
    loop x in 0..10 {
        if x * y == 24 {
            print(x)
            print(y)
            break outer     // Exits both loops immediately
        }
    }
}
```

---

## 8. Arrays, Slices, and Collection Loops

### 8.1 Fixed-Size Arrays
Write `[N]` after the variable name, where `N` is a compile-time constant or integer literal. If you provide fewer elements than `N`, the remaining elements are automatically zero-filled:

```
const N = 5

fn main() {
    var arr[N]: i32 = [10, 20, 30]   // [10, 20, 30, 0, 0]
    var zeros[100]: i32 = []         // all 100 elements set to 0
    var auto = [1, 2, 3]             // inferred as auto[3]: i32

    arr[3] = 40
    arr[4] += 5
    print(arr[0])                    // 10
    print(len(arr))                  // 5
}
```

### 8.2 Slices (`a[lo..hi]`) and Slice Parameters
A slice is a lightweight view (`{ptr, len}`) into an array. Declare slice variables or parameters with `[]` after the name:

```
fn sum(s[]: i32): i32 {
    var total = 0
    loop x in s {
        total += x
    }
    return total
}

fn main() {
    var arr[5]: i32 = [10, 20, 30, 40, 50]

    print(sum(arr))          // Pass full array as a slice -> 150
    print(sum(arr[1..4]))    // Slice elements 1, 2, 3     -> 90
    print(sum(arr[..2]))     // First 2 elements           -> 30
    print(sum(arr[3..]))     // From index 3 to the end    -> 90
}
```

### 8.3 Bounds Safety
- Constant out-of-bounds indices (`arr[5]` on `arr[5]`, or `arr[-1]`) are **compile-time errors**.
- Runtime out-of-bounds indices (`i < 0` or `i >= len`) **always trap** in both debug and release builds.

### 8.4 Looping Over Arrays and Slices
Use `loop x in arr` for values, or `loop i, x in arr` for both index (`i64`) and value:

```
loop x in arr {
    print(x)
}

loop i, x in arr {
    print(i)
    print(x)
}
```

---

## 9. Pointers (`*T`, `&x`, `*p`, `null`)

### 9.1 Taking an Address (`&`) and Dereferencing (`*`)
Write `*T` for a pointer to `T`, `&x` to take the address of a variable or array element, and `*p` to read or modify the pointed-to value:

```
fn swap(a: *i32, b: *i32) {
    var tmp = *a
    *a = *b
    *b = tmp
}

fn main() {
    var x = 10
    var y = 20
    swap(&x, &y)
    print(x)    // 20
    print(y)    // 10

    var p: *i32 = &x
    *p += 5
    print(x)    // 25
}
```

### 9.2 `null` Pointers and Debug Safety
Use `null` for an empty pointer (`var p: *i32 = null`). In `debug` builds, dereferencing or indexing a `null` pointer immediately traps with the exact source location:

```
var p: *i32 = null
if p == null {
    print("p is null")
}
```

### 9.3 Pointer Arithmetic, Indexing, and Slicing
Pointers support C-style arithmetic (`p + i`, `p - i`, `p += i`, `p -= i`, `p - q`), indexing (`p[i]`), and slicing with an explicit upper bound (`p[lo..hi]`):

```
var arr[5]: i32 = [10, 20, 30, 40, 50]
var p: *i32 = &arr[0]

p += 2
print(*p)           // 30
print(p[1])         // 40

var s[]: i32 = p[0..3]   // slice of [30, 40, 50]
print(len(s))            // 3
```

---

## 10. Structs (`struct`)

### 10.1 Declaring and Initializing Structs
Declare a struct with `struct Name { ... }` and initialize it with `Name { field: value }`. Any omitted fields are automatically zero-filled:

```
struct Vec2 {
    x: i32
    y: i32
}

struct Particle {
    pos: Vec2
    vel[2]: i32
    active: bool
}

fn main() {
    var a = Vec2 { x: 10, y: 20 }
    var b = Vec2 { x: 5 }           // b.y is automatically 0
    var zero = Vec2 {}              // x = 0, y = 0

    var p = Particle {
        pos: a,
        vel: [3, -2],
        active: true,
    }
    print(p.pos.x)                  // 10
    print(p.vel[0])                 // 3
}
```

### 10.2 Field Access on Pointers (Auto-Dereference `.`)
Antimony uses `.` for both struct values and `*Struct` pointers—there is no `->` operator:

```
fn move(v: *Vec2, dx: i32, dy: i32) {
    v.x += dx       // automatically dereferences 'v'
    v.y += dy
}

fn main() {
    var pt = Vec2 { x: 1, y: 2 }
    move(&pt, 10, 20)
    print(pt.x)     // 11
    print(pt.y)     // 22
}
```

---

## 11. Multi-File Modules (`import`) & Windows DLL FFI (`extern`)

### 11.1 Splitting Code Across Files (`import`)
Use `import "path.sb"` at the top level to include another Antimony source file (resolved relative to the importing file's directory). Imports are automatically deduplicated, so diamond or circular imports are safe:

```
// math_utils.sb
const SCALE = 10

fn scale(x: i32): i32 {
    return x * SCALE
}
```

```
// main.sb
import "math_utils.sb"

fn main() {
    print(scale(7))     // 70
}
```

### 11.2 Calling Windows DLLs Directly (`extern`)
Antimony emits imported functions directly into the PE32+ Import Table—no C compiler, header files, or `.lib` import libraries needed. You can declare a single `extern` function or group multiple imports from the same DLL:

```
// Single import
extern "kernel32.dll" fn GetTickCount64(): u64

// Block import from msvcrt.dll
extern "msvcrt.dll" {
    fn sqrt(x: f64): f64
    fn pow(x: f64, y: f64): f64
    fn strlen(s: *u8): usize
}

fn main() {
    print(GetTickCount64() > u64(0))    // true
    print(sqrt(2.0))                    // 1.414214
    print(pow(3.0, 4.0))                // 81.0
    print(strlen("Antimony"))           // 8 (string literals coerce to *u8 C strings)
}
```

---

## 12. Built-in Functions

| Function | Description |
|---|---|
| `print(x)` | Prints any integer, float (`f32`/`f64`), pointer, `bool`, or `str` followed by a newline |
| `len(x)` | Returns the length of an array, slice, or string |
| `exit(code: i32)` | Terminates the program immediately with exit status `code` |
| `rev(a..b [by s])` | Reverses a range inside a `loop` header |
| `T(x)` | Explicitly converts `x` to scalar integer or float type `T` (e.g. `i64(x)`, `f64(x)`, `u8(x)`) |

---

## 13. Complete Example Program

See [demo.sb](file:///c:/Users/soroosh/sb/examples/demo.sb) for a runnable tour of Antimony, or try this prime-counting benchmark:

```
fn is_prime(n: i32): bool {
    if n < 2 {
        return false
    }
    if n == 2 {
        return true
    }
    if n % 2 == 0 {
        return false
    }
    var d = 3
    loop d * d <= n {
        if n % d == 0 {
            return false
        }
        d += 2
    }
    return true
}

fn main() {
    var count = 0
    loop n in 2..10000 {
        if is_prime(n) {
            count += 1
        }
    }
    print("Primes below 10,000:")
    print(count)    // 1229
}
```
