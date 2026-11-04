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

# 12. Phase 7: defer, sizeof/alignof, ptr_cast, and std/ library
Test-CompileError "defer_return" "fn main() { defer return }" "'return' is not allowed inside a 'defer'"
Test-CompileError "defer_break"  "fn main() { loop { defer break } }" "'break' is not allowed inside a 'defer'"
Test-CompileError "defer_nested" "fn main() { defer { defer print(1) } }" "cannot nest 'defer'"
Test-CompileError "ptr_cast_bad" "fn main() { var x = ptr_cast(i32, 0) }" "first argument to 'ptr_cast' must be a pointer type"

Test-RunOk "defer_lifo_and_loops" @"
var log_val: i64 = 0

fn record(d: i64) {
    log_val = log_val * 10 + d
}

fn early_return(x: i32): i32 {
    var ret = 100
    defer record(1)
    defer {
        record(2)
        ret = 999
    }
    if x > 0 {
        defer record(3)
        return ret + x
    }
    defer record(4)
    return ret
}

fn main() {
    var r = early_return(5)
    print(r)
    print(log_val)

    log_val = 0
    loop i in 0..4 {
        defer record(i64(i + 1))
        if i == 1 {
            continue
        }
        if i == 2 {
            break
        }
        record(9)
    }
    print(log_val)
}
"@ "105`n321`n9123"

Test-RunOk "sizeof_alignof_ptrcast_and_std" @"
import "std/mem.sb"
import "std/io.sb"
import "std/time.sb"
import "std/str.sb"

struct Header {
    tag: u8
    id: i32
    val: i64
}

fn main() {
    print(sizeof(u8))
    print(sizeof(i32))
    print(sizeof(f64))
    print(sizeof(*Header))
    print(sizeof(Header))
    print(alignof(Header))

    var arr[4]: i32 = [1, 2, 3, 4]
    print(sizeof(arr))

    // Heap allocation + ptr_cast + defer free_mem
    var raw = alloc(sizeof(Header))
    defer free_mem(raw)
    var h = ptr_cast(*Header, raw)
    h.tag = 7
    h.id  = 42
    h.val = 123456789
    print(h.tag)
    print(h.id)
    print(h.val)
    print(usize(h) == usize(raw))

    // Arena allocator
    var arena: Arena
    arena_init(&arena, 1024)
    defer arena_free(&arena)
    var p1 = ptr_cast(*i64, arena_alloc(&arena, sizeof(i64), alignof(i64)))
    var p2 = ptr_cast(*i64, arena_alloc(&arena, sizeof(i64), alignof(i64)))
    *p1 = 111
    *p2 = 222
    print(*p1 + *p2)

    // std/str.sb
    print(cstr_len("Antimony"))
    print(cstr_eq("sb", "sb"))
    var digits[5]: u8 = [45, 52, 50, 48, 49]
    print(parse_i64(digits))

    // std/time.sb
    var t0 = perf_counter()
    var t1 = perf_counter()
    print(t1 >= t0)
    print(perf_freq() > 0)

    // std/io.sb
    var path: *u8 = "tests/tmp/std_io_test.bin"
    var payload[4]: u8 = [65, 66, 67, 68]
    print(write_file_bytes(path, payload))
    print(file_size(path))
    var read_buf[4]: u8
    print(read_file_into(path, read_buf))
    print(read_buf[0])
    print(read_buf[3])
    print(delete_file(path))
}
"@ "1`n4`n8`n8`n16`n8`n16`n7`n42`n123456789`ntrue`n333`n8`ntrue`n-4201`ntrue`ntrue`ntrue`n4`n4`n65`n68`ntrue"

# 13. Phase 8: Methods on Structs & Enums (fn (self: *T) / fn (self: T))
Test-CompileError "method_on_prim"   "fn (x: i32) foo() {}`nfn main() {}" "methods can only be defined on struct or enum types"
Test-CompileError "method_field_dup" "struct S { x: i32 }`nfn (s: *S) x() {}`nfn main() {}" "method 'x' conflicts with field 'S.x'"
Test-CompileError "method_unknown"   "struct S { x: i32 }`nfn main() { var s = S { x: 1 }`ns.nope() }" "type 'S' has no method named 'nope'"
Test-CompileError "method_lit_ptr"   "struct S { x: i32 }`nfn (s: *S) inc() { s.x += 1 }`nfn main() { (S { x: 1 }).inc() }" "cannot call pointer-receiver method 'S.inc' on non-addressable 'S' value"
Test-CompileError "method_val_mut"   "struct S { x: i32 }`nfn (s: S) bad() { s.x = 2 }`nfn main() {}" "cannot mutate or take address of read-only parameter 's'"

Test-RunOk "methods_struct_and_enum" @"
import "std/mem.sb"

enum State {
    Idle
    Running
    Done
}

fn (s: State) weight(): i32 {
    match s {
        State.Idle    { return 1 }
        State.Running { return 10 }
        State.Done    { return 100 }
    }
}

fn (s: *State) advance() {
    match *s {
        State.Idle    { *s = State.Running }
        State.Running { *s = State.Done }
        State.Done    { *s = State.Idle }
    }
}

struct Vec2 {
    x: i32
    y: i32
}

struct Entity {
    id: i32
    pos: Vec2
}

fn (v: *Vec2) translate(dx: i32, dy: i32) {
    v.x += dx
    v.y += dy
}

fn (v: *Vec2) add_xy(dx: i32, dy: i32): *Vec2 {
    v.x += dx
    v.y += dy
    return v
}

fn (v: Vec2) length_sq(): i32 {
    return v.x * v.x + v.y * v.y
}

fn (v: Vec2) dot(other: Vec2): i32 {
    return v.x * other.x + v.y * other.y
}

fn main() {
    var v = Vec2 { x: 3, y: 4 }
    print(v.length_sq())

    // Auto-reference value `v` to `*Vec2`
    v.translate(1, -2)
    print(v.x)
    print(v.y)

    // Auto-dereference pointer `pv` to `Vec2`
    var pv: *Vec2 = &v
    print(pv.length_sq())
    pv.translate(2, 1)
    print(v.x)
    print(v.y)

    // Chained method calls returning `*Vec2`
    v.add_xy(4, 3).add_xy(10, 20)
    print(v.x)
    print(v.y)

    // Struct value argument to method
    var w = Vec2 { x: 2, y: 3 }
    print(v.dot(w))

    // Nested field and array element method calls
    var e = Entity { id: 1, pos: Vec2 { x: 10, y: 20 } }
    e.pos.translate(5, 6)
    print(e.pos.x)
    print(e.pos.y)

    var arr[2]: Vec2 = [Vec2 { x: 1, y: 2 }, Vec2 { x: 7, y: 8 }]
    arr[1].translate(3, 2)
    print(arr[1].length_sq())

    // Enum methods (on variants, values, and pointers)
    var st = State.Idle
    print(st.weight())
    st.advance()
    print(st.weight())
    st.advance()
    print(st.weight())
    print(State.Running.weight())

    // Arena methods from std/mem.sb (including uninitialized struct receiver init)
    var arena: Arena
    arena.init(512)
    defer arena.free()
    var p = ptr_cast(*i32, arena.alloc(sizeof(i32), alignof(i32)))
    *p = 777
    print(*p)
}
"@ "25`n4`n2`n20`n6`n3`n20`n26`n118`n15`n26`n200`n1`n10`n100`n10`n777"

# 14. Phase 9: Value Returns (struct, str, []T), str Parameters/Fields, and str Indexing/Slicing/Equality
Test-CompileError "str_index_oob_const"  "fn main() { var b = `"abc`"[3] }" "string index 3 out of bounds"
Test-CompileError "str_slice_oob_const"  "fn main() { var s = `"abc`"[0..4] }" "slice upper bound 4 exceeds string length 3"
Test-CompileError "str_index_mutate"     "fn main() { var s = `"abc`"`ns[0] = 65 }" "cannot mutate or take address of byte in immutable 'str'"
Test-CompileError "ret_local_slice"      "fn bad(): []i32 { var a[3]: i32 = [1, 2, 3]`nreturn a[0..2] }`nfn main() {}" "cannot return slice of local variable 'a'"
Test-CompileError "ret_fixed_array"      "fn bad(): [3]i32 { var a[3]: i32 = [1, 2, 3]`nreturn a }`nfn main() {}" "returning fixed-size array '[3]i32' by value is not allowed"

Test-RunTrap "str_index_oob_runtime" @"
fn get_ch(s: str, i: i64): u8 {
    return s[i]
}
fn main() {
    print(get_ch("hello", 5))
}
"@ "index out of bounds"

Test-RunTrap "str_slice_oob_runtime" @"
fn sub(s: str, a: i64, b: i64): str {
    return s[a..b]
}
fn main() {
    print(sub("hello", 2, 6))
}
"@ "slice upper bound > length"

Test-RunOk "phase9_value_returns_and_strings" @"
import "std/str.sb"

struct Vec3 {
    x: i32
    y: i32
    z: i32
}

fn make_vec3(x: i32, y: i32, z: i32): Vec3 {
    return Vec3 { x: x, y: y, z: z }
}

fn (a: Vec3) add(b: Vec3): Vec3 {
    return Vec3 { x: a.x + b.x, y: a.y + b.y, z: a.z + b.z }
}

fn (a: Vec3) scale_with_defer(k: i32, trace: *i32): Vec3 {
    defer { *trace += 100 }
    return Vec3 { x: a.x * k, y: a.y * k, z: a.z * k }
}

fn (a: Vec3) sum(): i32 {
    return a.x + a.y + a.z
}

struct Token {
    lexeme: str
    digits: []i32
    line: i32
}

fn make_token(src: str, lo: i64, hi: i64, nums[]: i32, ln: i32): Token {
    return Token {
        lexeme: src[lo..hi],
        digits: nums[1..3],
        line: ln
    }
}

fn tail_slice(s[]: i32, start: i64): []i32 {
    return s[start..]
}

fn choose_greeting(flag: bool, s: str): str {
    defer {
        // verify defer preserves (RAX=ptr, RDX=len) on str return
        var dummy = 1 + 2
        if dummy == 0 { print(dummy) }
    }
    if flag {
        return s[0..5]
    }
    return s[6..14]
}

fn main() {
    // 1. Struct return by value + chained value-returning methods + field access on call
    var v1 = make_vec3(1, 2, 3)
    var v2 = make_vec3(10, 20, 30)
    var v3 = v1.add(v2).add(make_vec3(100, 200, 300))
    print(v3.x)
    print(v3.y)
    print(v3.z)
    print(make_vec3(7, 8, 9).y)
    print(v1.add(v2).sum())

    // Self-assignment with value return + defer on struct return
    var trace = 5
    v1 = v1.scale_with_defer(4, &trace)
    print(v1.x)
    print(v1.y)
    print(v1.z)
    print(trace)

    // 2. str parameters, str returns, str slicing, str indexing, str equality, loop over str
    var msg = "hello Antimony!"
    print(choose_greeting(true, msg))
    print(choose_greeting(false, msg))
    print(msg[0])
    print(msg[6])
    print(msg[0..5] == "hello")
    print(msg[6..14] == "Antimony")
    print(msg[0..5] != "world")

    var ascii_sum: i64 = 0
    loop i, ch in "ABC" {
        ascii_sum += i * 100 + i64(ch)
    }
    print(ascii_sum)

    // 3. Struct with str and []i32 fields + returning []i32 slice by value
    var arr[4]: i32 = [10, 20, 30, 40]
    var tok = make_token("fn main()", 3, 7, arr, 42)
    print(tok.lexeme)
    print(tok.lexeme == "main")
    print(len(tok.digits))
    print(tok.digits[0])
    print(tok.digits[1])
    print(tok.line)

    var sub[] = tail_slice(arr, 2)
    print(len(sub))
    print(sub[0])
    print(sub[1])

    // 4. std/str.sb rich str utilities
    print(str_starts_with("antimony.sb", "anti"))
    print(str_ends_with("antimony.sb", ".sb"))
    print(str_trim("   fast systems   "))
    print(str_find_byte("a:b:c", 58))
    print(str_parse_i64("  -98765  "))
}
"@ "111`n222`n333`n8`n66`n4`n8`n12`n105`nhello`nAntimony`n104`n65`ntrue`ntrue`ntrue`n498`nmain`ntrue`n2`n20`n30`n42`n2`n30`n40`ntrue`ntrue`nfast systems`n1`n-98765"

Write-Host "`nAll tests passed."
