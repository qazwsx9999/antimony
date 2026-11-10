# Antimony Language Specification — Draft v0.2

> Language: **Antimony**. Source file extension: **`.sb`**.
>
> First compiler target: **Windows x64**, emitting PE executables directly (no external assembler or linker).
>
> Status: draft. Sections marked **[Proposed]** were not explicitly decided yet and are open for review.

---

## 0. Design Principles

1. **Simple, consistent, readable syntax.**
2. **One way** to express each operation.
3. **Runtime performance on par with C/C++.**
4. **Very fast compilation.** The grammar is parseable with one token of lookahead and no symbol table; function signatures are fully known without reading bodies, so bodies can be checked in parallel.
5. **Static typing**, with local, forward-only type inference.
6. **Few keystrokes.**

---

## 1. Lexical Structure

### 1.1 Source text
Source files are UTF-8. Identifiers are ASCII: `[A-Za-z_][A-Za-z0-9_]*`.

### 1.2 Comments **[Proposed]**
Line comments only: `// ...` to end of line. (One way; no block comments.)

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
var  const  fn  struct  return  if  else  loop  in  by  break  continue
true  false  null
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

### 1.8 String literals **[Proposed, minimal]**
`"text"` with escapes `\n \t \\ \" \0 \xHH`. Type `str` (immutable). The full string design is deferred.

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

### 2.4 Arrays and Slices
- **Fixed-size array:** declared with `[N]` after the variable or parameter name (where `N` is a positive compile-time constant expression):
  ```
  var arr[5]: i32 = [10, 20, 30]     // remaining 2 elements zero-filled
  var zero[1024]: i32 = []           // all 1024 elements zero-filled
  var inferred = [10, 20, 30]        // length 3 and type i32 inferred
  ```
  Providing more elements than `N` is a compile error.
- **Slice (`{ptr, len}` view):** declared with `[]` after the variable or parameter name:
  ```
  var s[]: i32 = arr[1..4]
  fn sum(a[]: i32): i32 { ... }
  ```
  A fixed-size array of element type `T` implicitly converts to a slice of element type `T` when passed to a function parameter or assigned to a slice variable.
- **Indexing (`a[i]`):** `i` may be any integer type or untyped integer constant. Constant out-of-bounds indices are compile errors; runtime out-of-bounds indices (`i < 0` or `i >= len(a)`) trap in **all** build modes.
- **Slicing (`a[lo..hi]`, `a[..hi]`, `a[lo..]`, `a[..]`):** creates a slice view `[lo, hi)`. Checks `0 <= lo <= hi <= len(a)` in all build modes.
- **`len(a)`:** built-in returning the number of elements (an untyped constant for fixed-size arrays, or `i64` for slices and strings).

### 2.5 Pointers (`*T`)
- **Pointer type:** `*T` (and multi-level pointers `**T`, arrays of pointers `var ptrs[4]: *i32`, etc.).
- **Address-of (`&`):** `&x` (where `x` is a `var` variable), `&arr[i]`, or `&*p`. Taking `&x` marks `x` as initialized for definite-assignment analysis so out-parameters work cleanly.
- **Dereference (`*`):** `*p` reads or writes the pointee (`*p = 10`, `*p += 1`, `var y = *p`).
- **`null`:** represents the zero pointer address (`0`). Requires an explicit pointer type (`var p: *i32 = null`). In `debug` mode, dereferencing or indexing a `null` pointer traps with source file, line, and column.
- **Pointer arithmetic & indexing:**
  - `p + offset`, `p - offset`, `p += offset`, `p -= offset` advance or rewind `p` by `offset * sizeof(T)` bytes.
  - `p - q` (for two pointers of the same type `*T`) returns the element difference as `isize`.
  - `p[i]` is equivalent to `*(p + i)`.
  - `p[lo..hi]` creates a slice `[]T` of length `hi - lo` starting at `p + lo` (requires an explicit upper bound `hi`).

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
- Generic function calls support both explicit type arguments (`max_val[i64](a, b)`) and type argument inference from value parameters (`max_val(a, b)`, `swap_val(&x, &y)`).
- Standard generic collections are provided in `std/vec.sb` (`Vec[T]`) and `std/map.sb` (`HashMap[K, V]`, `Option[T]`).

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

### 4.5 Scope and shadowing **[Proposed]**
- A variable is visible from its declaration to the end of the enclosing block.
- **Shadowing is not allowed**: declaring a name that is already visible in an enclosing scope (inside the same function) is a compile error.

### 4.6 Top-level variables **[Proposed]**
`var` at top level is allowed; its initializer must be a constant expression (no hidden start-up code, no initialization-order problems).

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
- Parameters are read-only inside the function body **[Proposed]**.

### 5.2 Return
- In a function with a return type, every control path must end in `return expr`; otherwise it is a compile error.
- In a function without a return type, `return` takes no value and may be omitted at the end.

### 5.3 Declaration order
Functions may be called before they are declared in the file.

### 5.4 Entry point **[Proposed]**
```
fn main() { ... }
```
`main` takes no parameters and returns no value. The process exit code is set with the built-in `exit(code: i32)`; returning normally exits with `0`.

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

The loop variable is mutable in the body. **[Proposed]** The compiler emits a hint when a C-style loop is a plain counting loop (`var i = a; i < b; i += 1`, `i` not modified in the body) suggesting the range form.

### 6.8 `break` and `continue`
- `break` exits the innermost loop.
- `continue` skips to the next iteration of the innermost loop (running the step of a C-style loop, or advancing a range loop).

**[Proposed] Labels** for nested loops:
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

## 9. Built-ins **[Proposed, minimal]**
| Built-in | Description |
|---|---|
| `print(x)` | prints an integer, float, `bool`, or `str`, followed by a newline |
| `exit(code: i32)` | terminates the process with `code` |
| `rev(range)` | reverse range; only valid in a `loop` header |
| `T(x)` | conversion to scalar type `T` (§2.3) |

---

## 10. Grammar (EBNF)

`TERM` is a statement terminator (inserted newline, §1.3).

```ebnf
File        = { TopDecl TERM } ;
TopDecl     = FnDecl | VarDecl | ConstDecl ;

FnDecl      = "fn" Ident "(" [ Params ] ")" [ ":" Type ] Block ;
Params      = Param { "," Param } [ "," ] ;
Param       = Ident ":" Type ;

VarDecl     = "var" Ident ( ":" Type [ "=" Expr ] | "=" Expr ) ;
ConstDecl   = "const" Ident [ ":" Type ] "=" Expr ;

Type        = Ident ;                          (* scalar types for now *)

Block       = "{" { Stmt TERM } "}" ;
Stmt        = VarDecl | ConstDecl | Assign | ExprStmt
            | IfStmt | LoopStmt | ReturnStmt
            | "break" [ Ident ] | "continue" [ Ident ]
            | Ident ":" LoopStmt ;             (* labeled loop *)

Assign      = Ident AssignOp Expr ;
AssignOp    = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^="
            | "<<=" | ">>=" | "+%=" | "-%=" | "*%=" ;
ExprStmt    = CallExpr ;                       (* only calls may be statements *)
ReturnStmt  = "return" [ Expr ] ;

IfStmt      = "if" Expr Block [ "else" ( IfStmt | Block ) ] ;

LoopStmt    = "loop" ( Block                                   (* infinite    *)
                     | Ident "in" RangeExpr Block              (* range       *)
                     | VarDecl ";" Expr ";" Assign Block       (* C-style     *)
                     | Expr Block ) ;                          (* conditional *)
RangeExpr   = Range | "rev" "(" Range ")" ;
Range       = Expr ".." Expr [ "by" Expr ] ;

Expr        = OrExpr ;
OrExpr      = AndExpr { "||" AndExpr } ;
AndExpr     = CmpExpr { "&&" CmpExpr } ;
CmpExpr     = AddExpr [ CmpOp AddExpr ] ;
AddExpr     = MulExpr { ( "+" | "-" | "+%" | "-%" | "|" | "^" ) MulExpr } ;
MulExpr     = Unary { ( "*" | "/" | "%" | "*%" | "<<" | ">>" | "&" ) Unary } ;
Unary       = ( "-" | "!" | "~" ) Unary | Primary ;
Primary     = IntLit | FloatLit | StrLit | "true" | "false"
            | Ident | CallExpr | "(" Expr ")" ;
CallExpr    = Ident "(" [ Expr { "," Expr } [ "," ] ] ")" ;
```

Notes:
- Every `loop` form is chosen by at most two tokens of lookahead (`{`, `Ident "in"`, `var`, otherwise expression).
- `..` and `by` bind looser than all expression operators: `0..n+1` means `0..(n+1)`.

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

## 13. Open Items

1. Sections marked **[Proposed]**: comments, shadowing, top-level `var`, read-only parameters, `main`/`exit`, labels, built-ins.
2. Arrays and slices (and `loop x in arr`).
3. Structs, and the brace-ambiguity rule for struct literals in conditions.
4. Pointers / references, including read-only references (important for aliasing optimizations).
5. Strings (full design).
6. Modules / imports / visibility.
7. Error handling.
8. Generics (impacts compile time).
9. Target platform for the first native backend (OS, object format, calling convention).
