$ErrorActionPreference = "Continue"
$sbc = "c:\Users\soroosh\sb\sbc.exe"
$tmp = "c:\Users\soroosh\sb\tests\tmp"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

function Test-CompileError([string]$name, [string]$code, [string]$expectSubstr) {
    $sb = Join-Path $tmp "$name.sb"
    $exe = Join-Path $tmp "$name.exe"
    Set-Content -Path $sb -Value $code -Encoding UTF8
    $out = (& $sbc $sb -o $exe 2>&1) -join "`n"
    if ($LASTEXITCODE -eq 0) {
        throw "FAIL [$name]: expected compile error, but succeeded"
    }
    if (!$out.Contains($expectSubstr)) {
        throw "FAIL [$name]: expected '$expectSubstr' in error output, got:`n$out"
    }
    Write-Host "PASS [compile-error] $name"
}

function Test-RunTrap([string]$name, [string]$code, [string]$expectSubstr, [switch]$Release) {
    $sb = Join-Path $tmp "$name.sb"
    $exe = Join-Path $tmp "$name.exe"
    Set-Content -Path $sb -Value $code -Encoding UTF8
    if ($Release) { $null = & $sbc $sb -o $exe --release 2>&1 }
    else          { $null = & $sbc $sb -o $exe 2>&1 }
    if ($LASTEXITCODE -ne 0) { throw "FAIL [$name]: failed to compile" }
    $out = (& $exe 2>&1) -join "`n"
    if ($LASTEXITCODE -eq 0) {
        throw "FAIL [$name]: expected runtime trap, but exited with 0"
    }
    if (!$out.Contains($expectSubstr)) {
        throw "FAIL [$name]: expected '$expectSubstr' in panic output, got:`n$out"
    }
    Write-Host "PASS [runtime-trap]  $name"
}

function Test-RunOk([string]$name, [string]$code, [string]$expected, [switch]$Release) {
    $sb = Join-Path $tmp "$name.sb"
    $exe = Join-Path $tmp "$name.exe"
    Set-Content -Path $sb -Value $code -Encoding UTF8
    if ($Release) { $null = & $sbc $sb -o $exe --release 2>&1 }
    else          { $null = & $sbc $sb -o $exe 2>&1 }
    if ($LASTEXITCODE -ne 0) { throw "FAIL [$name]: failed to compile" }
    $out = ((& $exe) -join "`n").Replace("`r`n", "`n").Trim()
    if ($LASTEXITCODE -ne 0) { throw "FAIL [$name]: exited with code $LASTEXITCODE" }
    if ($out -ne $expected) {
        throw "FAIL [$name]: expected '$expected', got '$out'"
    }
    Write-Host "PASS [run-ok]        $name"
}

# 1. Compile-time checks
Test-CompileError "lit_overflow_i32" "fn main() {`n var x = 3000000000`n}" "does not fit in 'i32'"
Test-CompileError "lit_overflow_u8"  "fn main() {`n var x: u8 = 300`n}"    "does not fit in 'u8'"
Test-CompileError "const_expr_ovf"   "fn main() {`n var x = 2000000000 * 2`n}" "does not fit in 'i32'"
Test-CompileError "stray_semicolon"  "fn main() {`n var x = 5;`n}"         "unexpected ';'"
Test-CompileError "unassigned_read"  "fn main() {`n var x: i32`n print(x)`n}" "before it is assigned"
Test-CompileError "branch_uninit"    "fn main() {`n var x: i32`n if true { x = 1 }`n print(x)`n}" "before it is assigned"
Test-CompileError "missing_return"   "fn f(a: i32): i32 {`n if a > 0 { return 1 }`n}`nfn main() {}" "missing return"
Test-CompileError "range_var_mut"    "fn main() {`n loop i in 0..5 { i = 3 }`n}" "loop variable"
Test-CompileError "implicit_conv"    "fn main() {`n var a: i32 = 1`n var b: i64 = a`n}" "type mismatch"
Test-CompileError "shadowing"        "fn main() {`n var x = 1`n if true { var x = 2 }`n}" "shadowing is not allowed"
Test-CompileError "arr_too_many"     "fn main() {`n var a[2]: i32 = [1, 2, 3]`n}" "only holds 2"
Test-CompileError "arr_oob_const"    "fn main() {`n var a[3]: i32 = [1, 2, 3]`n print(a[3])`n}" "out of bounds"
Test-CompileError "arr_neg_const"    "fn main() {`n var a[3]: i32 = [1, 2, 3]`n print(a[-1])`n}" "cannot be negative"
Test-CompileError "slice_bad_range"  "fn main() {`n var a[4]: i32 = [1, 2, 3, 4]`n var s[]: i32 = a[3..2]`n}" "lower bound (3) > upper bound (2)"
Test-CompileError "null_no_type"     "fn main() {`n var p = null`n}" "'null' requires an explicit pointer type"
Test-CompileError "deref_non_ptr"    "fn main() {`n var x = 5`n print(*x)`n}" "cannot dereference non-pointer"
Test-CompileError "ptr_type_mismatch" "fn main() {`n var x: i32 = 1`n var p: *i64 = &x`n}" "type mismatch"
Test-CompileError "struct_bad_field" "struct P { x: i32 }`nfn main() {`n var p = P { z: 1 }`n}" "no field named 'z'"
Test-CompileError "struct_dup_init"  "struct P { x: i32 }`nfn main() {`n var p = P { x: 1, x: 2 }`n}" "duplicate initializer"
Test-CompileError "struct_recursive" "struct Bad { b: Bad }`nfn main() {}" "recursive struct"

# 2. Runtime traps in debug mode
Test-RunTrap "add_overflow_i32" "fn main() {`n var x: i32 = 2147483647`n x += 1`n print(x)`n}" "integer addition overflow"
Test-RunTrap "sub_overflow_u32" "fn main() {`n var x: u32 = 0`n x -= 1`n print(x)`n}" "integer subtraction overflow"
Test-RunTrap "narrow_conv_trap" "fn main() {`n var x: i64 = 500`n var y = i8(x)`n print(y)`n}" "conversion out of range"
Test-RunTrap "div_by_zero_rel"  "fn main() {`n var z = 0`n print(10 / z)`n}" "division by zero" -Release
Test-RunTrap "arr_oob_runtime"  "fn main() {`n var a[3]: i32 = [1, 2, 3]`n var i = 3`n print(a[i])`n}" "index out of bounds" -Release
Test-RunTrap "arr_neg_runtime"  "fn main() {`n var a[3]: i32 = [1, 2, 3]`n var i = -1`n print(a[i])`n}" "index out of bounds" -Release
Test-RunTrap "slice_oob_rt"     "fn main() {`n var a[3]: i32 = [1, 2, 3]`n var h = 5`n var s = a[1..h]`n print(len(s))`n}" "slice upper bound > length" -Release
Test-RunTrap "null_deref_trap"  "fn main() {`n var p: *i32 = null`n print(*p)`n}" "null pointer dereference"
Test-RunTrap "null_index_trap"  "fn main() {`n var p: *i32 = null`n print(p[0])`n}" "null pointer dereference"
Test-RunTrap "null_field_trap"  "struct P { x: i32 }`nfn main() {`n var p: *P = null`n print(p.x)`n}" "null pointer dereference"

# 3. Release mode wraps on overflow, and +% always wraps in debug mode
Test-RunOk "release_wraps" "fn main() {`n var x: i32 = 2147483647`n x += 1`n print(x)`n}" "-2147483648" -Release
Test-RunOk "wrap_op_debug" "fn main() {`n var x: i32 = 2147483647`n x +%= 1`n print(x)`n}" "-2147483648"

# 4. Edge case: -2147483648 literal and labeled break/continue
Test-RunOk "min_i32_and_labels" @"
fn main() {
    var m: i32 = -2147483648
    print(m)
    var found = 0
    outer: loop y in 0..5 {
        loop x in 0..5 {
            if x * y == 6 {
                found = x * 10 + y
                break outer
            }
        }
    }
    print(found)
}
"@ "-2147483648`n32"

# 5. Arrays, partial zero-fill, slices, len(), and collection loops
Test-RunOk "arrays_and_slices" @"
const N = 5

fn sum(s[]: i32): i32 {
    var total = 0
    loop x in s {
        total += x
    }
    return total
}

fn dot(a[3]: i32, b[3]: i32): i32 {
    var d = 0
    loop i in 0..3 {
        d += a[i] * b[i]
    }
    return d
}

fn main() {
    var arr[N]: i32 = [10, 20, 30]
    print(arr[0])
    print(arr[3])
    arr[4] = 50
    arr[1] += 5
    print(sum(arr))

    var sub[]: i32 = arr[1..4]
    print(len(sub))
    print(sum(sub))

    var v1 = [1, 2, 3]
    var v2 = [4, 5, 6]
    print(dot(v1, v2))

    var acc: i64 = 0
    loop i, v in arr[..3] {
        acc += i * 100 + i64(v)
    }
    print(acc)
}
"@ "10`n0`n115`n3`n55`n32`n365"

# 6. Pointers: &x, *p, **pp, null, pointer arithmetic (p + i, p - q, p += i), p[i], p[lo..hi]
Test-RunOk "pointers_full" @"
fn swap(a: *i32, b: *i32) {
    var t = *a
    *a = *b
    *b = t
}

fn fill_via_ptr(p: *i32, n: i32) {
    loop i in 0..n {
        p[i] = (i + 1) * 10
    }
}

fn main() {
    var x: i32
    var p: *i32 = null
    print(p == null)
    p = &x
    print(p != null)
    *p = 42
    *p += 8
    print(x)

    var pp: **i32 = &p
    **pp = 99
    print(x)

    var arr[5]: i32 = []
    fill_via_ptr(&arr[0], 5)
    swap(&arr[0], &arr[4])
    print(arr[0])
    print(arr[4])

    var q: *i32 = &arr[0]
    var r: *i32 = q + 3
    print(*r)
    print(r - q)
    r -= 2
    print(*r)

    var s[]: i32 = q[1..4]
    var sum = 0
    loop v in s {
        sum += v
    }
    print(sum)
}
"@ "true`ntrue`n50`n99`n50`n10`n40`n3`n20`n90"

# 7. Structs: nested structs, array fields, auto-deref ptr.field, arrays of structs, linked nodes
Test-RunOk "structs_full" @"
struct Vec2 {
    x: i32
    y: i32
}

struct Particle {
    pos: Vec2
    vel[2]: i32
    active: bool
}

struct Node {
    val: i32
    next: *Node
}

fn move_particle(p: *Particle, dt: i32) {
    p.pos.x += p.vel[0] * dt
    p.pos.y += p.vel[1] * dt
}

fn sum_list(head: *Node): i32 {
    var total = 0
    var cur: *Node = head
    loop cur != null {
        total += cur.val
        cur = cur.next
    }
    return total
}

fn main() {
    var p = Particle {
        pos: Vec2 { x: 10, y: 20 },
        vel: [3, -2],
        active: true,
    }
    move_particle(&p, 4)
    print(p.pos.x)
    print(p.pos.y)
    print(p.active)

    // Partial initialization zero-fills omitted fields
    var v = Vec2 { x: 7 }
    print(v.x)
    print(v.y)

    // Array of structs + collection loop
    var pts[3]: Vec2 = [
        Vec2 { x: 1, y: 2 },
        Vec2 { x: 3, y: 4 },
        Vec2 { x: 5, y: 6 },
    ]
    pts[1].x += 10
    var sx = 0
    loop item in pts {
        sx += item.x
    }
    print(sx)

    // Linked list via self-referential *Node pointer
    var n3 = Node { val: 30, next: null }
    var n2 = Node { val: 20, next: &n3 }
    var n1 = Node { val: 10, next: &n2 }
    print(sum_list(&n1))
}
"@ "22`n12`ntrue`n7`n0`n19`n60"

# 8. Floating-point f32 and f64: SSE2 arithmetic, comparisons, conversions, arrays, structs
Test-RunOk "floats_f32_f64" @"
const PI = 3.141592653589793

struct Circle {
    r: f64
}

fn circle_area(c: *Circle): f64 {
    return PI * c.r * c.r
}

fn main() {
    var a = 2.5
    var b: f64 = 1.25
    a += b
    print(a)
    print(-a)
    print(a > 3.0)
    print(a < 2.0)

    var f: f32 = 0.5
    f *= 3.0
    print(f)

    var n = i32(3.9)
    print(n)
    var d = f64(n) + 0.5
    print(d)

    var c = Circle { r: 2.0 }
    print(circle_area(&c))

    var vals[3]: f64 = [1.5, 2.5, 3.0]
    var sum = 0.0
    loop v in vals {
        sum += v
    }
    print(sum)
}
"@ "3.75`n-3.75`ntrue`nfalse`n1.5`n3`n3.5`n12.566371`n7.0"

# 9. Phase 4 optimizations: register allocation (RBX, R12..R15), branch fusion, immediate ops, fast indexing
Test-RunOk "phase4_reg_alloc_and_opt" @"
fn helper(x: i64, y: i64): i64 {
    var a = x + 10
    var b = y * 2
    var c = a - b
    var d = c ^ 0x55
    var e = (d << 2) >> 1
    return e + a + b
}

fn fib_iter(n: i32): i64 {
    var a: i64 = 0
    var b: i64 = 1
    loop i in 0..n {
        var next = a + b
        a = b
        b = next
    }
    return a
}

fn main() {
    // 8 active local variables across nested loops and function calls
    // (5 promoted to RBX, R12..R15 + 3 on stack, plus 1 address-taken variable)
    var v1: i64 = 1
    var v2: i64 = 2
    var v3: i64 = 3
    var v4: i64 = 4
    var v5: i64 = 5
    var v6: i64 = 6
    var v7: i64 = 7
    var pinned: i64 = 100
    var p: *i64 = &pinned

    var acc: i64 = 0
    loop i in 0..10 {
        loop j in 0..5 {
            if !(i == 9 && j == 4) && (i < 5 || j >= 2) {
                acc += helper(v1 + i64(i), v2 + i64(j))
            }
            v1 += 1
            v2 += 2
            v3 +%= 3
            v4 -= 1
            v5 ^= 1
            v6 += 1
            v7 += 1
        }
    }
    *p += v6 + v7
    print(acc)
    print(v1)
    print(v3)
    print(v4)
    print(pinned)
    print(fib_iter(20))
}
"@ "-564`n51`n153`n-46`n213`n6765"

# 10. Phase 5: Multi-file imports & Windows DLL FFI (extern fn)
Test-CompileError "import_missing"    "import `"no_such_module_xyz.sb`"`nfn main() {}" "cannot open imported module"
Test-CompileError "extern_with_body"  "extern `"kernel32.dll`" fn Foo() {}`nfn main() {}" "cannot have a body"
Test-CompileError "extern_slice_param" "extern `"kernel32.dll`" fn Foo(s[]: i32)`nfn main() {}" "cannot have type '[]i32' by value"

Test-RunOk "extern_win32_and_msvcrt" @"
extern "kernel32.dll" fn GetCurrentProcessId(): u32
extern "kernel32.dll" fn GetTickCount64(): u64

extern "msvcrt.dll" {
    fn abs(x: i32): i32
    fn sqrt(x: f64): f64
    fn pow(x: f64, y: f64): f64
    fn strlen(s: *u8): usize
    fn strcmp(a: *u8, b: *u8): i32
}

fn main() {
    var pid = GetCurrentProcessId()
    var ticks = GetTickCount64()
    print(pid > u32(0))
    print(ticks > u64(0))
    print(abs(-42))
    print(sqrt(2.0))
    print(pow(3.0, 4.0))
    print(strlen("Antimony"))
    print(strcmp("abc", "abc") == 0)
    print(strcmp("abc", "abd") < 0)
}
"@ "true`ntrue`n42`n1.414214`n81.0`n8`ntrue`ntrue"

# Multi-file diamond/circular import test
Set-Content -Path (Join-Path $tmp "mod_math.sb") -Value @"
import "mod_geom.sb"

const SCALE = 10

extern "msvcrt.dll" fn abs(x: i32): i32

fn scaled_abs(v: i32): i32 {
    return abs(v) * SCALE
}
"@ -Encoding UTF8

Set-Content -Path (Join-Path $tmp "mod_geom.sb") -Value @"
import "mod_math"

struct Box {
    w: i32
    h: i32
}

extern "msvcrt.dll" fn abs(x: i32): i32

fn box_area(b: *Box): i32 {
    return abs(b.w) * abs(b.h)
}
"@ -Encoding UTF8

Test-RunOk "multi_file_imports" @"
import "mod_math.sb"
import "mod_geom.sb"

fn main() {
    var b = Box { w: -6, h: 7 }
    print(box_area(&b))
    print(scaled_abs(-9))
}
"@ "42`n90"

# 11. Phase 6: Enums & Pattern Matching (enum & match)
Test-CompileError "enum_empty"           "enum E {}`nfn main() {}" "must have at least one variant"
Test-CompileError "enum_dup_variant"     "enum E { A, B, A }`nfn main() {}" "duplicate variant 'A'"
Test-CompileError "enum_dup_discrim"     "enum E { A = 1, B = 1 }`nfn main() {}" "duplicate discriminant value 1"
Test-CompileError "enum_no_variant"      "enum E { A, B }`nfn main() { var x = E.C }" "has no variant named 'C'"
Test-CompileError "enum_assign_variant"  "enum E { A, B }`nfn main() { E.A = E.B }" "cannot assign to constant or enum variant"
Test-CompileError "match_non_exhaustive_enum" @"
enum Color { Red, Green, Blue }
fn main() {
    var c = Color.Red
    match c {
        Color.Red { print(1) }
        Color.Green { print(2) }
    }
}
"@ "variant 'Color.Blue' is not covered"
Test-CompileError "match_non_exhaustive_int" @"
fn main() {
    var x = 10
    match x {
        1 { print(1) }
        2 { print(2) }
    }
}
"@ "non-exhaustive 'match' on integer type 'i32'"
Test-CompileError "match_dup_pattern" @"
enum Color { Red, Green }
fn main() {
    var c = Color.Red
    match c {
        Color.Red { print(1) }
        Color.Red, Color.Green { print(2) }
    }
}
"@ "duplicate match pattern"
Test-CompileError "match_unreachable_else" @"
enum Color { Red, Green }
fn main() {
    var c = Color.Red
    match c {
        Color.Red { print(1) }
        Color.Green { print(2) }
        else { print(3) }
    }
}
"@ "unreachable 'else' arm"
Test-CompileError "match_uninit_arm" @"
enum Color { Red, Green }
fn main() {
    var c = Color.Red
    var y: i32
    match c {
        Color.Red { y = 10 }
        Color.Green { print(0) }
    }
    print(y)
}
"@ "variable 'y' may be read before it is assigned"

Test-RunOk "enums_and_match" @"
const BASE_CODE = 100

enum Status {
    Idle
    Running
    Paused = 10
    Done
    Error = BASE_CODE + 5
}

struct Task {
    id: i32
    state: Status
}

fn describe_status(s: Status): i32 {
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

fn classify_code(x: i32): i32 {
    var out: i32
    match x {
        0 {
            out = 10
        }
        1, 2, 3 {
            out = 20
        }
        else {
            out = 99
        }
    }
    return out
}

fn main() {
    var s = Status.Idle
    print(i32(s))
    print(i32(Status.Running))
    print(i32(Status.Paused))
    print(i32(Status.Done))
    print(i32(Status.Error))

    var states[5]: Status = [
        Status.Idle,
        Status.Running,
        Status.Paused,
        Status.Done,
        Status.Error
    ]
    var sum = 0
    loop st in states {
        sum += describe_status(st)
    }
    print(sum)

    var t = Task { id: 7, state: Status.Paused }
    if t.state == Status.Paused && t.state != Status.Done {
        t.state = Status.Done
    }
    print(describe_status(t.state))

    print(classify_code(0))
    print(classify_code(2))
    print(classify_code(42))

    var flag = true
    match flag {
        true { print(111) }
        false { print(222) }
    }
}
"@ "0`n1`n10`n11`n105`n3`n2`n10`n20`n99`n111"

Write-Host "`nAll tests passed."
