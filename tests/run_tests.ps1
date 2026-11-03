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
    if ($out -notlike "*$expectSubstr*") {
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
    if ($out -notlike "*$expectSubstr*") {
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

Write-Host "`nAll tests passed."
