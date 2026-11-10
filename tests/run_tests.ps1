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

# 15. Phase 10: match on str, str([]u8) views, StringBuilder, std/os.sb
Test-CompileError "match_str_no_else"   "fn main() { var s = `"a`"`nmatch s {`n`"a`" { print(1) }`n} }" "non-exhaustive 'match' on 'str': an 'else' branch is required"
Test-CompileError "match_str_dup"       "fn main() { var s = `"a`"`nmatch s {`n`"a`" { print(1) }`n`"b`", `"a`" { print(2) }`nelse { print(3) }`n} }" "duplicate match pattern"
Test-CompileError "match_str_nonlit"    "fn main() { var s = `"a`"`nvar t = `"b`"`nmatch s {`nt { print(1) }`nelse { print(2) }`n} }" "string match pattern must be a string literal"
Test-CompileError "str_conv_bad"        "fn main() { var a[3]: i32 = [1, 2, 3]`nvar s = str(a) }" "cannot convert '[3]i32' to 'str'"

Test-RunOk "phase10_strings_and_os" @"
import "std/str.sb"
import "std/os.sb"
import "std/io.sb"

fn command_id(cmd: str): i32 {
    match cmd {
        "build", "b" {
            return 1
        }
        "run", "r" {
            return 2
        }
        "" {
            return 3
        }
        else {
            return 0
        }
    }
}

fn main() {
    // 1. match on str
    print(command_id("build"))
    print(command_id("b"))
    print(command_id("run"))
    print(command_id("r"))
    print(command_id(""))
    print(command_id("test"))
    print(command_id("buil"))

    // 2. zero-copy str views over byte arrays/slices
    var bytes[5]: u8 = [72, 101, 108, 108, 111]
    var view = str(bytes)
    print(view)
    print(view == "Hello")
    print(str(bytes[1..4]))
    print(len(str(bytes[2..])))

    // 3. StringBuilder
    var sb: StringBuilder
    sb.init(4)
    defer sb.free()
    sb.append("value=")
    sb.append_i64(-1234567)
    sb.append_byte(59)
    sb.append(" ok")
    loop i in 0..20 {
        sb.append_byte(u8(97 + i % 26))
    }
    print(sb.as_str())
    print(i64(sb.len))
    print(sb.as_str()[0..6] == "value=")
    sb.clear()
    sb.append_i64(0)
    sb.append_i64(9223372036854775807)
    print(sb.as_str())

    // 4. C string <-> str interop
    print(cstr_to_str(sb.cstr()) == sb.as_str())
    print(len(str_from_ptr(ptr_cast(*u8, "abcdef"), 3)))

    // 5. command-line parsing helpers (deterministic input)
    var line = "prog.exe  build \"C:\\My Dir\\a.sb\" -o out.exe"
    print(parse_arg_count(line))
    print(parse_arg_at(line, 0))
    print(parse_arg_at(line, 1))
    print(parse_arg_at(line, 2))
    print(parse_arg_at(line, 4))
    print(parse_arg_at(line, 9) == "")
    print(arg_count() >= 1)

    // 6. raw stdout/stderr and file I/O with str
    print_raw("raw-")
    print_raw("output\n")
    eprint("this goes to stderr")
    var path = "phase10_tmp.txt"
    print(write_file_str(path, "Antimony str file"))
    print(file_size(path))
    print(delete_file(path))

    // 7. environment variables
    var envbuf[260]: u8
    print(len(getenv_into("SystemRoot", envbuf)) > 0)
    print(getenv_into("ANTIMONY_SURELY_UNSET_VAR_42", envbuf) == "")
}
"@ "1`n1`n2`n2`n3`n0`n0`nHello`ntrue`nell`n3`nvalue=-1234567; okabcdefghijklmnopqrst`n38`ntrue`n09223372036854775807`ntrue`n3`n5`nprog.exe`nbuild`nC:\My Dir\a.sb`nout.exe`ntrue`ntrue`nraw-output`ntrue`n17`ntrue`ntrue`ntrue"

# 19. Phase 11: Optimizing / LLVM backend (--llvm / -O3 and --emit-llvm)
function Test-RunLlvm([string]$name, [string]$code, [string]$expected) {
    $sb = Join-Path $tmp "$name.sb"
    $exe = Join-Path $tmp "$name.exe"
    $ll = Join-Path $tmp "$name.ll"
    Set-Content -Path $sb -Value $code -Encoding UTF8
    $null = & $sbc $sb -o $ll --emit-llvm 2>&1
    if ($LASTEXITCODE -ne 0 -or !(Test-Path $ll)) { throw "FAIL [$name]: failed --emit-llvm" }
    $optOut = (& $sbc $sb -o $exe --llvm 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "FAIL [$name]: failed to compile with --llvm:`n$optOut" }
    $out = ((& $exe) -join "`n").Replace("`r`n", "`n").Trim()
    if ($LASTEXITCODE -ne 0) { throw "FAIL [$name]: exited with code $LASTEXITCODE" }
    if ($out -ne $expected) {
        throw "FAIL [$name]: expected '$expected', got '$out'"
    }
    Write-Host "PASS [llvm-O3]       $name"
}

Test-RunLlvm "phase11_llvm_opt_backend" @"
import "std/str"

struct Pair {
    a: i64
    b: i64
}

fn make_pair(a: i64, b: i64): Pair {
    return Pair { a: a, b: b }
}

fn (self: Pair) sum(): i64 {
    return self.a + self.b
}

fn fib(n: i32): i32 {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}

fn main() {
    var p = make_pair(40, 2)
    print(p.sum())
    print(fib(15))

    var sb: StringBuilder
    sb.init(8)
    defer sb.free()
    sb.append("llvm=")
    sb.append_i64(p.sum())
    print(sb.as_str())
}
"@ "42`n610`nllvm=42"

# 20. Phase 12: Tagged Unions (Payload Enums), Pattern-Matching Destructuring, and `?` Operator
Test-CompileError "tagged_enum_missing_payload" @"
enum Opt { Some(i32), None }
fn main() {
    var x: Opt = Opt.Some
}
"@ "requires a payload of type"

Test-CompileError "tagged_enum_unwanted_payload" @"
enum Opt { Some(i32), None }
fn main() {
    var x: Opt = Opt.None(1)
}
"@ "does not take a payload"

Test-CompileError "inferred_enum_no_expected" @"
enum Opt { Some(i32), None }
fn main() {
    var x = .None
}
"@ "requires a known enum type context"

Test-CompileError "match_multi_payload_binding" @"
enum Msg { A(i32), B(i32), C }
fn main() {
    var m = Msg.A(1)
    match m {
        Msg.A(x), Msg.B(x) { print(x) }
        Msg.C {}
    }
}
"@ "cannot combine payload-binding pattern"

Test-CompileError "try_outside_result_fn" @"
enum Res { Ok(i32), Err(str) }
fn fail(): Res { return .Err("bad") }
fn main() {
    var x = fail()?
    print(x)
}
"@ "enclosing function must return a compatible 'Result' or 'Option' enum"

Test-CompileError "try_err_type_mismatch" @"
enum ResA { Ok(i32), Err(i32) }
enum ResB { Ok(i32), Err(str) }
fn a(): ResA { return .Err(404) }
fn b(): ResB {
    var v = a()?
    return .Ok(v)
}
fn main() {}
"@ "cannot propagate 'ResA.Err' via '?'"

$phase12_code = @"
struct Point {
    x: i32
    y: i32
}

enum Shape {
    Circle(i32),
    Rect(Point),
    Label(str),
    Empty
}

enum OptionI32 {
    Some(i32),
    None
}

enum ParseRes {
    Ok(i32),
    Err(str)
}

enum PointRes {
    Ok(Point),
    Err(str)
}

fn (self: Shape) describe(): i32 {
    match self {
        .Circle(r) { return r * r }
        Shape.Rect(pt) { return pt.x + pt.y }
        .Label(s) { return i32(len(s)) }
        .Empty { return 0 }
    }
}

fn find_pos(arr: []i32, target: i32): OptionI32 {
    loop i, v in arr {
        if v == target {
            return .Some(i32(i))
        }
    }
    return .None
}

fn sum_two_targets(arr: []i32, a: i32, b: i32): OptionI32 {
    defer print("defer-opt")
    var idx_a = find_pos(arr, a)?
    var idx_b = find_pos(arr, b)?
    return .Some(idx_a + idx_b)
}

fn parse_positive(s: str): ParseRes {
    if len(s) == 0 {
        return .Err("empty input")
    }
    if s[0] == 45 {
        return .Err("negative number")
    }
    var acc: i32 = 0
    loop b in s {
        if b < 48 || b > 57 {
            return .Err("invalid digit")
        }
        acc = acc * 10 + i32(b - 48)
    }
    return .Ok(acc)
}

fn parse_point(sx: str, sy: str): PointRes {
    defer print("defer-parse-point")
    var x = parse_positive(sx)?
    var y = parse_positive(sy)?
    return .Ok(Point { x: x, y: y })
}

fn main() {
    // 1. Tagged union construction, inferred `.Variant`, methods, and destructuring match
    var s1: Shape = .Circle(7)
    var s2 = Shape.Rect(Point { x: 15, y: 27 })
    var s3: Shape = .Label("Antimony")
    var s4: Shape = .Empty
    print(s1.describe())
    print(s2.describe())
    print(s3.describe())
    print(s4.describe())

    // 2. Array of tagged unions & iteration
    var shapes[3]: Shape = [
        Shape.Circle(3),
        Shape.Rect(Point { x: 10, y: 20 }),
        Shape.Empty
    ]
    var total: i32 = 0
    loop sh in shapes {
        total += sh.describe()
    }
    print(total)

    // 3. Option `?` propagation (both Some and None paths + defer execution)
    var nums[4]: i32 = [10, 20, 30, 40]
    match sum_two_targets(nums, 20, 40) {
        .Some(v) { print(v) }
        .None { print(-1) }
    }
    match sum_two_targets(nums, 20, 99) {
        .Some(v) { print(v) }
        .None { print(-1) }
    }

    // 4. Result `?` propagation (Ok and Err paths with `str` payload + struct Ok payload + defer)
    match parse_point("12", "34") {
        .Ok(pt) { print(pt.x * 100 + pt.y) }
        .Err(msg) { print(msg) }
    }
    match parse_point("12", "-5") {
        .Ok(pt) { print(pt.x * 100 + pt.y) }
        .Err(msg) { print(msg) }
    }
    match parse_point("12x", "34") {
        .Ok(pt) { print(pt.x * 100 + pt.y) }
        .Err(msg) { print(msg) }
    }
}
"@
$phase12_expected = "49`n42`n8`n0`n39`ndefer-opt`n4`ndefer-opt`n-1`ndefer-parse-point`n1234`ndefer-parse-point`nnegative number`ndefer-parse-point`ninvalid digit"

Test-RunOk "phase12_tagged_unions_and_try" $phase12_code $phase12_expected
Test-RunLlvm "phase12_tagged_unions_and_try_llvm" $phase12_code $phase12_expected

# 21. Phase 13: Compile-Time Generics ([T]) & Standard Collections (Vec[T], HashMap[K, V])
Test-CompileError "generic_type_arity_mismatch" @"
struct Box[T] { val: T }
fn main() {
    var b: Box[i32, i64]
}
"@ "expects 1 type argument(s), got 2"

Test-CompileError "generic_fn_infer_fail" @"
fn id[T](x: i32): T {
    return 0
}
fn main() {
    var y = id(5)
}
"@ "cannot infer type parameter 'T' for call to 'id'"

Test-CompileError "generic_fn_arity_mismatch" @"
fn pair_sum[T](a: T, b: T): T {
    return a + b
}
fn main() {
    print(pair_sum[i32, i64](1, 2))
}
"@ "expects 1 type argument(s), got 2"

$phase13_code = @"
import "std/vec.sb"
import "std/map.sb"

struct Pair[A, B] {
    first: A
    second: B
}

fn (p: Pair[A, B]) swap(): Pair[B, A] {
    return Pair[B, A] { first: p.second, second: p.first }
}

enum Result[T, E] {
    Ok(T)
    Err(E)
}

fn max_val[T](a: T, b: T): T {
    if a > b {
        return a
    }
    return b
}

fn swap_val[T](a: *T, b: *T) {
    var tmp = *a
    *a = *b
    *b = tmp
}

fn checked_div(a: i64, b: i64): Result[i64, str] {
    if b == 0 {
        return .Err("div by zero")
    }
    return .Ok(a / b)
}

fn compute_ratio(x: i64, y: i64, z: i64): Result[Pair[i64, i64], str] {
    var q1 = checked_div(x, y)?
    var q2 = checked_div(q1, z)?
    return .Ok(Pair[i64, i64] { first: q1, second: q2 })
}

struct Point {
    x: i32
    y: i32
}

fn main() {
    // 1. Generic struct, generic method, and nested generic struct
    var p = Pair[i32, str] { first: 42, second: "hello" }
    print(p.first)
    print(p.second)
    var sw = p.swap()
    print(sw.first)
    print(sw.second)

    // 2. Generic functions (both explicit [T] and type-inferred calls)
    print(max_val[i64](100, 250))
    print(max_val(99, 12))
    var x: i32 = 10
    var y: i32 = 90
    swap_val(&x, &y)
    print(x)
    print(y)

    var s1 = "alpha"
    var s2 = "beta"
    swap_val(&s1, &s2)
    print(s1)
    print(s2)

    // 3. Generic Result[T, E] with `?` operator propagation
    match compute_ratio(100, 5, 4) {
        .Ok(res) {
            print(res.first)
            print(res.second)
        }
        .Err(msg) { print(msg) }
    }
    match compute_ratio(100, 0, 4) {
        .Ok(res) { print(res.first) }
        .Err(msg) { print(msg) }
    }

    // 4. Standard generic dynamic array Vec[T] (Vec[i64], Vec[str], Vec[Point])
    var nums: Vec[i64]
    nums.init(2)
    defer nums.free()
    loop i in 1..7 {
        nums.push(i64(i * 10))
    }
    print(nums.len)
    print(nums.pop())
    nums.set(0, 99)
    var sum: i64 = 0
    loop v in nums.as_slice() {
        sum += v
    }
    print(sum)

    var words: Vec[str]
    words.init(0)
    defer words.free()
    words.push("Antimony")
    words.push("Generics")
    words.push("Fast")
    print(words.len)
    print(words.get(0))
    print(words.get(1))
    print(words.pop())

    var pts: Vec[Point]
    pts.init(2)
    defer pts.free()
    pts.push(Point { x: 3, y: 4 })
    pts.push(Point { x: 10, y: 20 })
    var pt_sum: i32 = 0
    loop pt in pts.as_slice() {
        pt_sum += pt.x + pt.y
    }
    print(pt_sum)

    // 5. Standard generic hash table HashMap[K, V] (str keys and integer keys)
    var scores: HashMap[str, i64]
    scores.init(4)
    defer scores.free()
    scores.put("alice", 100)
    scores.put("bob", 200)
    scores.put("charlie", 300)
    scores.put("alice", 150)
    print(scores.len)
    print(scores.contains("bob"))
    print(scores.contains("dave"))
    // Lookup using a sliced string (different pointer, same content)
    var full = "prefix:alice:suffix"
    var sliced_key = full[7..12]
    match scores.get(sliced_key) {
        .Some(v) { print(v) }
        .None { print(-1) }
    }
    print(scores.get_or("dave", -99))
    print(scores.remove("bob"))
    print(scores.contains("bob"))
    print(scores.len)

    var id_map: HashMap[i32, str]
    id_map.init(4)
    defer id_map.free()
    loop k in 1..21 {
        if k % 2 == 0 {
            id_map.put(k, "even")
        } else {
            id_map.put(k, "odd")
        }
    }
    print(id_map.len)
    print(id_map.get_or(7, "none"))
    print(id_map.get_or(18, "none"))
    print(id_map.get_or(99, "none"))
}
"@
$phase13_expected = "42`nhello`nhello`n42`n250`n99`n90`n10`nbeta`nalpha`n20`n5`ndiv by zero`n6`n60`n239`n3`nAntimony`nGenerics`nFast`n37`n3`ntrue`nfalse`n150`n-99`ntrue`nfalse`n2`n20`nodd`neven`nnone"

Test-RunOk "phase13_generics_and_collections" $phase13_code $phase13_expected
Test-RunLlvm "phase13_generics_and_collections_llvm" $phase13_code $phase13_expected

# 22. Phase 14: First-Class Function Pointers, Anonymous Functions & std/sort.sb
Test-CompileError "fnptr_call_non_fn" @"
fn main() {
    var x: i32 = 42
    x(10)
}
"@ "'x' is not a function"

Test-CompileError "fnptr_wrong_arg_count" @"
fn add(a: i32, b: i32): i32 { return a + b }
fn main() {
    var f: fn(i32, i32): i32 = add
    print(f(1))
}
"@ "expects 2 argument(s), got 1"

Test-CompileError "fnptr_type_mismatch" @"
fn add(a: i32, b: i32): i32 { return a + b }
fn main() {
    var f: fn(i64): i64 = add
}
"@ "type mismatch: expected 'fn(i64):i64', got 'fn(i32,i32):i32'"

Test-CompileError "anon_fn_no_capture" @"
fn main() {
    var outer: i32 = 10
    var f = fn(x: i32): i32 { return x + outer }
    print(f(5))
}
"@ "undeclared identifier 'outer'"

Test-RunTrap "fnptr_null_call_trap" @"
fn main() {
    var f: fn(i32): i32 = null
    print(f(42))
}
"@ "null function pointer call"

$phase14_code = @"
import "std/sort.sb"

fn add(a: i64, b: i64): i64 { return a + b }
fn mul(a: i64, b: i64): i64 { return a * b }

struct Handler {
    name: str
    op: fn(i64, i64): i64
}

fn choose_op(is_mul: bool): fn(i64, i64): i64 {
    if is_mul {
        return mul
    }
    return add
}

fn fold[T](s: []T, init: T, combine: fn(T, T): T): T {
    var acc = init
    loop v in s {
        acc = combine(acc, v)
    }
    return acc
}

struct Player {
    name: str
    score: i32
}

fn main() {
    // 1. Function pointer variables, equality with null, and returning function pointers
    var fp: fn(i64, i64): i64 = null
    print(fp == null)
    fp = choose_op(false)
    print(fp != null)
    print(fp == add)
    print(fp(15, 27))
    print(choose_op(true)(6, 7))

    // 2. Function pointers inside structs and fixed arrays
    var h = Handler { name: "adder", op: add }
    print(h.name)
    print(h.op(100, 23))

    var ops: [2]fn(i64, i64): i64 = [add, mul]
    print(ops[0](10, 5))
    print(ops[1](10, 5))

    // 3. Stateless anonymous functions (`fn(...) { ... }`) & generic higher-order functions
    var vals: [5]i64 = [1, 2, 3, 4, 5]
    var sum = fold(vals, 0, fn(acc: i64, x: i64): i64 { return acc + x })
    var prod = fold(vals, 1, fn(acc: i64, x: i64): i64 { return acc * x })
    print(sum)
    print(prod)

    // 4. Standard generic sorting & binary search (`std/sort.sb`)
    var nums: [10]i32 = [42, 7, 19, -5, 100, 3, 42, 0, 88, 12]
    print(is_sorted(nums))
    sort(nums)
    print(is_sorted(nums))
    print(nums[0])
    print(nums[4])
    print(nums[9])

    print(binary_search(nums, 19))
    print(binary_search(nums, 999))

    // Custom comparator sort (descending order) using anonymous function
    sort_by(nums, fn(a: i32, b: i32): bool { return a > b })
    print(is_sorted_by(nums, fn(a: i32, b: i32): bool { return a > b }))
    print(nums[0])
    print(nums[9])

    reverse(nums)
    print(is_sorted(nums))

    // Sorting structs by field using `sort_by` + anonymous function
    var players: [4]Player = [
        Player { name: "Bob", score: 120 },
        Player { name: "Alice", score: 350 },
        Player { name: "Dave", score: 90 },
        Player { name: "Carol", score: 240 }
    ]
    sort_by(players, fn(a: Player, b: Player): bool { return a.score > b.score })
    print(players[0].name)
    print(players[1].name)
    print(players[2].name)
    print(players[3].name)
}
"@
$phase14_expected = "true`ntrue`ntrue`n42`n42`nadder`n123`n15`n50`n15`n120`nfalse`ntrue`n-5`n12`n100`n5`n-1`ntrue`n100`n-5`ntrue`nAlice`nCarol`nBob`nDave"

Test-RunOk "phase14_fnptr_and_sort" $phase14_code $phase14_expected
Test-RunLlvm "phase14_fnptr_and_sort_llvm" $phase14_code $phase14_expected

Write-Host "`nAll tests passed."
