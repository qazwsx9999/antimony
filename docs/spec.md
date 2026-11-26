# Antimony Language Specification — v1.0

> Language: **Antimony**. Source file extension: **`.sb`**.
>
> First compiler target: **Windows x64**, emitting PE executables directly (no external assembler or linker).
>
> Status: **core language complete.** Every rule in this document is implemented by the reference compiler `sbc` and exercised by the test suite (`tests/run_tests.ps1`). Planned work and known implementation limits are listed in §13.

---

## 0. Design Principles

1. **Simple, consistent, readable syntax.**
2. **One way** to express each operation.
3. **Runtime performance on par with C/C++.**
4. **Very fast compilation.** The grammar is parseable without a symbol table, using a small fixed lookahead (plus a bracket scan to distinguish generic arguments `f[T](x)` from indexing, §10); function signatures are fully known without reading bodies, so bodies can be checked in parallel.
5. **Static typing**, with local, forward-only type inference.
6. **Few keystrokes.**

---

## 1. Lexical Structure

### 1.1 Source text
Source files are UTF-8. Identifiers are ASCII: `[A-Za-z_][A-Za-z0-9_]*`.

### 1.2 Comments
Line comments only: `// ...` to end of line. There are no block comments (one way to do it); `/* ... */` is a syntax error.

### 1.3 Statement termination
A **newline ends a statement**. Formally (same rule as Go), the lexer inserts a statement terminator at a newline if the last token on the line is one of:

- an identifier or a literal
- one of the keywords `return`, `break`, `continue`
- one of `)`, `]`, `}`

Consequences:
- A line ending in a binary operator, `,`, or `(` continues on the next line:
  ```
  var total = a +
      b
  ```
- `{` must be on the same line as its `fn` / `if` / `loop` / `else`, and `else` must be on the same line as the preceding `}`. There is exactly one brace style.

An **explicit `;` is only allowed as a separator inside a C-style `loop` header** (§6.4). An explicit `;` anywhere else is a compile error.

### 1.4 Keywords
```
var  const  type  fn  struct  enum  match  return  if  else  loop  in  by
break  continue  defer  import  extern  pub  priv  true  false  null
```
Built-in type names (`i32`, `bool`, …) and built-in functions (`print`, `rev`, …) are predeclared identifiers, not keywords.

### 1.5 Integer literals
| Form | Example |
|---|---|
| decimal | `1234` |
| hexadecimal | `0xFF` |
| binary | `0b1010` |
| octal | `0o755` |

- `_` may appear **between digits** as a separator: `1_000_000`, `0xFFFF_FFFF`.
- There are **no type suffixes**. The type of a literal comes from context (§3.3); use a conversion such as `i64(5)` when no context exists.
- Leading `-` is the unary minus operator, not part of the literal; constant evaluation (§3.3) makes `-2147483648` valid for `i32`.

### 1.6 Float literals
`1.5`, `0.25`, `1e9`, `2.5e-3`. A digit is required on both sides of `.`. Default type `f64`.

### 1.7 Boolean literals
`true`, `false`.

### 1.8 String literals
- **Escaped string:** `"text"` with escapes `\n \t \r \\ \" \' \0 \xHH`. Type `str` (immutable `{ptr, len}`).
- **Raw string:** `` `raw text` `` enclosed in backticks; escape sequences are not interpreted (`\n` stays two characters `\` and `n`), and CRLF line endings inside multi-line raw strings are normalized to `\n`.

### 1.9 Character literals
`'a'`, `'\n'`, `'\t'`, `'\r'`, `'\\'`, `'\''`, `'\"'`, `'\0'`, `'\xHH'`. A character literal represents a single byte/ASCII code point as an untyped integer constant (adapting to `u8`, `i32`, `i64`, etc. from context, or defaulting to `i32`). Empty (`''`) or multi-character (`'ab'`) literals are compile-time errors.

---

## 2. Types

### 2.1 Built-in scalar types
| Type | Description |
|---|---|
| `i8 i16 i32 i64` | signed two's-complement integers |
| `u8 u16 u32 u64` | unsigned integers |
| `isize usize` | pointer-sized signed/unsigned integers (sizes, indices) |
| `f32 f64` | IEEE-754 floats |
| `bool` | `true` / `false` |
| `str` | immutable string (minimal, see §1.8) |

### 2.2 No implicit conversions
Values of different types never mix implicitly. `i32 + i64` is a compile error. Conversions are explicit and use the target type name as a function:
```
var a: i32 = 5
var b: i64 = i64(a) + 1
```

### 2.3 Conversion semantics
| From → To | Behavior |
|---|---|
| integer → wider integer of same signedness | exact |
| integer → integer that cannot hold all values | **checked like overflow** (§8): trap in debug, truncate in release |
| integer → float | rounds to nearest |
| float → integer | truncates toward zero; out-of-range is checked like overflow |
| `bool` ↔ integer | not allowed |

### 2.4 Arrays and Slices
- **Fixed-size array:** declared with `[N]` after the variable or parameter name (where `N` is a positive compile-time constant expression):
  ```
  var arr[5]: i32 = [10, 20, 30]     // remaining 2 elements zero-filled
  var zero[1024]: i32 = []           // all 1024 elements zero-filled
  var inferred = [10, 20, 30]        // length 3 and type i32 inferred
  ```
  Providing more elements than `N` is a compile error.
- **Slice (`{ptr, len}` view):** declared with `[]` after the variable or parameter name (or prefix `[]T`):
  ```
  var s[]: i32 = arr[1..4]
  var ro[]: const i32 = arr    // read-only slice view (`[]const i32`)
  ```
- **Read-only slices (`[]const T` / `var s[]: const T`):** Allow reading (`s[i]`), slicing (`s[lo..hi]`, producing `[]const T`), and `len(s)`, while forbidding element assignment (`s[i] = ...`), taking mutable `&s[i]`, or converting `[]const T` back to `[]T`. Mutable `[]T` and `[N]T` implicitly coerce to `[]const T`.
  fn sum(a[]: i32): i32 { ... }
  ```
  A fixed-size array of element type `T` implicitly converts to a slice of element type `T` when passed to a function parameter or assigned to a slice variable.
- **Indexing (`a[i]`):** `i` may be any integer type or untyped integer constant. Constant out-of-bounds indices are compile errors; runtime out-of-bounds indices (`i < 0` or `i >= len(a)`) trap in **all** build modes.
- **Slicing (`a[lo..hi]`, `a[..hi]`, `a[lo..]`, `a[..]`):** creates a slice view `[lo, hi)`. Checks `0 <= lo <= hi <= len(a)` in all build modes.
- **`len(a)`:** built-in returning the number of elements (an untyped constant for fixed-size arrays, or `i64` for slices and strings).

### 2.5 Pointers (`*T` and `*const T`)
- **Pointer type:** `*T` for a mutable pointer and `*const T` for a read-only pointer (multi-level pointers like `**T`, `*const *T`, and arrays of pointers `var ptrs[4]: *i32` are supported).
- **Read-only coercion & enforcement:** A mutable pointer `*T` implicitly coerces to `*const T` (e.g. when passing `&x` to a `fn f(p: *const i32)` parameter), whereas `*const T` cannot coerce to `*T`. Writing through `*const T` (`*p = v`, `p[i] = v`, `p.field = v`, compound assignments), taking a mutable `&*p` / `&p[i]` / `&p.field`, calling a mutable `*T`-receiver method on `*const T`, or passing `*const i64` to atomic write built-ins is rejected at compile time. Slicing `p[lo..hi]` on `*const T` yields `[]const T`.
- **Address-of (`&`):** `&x` (where `x` is a `var` variable), `&arr[i]`, or `&*p`. Taking `&x` (except when coerced only via read-only inspection) marks `x` as initialized for definite-assignment analysis so out-parameters work cleanly.
- **Dereference (`*`):** `*p` reads or writes the pointee (`*p = 10`, `*p += 1`, `var y = *p`).
- **`null`:** represents the zero pointer address (`0`). Requires an explicit pointer type (`var p: *i32 = null`). In `debug` mode, dereferencing or indexing a `null` pointer traps with source file, line, and column.
- **Pointer arithmetic & indexing:**
  - `p + offset`, `p - offset`, `p += offset`, `p -= offset` advance or rewind `p` by `offset * sizeof(T)` bytes.
  - `p - q` (between pointers to the same element type `T`, including `*T` and `*const T`) returns the element difference as `isize`.
  - `p[i]` is equivalent to `*(p + i)`.
  - `p[lo..hi]` creates a slice `[]T` (or `[]const T` if `p` is `*const T`) of length `hi - lo` starting at `p + lo` (requires an explicit upper bound `hi`).

### 2.6 Structs (`struct`)
- **Declaration:**
  ```
  struct Vec2 {
      x: i32
      y: i32
  }
  ```
  Fields are laid out in declaration order with natural alignment (1, 2, 4, or 8 bytes). A struct may contain scalars, pointers (including self-referential `*Node`), fixed-size arrays (`vel[2]: i32`), and nested structs by value.
- **Initialization (`Type { field: value, ... }`):**
  ```
  var v = Vec2 { x: 10, y: 20 }
  var partial = Vec2 { x: 10 }     // omitted fields are zero-filled
  var zero = Vec2 {}               // all fields zero-filled
  ```
  To keep `if` and `loop` headers unambiguous without parentheses, a struct literal may not appear directly at the top level of an `if` or `loop` header unless enclosed in `(...)`.
- **Field access (`.`):** `.` accesses a field on both a struct value (`v.x`) and a pointer to a struct (`ptr.x`), automatically dereferencing the pointer (with a null check in `debug` mode). There is no `->` operator.

### 2.7 Enums, Tagged Unions, and `?` Error Propagation
- **Simple Enums (`enum`):** 32-bit signed nominal types (`i32` representation) with named variants.
- **Tagged Unions (Payload Enums):** Any variant may declare a payload type `Variant(T)` (`enum Result[T, E] { Ok(T), Err(E) }`). Tagged unions have a 4-byte discriminant tag at offset 0 followed by a naturally aligned union payload.
- **Inferred Variant Shorthand (`.Variant` / `.Variant(arg)`):** Supported wherever the expected `enum` type is known from context.
- **Pattern Destructuring (`match`):** `match val { .Variant(x) { ... } }` binds the variant payload to `x` inside the arm.
- **`?` Propagation (`expr?`):** Unwraps `.Ok(v)` / `.Some(v)` or runs active `defer` blocks in LIFO order and returns `.Err(e)` / `.None` early from the enclosing function.

### 2.8 Compile-Time Generics (`[T]`)
- `struct Name[T, U]`, `enum Name[T, E]`, `fn name[T](...)`, and `fn (self: *Name[T]) method(...)` are monomorphized on demand at compile time.
- Generic function calls support both explicit type arguments (`max_val[i64](a, b)`) and recursive type argument inference from value parameters, including pointers, slices, arrays, function pointers, and nested generic types (`max_val(a, b)`, `swap_val(&x, &y)`, `first_or(vec, 0)`, `unwrap_pair(pair)`).
- Standard generic collections are provided in `std/vec.sb` (`Vec[T]`) and `std/map.sb` (`HashMap[K, V]`, `Option[T]`).

### 2.9 First-Class Function Pointers (`fn(T1, T2): R`) & Stateless Anonymous Functions
- **Function Pointer Type:** `fn(T1, T2): R` (or `fn(T1, T2)` for `void` return) is an 8-byte code pointer (`TY_FN`). It may be stored in variables, struct fields, arrays, and slices, compared with `null` or another function pointer of identical signature (`==`, `!=`), passed as an argument, or returned from a function.
- **Indirect Calls:** Any expression of type `fn(T1, ...): R` can be invoked directly (`fp(a, b)`, `s.cb(x)`, `ops[i](a, b)`). In `debug` mode, calling a `null` function pointer traps with `"null function pointer call"`.
- **Stateless Anonymous Functions:** `fn(a: i32, b: i32): bool { return a < b }` defines an inline anonymous function and evaluates to its function pointer (`fn(i32, i32): bool`). Anonymous functions are stateless and cannot capture outer local variables (preserving zero hidden heap allocations).
- **Standard Generic Sorting & Binary Search (`std/sort.sb`):** Provides `sort[T]`, `sort_desc[T]`, `sort_by[T]`, `is_sorted[T]`, `is_sorted_by[T]`, `binary_search[T]`, `binary_search_by[T]`, and `reverse[T]`.
- **Concurrency & Hardware Atomics (`std/thread.sb`):** Provides Win32 OS threads (`Thread`, `spawn`), `Mutex` / `RwLock` (`SRWLOCK`), `CondVar` (`CONDITION_VARIABLE`), `AtomicI64`, and hardware atomic built-ins (`atomic_load`, `atomic_store`, `atomic_add`, `atomic_cas`).

### 2.10 Type Aliases (`type`)
- **Syntax:** `type Alias = TargetType` or generic `type Alias[T, ...] = TargetType`, usable at top level and (without type parameters) inside function blocks:
  ```
  type Byte = u8
  type IntVec = Vec[i64]
  type Pair[T] = Box[T]
  type ReadBuf = []const u8
  ```
- Type aliases are transparent (not distinct nominal types): `Alias` and `TargetType` are interchangeable anywhere a type is expected, including struct literals, methods, and pointer/slice types. Recursive alias cycles are compile-time errors.

---

## 3. Constants and Literal Typing

### 3.1 Constant expressions
A **constant expression** is built only from literals, `const` names, operators, and conversions of constant expressions. It is evaluated **at compile time with exact (arbitrary-precision) arithmetic**.

### 3.2 `const` declarations
```
const N = 1024              // untyped constant
const MASK: u32 = 0xFFFF_FFFF  // typed constant
```
- The initializer must be a constant expression.
- An **untyped** `const` behaves exactly like a literal: it adapts to its context.
- A `const` can never be assigned.
- `const` may appear at top level or inside a function.

### 3.3 Typing rule for untyped constants
When an untyped constant (literal or untyped `const`) is used:
1. If the context supplies an **expected type** — a declared variable type, a parameter type, a return type, the type of the other operand of a binary operator, or the target of an assignment — the constant takes that type.
2. Otherwise it takes its **default type**: `i32` for integers, `f64` for floats.
3. It is a **compile error** if the exact value does not fit in the resulting type.

```
var a = 10                 // i32
var b: i64 = 3_000_000_000 // OK: takes i64 from context
var c = 3_000_000_000      // ERROR: does not fit in default type i32
var d: u8 = 300            // ERROR: does not fit in u8
var e: i32 = -2147483648   // OK: exact value fits
var f = 2_000_000_000 * 2  // ERROR: constant overflows i32
var g: i64 = b + 1         // 1 takes type i64 from b
```

---

## 4. Variables

### 4.1 Declarations
```
var x: i32          // explicit type, no initial value
var x: i32 = 5      // explicit type with initial value
var x = 5           // type inferred from initializer
```
- The type is fixed at declaration and never changes.
- Type inference is local and forward-only: the type comes from the initializer expression alone.

### 4.2 Definite assignment
Reading a variable before it has been assigned **on every path** is a compile error. There is no implicit zero-initialization and no garbage value.
```
var x: i32
if cond { x = 1 }
print(x)            // ERROR: x may be unassigned
```

### 4.3 Assignment
Assignment is a **statement**, not an expression (so `if x = 5 {` cannot compile).
```
x = 10
x += 1      // also: -=  *=  /=  %=  &=  |=  ^=  <<=  >>=  +%=  -%=  *%=
```
There is no `++` / `--`.

### 4.4 Mutability
All variables declared with `var` are mutable. There is no `let`. Compile-time values use `const`.

### 4.5 Scope and shadowing
- A variable is visible from its declaration to the end of the enclosing block. Sibling blocks may reuse a name.
- **Shadowing is not allowed inside a function**: declaring a local (variable, constant, parameter, or loop variable) whose name is already visible in an enclosing scope of the same function is a compile error.
- A local may reuse the name of a top-level function or variable, but never the name of a type or built-in (`var i32 = 1` and `var print = 1` are compile errors).

### 4.6 Top-level variables
`var` at top level is allowed; its initializer must be a constant expression (no hidden start-up code, no initialization-order problems). A non-constant initializer is a compile error.

---

## 5. Functions

### 5.1 Declaration
```
fn add(a: i32, b: i32): i32 {
    return a + b
}

fn log(msg: str) {
    print(msg)
}
```
- Every parameter has an explicit type.
- **If the return type is omitted, the function returns no value.** Return types are never inferred.
- Parameters are **read-only** inside the function body: assigning to a parameter or taking its address is a compile error. To let a callee modify a value, pass a pointer (`p: *T`).

### 5.2 Return
- In a function with a return type, every control path must end in `return expr`; otherwise it is a compile error.
- In a function without a return type, `return` takes no value and may be omitted at the end.

### 5.3 Declaration order
Functions may be called before they are declared in the file.

### 5.4 Entry point
```
fn main() { ... }
```
`main` takes no parameters and returns no value; any other signature is a compile error. The process exit code is set with the built-in `exit(code: i32)`; returning normally exits with `0`. Command-line arguments are available through `std/os.sb` (`arg_count`, `arg_at`).

### 5.5 Module visibility (`pub` / `priv`)
Top-level declarations (`fn`, `struct`, `enum`, `type`, `var`, `const`, `extern`) support optional `pub` or `priv` modifiers:
- `priv` restricts a symbol (including methods declared `priv fn (self: *S) helper()`) to the source file in which it was declared; referencing it from another imported file is a compile-time error.
- **Opt-in module encapsulation:** If a file declares at least one `pub` symbol, all top-level declarations in that file without `pub` become private (`priv`) to that file automatically. Files that use neither `pub` nor `priv` remain public to importing files for backwards compatibility.

---

## 6. Statements and Control Flow

### 6.1 Blocks
`{ ... }` introduces a new scope. Bodies of `if`, `else`, `loop`, and `fn` must always be blocks.

### 6.2 `if` / `else`
```
if age >= 18 {
    print("Adult")
} else if age >= 13 {
    print("Teen")
} else {
    print("Child")
}
```
- No parentheses are required around the condition.
- The condition must have type `bool`. There is no implicit "truthiness".

### 6.3 `loop` — overview
`loop` is the only loop keyword. It has four forms, selected by the tokens following `loop`:

| After `loop` | Form | Example |
|---|---|---|
| `{` | infinite | `loop { }` |
| identifier, then `in` or `,` | range | `loop i in 0..n { }` |
| `var` | C-style | `loop var i = 0; i < n; i += 1 { }` |
| anything else | conditional | `loop i < n { }` |

### 6.4 Infinite loop
```
loop {
    do_work()
}
```

### 6.5 Conditional loop
```
loop i < 10 {
    print(i)
    i += 1
}
```
The condition must have type `bool` and is evaluated before each iteration.

### 6.6 Range loop
```
loop i in 0..10 { }            // 0, 1, ..., 9
loop i in 0..10 by 2 { }       // 0, 2, 4, 6, 8
loop i in rev(0..10) { }       // 9, 8, ..., 0
loop i in rev(0..10 by 2) { }  // 8, 6, 4, 2, 0
```
Semantics:
- `a..b` is the half-open integer range `[a, b)`. There is no inclusive range syntax.
- `a`, `b`, and the step `s` are each **evaluated exactly once**, before the first iteration, in that order.
- `a` and `b` must have the same integer type (untyped constants adapt per §3.3); that is the type of `i`.
- The step `s` (default `1`) must be positive. A constant step ≤ 0 is a compile error; a non-constant step ≤ 0 is checked like overflow (§8).
- The values produced are `a, a+s, a+2s, …` while `< b`. If `a >= b` the body does not run.
- `rev(r)` produces **exactly the same values as `r`, in reverse order**.
- The loop variable `i` is **read-only** in the body and is scoped to the loop.
- The loop's own counter arithmetic **never overflows**; the compiler emits no overflow checks for it.
- Ranges and `rev` are **only valid in a `loop` header**; they are not first-class values (yet).

Iteration over arrays and slices:
```
loop x in arr {
    print(x)
}

loop i, x in arr[1..4] {
    print(i)
    print(x)
}
```
The collection expression is evaluated once before the first iteration, and the loop variables (`x` and `i`) are read-only inside the body.

### 6.7 C-style loop
```
loop var i = 0; i < n; i += 1 {
    print(i)
}
```
Restrictions:
1. **All three parts are required.** Empty parts are a compile error (use the infinite or conditional form instead).
2. The first part **must be a `var` declaration**. The variable is scoped to the loop (header and body).
3. The third part **must be an assignment statement** (`=` or a compound assignment).
4. Explicit `;` is required between the parts (the only place `;` is allowed).

Semantics: run the declaration once; before each iteration evaluate the condition (must be `bool`); after each iteration (including after `continue`) run the step.

The loop variable is mutable in the body. The compiler emits a non-fatal **hint** when a C-style loop is a plain counting loop (`var i = a; i < b; i += 1`, `i` not modified in the body), suggesting the range form.

### 6.8 `break` and `continue`
- `break` exits the innermost loop.
- `continue` skips to the next iteration of the innermost loop (running the step of a C-style loop, or advancing a range loop).

**Labels** target an enclosing loop from a nested one. A label is written `name:` immediately before `loop` and may be used by `break name` or `continue name`. A label on anything other than `loop`, or an unknown label, is a compile error.
```
outer: loop y in 0..h {
    loop x in 0..w {
        if grid_at(x, y) == target { break outer }
    }
}
```

---

## 7. Expressions and Operators

### 7.1 Precedence (highest first)
| Level | Operators | Associativity |
|---|---|---|
| 6 | unary `-`  `!`  `~` | right |
| 5 | `*`  `/`  `%`  `*%`  `<<`  `>>`  `&` | left |
| 4 | `+`  `-`  `+%`  `-%`  `\|`  `^` | left |
| 3 | `==`  `!=`  `<`  `<=`  `>`  `>=` | none (no chaining) |
| 2 | `&&` | left |
| 1 | `\|\|` | left |

Only 6 levels (Go's model). Bitwise operators bind tighter than comparisons, avoiding C's `x & MASK == 0` trap. Comparisons do not chain: `a < b < c` is a compile error.

### 7.2 Operator rules
- Arithmetic and bitwise operators require both operands to have the **same type** (after untyped-constant typing, §3.3).
- `&&` and `||` require `bool` operands and **short-circuit**.
- `!` requires `bool`; `~` requires an integer.
- Integer `/` truncates toward zero; `%` has the sign of the dividend (same as C).
- Shift: the right operand must be an unsigned integer or an untyped constant; the result has the type of the left operand. `>>` is arithmetic for signed types, logical for unsigned.
- Comparison operators produce `bool`.
- Evaluation order of operands is **left to right**.

### 7.3 Wrapping operators
`+%`, `-%`, `*%` (and `+%=`, `-%=`, `*%=`) perform two's-complement wrapping arithmetic on integers. They are never checked and behave identically in all build modes. Use them whenever wrapping is intended (hashing, checksums, RNGs, crypto).

---

## 8. Integer Overflow and Runtime Checks

### 8.1 Rule
**Integer overflow of `+ - * / unary-` and of narrowing conversions is a program error.** Correct programs must not rely on any particular result.

### 8.2 Compile-time detection
Overflow in a **constant expression** (§3.1) is always a **compile error**. Compile errors are produced **only** from this rule — never from optimizer range analysis — so whether a program compiles does not depend on compiler version or optimization level.

### 8.3 Run-time behavior by build mode
| Build mode | Overflow | Narrowing conversion out of range | Range-loop step ≤ 0 |
|---|---|---|---|
| `debug` | trap | trap | trap |
| `release` | wraps (two's complement) | truncates | trap |

The compiler may remove any check it proves can never fail (range analysis is used **only to remove checks**).

A future `fast` mode, in which overflow is undefined behavior, may be added without affecting correct programs.

### 8.4 Always-checked operations (all build modes)
| Operation | Behavior |
|---|---|
| integer division or `%` by zero | trap |
| `MIN / -1` and `MIN % -1` for signed types | trap |

(These trap in hardware on x86-64, so checking costs nothing extra in the common case.)

### 8.5 Shifts
Shift amount ≥ bit width of the left operand: trap in `debug`; in `release`, the amount is masked to the bit width (x86-64 behavior).

### 8.6 Trap
A trap terminates the process immediately with a non-zero exit code and, in `debug`, prints the source file, line, and reason.

### 8.7 Floats
IEEE-754 semantics. Floating-point arithmetic is never checked (infinities and NaN are produced normally).

---

## 9. Built-ins
Built-ins are predeclared identifiers (not keywords); they cannot be shadowed (§4.5).

| Built-in | Description |
|---|---|
| `print(x)` | prints an integer, float, `bool`, enum, pointer, or `str`, followed by a newline |
| `print(fmt, args...)` | formatted output: `fmt` is a string literal using `{}` placeholders for each argument (`{{` and `}}` escape literal braces), followed by a newline |
| `len(x)` | element count of an array (untyped constant), slice, or `str` (`i64`) |
| `sizeof(T_or_expr)` / `alignof(T_or_expr)` | compile-time size / alignment in bytes of a type or expression (untyped constant) |
| `ptr_cast(*T, x)` | reinterprets a pointer, `str`, or 64-bit integer address `x` as pointer type `*T` (zero cost) |
| `str(bytes)` | zero-copy `str` view of a `[]u8` or `[N]u8` |
| `atomic_load(p: *i64): i64` | sequentially consistent 64-bit atomic load |
| `atomic_store(p: *i64, v: i64)` | sequentially consistent 64-bit atomic store |
| `atomic_add(p: *i64, d: i64): i64` | atomic fetch-and-add; returns the previous value |
| `atomic_cas(p: *i64, expected: i64, desired: i64): i64` | atomic compare-and-swap; returns the previous value |
| `exit(code: i32)` | terminates the process with `code` |
| `rev(range)` | reverse range; only valid in a `loop` header (§6.6) |
| `T(x)` | conversion to scalar or enum type `T` (§2.3) |

The atomic built-ins reject `*const i64` arguments (§2.5).

---

## 10. Grammar (EBNF)

`TERM` is a statement terminator (inserted newline, §1.3). `IntLit` includes character literals (§1.9); `StrLit` includes raw strings (§1.8).

```ebnf
(* ---- Declarations ---- *)
File        = { TopDecl TERM } ;
TopDecl     = ImportDecl
            | [ "pub" | "priv" ] ( FnDecl | StructDecl | EnumDecl | TypeDecl
                                 | VarDecl | ConstDecl | ExternDecl ) ;
ImportDecl  = "import" StrLit ;
ExternDecl  = "extern" StrLit ( FnHeader | "{" { FnHeader TERM } "}" ) ;

TypeParams  = "[" Ident { "," Ident } "]" ;
TypeDecl    = "type" Ident [ TypeParams ] "=" Type ;
StructDecl  = "struct" Ident [ TypeParams ] "{" { Field ( TERM | "," ) } "}" ;
Field       = Ident [ Dims ] ":" Type ;
EnumDecl    = "enum" Ident [ TypeParams ] "{" { Variant ( TERM | "," ) } "}" ;
Variant     = Ident [ "(" Type ")" ] [ "=" Expr ] ;

FnDecl      = FnHeader Block ;
FnHeader    = "fn" [ TypeParams ] [ Receiver ] Ident [ TypeParams ]
              "(" [ Params ] ")" [ ":" Type ] ;
Receiver    = "(" Ident ":" Type ")" ;          (* T or *T, T a struct or enum *)
Params      = Param { "," Param } [ "," ] ;
Param       = Ident [ Dims ] ":" Type ;
Dims        = "[" [ Expr ] "]" ;                 (* [N] array, [] slice *)

VarDecl     = "var" Ident [ Dims ] ( ":" Type [ "=" Expr ] | "=" Expr ) ;
ConstDecl   = "const" Ident [ ":" Type ] "=" Expr ;

(* ---- Types ---- *)
Type        = [ "[" Expr "]" | "[" "]" [ "const" ] ] PtrType ;
PtrType     = { "*" [ "const" ] } BaseType ;
BaseType    = Ident [ TypeArgs ] | FnType ;
TypeArgs    = "[" Type { "," Type } "]" ;
FnType      = "fn" "(" [ FnTypeParam { "," FnTypeParam } ] ")" [ ":" Type ] ;
FnTypeParam = [ Ident ":" ] Type ;

(* ---- Statements ---- *)
Block       = "{" { Stmt TERM } "}" ;
Stmt        = VarDecl | ConstDecl | TypeDecl | Assign | CallStmt
            | IfStmt | MatchStmt | [ Ident ":" ] LoopStmt
            | DeferStmt | ReturnStmt | Block
            | "break" [ Ident ] | "continue" [ Ident ] ;

Assign      = LValue AssignOp Expr ;
LValue      = Ident | Postfix | "*" Unary ;      (* Postfix ending in .field or [index] *)
AssignOp    = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^="
            | "<<=" | ">>=" | "+%=" | "-%=" | "*%=" ;
CallStmt    = Postfix ;                          (* must end in a call or `?` *)
DeferStmt   = "defer" ( Block | Stmt ) ;
ReturnStmt  = "return" [ Expr ] ;

IfStmt      = "if" Expr Block [ "else" ( IfStmt | Block ) ] ;
MatchStmt   = "match" Expr "{" { MatchArm TERM } [ "else" Block TERM ] "}" ;
MatchArm    = Expr { "," Expr } Block ;

LoopStmt    = "loop" ( Block                                       (* infinite    *)
                     | Ident "in" RangeExpr Block                  (* range       *)
                     | Ident [ "," Ident ] "in" Expr Block         (* collection  *)
                     | VarDecl ";" Expr ";" Ident AssignOp Expr Block  (* C-style *)
                     | Expr Block ) ;                              (* conditional *)
RangeExpr   = Range | "rev" "(" Range ")" ;
Range       = Expr ".." Expr [ "by" Expr ] ;

(* ---- Expressions ---- *)
Expr        = OrExpr ;
OrExpr      = AndExpr { "||" AndExpr } ;
AndExpr     = CmpExpr { "&&" CmpExpr } ;
CmpExpr     = AddExpr [ CmpOp AddExpr ] ;
CmpOp       = "==" | "!=" | "<" | "<=" | ">" | ">=" ;
AddExpr     = MulExpr { ( "+" | "-" | "+%" | "-%" | "|" | "^" ) MulExpr } ;
MulExpr     = Unary { ( "*" | "/" | "%" | "*%" | "<<" | ">>" | "&" ) Unary } ;
Unary       = ( "-" | "!" | "~" | "&" | "*" ) Unary | Postfix ;
Postfix     = Primary { "." Ident | Args | "[" Index "]" | "?" } ;
Index       = Expr | [ Expr ] ".." [ Expr ] ;
Args        = "(" [ Expr { "," Expr } [ "," ] ] ")" ;
Primary     = IntLit | FloatLit | StrLit | "true" | "false" | "null"
            | Ident [ TypeArgs ] [ StructLit ]
            | "." Ident                          (* inferred enum variant *)
            | ArrayLit | FnLit | "(" Expr ")" ;
StructLit   = "{" [ FieldInit { ( "," | TERM ) FieldInit } [ "," ] ] "}" ;
FieldInit   = Ident ":" Expr ;
ArrayLit    = "[" [ Expr { "," Expr } [ "," ] ] "]" ;
FnLit       = "fn" "(" [ Params ] ")" [ ":" Type ] Block ;
```

Notes:
- The `loop` form is chosen by at most four tokens of lookahead (`{`, `Ident "in"`, `Ident "," Ident "in"`, `var`, otherwise expression). A range header is recognized by the `..` after its first expression.
- `..` and `by` bind looser than all expression operators: `0..n+1` means `0..(n+1)`.
- After an identifier in an expression, `[` opens a type-argument list (`max[i64](a, b)`, `Pair[i32, str] { ... }`) only if the bracket contents are syntactically types and are followed by `(`, `.`, or `{`, or contain a top-level `,`; otherwise it is indexing or slicing.
- A `StructLit` may not appear at the top level of an `if`, `loop`, or `match` header unless enclosed in `( ... )` (§2.6).
- A postfix `[]` on a declared name may be followed by `const` in the type: `var s[]: const u8`.
- `,` and newlines may be used interchangeably between struct fields, enum variants, and struct-literal field initializers.

---

## 11. Complete Example

```
const LIMIT = 10

fn add(a: i32, b: i32): i32 {
    return a + b
}

fn fact(n: i32): i64 {
    if n <= 1 {
        return 1
    }
    return i64(n) * fact(n - 1)
}

fn hash(x: u32): u32 {
    return x *% 2654435761      // intentional wrapping
}

fn main() {
    var total: i64 = 3_000_000_000
    var count = 0                     // i32
    var flag: bool

    loop i in 0..LIMIT {
        if i == 5 {
            continue
        }
        count += 1
    }

    loop var i = 1; i < 1000; i *= 2 {
        print(i)
    }

    loop count > 0 {
        count -= 1
    }

    if count == 0 {
        flag = true
    } else {
        flag = false
    }

    print(flag)
    print(add(2, 3))
    print(fact(20))
    print(total)
}
```

---

## 12. Decision Log

| # | Topic | Decision |
|---|---|---|
| D1 | Integer type names | `i8…i64`, `u8…u64`, `isize`, `usize`, `f32`, `f64`; no aliases |
| D2 | Integer literals | untyped constants; context type, else `i32`; error if out of range |
| D3 | Statement end | newline; `;` only inside C-style loop header |
| D4 | Omitted return type | function returns nothing; no return-type inference |
| D5 | Uninitialized variables | definite-assignment analysis |
| D6 | Overflow | constant exprs: compile error; debug: trap; release: wrap (not to be relied on); `+% -% *%` for wrapping |
| D7 | Mutability | `var` only (no `let`); `const` for compile-time values |
| D8 | Compound assignment | `+=` family yes; `++`/`--` no |
| D9 | Loops | infinite, conditional, range, and restricted C-style |
| D10 | Ranges | `..` half-open only; `by` step; `rev()` reverse |
| D11 | Conversions | no implicit conversions; `T(x)` |
| D12 | Assignment | statement, not expression |
| D13 | Comments | `//` line comments only; no block comments |
| D14 | Shadowing | not allowed within a function; types and built-ins can never be shadowed |
| D15 | Top-level `var` | allowed, constant initializer only (no start-up code) |
| D16 | Parameters | read-only; mutate through a pointer parameter |
| D17 | Entry point | `fn main()` with no parameters or result; `exit(code)` sets the exit code |
| D18 | Nested loop control | `label: loop`, `break label`, `continue label` |
| D19 | Visibility | per-file `pub` / `priv`; any `pub` makes the file's other declarations private |
| D20 | Generics | `[T]` type parameters, monomorphized at compile time; type arguments inferred from value arguments |
| D21 | Error handling | tagged unions (`Result`/`Option` shape) plus postfix `?`; no exceptions |

## 13. Open Items

Planned work:
1. Strings: an ownership/allocation design beyond `str` views and `std/str.sb`'s `StringBuilder`.
2. Ranges as first-class values (currently valid only in `loop` headers, §6.6).
3. A `fast` build mode in which overflow is undefined behavior (§8.3).
4. Additional targets (Linux ELF, macOS Mach-O) and self-hosting.
