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

### 10.3 Methods on Structs & Enums (`fn (self: *T)` / `fn (self: T)`)
Declare a method on any `struct` or `enum` by placing the receiver `(name: *T)` or `(name: T)` right after `fn`.
- **Zero overhead:** Methods compile directly to static function calls with the receiver passed as the first argument.
- **Auto-referencing & auto-dereferencing:** Calling a `*T` method on an addressable `T` variable automatically passes `&v`; calling a `T` method on a `*T` pointer automatically dereferences `*p`.

```
fn (v: *Vec2) translate(dx: i32, dy: i32) {
    v.x += dx
    v.y += dy
}

fn (v: Vec2) length_sq(): i32 {
    return v.x * v.x + v.y * v.y
}

fn (v: Vec2) add(other: Vec2): Vec2 {
    return Vec2 { x: v.x + other.x, y: v.y + other.y }
}

fn main() {
    var pt = Vec2 { x: 3, y: 4 }
    print(pt.length_sq())       // 25
    pt.translate(10, 20)        // auto-references &pt
    var sum = pt.add(Vec2 { x: 1, y: 2 })
    print(sum.x)                // 14
    print(sum.y)                // 26
}
```

### 10.4 Returning `struct`, `str`, and `[]T` by Value + First-Class `str` Operations
Functions and methods can return `struct`, `str`, and slice `[]T` values directly by value, and `struct` definitions can hold `str` and `[]T` fields:
- **Zero-copy `str` slicing & indexing:** `s[i]` returns byte `u8` (bounds-checked), and `s[lo..hi]` returns a sub-string `str` without allocating.
- **`str` equality & iteration:** Compare strings with `s1 == s2` or `s1 != s2`, and iterate over bytes with `loop b in s` or `loop i, b in s`.
- **Escape safety:** Returning a slice `[]T` pointing to a local stack variable is rejected at compile time.

```
struct Token {
    lexeme: str
    line: i32
}

fn scan_ident(src: str, lo: i64, hi: i64, ln: i32): Token {
    return Token { lexeme: src[lo..hi], line: ln }
}

fn main() {
    var tok = scan_ident("fn main()", 3, 7, 1)
    print(tok.lexeme)               // main
    print(tok.lexeme == "main")     // true
    print(tok.lexeme[0])            // 109 ('m')
}
```

### 10.5 Matching on `str`, Zero-Copy `str(...)` Views & `StringBuilder`
- **`match` on `str`:** Arms take string literals (comma-separated alternatives allowed). Duplicate patterns are compile errors, and an `else` arm is required because strings can't be enumerated exhaustively.
- **`str(bytes)`:** Turns a `[]u8` or `[N]u8` into a `str` view with no copy and no allocation.
- **`StringBuilder`** (from `std/str.sb`): a growable heap buffer that always keeps a trailing `'\0'`, so `sb.cstr()` can be passed straight to C/Win32 APIs.

```
import "std/str.sb"
import "std/os.sb"

fn command_id(cmd: str): i32 {
    match cmd {
        "build", "b" { return 1 }
        "run", "r"   { return 2 }
        else         { return 0 }
    }
}

fn main() {
    var bytes[5]: u8 = [72, 101, 108, 108, 111]
    print(str(bytes))               // Hello

    var sb: StringBuilder
    sb.init(64)
    defer sb.free()
    sb.append("answer=")
    sb.append_i64(42)
    print(sb.as_str())              // answer=42

    print(command_id(arg_at(1)))    // depends on the first CLI argument
    print_raw("no newline\n")
    eprint("written to stderr")
}
```

---

## 11. Enums (`enum`) & Pattern Matching (`match`)

### 11.1 Declaring Enums
An `enum` defines a distinct 32-bit signed nominal type (`i32` representation) with named variants. Variants auto-increment from `0` unless given an explicit compile-time constant value (`= expr`), and variants are accessed with `.` (`EnumName.Variant`):

```
enum Status {
    Idle
    Running
    Paused = 10
    Done
    Error = -1
}

fn main() {
    var s = Status.Running
    if s != Status.Done {
        s = Status.Done
    }
    print(i32(s))           // 11 (explicit conversion to integer)
}
```

### 11.2 Exhaustive Pattern Matching (`match`)
`match` compares an `enum`, integer, or `bool` value against compile-time constant patterns. Arms can match a single value or multiple comma-separated values (`pat1, pat2 { ... }`), with an optional `else { ... }` catch-all arm at the end.

Every `match` in Antimony is **checked for exhaustiveness at compile time**:
- Matching on an `enum` without an `else` branch must cover every variant of the enum (if you add a new variant later, the compiler tells you which `match` is missing it!).
- Duplicate patterns and unreachable `else` branches are compile-time errors.
- Definite-assignment and return-path analysis flow through exhaustive `match` statements just like `if / else`.

```
fn status_code(s: Status): i32 {
    match s {
        Status.Idle {
            return 0
        }
        Status.Running, Status.Paused {
            return 1
        }
        Status.Done {
            return 2
        }
        Status.Error {
            return -1
        }
    }
}

fn classify_http(code: i32) {
    var ok: bool
    match code {
        200, 201, 204 {
            ok = true
        }
        else {
            ok = false
        }
    }
    print(ok)
}
```

---

## 12. Multi-File Modules (`import`) & Windows DLL FFI (`extern`)

### 12.1 Splitting Code Across Files (`import`)
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

### 12.2 Calling Windows DLLs Directly (`extern`)
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

## 13. `defer`, `sizeof` / `alignof`, `ptr_cast`, and the Standard Library (`std/`)

### 13.1 Scope-Based Cleanup (`defer`)
`defer` schedules a statement or `{ ... }` block to run in **reverse (LIFO) order** when the enclosing scope exits—whether at the end of a block, or via `return`, `break`, or `continue`. When `return expr` runs, `expr` is evaluated before the deferred cleanup statements execute:

```
import "std/mem.sb"

fn process_buffer(n: usize): i32 {
    var buf = alloc(n)
    defer free_mem(buf)     // guaranteed to run on every return path!

    if buf == null {
        return -1
    }
    buf[0] = 42
    return i32(buf[0])
}
```

### 13.2 `sizeof`, `alignof`, and `ptr_cast`
- `sizeof(T)` and `alignof(T)` evaluate at compile time to the size and alignment in bytes of any type or expression (`sizeof(Vec2)`, `sizeof(*u8)`, `sizeof(my_array)`).
- `ptr_cast(*T, expr)` converts a raw pointer (such as `*u8` returned from `alloc`) or 64-bit integer address into `*T` with zero runtime overhead:

```
import "std/mem.sb"

struct Node {
    id: i32
    val: i64
}

fn main() {
    var raw = alloc(sizeof(Node))
    defer free_mem(raw)

    var node = ptr_cast(*Node, raw)
    node.id  = 1
    node.val = 9000
    print(node.val)         // 9000
}
```

### 13.3 Standard Library Modules (`std/`)
Antimony ships with native standard library modules in `std/` that you can import from any directory:
- `import "std/mem.sb"` — Win32 process heap (`alloc`, `alloc_uninit`, `realloc_mem`, `free_mem`, `copy_mem`, `set_mem`, `cmp_mem`) and fast bump-pointer `Arena` (`arena_init`, `arena_alloc`, `arena_reset`, `arena_free`).
- `import "std/io.sb"` — Native Win32 file I/O (`write_file_str`, `write_file_bytes`, `write_file_ptr`, `file_size`, `read_file_into`, `delete_file`).
- `import "std/time.sb"` — High-resolution performance counters (`perf_counter`, `perf_freq`, `elapsed_us`, `ticks_ms`, `sleep_ms`).
- `import "std/str.sb"` — String utilities: C-string interop (`cstr_len`, `cstr_eq`, `cstr_to_str`, `str_from_ptr`), ASCII classification (`is_digit`, `is_alpha`, `is_space`), `str` helpers (`str_starts_with`, `str_ends_with`, `str_trim`, `str_find_byte`, `str_parse_i64`, `parse_i64`), and the growable `StringBuilder` (`init`, `append`, `append_byte`, `append_i64`, `as_str`, `cstr`, `clear`, `free`).
- `import "std/os.sb"` — Console streams and process environment: `print_raw`, `eprint_raw`, `eprint`, `cmd_line`, `arg_count`, `arg_at`, `parse_arg_count`, `parse_arg_at`, `getenv_into`.

---

## 14. Built-in Functions

| Function | Description |
|---|---|
| `print(x)` | Prints any integer, float (`f32`/`f64`), enum, pointer, `bool`, or `str` followed by a newline |
| `len(x)` | Returns the length of an array, slice, or string |
| `sizeof(T_or_expr)` | Compile-time size in bytes of a type or expression |
| `alignof(T_or_expr)` | Compile-time alignment in bytes of a type or expression |
| `ptr_cast(*T, x)` | Zero-cost cast of a pointer or 64-bit integer address `x` to pointer type `*T` |
| `exit(code: i32)` | Terminates the program immediately with exit status `code` |
| `rev(a..b [by s])` | Reverses a range inside a `loop` header |
| `T(x)` | Explicitly converts `x` to scalar integer, float, or enum type `T` (e.g. `i64(x)`, `f64(x)`, `usize(ptr)`) |

---

## 15. Complete Example Program

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
