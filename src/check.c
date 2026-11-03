// check.c - type checking, untyped-constant resolution, definite-assignment
// analysis, return-path checking, and stack-slot assignment.
#include "sb.h"

Type ty_void          = {TY_VOID,          "void",  0, 1, false, NULL, 0, NULL, {0}, 0};
Type ty_bool          = {TY_BOOL,          "bool",  1, 1, false, NULL, 0, NULL, {0}, 0};
Type ty_str           = {TY_STR,           "str",  16, 8, false, NULL, 0, NULL, {0}, 0};
Type ty_untyped_int   = {TY_UNTYPED_INT,   "untyped int",   0, 1, true, NULL, 0, NULL, {0}, 0};
Type ty_untyped_float = {TY_UNTYPED_FLOAT, "untyped float", 0, 1, true, NULL, 0, NULL, {0}, 0};
Type ty_null          = {TY_NULL,          "null",  8, 8, false, NULL, 0, NULL, {0}, 0};

Type ty_i8    = {TY_INT,   "i8",    1, 1, true,  NULL, 0, NULL, {0}, 0};
Type ty_i16   = {TY_INT,   "i16",   2, 2, true,  NULL, 0, NULL, {0}, 0};
Type ty_i32   = {TY_INT,   "i32",   4, 4, true,  NULL, 0, NULL, {0}, 0};
Type ty_i64   = {TY_INT,   "i64",   8, 8, true,  NULL, 0, NULL, {0}, 0};
Type ty_isize = {TY_INT,   "isize", 8, 8, true,  NULL, 0, NULL, {0}, 0};
Type ty_u8    = {TY_INT,   "u8",    1, 1, false, NULL, 0, NULL, {0}, 0};
Type ty_u16   = {TY_INT,   "u16",   2, 2, false, NULL, 0, NULL, {0}, 0};
Type ty_u32   = {TY_INT,   "u32",   4, 4, false, NULL, 0, NULL, {0}, 0};
Type ty_u64   = {TY_INT,   "u64",   8, 8, false, NULL, 0, NULL, {0}, 0};
Type ty_usize = {TY_INT,   "usize", 8, 8, false, NULL, 0, NULL, {0}, 0};
Type ty_f32   = {TY_FLOAT, "f32",   4, 4, true,  NULL, 0, NULL, {0}, 0};
Type ty_f64   = {TY_FLOAT, "f64",   8, 8, true,  NULL, 0, NULL, {0}, 0};

bool is_int(Type *t) {
    return t && (t->kind == TY_INT || t->kind == TY_UNTYPED_INT);
}

bool is_float(Type *t) {
    return t && (t->kind == TY_FLOAT || t->kind == TY_UNTYPED_FLOAT);
}

static void type_bounds(Type *t, i128 *lo, i128 *hi) {
    int bits = t->size * 8;
    if (t->is_signed) {
        *lo = -((i128)1 << (bits - 1));
        *hi =  ((i128)1 << (bits - 1)) - 1;
    } else {
        *lo = 0;
        *hi = (bits == 64) ? (i128)UINT64_MAX : (((i128)1 << bits) - 1);
    }
}

bool fits(i128 v, Type *t) {
    if (t->kind != TY_INT) return false;
    i128 lo, hi;
    type_bounds(t, &lo, &hi);
    return v >= lo && v <= hi;
}

static i128 wrap_to_type(i128 v, Type *t) {
    int bits = t->size * 8;
    uint64_t mask = (bits == 64) ? UINT64_MAX : ((1ULL << bits) - 1);
    uint64_t u = (uint64_t)v & mask;
    if (t->is_signed) {
        uint64_t sign_bit = 1ULL << (bits - 1);
        if (u & sign_bit) {
            int64_t s = (int64_t)(u | ~mask);
            return (i128)s;
        }
    }
    return (i128)u;
}

// ---------------------------------------------------------------------------
// Scopes and symbol lookup
// ---------------------------------------------------------------------------

typedef struct Scope {
    struct Scope *parent;
    bool is_fn_root;        // function scope (below globals)
    VEC(Symbol *) syms;
} Scope;

typedef uint64_t AssignSet; // bit i = local variable i is definitely assigned
#define ALL_ASSIGNED UINT64_MAX

typedef struct {
    Scope *global;
    Scope *cur;
    Node *cur_fn;
    Type *ret_type;
    VEC(Node *) loops;
    int next_slot;
    int next_var_index;
    AssignSet assigned;     // current definite-assignment state
} Checker;

static Scope *push_scope(Checker *c, bool is_fn_root) {
    Scope *s = xcalloc(1, sizeof(Scope));
    s->parent = c->cur;
    s->is_fn_root = is_fn_root;
    c->cur = s;
    return s;
}

static void pop_scope(Checker *c) {
    c->cur = c->cur->parent;
}

static Symbol *lookup_in(Scope *s, const char *name) {
    for (int i = 0; i < s->syms.len; i++) {
        if (s->syms.items[i]->name == name) return s->syms.items[i];
    }
    return NULL;
}

static Symbol *lookup(Checker *c, const char *name) {
    for (Scope *s = c->cur; s; s = s->parent) {
        Symbol *sym = lookup_in(s, name);
        if (sym) return sym;
    }
    return NULL;
}

// Spec §4.5: shadowing inside a function is not allowed.
static Symbol *lookup_in_fn(Checker *c, const char *name) {
    for (Scope *s = c->cur; s && s != c->global; s = s->parent) {
        Symbol *sym = lookup_in(s, name);
        if (sym) return sym;
    }
    return NULL;
}

static Symbol *define(Checker *c, SymKind kind, const char *name, Type *type, Pos pos) {
    if (c->cur == c->global) {
        Symbol *old = lookup_in(c->global, name);
        if (old) fatal(pos, "redeclaration of '%s'", name);
    } else {
        Symbol *old = lookup_in_fn(c, name);
        if (old) {
            fatal(pos, "cannot redeclare '%s' (already declared at line %d; shadowing is not allowed)",
                  name, old->pos.line);
        }
        Symbol *g = lookup_in(c->global, name);
        if (g && (g->kind == SYM_TYPE || g->kind == SYM_BUILTIN)) {
            fatal(pos, "cannot shadow built-in '%s'", name);
        }
    }
    Symbol *sym = xcalloc(1, sizeof(Symbol));
    sym->kind = kind;
    sym->name = name;
    sym->type = type;
    sym->pos = pos;
    sym->is_global = (c->cur == c->global);
    sym->var_index = -1;
    sym->hw_reg = -1;
    vec_push(c->cur->syms, sym);
    return sym;
}

static void check_expr(Checker *c, Node *n, Type *expected);
static void resolve_global_const(Checker *c, Symbol *s);
static void layout_struct(Checker *c, Type *st);

static Type *resolve_type(Checker *c, const char *name, Pos pos) {
    Symbol *s = lookup_in(c->global, name);
    if (!s || s->kind != SYM_TYPE) {
        fatal(pos, "unknown type '%s'", name);
    }
    return s->type;
}

static VEC(Type *) array_types;
static VEC(Type *) slice_types;
static VEC(Type *) ptr_types;

static Type *make_ptr_type(Type *elem, Pos pos) {
    if (elem == &ty_void || elem == &ty_str || elem->kind == TY_SLICE) {
        fatal(pos, "pointer to '%s' is not supported yet", elem->name);
    }
    for (int i = 0; i < ptr_types.len; i++) {
        Type *t = ptr_types.items[i];
        if (t->elem == elem) return t;
    }
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "*%s", elem->name);
    Type *t = xcalloc(1, sizeof(Type));
    t->kind = TY_PTR;
    t->name = intern(buf, n);
    t->elem = elem;
    t->size = 8;
    t->align = 8;
    vec_push(ptr_types, t);
    return t;
}

static Type *make_array_type(Type *elem, int len, Pos pos) {
    if (len <= 0) fatal(pos, "array length must be > 0 (got %d)", len);
    if (elem == &ty_str || elem->kind == TY_ARRAY || elem->kind == TY_SLICE) {
        fatal(pos, "arrays of '%s' are not supported yet", elem->name);
    }
    for (int i = 0; i < array_types.len; i++) {
        Type *t = array_types.items[i];
        if (t->elem == elem && t->array_len == len) return t;
    }
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "[%d]%s", len, elem->name);
    Type *t = xcalloc(1, sizeof(Type));
    t->kind = TY_ARRAY;
    t->name = intern(buf, n);
    t->elem = elem;
    t->array_len = len;
    t->size = elem->size * len;
    t->align = elem->align ? elem->align : 8;
    vec_push(array_types, t);
    return t;
}

static Type *make_slice_type(Type *elem, Pos pos) {
    if (elem == &ty_str || elem->kind == TY_ARRAY || elem->kind == TY_SLICE) {
        fatal(pos, "slices of '%s' are not supported yet", elem->name);
    }
    for (int i = 0; i < slice_types.len; i++) {
        Type *t = slice_types.items[i];
        if (t->elem == elem) return t;
    }
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "[]%s", elem->name);
    Type *t = xcalloc(1, sizeof(Type));
    t->kind = TY_SLICE;
    t->name = intern(buf, n);
    t->elem = elem;
    t->size = 16; // {ptr, len}
    t->align = 8;
    vec_push(slice_types, t);
    return t;
}

static int alloc_slot(Checker *c) {
    return ++c->next_slot;
}

// Allocates `nbytes` on the function stack (8-byte aligned); returns the TOP
// slot index `s` such that `[RBP - 8*s]` is the lowest address (base pointer)
// of the contiguous region `[RBP - 8*s .. RBP - 8*(s - nslots + 1)]`.
static int alloc_slots_bytes(Checker *c, int nbytes) {
    int nslots = (nbytes + 7) / 8;
    if (nslots < 1) nslots = 1;
    c->next_slot += nslots;
    return c->next_slot;
}

static int eval_array_len(Checker *c, Node *len_expr) {
    check_expr(c, len_expr, &ty_i64);
    if (!len_expr->is_const) {
        fatal(len_expr->pos, "array length must be a compile-time constant");
    }
    if (len_expr->cval <= 0 || len_expr->cval > 10000000) {
        fatal(len_expr->pos, "array length must be between 1 and 10,000,000");
    }
    return (int)len_expr->cval;
}

static Type *resolve_decl_type(Checker *c, Node *d) {
    if (!d->type_name) return NULL;
    Type *base = resolve_type(c, d->type_name, d->type_pos);
    if (d->ptr_depth == 0 && base->kind == TY_STRUCT && base->state != 2) {
        layout_struct(c, base);
    }
    for (int i = 0; i < d->ptr_depth; i++) {
        base = make_ptr_type(base, d->type_pos);
    }
    if (d->decl_kind == DECL_ARRAY) {
        int n = eval_array_len(c, d->len_expr);
        return make_array_type(base, n, d->pos);
    }
    if (d->decl_kind == DECL_SLICE) {
        return make_slice_type(base, d->pos);
    }
    return base;
}

static void layout_struct(Checker *c, Type *st) {
    if (st->state == 2) return;
    if (st->state == 1) {
        fatal(st->decl->pos, "recursive struct '%s' cannot contain itself by value (use '*%s')",
              st->name, st->name);
    }
    st->state = 1;
    Node *d = st->decl;
    if (d->list.len == 0) {
        fatal(d->pos, "struct '%s' must have at least one field", st->name);
    }
    int offset = 0;
    int max_align = 1;
    for (int i = 0; i < d->list.len; i++) {
        Node *fd = d->list.items[i];
        for (int j = 0; j < st->fields.len; j++) {
            if (st->fields.items[j].name == fd->name) {
                fatal(fd->pos, "duplicate field '%s' in struct '%s'", fd->name, st->name);
            }
        }
        Type *ft = resolve_decl_type(c, fd);
        if (ft == &ty_str || ft->kind == TY_SLICE) {
            fatal(fd->pos, "field '%s' of type '%s' is not supported in structs yet", fd->name, ft->name);
        }
        int a = ft->align ? ft->align : 8;
        if (a > max_align) max_align = a;
        if (offset % a != 0) offset += a - (offset % a);

        StructField sf = {fd->name, ft, offset, fd->pos};
        vec_push(st->fields, sf);
        offset += ft->size;
    }
    if (offset % max_align != 0) offset += max_align - (offset % max_align);
    st->size = offset;
    st->align = max_align;
    st->state = 2;
}

static StructField *find_field(Type *st, const char *name) {
    for (int i = 0; i < st->fields.len; i++) {
        if (st->fields.items[i].name == name) return &st->fields.items[i];
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Expressions and constant folding (spec §3, §7, §8.2)
// ---------------------------------------------------------------------------

static void check_expr(Checker *c, Node *n, Type *expected);
static void resolve_global_const(Checker *c, Symbol *s);

// Locks an expression (and any untyped sub-expressions) to a concrete type,
// checking that all constant values fit in that type.
static void coerce(Node *n, Type *target) {
    if (!target) return;
    if (n->type == target) {
        if (n->is_const && target->kind == TY_INT && !fits(n->cval, target)) {
            char buf[48];
            fatal(n->pos, "constant value %s overflows '%s'", i128_str(n->cval, buf), target->name);
        }
        return;
    }
    // Fixed-size array `[N]T` implicitly coerces to slice `[]T` (e.g. when passed to a function).
    if (target->kind == TY_SLICE && n->type && n->type->kind == TY_ARRAY &&
        n->type->elem == target->elem) {
        n->implicit_slice = true;
        return;
    }
    // `null` coerces to any pointer type `*T`.
    if (target->kind == TY_PTR && n->type == &ty_null) {
        n->type = target;
        return;
    }
    if (n->type == &ty_untyped_float) {
        if (target->kind != TY_FLOAT) {
            fatal(n->pos, "cannot use float constant as '%s'", target->name);
        }
        if (n->kind == N_UNARY) {
            coerce(n->lhs, target);
        } else if (n->kind == N_BINARY) {
            coerce(n->lhs, target);
            coerce(n->rhs, target);
        }
        n->type = target;
        return;
    }
    if (n->type == &ty_untyped_int && target->kind == TY_FLOAT) {
        if (n->kind == N_UNARY) {
            coerce(n->lhs, target);
        } else if (n->kind == N_BINARY) {
            coerce(n->lhs, target);
            coerce(n->rhs, target);
        }
        n->fval = (double)n->cval;
        n->type = target;
        return;
    }
    if (n->type == &ty_untyped_int) {
        if (target->kind != TY_INT) {
            fatal(n->pos, "cannot use integer constant as '%s'", target->name);
        }
        if (n->kind == N_UNARY) {
            // Do not range-check the positive operand of `-2147483648` before negation;
            // only check the final negated value on `n` below.
            if (n->op != T_MINUS) coerce(n->lhs, target);
            else n->lhs->type = target;
        } else if (n->kind == N_BINARY) {
            if (n->op == T_SHL || n->op == T_SHR) {
                coerce(n->lhs, target);
                if (n->rhs->type == &ty_untyped_int) coerce(n->rhs, &ty_u32);
            } else {
                coerce(n->lhs, target);
                coerce(n->rhs, target);
            }
        }
        if (n->is_const && !fits(n->cval, target)) {
            char buf[48];
            fatal(n->pos, "constant value %s does not fit in '%s'", i128_str(n->cval, buf), target->name);
        }
        n->type = target;
        return;
    }
    fatal(n->pos, "type mismatch: expected '%s', got '%s' (implicit conversions are not allowed; use '%s(...)')",
          target->name, n->type->name, target->name);
}

static void materialize(Node *n) {
    if (n->type == &ty_untyped_int) {
        coerce(n, &ty_i32);
    } else if (n->type == &ty_untyped_float) {
        coerce(n, &ty_f64);
    } else if (n->type == &ty_null) {
        fatal(n->pos, "'null' requires an explicit pointer type (e.g. 'var p: *i32 = null')");
    }
}

static i128 eval_const_unary(Pos pos, int op, i128 v, Type *t) {
    switch (op) {
    case T_MINUS:
        if (t && t->kind == TY_INT && !t->is_signed && v != 0) {
            fatal(pos, "cannot negate unsigned value");
        }
        return -v;
    case T_BANG:
        return !v;
    case T_TILDE:
        if (t && t->kind == TY_INT) return wrap_to_type(~v, t);
        return ~v;
    default:
        return v;
    }
}

static i128 eval_const_binary(Pos pos, int op, i128 a, i128 b, Type *lt) {
    switch (op) {
    case T_PLUS:    return a + b;
    case T_MINUS:   return a - b;
    case T_STAR:    return a * b;
    case T_SLASH:
        if (b == 0) fatal(pos, "division by zero in constant expression");
        return a / b;
    case T_PERCENT:
        if (b == 0) fatal(pos, "remainder by zero in constant expression");
        return a % b;
    case T_PLUSW:
        if (lt->kind != TY_INT) fatal(pos, "wrapping '+%%' requires a typed integer operand");
        return wrap_to_type(a + b, lt);
    case T_MINUSW:
        if (lt->kind != TY_INT) fatal(pos, "wrapping '-%%' requires a typed integer operand");
        return wrap_to_type(a - b, lt);
    case T_STARW:
        if (lt->kind != TY_INT) fatal(pos, "wrapping '*%%' requires a typed integer operand");
        return wrap_to_type(a * b, lt);
    case T_AMP:     return a & b;
    case T_PIPE:    return a | b;
    case T_CARET:   return a ^ b;
    case T_SHL: {
        int maxb = (lt->kind == TY_INT) ? lt->size * 8 : 64;
        if (b < 0 || b >= maxb) fatal(pos, "shift count %d out of range", (int)b);
        return a << (int)b;
    }
    case T_SHR: {
        int maxb = (lt->kind == TY_INT) ? lt->size * 8 : 64;
        if (b < 0 || b >= maxb) fatal(pos, "shift count %d out of range", (int)b);
        return a >> (int)b;
    }
    case T_EQ:      return a == b;
    case T_NE:      return a != b;
    case T_LT:      return a < b;
    case T_LE:      return a <= b;
    case T_GT:      return a > b;
    case T_GE:      return a >= b;
    case T_ANDAND:  return (a != 0) && (b != 0);
    case T_OROR:    return (a != 0) || (b != 0);
    default:        return 0;
    }
}

static void check_expr(Checker *c, Node *n, Type *expected) {
    switch (n->kind) {
    case N_INT:
        n->type = &ty_untyped_int;
        n->is_const = true;
        if (expected) coerce(n, expected);
        return;

    case N_FLOAT:
        n->type = &ty_untyped_float;
        n->is_const = true;
        if (expected) coerce(n, expected);
        return;

    case N_BOOL:
        n->type = &ty_bool;
        n->is_const = true;
        if (expected) coerce(n, expected);
        return;

    case N_NULL:
        n->type = &ty_null;
        n->is_const = true;
        n->cval = 0;
        if (expected) coerce(n, expected);
        return;

    case N_ADDR: {
        // Address-of `&x`, `&arr[i]`, or `&*p`.
        Node *opd = n->lhs;
        if (opd->kind == N_IDENT) {
            Symbol *s = lookup(c, opd->name);
            if (!s) fatal(opd->pos, "undeclared identifier '%s'", opd->name);
            if (s->kind != SYM_VAR) {
                fatal(opd->pos, "can only take address '&' of a variable or array element (not '%s')", opd->name);
            }
            if (s->readonly) {
                fatal(opd->pos, "cannot take address of read-only loop variable '%s'", opd->name);
            }
            // Taking `&x` counts as initializing `x` so `fn init(p: *i32)` works on uninitialized vars,
            // and marks `x` as assigned.
            if (!s->is_global && s->var_index >= 0) {
                c->assigned |= (1ULL << s->var_index);
            }
            opd->sym = s;
            opd->type = s->type;
        } else if (opd->kind == N_INDEX || opd->kind == N_DEREF || opd->kind == N_FIELD) {
            check_expr(c, opd, NULL);
        } else {
            fatal(n->pos, "operand of '&' must be a variable, array element, struct field, or pointer dereference");
        }
        n->type = make_ptr_type(opd->type, n->pos);
        if (expected) coerce(n, expected);
        return;
    }

    case N_DEREF: {
        check_expr(c, n->lhs, NULL);
        if (n->lhs->type->kind != TY_PTR) {
            fatal(n->pos, "cannot dereference non-pointer type '%s'", n->lhs->type->name);
        }
        n->type = n->lhs->type->elem;
        if (expected) coerce(n, expected);
        return;
    }

    case N_STRUCT_LIT: {
        Symbol *s = lookup_in(c->global, n->name);
        if (!s || s->kind != SYM_TYPE || s->type->kind != TY_STRUCT) {
            fatal(n->pos, "'%s' is not a struct type", n->name);
        }
        Type *st = s->type;
        if (st->state != 2) layout_struct(c, st);
        for (int i = 0; i < n->list.len; i++) {
            Node *fi = n->list.items[i];
            StructField *sf = find_field(st, fi->name);
            if (!sf) {
                fatal(fi->pos, "struct '%s' has no field named '%s'", st->name, fi->name);
            }
            for (int j = 0; j < i; j++) {
                if (n->list.items[j]->name == fi->name) {
                    fatal(fi->pos, "duplicate initializer for field '%s'", fi->name);
                }
            }
            check_expr(c, fi->rhs, sf->type);
            fi->type = sf->type;
            fi->field_offset = sf->offset;
        }
        n->type = st;
        // Allocate temporary stack storage if the struct literal is used as an rvalue expression
        // (e.g. `foo(Point { x: 1, y: 2 })` or `(Point { x: 1, y: 2 }).x`).
        if (c->cur_fn) {
            n->slot_start = alloc_slots_bytes(c, st->size);
        }
        if (expected) coerce(n, expected);
        return;
    }

    case N_FIELD: {
        // If `lhs` is an uninitialized variable `var p: Point`, allow writing `p.x = 1`,
        // and mark `p` as assigned on first field access.
        if (n->lhs->kind == N_IDENT) {
            Symbol *s = lookup(c, n->lhs->name);
            if (s && s->kind == SYM_VAR && !s->is_global && s->var_index >= 0 &&
                s->type && s->type->kind == TY_STRUCT) {
                c->assigned |= (1ULL << s->var_index);
            }
        }
        check_expr(c, n->lhs, NULL);
        Type *bt = n->lhs->type;
        Type *st = NULL;
        if (bt->kind == TY_STRUCT) {
            st = bt;
        } else if (bt->kind == TY_PTR && bt->elem->kind == TY_STRUCT) {
            // Auto-dereference `ptr.field`!
            st = bt->elem;
        } else {
            fatal(n->pos, "cannot access field '.%s' on type '%s' (expected struct or pointer to struct)",
                  n->name, bt->name);
        }
        if (st->state != 2) layout_struct(c, st);
        StructField *sf = find_field(st, n->name);
        if (!sf) {
            fatal(n->pos, "struct '%s' has no field named '%s'", st->name, n->name);
        }
        n->type = sf->type;
        n->field_offset = sf->offset;
        if (expected) coerce(n, expected);
        return;
    }

    case N_STR:
        n->type = &ty_str;
        n->is_const = false;
        if (expected) coerce(n, expected);
        return;

    case N_ARRAY_LIT: {
        Type *elem_t = (expected && expected->kind == TY_ARRAY) ? expected->elem : NULL;
        for (int i = 0; i < n->list.len; i++) {
            Node *el = n->list.items[i];
            check_expr(c, el, elem_t);
            if (!elem_t && el->type != &ty_untyped_int) {
                elem_t = el->type;
            }
        }
        if (!elem_t) {
            if (n->list.len == 0) {
                fatal(n->pos, "empty array literal '[]' requires an explicit array type (e.g. 'var a[5]: i32 = []')");
            }
            elem_t = &ty_i32;
        }
        for (int i = 0; i < n->list.len; i++) {
            coerce(n->list.items[i], elem_t);
        }
        int arr_len = n->list.len;
        if (expected && expected->kind == TY_ARRAY) {
            if (n->list.len > expected->array_len) {
                fatal(n->pos, "array literal has %d elements, but '%s' only holds %d",
                      n->list.len, expected->name, expected->array_len);
            }
            arr_len = expected->array_len;
        }
        n->type = make_array_type(elem_t, arr_len, n->pos);
        if (expected) coerce(n, expected);
        return;
    }

    case N_INDEX: {
        check_expr(c, n->lhs, NULL);
        Type *bt = n->lhs->type;
        if (bt->kind != TY_ARRAY && bt->kind != TY_SLICE && bt->kind != TY_PTR) {
            fatal(n->pos, "cannot index value of type '%s' (expected array, slice, or pointer)", bt->name);
        }
        check_expr(c, n->rhs, NULL);
        if (!is_int(n->rhs->type)) {
            fatal(n->rhs->pos, "index must be an integer, got '%s'", n->rhs->type->name);
        }
        materialize(n->rhs);
        if (bt->kind != TY_PTR && n->rhs->is_const) {
            if (n->rhs->cval < 0) {
                char buf[48];
                fatal(n->rhs->pos, "array index %s cannot be negative", i128_str(n->rhs->cval, buf));
            }
            if (bt->kind == TY_ARRAY && n->rhs->cval >= bt->array_len) {
                char buf[48];
                fatal(n->rhs->pos, "array index %s out of bounds for '%s' (length %d)",
                      i128_str(n->rhs->cval, buf), bt->name, bt->array_len);
            }
        }
        n->type = bt->elem;
        if (expected) coerce(n, expected);
        return;
    }

    case N_SLICE: {
        check_expr(c, n->lhs, NULL);
        Type *bt = n->lhs->type;
        if (bt->kind != TY_ARRAY && bt->kind != TY_SLICE && bt->kind != TY_PTR) {
            fatal(n->pos, "cannot slice value of type '%s' (expected array, slice, or pointer)", bt->name);
        }
        if (bt->kind == TY_PTR && !n->hi) {
            fatal(n->pos, "slicing a raw pointer requires an explicit upper bound 'p[lo..hi]'");
        }
        if (n->lo) {
            check_expr(c, n->lo, NULL);
            if (!is_int(n->lo->type)) fatal(n->lo->pos, "slice lower bound must be an integer");
            materialize(n->lo);
            if (n->lo->is_const && n->lo->cval < 0) {
                fatal(n->lo->pos, "slice lower bound cannot be negative");
            }
            if (bt->kind == TY_ARRAY && n->lo->is_const && n->lo->cval > bt->array_len) {
                fatal(n->lo->pos, "slice lower bound %d exceeds array length %d",
                      (int)n->lo->cval, bt->array_len);
            }
        }
        if (n->hi) {
            check_expr(c, n->hi, NULL);
            if (!is_int(n->hi->type)) fatal(n->hi->pos, "slice upper bound must be an integer");
            materialize(n->hi);
            if (n->hi->is_const && n->hi->cval < 0) {
                fatal(n->hi->pos, "slice upper bound cannot be negative");
            }
            if (bt->kind == TY_ARRAY && n->hi->is_const && n->hi->cval > bt->array_len) {
                fatal(n->hi->pos, "slice upper bound %d exceeds array length %d",
                      (int)n->hi->cval, bt->array_len);
            }
        }
        if (n->lo && n->hi && n->lo->is_const && n->hi->is_const && n->lo->cval > n->hi->cval) {
            fatal(n->pos, "slice lower bound (%d) > upper bound (%d)",
                  (int)n->lo->cval, (int)n->hi->cval);
        }
        n->type = make_slice_type(bt->elem, n->pos);
        if (expected) coerce(n, expected);
        return;
    }

    case N_IDENT: {
        Symbol *s = lookup(c, n->name);
        if (!s) fatal(n->pos, "undeclared identifier '%s'", n->name);
        if (s->kind == SYM_TYPE || s->kind == SYM_FN || s->kind == SYM_BUILTIN) {
            fatal(n->pos, "'%s' is not a value", n->name);
        }
        if (s->kind == SYM_CONST && s->is_global && s->state != 2) {
            resolve_global_const(c, s);
        }
        if (s->kind == SYM_VAR && !s->is_global && s->var_index >= 0) {
            if (!(c->assigned & (1ULL << s->var_index))) {
                fatal(n->pos, "variable '%s' may be read before it is assigned", n->name);
            }
        }
        n->sym = s;
        n->type = s->type;
        if (s->kind == SYM_CONST) {
            n->is_const = true;
            n->cval = s->cval;
            n->fval = s->fval;
        }
        if (expected) coerce(n, expected);
        return;
    }

    case N_UNARY: {
        if (n->op == T_BANG) {
            check_expr(c, n->lhs, &ty_bool);
            n->type = &ty_bool;
            if (n->lhs->is_const) {
                n->is_const = true;
                n->cval = !n->lhs->cval;
            }
            if (expected) coerce(n, expected);
            return;
        }
        // Unary '-' or '~': do not force expected onto lhs before negation
        // so that `-2147483648` works for i32.
        check_expr(c, n->lhs, NULL);
        if (n->op == T_MINUS && is_float(n->lhs->type)) {
            n->type = n->lhs->type;
            if (n->lhs->is_const) {
                n->is_const = true;
                n->fval = -n->lhs->fval;
            }
            if (expected) coerce(n, expected);
            return;
        }
        if (!is_int(n->lhs->type)) {
            fatal(n->pos, "operator %s requires an integer operand, got '%s'",
                  tok_name(n->op), n->lhs->type->name);
        }
        if (n->op == T_MINUS && n->lhs->type->kind == TY_INT && !n->lhs->type->is_signed) {
            fatal(n->pos, "cannot negate unsigned type '%s'", n->lhs->type->name);
        }
        if (n->op == T_TILDE && n->lhs->type == &ty_untyped_int && !expected) {
            coerce(n->lhs, &ty_i32);
        }
        n->type = n->lhs->type;
        if (n->lhs->is_const) {
            n->is_const = true;
            n->cval = eval_const_unary(n->pos, n->op, n->lhs->cval,
                                       expected ? expected : n->type);
            if (n->type->kind == TY_INT && !fits(n->cval, n->type)) {
                char buf[48];
                fatal(n->pos, "constant overflow: %s does not fit in '%s'",
                      i128_str(n->cval, buf), n->type->name);
            }
        }
        if (expected) coerce(n, expected);
        return;
    }

    case N_BINARY: {
        TokKind op = n->op;
        if (op == T_ANDAND || op == T_OROR) {
            check_expr(c, n->lhs, &ty_bool);
            check_expr(c, n->rhs, &ty_bool);
            n->type = &ty_bool;
            if (n->lhs->is_const && n->rhs->is_const) {
                n->is_const = true;
                n->cval = eval_const_binary(n->pos, op, n->lhs->cval, n->rhs->cval, &ty_bool);
            }
            if (expected) coerce(n, expected);
            return;
        }

        if (op == T_SHL || op == T_SHR) {
            check_expr(c, n->lhs, expected);
            if (!is_int(n->lhs->type)) {
                fatal(n->pos, "shift left operand must be an integer, got '%s'", n->lhs->type->name);
            }
            if (n->lhs->type == &ty_untyped_int && expected) coerce(n->lhs, expected);
            check_expr(c, n->rhs, NULL);
            if (n->rhs->type == &ty_untyped_int) {
                if (n->rhs->is_const && n->rhs->cval < 0)
                    fatal(n->rhs->pos, "shift count cannot be negative");
                coerce(n->rhs, &ty_u32);
            } else if (n->rhs->type->kind != TY_INT || n->rhs->type->is_signed) {
                fatal(n->rhs->pos, "shift count must be an unsigned integer or constant, got '%s'",
                      n->rhs->type->name);
            }
            if (n->rhs->is_const && n->lhs->type->kind == TY_INT) {
                int bits = n->lhs->type->size * 8;
                if (n->rhs->cval < 0 || n->rhs->cval >= bits) {
                    fatal(n->pos, "shift count %d >= width of '%s' (%d bits)",
                          (int)n->rhs->cval, n->lhs->type->name, bits);
                }
            }
            n->type = n->lhs->type;
            if (n->lhs->is_const && n->rhs->is_const) {
                n->is_const = true;
                n->cval = eval_const_binary(n->pos, op, n->lhs->cval, n->rhs->cval, n->type);
                if (n->type->kind == TY_INT && !fits(n->cval, n->type)) {
                    char buf[48];
                    fatal(n->pos, "constant shift result %s overflows '%s'",
                          i128_str(n->cval, buf), n->type->name);
                }
            }
            if (expected) coerce(n, expected);
            return;
        }

        bool is_cmp = (op == T_EQ || op == T_NE || op == T_LT ||
                       op == T_LE || op == T_GT || op == T_GE);
        Type *sub_expect = is_cmp ? NULL : expected;
        check_expr(c, n->lhs, sub_expect);
        check_expr(c, n->rhs, NULL);

        // Pointer arithmetic (`p + offset`, `p - offset`, and `p - q`).
        if (n->lhs->type->kind == TY_PTR && (op == T_PLUS || op == T_MINUS)) {
            if (op == T_MINUS && n->rhs->type == n->lhs->type) {
                // `p - q` -> element difference of type `isize`.
                n->type = &ty_isize;
                if (expected) coerce(n, expected);
                return;
            }
            if (!is_int(n->rhs->type)) {
                fatal(n->rhs->pos, "pointer offset must be an integer, got '%s'", n->rhs->type->name);
            }
            materialize(n->rhs);
            n->type = n->lhs->type;
            if (expected) coerce(n, expected);
            return;
        }

        // Pointer comparisons (`p == null`, `p != q`, `p < q`, ...).
        if (is_cmp && (n->lhs->type->kind == TY_PTR || n->rhs->type->kind == TY_PTR)) {
            if (n->lhs->type == &ty_null && n->rhs->type->kind == TY_PTR) {
                coerce(n->lhs, n->rhs->type);
            } else if (n->rhs->type == &ty_null && n->lhs->type->kind == TY_PTR) {
                coerce(n->rhs, n->lhs->type);
            } else if (n->lhs->type != n->rhs->type) {
                fatal(n->pos, "cannot compare different pointer types '%s' and '%s'",
                      n->lhs->type->name, n->rhs->type->name);
            }
            n->type = &ty_bool;
            if (expected) coerce(n, expected);
            return;
        }
        if (sub_expect) coerce(n->rhs, sub_expect);

        // Unify operand types using the untyped-constant rule (§3.3).
        if (is_float(n->lhs->type) || is_float(n->rhs->type)) {
            if (n->lhs->type->kind == TY_FLOAT && (n->rhs->type == &ty_untyped_float || n->rhs->type == &ty_untyped_int)) {
                coerce(n->rhs, n->lhs->type);
            } else if (n->rhs->type->kind == TY_FLOAT && (n->lhs->type == &ty_untyped_float || n->lhs->type == &ty_untyped_int)) {
                coerce(n->lhs, n->rhs->type);
            } else if (n->lhs->type == &ty_untyped_float && n->rhs->type == &ty_untyped_int) {
                coerce(n->rhs, &ty_untyped_float);
                n->rhs->fval = (double)n->rhs->cval;
            } else if (n->rhs->type == &ty_untyped_float && n->lhs->type == &ty_untyped_int) {
                coerce(n->lhs, &ty_untyped_float);
                n->lhs->fval = (double)n->lhs->cval;
            } else if (n->lhs->type != n->rhs->type) {
                fatal(n->pos, "mismatched types '%s' and '%s' in %s (use explicit conversion)",
                      n->lhs->type->name, n->rhs->type->name, tok_name(op));
            }
            if (is_cmp && n->lhs->type == &ty_untyped_float) {
                coerce(n->lhs, &ty_f64);
                coerce(n->rhs, &ty_f64);
            }
            if (!is_cmp && op != T_PLUS && op != T_MINUS && op != T_STAR && op != T_SLASH) {
                fatal(n->pos, "operator %s is not defined for floating-point type '%s'",
                      tok_name(op), n->lhs->type->name);
            }
            n->type = is_cmp ? &ty_bool : n->lhs->type;
            if (n->lhs->is_const && n->rhs->is_const) {
                n->is_const = true;
                double a = n->lhs->fval, b = n->rhs->fval;
                switch (op) {
                case T_PLUS:  n->fval = a + b; break;
                case T_MINUS: n->fval = a - b; break;
                case T_STAR:  n->fval = a * b; break;
                case T_SLASH: n->fval = a / b; break;
                case T_EQ:    n->cval = (a == b); break;
                case T_NE:    n->cval = (a != b); break;
                case T_LT:    n->cval = (a <  b); break;
                case T_LE:    n->cval = (a <= b); break;
                case T_GT:    n->cval = (a >  b); break;
                case T_GE:    n->cval = (a >= b); break;
                default: break;
                }
            }
            if (expected) coerce(n, expected);
            return;
        }

        if (n->lhs->type == &ty_untyped_int && n->rhs->type->kind == TY_INT) {
            coerce(n->lhs, n->rhs->type);
        } else if (n->rhs->type == &ty_untyped_int && n->lhs->type->kind == TY_INT) {
            coerce(n->rhs, n->lhs->type);
        } else if (is_cmp && n->lhs->type == &ty_untyped_int && n->rhs->type == &ty_untyped_int) {
            coerce(n->lhs, &ty_i32);
            coerce(n->rhs, &ty_i32);
        } else if (n->lhs->type != n->rhs->type) {
            fatal(n->pos, "mismatched types '%s' and '%s' in %s (use explicit conversion)",
                  n->lhs->type->name, n->rhs->type->name, tok_name(op));
        }

        Type *ot = n->lhs->type;
        if (is_cmp) {
            if (op == T_EQ || op == T_NE) {
                if (!is_int(ot) && ot != &ty_bool) {
                    fatal(n->pos, "operator %s is not defined for '%s'", tok_name(op), ot->name);
                }
            } else {
                if (!is_int(ot)) {
                    fatal(n->pos, "comparison %s requires integer operands, got '%s'", tok_name(op), ot->name);
                }
            }
            n->type = &ty_bool;
        } else {
            if (!is_int(ot)) {
                fatal(n->pos, "operator %s requires integer operands, got '%s'", tok_name(op), ot->name);
            }
            if ((op == T_PLUSW || op == T_MINUSW || op == T_STARW) && ot == &ty_untyped_int) {
                Type *wt = expected ? expected : &ty_i32;
                coerce(n->lhs, wt);
                coerce(n->rhs, wt);
                ot = wt;
            }
            n->type = ot;
        }

        // Check division by a constant zero even when the dividend is not constant.
        if ((op == T_SLASH || op == T_PERCENT) && n->rhs->is_const && n->rhs->cval == 0) {
            fatal(n->pos, "division by zero");
        }
        if ((op == T_SLASH || op == T_PERCENT) && ot->kind == TY_INT && ot->is_signed &&
            n->lhs->is_const && n->rhs->is_const) {
            i128 lo, hi;
            type_bounds(ot, &lo, &hi);
            if (n->lhs->cval == lo && n->rhs->cval == -1) {
                fatal(n->pos, "constant division overflow: MIN / -1 overflows '%s'", ot->name);
            }
        }

        if (n->lhs->is_const && n->rhs->is_const) {
            n->is_const = true;
            n->cval = eval_const_binary(n->pos, op, n->lhs->cval, n->rhs->cval, ot);
            if (n->type->kind == TY_INT && !fits(n->cval, n->type)) {
                char buf[48];
                fatal(n->pos, "constant overflow: result %s does not fit in '%s'",
                      i128_str(n->cval, buf), n->type->name);
            }
        }
        if (expected) coerce(n, expected);
        return;
    }

    case N_CALL: {
        Symbol *s = lookup(c, n->name);
        if (!s) fatal(n->pos, "undeclared function or type '%s'", n->name);

        // Type conversion: `i64(x)`, `f64(x)`, `f32(x)` (spec §2.2, §2.3)
        if (s->kind == SYM_TYPE) {
            if (n->list.len != 1) {
                fatal(n->pos, "type conversion '%s(...)' takes exactly 1 argument", n->name);
            }
            Node *arg = n->list.items[0];
            check_expr(c, arg, NULL);
            if (s->type->kind == TY_FLOAT) {
                if (arg->type == &ty_untyped_float || arg->type == &ty_untyped_int) {
                    coerce(arg, s->type);
                } else if (!is_int(arg->type) && !is_float(arg->type)) {
                    fatal(n->pos, "cannot convert '%s' to '%s'", arg->type->name, s->type->name);
                }
                n->kind = N_CONV;
                n->lhs = arg;
                n->type = s->type;
                if (arg->is_const) {
                    n->is_const = true;
                    n->fval = is_float(arg->type) ? arg->fval : (double)arg->cval;
                }
                if (expected) coerce(n, expected);
                return;
            }
            if (arg->type == &ty_untyped_int) {
                coerce(arg, s->type);
            } else if (is_float(arg->type) && is_int(s->type)) {
                materialize(arg);
                n->kind = N_CONV;
                n->lhs = arg;
                n->type = s->type;
                if (arg->is_const) {
                    n->is_const = true;
                    n->cval = (i128)(int64_t)arg->fval;
                }
                if (expected) coerce(n, expected);
                return;
            } else if (!is_int(arg->type) || !is_int(s->type)) {
                fatal(n->pos, "cannot convert '%s' to '%s'", arg->type->name, s->type->name);
            }
            n->kind = N_CONV;
            n->lhs = arg;
            n->type = s->type;
            if (arg->is_const) {
                if (!fits(arg->cval, s->type)) {
                    char buf[48];
                    fatal(n->pos, "constant %s out of range for '%s'",
                          i128_str(arg->cval, buf), s->type->name);
                }
                n->is_const = true;
                n->cval = arg->cval;
            }
            if (expected) coerce(n, expected);
            return;
        }

        // Built-in functions (`print`, `exit`, `rev`).
        if (s->kind == SYM_BUILTIN) {
            n->builtin = s->builtin;
            if (s->builtin == B_REV) {
                fatal(n->pos, "'rev(...)' is only valid inside a 'loop' range header");
            }
            if (s->builtin == B_PRINT) {
                if (n->list.len != 1) fatal(n->pos, "'print' takes 1 argument");
                Node *arg = n->list.items[0];
                check_expr(c, arg, NULL);
                materialize(arg);
                if (arg->type->kind == TY_VOID) {
                    fatal(arg->pos, "cannot print a void expression");
                }
                n->type = &ty_void;
                if (expected) coerce(n, expected);
                return;
            }
            if (s->builtin == B_EXIT) {
                if (n->list.len != 1) fatal(n->pos, "'exit' takes 1 argument (exit code: i32)");
                check_expr(c, n->list.items[0], &ty_i32);
                n->type = &ty_void;
                if (expected) coerce(n, expected);
                return;
            }
            if (s->builtin == B_LEN) {
                if (n->list.len != 1) fatal(n->pos, "'len' takes 1 argument");
                Node *arg = n->list.items[0];
                check_expr(c, arg, NULL);
                Type *at = arg->type;
                if (at->kind == TY_ARRAY) {
                    n->type = &ty_untyped_int;
                    n->is_const = true;
                    n->cval = at->array_len;
                    if (expected) coerce(n, expected);
                    return;
                }
                if (at->kind == TY_SLICE || at == &ty_str) {
                    n->type = &ty_i64;
                    if (expected) coerce(n, expected);
                    return;
                }
                fatal(arg->pos, "'len' requires an array, slice, or string argument (got '%s')", at->name);
            }
        }

        if (s->kind != SYM_FN) {
            fatal(n->pos, "'%s' is not a function", n->name);
        }
        Node *fn = s->decl;
        if (n->list.len != fn->list.len) {
            fatal(n->pos, "function '%s' expects %d argument(s), got %d",
                  n->name, fn->list.len, n->list.len);
        }
        for (int i = 0; i < n->list.len; i++) {
            Type *pt = fn->list.items[i]->type;
            check_expr(c, n->list.items[i], pt);
        }
        n->sym = s;
        n->type = s->type;
        if (expected) {
            if (n->type == &ty_void) {
                fatal(n->pos, "function '%s' returns no value", n->name);
            }
            coerce(n, expected);
        }
        return;
    }

    default:
        fatal(n->pos, "internal error: unexpected expression node");
    }
}

// ---------------------------------------------------------------------------
// Statements, definite-assignment analysis (§4.2), and return analysis (§5.2)
// ---------------------------------------------------------------------------

// Returns true if the statement is guaranteed to terminate (return, break,
// continue, exit(), or an infinite loop without breaks).
static bool check_stmt(Checker *c, Node *n);
static bool check_block(Checker *c, Node *b, bool new_scope);

static bool loop_has_break_to(Node *stmt, Node *target) {
    if (!stmt) return false;
    switch (stmt->kind) {
    case N_BREAK:
        return stmt->target == target;
    case N_BLOCK:
        for (int i = 0; i < stmt->list.len; i++) {
            if (loop_has_break_to(stmt->list.items[i], target)) return true;
        }
        return false;
    case N_IF:
        return loop_has_break_to(stmt->body, target) || loop_has_break_to(stmt->els, target);
    case N_LOOP_INF:
    case N_LOOP_COND:
    case N_LOOP_RANGE:
    case N_LOOP_ITER:
    case N_LOOP_C:
        return loop_has_break_to(stmt->body, target);
    default:
        return false;
    }
}

// Spec §6.7: emit a hint when a C-style loop is a plain counting loop.
static bool body_assigns_var(Node *stmt, Symbol *v) {
    if (!stmt) return false;
    switch (stmt->kind) {
    case N_ASSIGN:
        return stmt->sym == v;
    case N_BLOCK:
        for (int i = 0; i < stmt->list.len; i++)
            if (body_assigns_var(stmt->list.items[i], v)) return true;
        return false;
    case N_IF:
        return body_assigns_var(stmt->body, v) || body_assigns_var(stmt->els, v);
    case N_LOOP_INF:
    case N_LOOP_COND:
    case N_LOOP_RANGE:
    case N_LOOP_C:
        return body_assigns_var(stmt->body, v);
    default:
        return false;
    }
}

static void check_assign(Checker *c, Node *n) {
    Type *target_type = NULL;

    if (n->lhs && (n->lhs->kind == N_INDEX || n->lhs->kind == N_DEREF || n->lhs->kind == N_FIELD)) {
        // Element, field, or dereference assignment: `arr[i] = val`, `p.x = val`, `*p = val`
        check_expr(c, n->lhs, NULL);
        target_type = n->lhs->type;
        n->type = target_type;
        if (n->op == T_ASSIGN) {
            check_expr(c, n->rhs, target_type);
            return;
        }
    } else {
        Symbol *s = lookup(c, n->name);
        if (!s) fatal(n->pos, "undeclared identifier '%s'", n->name);
        if (s->kind == SYM_CONST) fatal(n->pos, "cannot assign to constant '%s'", n->name);
        if (s->kind == SYM_PARAM) fatal(n->pos, "cannot assign to parameter '%s' (parameters are read-only)", n->name);
        if (s->kind != SYM_VAR)   fatal(n->pos, "cannot assign to '%s'", n->name);
        if (s->readonly)          fatal(n->pos, "cannot assign to loop variable '%s'", n->name);

        n->sym = s;
        n->type = s->type;
        target_type = s->type;

        if (n->op == T_ASSIGN) {
            check_expr(c, n->rhs, s->type);
            if (!s->is_global && s->var_index >= 0) {
                c->assigned |= (1ULL << s->var_index);
            }
            return;
        }

        // Compound assignment: the variable is both read and written.
        if (!s->is_global && s->var_index >= 0 && !(c->assigned & (1ULL << s->var_index))) {
            fatal(n->pos, "variable '%s' may be read before it is assigned", n->name);
        }
    }

    // Pointer compound assignment (`p += offset`, `p -= offset`).
    if (target_type->kind == TY_PTR && (n->op == T_PLUS_ASSIGN || n->op == T_MINUS_ASSIGN)) {
        check_expr(c, n->rhs, NULL);
        if (!is_int(n->rhs->type)) {
            fatal(n->rhs->pos, "pointer offset must be an integer, got '%s'", n->rhs->type->name);
        }
        materialize(n->rhs);
        return;
    }

    // Float compound assignment (`x += 1.5`, `x -= y`, `x *= 2.0`, `x /= 3.0`).
    if (target_type->kind == TY_FLOAT) {
        if (n->op != T_PLUS_ASSIGN && n->op != T_MINUS_ASSIGN &&
            n->op != T_STAR_ASSIGN && n->op != T_SLASH_ASSIGN) {
            fatal(n->pos, "compound assignment %s is not defined for float type '%s'",
                  tok_name(n->op), target_type->name);
        }
        check_expr(c, n->rhs, target_type);
        return;
    }

    if (!is_int(target_type)) {
        fatal(n->pos, "compound assignment %s requires an integer target, got '%s'",
              tok_name(n->op), target_type->name);
    }
    if (n->op == T_SHL_ASSIGN || n->op == T_SHR_ASSIGN) {
        check_expr(c, n->rhs, NULL);
        if (n->rhs->type == &ty_untyped_int) {
            if (n->rhs->is_const && n->rhs->cval < 0)
                fatal(n->rhs->pos, "shift count cannot be negative");
            coerce(n->rhs, &ty_u32);
        } else if (n->rhs->type->kind != TY_INT || n->rhs->type->is_signed) {
            fatal(n->rhs->pos, "shift count must be an unsigned integer or constant, got '%s'",
                  n->rhs->type->name);
        }
        if (n->rhs->is_const) {
            int bits = target_type->size * 8;
            if (n->rhs->cval < 0 || n->rhs->cval >= bits) {
                fatal(n->pos, "shift count %d >= width of '%s' (%d bits)",
                      (int)n->rhs->cval, target_type->name, bits);
            }
        }
    } else {
        check_expr(c, n->rhs, target_type);
        if ((n->op == T_SLASH_ASSIGN || n->op == T_PERCENT_ASSIGN) &&
            n->rhs->is_const && n->rhs->cval == 0) {
            fatal(n->pos, "division by zero");
        }
    }
}

static bool check_stmt(Checker *c, Node *n) {
    switch (n->kind) {
    case N_VAR: {
        Type *decl_t = resolve_decl_type(c, n);
        int explicit_len = 0;
        if (!decl_t && n->decl_kind == DECL_ARRAY) {
            explicit_len = eval_array_len(c, n->len_expr);
        }
        if (n->rhs) {
            check_expr(c, n->rhs, decl_t);
            if (!decl_t) {
                materialize(n->rhs);
                if (n->rhs->type == &ty_void) {
                    fatal(n->pos, "cannot initialize '%s' with a void expression", n->name);
                }
                if (n->decl_kind == DECL_ARRAY) {
                    if (n->rhs->kind != N_ARRAY_LIT) {
                        fatal(n->pos, "expected array literal to initialize '%s[%d]'", n->name, explicit_len);
                    }
                    if (n->rhs->type->array_len > explicit_len) {
                        fatal(n->rhs->pos, "array literal has %d elements, but '%s[%d]' only holds %d",
                              n->rhs->type->array_len, n->name, explicit_len, explicit_len);
                    }
                    decl_t = make_array_type(n->rhs->type->elem, explicit_len, n->pos);
                    n->rhs->type = decl_t;
                } else if (n->decl_kind == DECL_SLICE) {
                    if (n->rhs->type->kind == TY_ARRAY) {
                        decl_t = make_slice_type(n->rhs->type->elem, n->pos);
                        n->rhs->implicit_slice = true;
                    } else if (n->rhs->type->kind == TY_SLICE) {
                        decl_t = n->rhs->type;
                    } else {
                        fatal(n->pos, "cannot initialize slice '%s[]' with '%s'", n->name, n->rhs->type->name);
                    }
                } else {
                    decl_t = n->rhs->type;
                }
            }
        } else if (!decl_t) {
            fatal(n->pos, "variable '%s' requires a type annotation or initializer", n->name);
        }
        Symbol *s = define(c, SYM_VAR, n->name, decl_t, n->pos);
        s->decl = n;
        if (decl_t->kind == TY_ARRAY || decl_t->kind == TY_STRUCT) {
            s->slot = alloc_slots_bytes(c, decl_t->size);
            s->var_index = -1; // array/struct storage is allocated on entry
        } else if (decl_t->kind == TY_SLICE || decl_t == &ty_str) {
            s->slot = alloc_slots_bytes(c, 16); // {ptr, len}
            if (n->rhs) {
                s->var_index = -1;
            } else {
                if (c->next_var_index >= 63) fatal(n->pos, "too many uninitialized variables");
                s->var_index = c->next_var_index++;
                c->assigned &= ~(1ULL << s->var_index);
            }
        } else {
            s->slot = alloc_slot(c);
            if (n->rhs) {
                s->var_index = -1; // initialized at declaration
            } else {
                if (c->next_var_index >= 63) {
                    fatal(n->pos, "too many uninitialized variables in one function (limit 63)");
                }
                s->var_index = c->next_var_index++;
                c->assigned &= ~(1ULL << s->var_index);
            }
        }
        n->sym = s;
        n->type = decl_t;
        return false;
    }

    case N_CONST: {
        Type *decl_t = n->type_name ? resolve_type(c, n->type_name, n->type_pos) : NULL;
        check_expr(c, n->rhs, decl_t);
        if (!n->rhs->is_const) {
            fatal(n->rhs->pos, "const initializer for '%s' is not a compile-time constant", n->name);
        }
        Type *ct = decl_t ? decl_t : n->rhs->type;
        Symbol *s = define(c, SYM_CONST, n->name, ct, n->pos);
        s->decl = n;
        s->cval = n->rhs->cval;
        s->fval = n->rhs->fval;
        s->state = 2;
        n->sym = s;
        n->type = ct;
        return false;
    }

    case N_ASSIGN:
        check_assign(c, n);
        return false;

    case N_EXPR_STMT:
        check_expr(c, n->lhs, NULL);
        if (n->lhs->kind == N_CALL && n->lhs->builtin == B_EXIT) {
            c->assigned = ALL_ASSIGNED;
            return true;
        }
        return false;

    case N_RETURN:
        if (c->ret_type == &ty_void) {
            if (n->lhs) {
                fatal(n->pos, "function '%s' has no return type and cannot return a value",
                      c->cur_fn->name);
            }
        } else {
            if (!n->lhs) {
                fatal(n->pos, "missing return value (expected '%s')", c->ret_type->name);
            }
            check_expr(c, n->lhs, c->ret_type);
        }
        c->assigned = ALL_ASSIGNED;
        return true;

    case N_BREAK:
    case N_CONTINUE: {
        if (c->loops.len == 0) {
            fatal(n->pos, "'%s' outside of a loop", n->kind == N_BREAK ? "break" : "continue");
        }
        Node *target = NULL;
        if (n->name) {
            for (int i = c->loops.len - 1; i >= 0; i--) {
                if (c->loops.items[i]->label == n->name) {
                    target = c->loops.items[i];
                    break;
                }
            }
            if (!target) fatal(n->pos, "unknown loop label '%s'", n->name);
        } else {
            target = c->loops.items[c->loops.len - 1];
        }
        n->target = target;
        c->assigned = ALL_ASSIGNED;
        return true;
    }

    case N_IF: {
        check_expr(c, n->cond, &ty_bool);
        AssignSet before = c->assigned;
        c->assigned = before;
        bool t_ret = check_block(c, n->body, true);
        AssignSet after_then = c->assigned;

        if (n->els) {
            c->assigned = before;
            bool e_ret = (n->els->kind == N_IF) ? check_stmt(c, n->els)
                                                : check_block(c, n->els, true);
            AssignSet after_else = c->assigned;
            c->assigned = after_then & after_else;
            return t_ret && e_ret;
        } else {
            c->assigned = before;
            return false;
        }
    }

    case N_LOOP_INF: {
        AssignSet before = c->assigned;
        vec_push(c->loops, n);
        check_block(c, n->body, true);
        c->loops.len--;
        bool has_break = loop_has_break_to(n->body, n);
        c->assigned = has_break ? before : ALL_ASSIGNED;
        return !has_break;
    }

    case N_LOOP_COND: {
        check_expr(c, n->cond, &ty_bool);
        AssignSet before = c->assigned;
        vec_push(c->loops, n);
        check_block(c, n->body, true);
        c->loops.len--;
        c->assigned = before; // body may run 0 times
        return false;
    }

    case N_LOOP_RANGE: {
        check_expr(c, n->lo, NULL);
        check_expr(c, n->hi, NULL);
        if (!is_int(n->lo->type) || !is_int(n->hi->type)) {
            fatal(n->pos, "range bounds must be integers");
        }
        if (n->lo->type == &ty_untyped_int && n->hi->type->kind == TY_INT) {
            coerce(n->lo, n->hi->type);
        } else if (n->hi->type == &ty_untyped_int && n->lo->type->kind == TY_INT) {
            coerce(n->hi, n->lo->type);
        } else if (n->lo->type == &ty_untyped_int && n->hi->type == &ty_untyped_int) {
            coerce(n->lo, &ty_i32);
            coerce(n->hi, &ty_i32);
        } else if (n->lo->type != n->hi->type) {
            fatal(n->pos, "range bounds have mismatched types '%s' and '%s'",
                  n->lo->type->name, n->hi->type->name);
        }
        Type *it = n->lo->type;
        if (n->by) {
            check_expr(c, n->by, it);
            if (n->by->is_const && n->by->cval <= 0) {
                fatal(n->by->pos, "range step must be > 0");
            }
        }

        AssignSet before = c->assigned;
        push_scope(c, false);
        Symbol *iv = define(c, SYM_VAR, n->name, it, n->pos);
        iv->readonly = true;
        iv->slot = alloc_slot(c);
        n->sym = iv;
        n->type = it;
        n->slot_start = alloc_slot(c);
        n->slot_end   = alloc_slot(c);
        n->slot_step  = alloc_slot(c);

        vec_push(c->loops, n);
        check_block(c, n->body, false);
        c->loops.len--;
        pop_scope(c);
        c->assigned = before;
        return false;
    }

    case N_LOOP_ITER: {
        check_expr(c, n->lhs, NULL);
        Type *ct = n->lhs->type;
        if (ct->kind != TY_ARRAY && ct->kind != TY_SLICE) {
            fatal(n->lhs->pos, "cannot iterate over '%s' (expected array or slice)", ct->name);
        }
        Type *elem_t = ct->elem;
        AssignSet before = c->assigned;
        push_scope(c, false);
        if (n->name2) {
            // `loop i, x in arr`
            Symbol *idx_sym = define(c, SYM_VAR, n->name, &ty_i64, n->pos);
            idx_sym->readonly = true;
            idx_sym->slot = alloc_slot(c);
            Symbol *val_sym = define(c, SYM_VAR, n->name2, elem_t, n->pos);
            val_sym->readonly = true;
            val_sym->slot = (elem_t->kind == TY_STRUCT) ? alloc_slots_bytes(c, elem_t->size) : alloc_slot(c);
            n->sym  = idx_sym;
            n->sym2 = val_sym;
        } else {
            // `loop x in arr`
            Symbol *val_sym = define(c, SYM_VAR, n->name, elem_t, n->pos);
            val_sym->readonly = true;
            val_sym->slot = (elem_t->kind == TY_STRUCT) ? alloc_slots_bytes(c, elem_t->size) : alloc_slot(c);
            n->sym  = val_sym;
            n->sym2 = NULL;
        }
        n->type = elem_t;
        n->slot_start = alloc_slot(c); // data pointer
        n->slot_end   = alloc_slot(c); // length
        n->slot_step  = alloc_slot(c); // current index

        vec_push(c->loops, n);
        check_block(c, n->body, false);
        c->loops.len--;
        pop_scope(c);
        c->assigned = before;
        return false;
    }

    case N_LOOP_C: {
        push_scope(c, false);
        check_stmt(c, n->init);
        check_expr(c, n->cond, &ty_bool);
        AssignSet after_init = c->assigned;
        vec_push(c->loops, n);
        check_block(c, n->body, false);
        c->loops.len--;
        check_assign(c, n->step);
        pop_scope(c);
        c->assigned = after_init;

        // Hint if this C-style loop is a plain `var i = a; i < b; i += 1`.
        Symbol *iv = n->init->sym;
        if (n->init->rhs &&
            n->cond->kind == N_BINARY && n->cond->op == T_LT &&
            n->cond->lhs->kind == N_IDENT && n->cond->lhs->sym == iv &&
            n->step->sym == iv && n->step->op == T_PLUS_ASSIGN &&
            n->step->rhs->is_const && n->step->rhs->cval == 1 &&
            !body_assigns_var(n->body, iv)) {
            fprintf(stderr, "%s:%d:%d: hint: plain counting loop can be written 'loop %s in a..b'\n",
                    g_filename, n->pos.line, n->pos.col, iv->name);
        }
        return false;
    }

    case N_BLOCK:
        return check_block(c, n, true);

    default:
        fatal(n->pos, "internal error: unexpected statement");
    }
}

static bool check_block(Checker *c, Node *b, bool new_scope) {
    if (new_scope) push_scope(c, false);
    bool terminated = false;
    for (int i = 0; i < b->list.len; i++) {
        if (check_stmt(c, b->list.items[i])) {
            terminated = true;
        }
    }
    if (new_scope) pop_scope(c);
    return terminated;
}

// ---------------------------------------------------------------------------
// Top-level declarations
// ---------------------------------------------------------------------------

static void resolve_global_const(Checker *c, Symbol *s) {
    if (s->state == 2) return;
    if (s->state == 1) fatal(s->pos, "cyclic constant definition involving '%s'", s->name);
    s->state = 1;
    Node *n = s->decl;
    Type *decl_t = n->type_name ? resolve_type(c, n->type_name, n->type_pos) : NULL;
    check_expr(c, n->rhs, decl_t);
    if (!n->rhs->is_const) {
        fatal(n->rhs->pos, "const initializer for '%s' is not a compile-time constant", s->name);
    }
    s->type = decl_t ? decl_t : n->rhs->type;
    s->cval = n->rhs->cval;
    s->fval = n->rhs->fval;
    s->state = 2;
    n->type = s->type;
}

static void register_builtins(Checker *c) {
    Type *types[] = {
        &ty_i8, &ty_i16, &ty_i32, &ty_i64, &ty_isize,
        &ty_u8, &ty_u16, &ty_u32, &ty_u64, &ty_usize,
        &ty_f32, &ty_f64, &ty_bool, &ty_str
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        Type *t = types[i];
        t->name = intern(t->name, (int)strlen(t->name));
        Symbol *s = define(c, SYM_TYPE, t->name, t, (Pos){0, 0});
        (void)s;
    }
    struct { const char *name; int id; } builtins[] = {
        {"print", B_PRINT}, {"exit", B_EXIT}, {"rev", B_REV}, {"len", B_LEN}
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        const char *nm = intern(builtins[i].name, (int)strlen(builtins[i].name));
        Symbol *s = define(c, SYM_BUILTIN, nm, &ty_void, (Pos){0, 0});
        s->builtin = builtins[i].id;
    }
}

Program check(NodeVec decls) {
    Checker c = {0};
    c.global = push_scope(&c, false);
    register_builtins(&c);

    Program prog = {0};

    // Pass 1: register all top-level names so structs, constants, and functions
    // can be referenced in any declaration order (spec §5.3).
    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        if (d->kind == N_STRUCT) {
            Type *st = xcalloc(1, sizeof(Type));
            st->kind = TY_STRUCT;
            st->name = d->name;
            st->decl = d;
            Symbol *s = define(&c, SYM_TYPE, d->name, st, d->pos);
            d->sym = s;
            d->type = st;
        } else if (d->kind == N_CONST) {
            Symbol *s = define(&c, SYM_CONST, d->name, &ty_untyped_int, d->pos);
            s->decl = d;
            d->sym = s;
        }
    }
    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        if (d->kind == N_CONST) resolve_global_const(&c, d->sym);
    }
    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        if (d->kind == N_STRUCT) layout_struct(&c, d->type);
    }

    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        if (d->kind == N_FN) {
            Type *ret = d->type_name ? resolve_decl_type(&c, d) : &ty_void;
            if (ret == &ty_str || ret->kind == TY_ARRAY || ret->kind == TY_SLICE || ret->kind == TY_STRUCT) {
                fatal(d->type_pos, "returning '%s' by value is not supported yet (return '*%s' or pass a pointer)",
                      ret->name, ret->name);
            }
            for (int j = 0; j < d->list.len; j++) {
                Node *p = d->list.items[j];
                p->type = resolve_decl_type(&c, p);
                if (p->type == &ty_str) {
                    fatal(p->type_pos, "'str' parameters are not supported yet");
                }
            }
            Symbol *s = define(&c, SYM_FN, d->name, ret, d->pos);
            s->decl = d;
            d->sym = s;
            d->type = ret;
            vec_push(prog.fns, d);
        } else if (d->kind == N_VAR) {
            Symbol *s = define(&c, SYM_VAR, d->name, NULL, d->pos);
            s->decl = d;
            d->sym = s;
            vec_push(prog.globals, s);
        }
    }

    // Pass 3: check top-level variables (initializer must be constant, spec §4.6).
    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        if (d->kind != N_VAR) continue;
        Type *decl_t = resolve_decl_type(&c, d);
        int explicit_len = 0;
        if (!decl_t && d->decl_kind == DECL_ARRAY) {
            explicit_len = eval_array_len(&c, d->len_expr);
        }
        if (d->rhs) {
            check_expr(&c, d->rhs, decl_t);
            if (d->rhs->kind == N_ARRAY_LIT || d->rhs->kind == N_STRUCT_LIT) {
                for (int j = 0; j < d->rhs->list.len; j++) {
                    Node *it = d->rhs->list.items[j];
                    Node *val = (d->rhs->kind == N_STRUCT_LIT) ? it->rhs : it;
                    if (!val->is_const) {
                        fatal(val->pos, "top-level initializer element must be a compile-time constant");
                    }
                }
                if (!decl_t) {
                    if (d->decl_kind == DECL_ARRAY) {
                        if (d->rhs->type->array_len > explicit_len) {
                            fatal(d->rhs->pos, "too many elements for '%s[%d]'", d->name, explicit_len);
                        }
                        decl_t = make_array_type(d->rhs->type->elem, explicit_len, d->pos);
                        d->rhs->type = decl_t;
                    } else {
                        decl_t = d->rhs->type;
                    }
                }
            } else {
                if (!d->rhs->is_const) {
                    fatal(d->rhs->pos, "top-level variable '%s' must have a constant initializer", d->name);
                }
                if (!decl_t) {
                    materialize(d->rhs);
                    decl_t = d->rhs->type;
                }
                d->sym->cval = d->rhs->cval;
                d->sym->fval = d->rhs->fval;
            }
        } else if (!decl_t) {
            fatal(d->pos, "top-level variable '%s' requires a type or initializer", d->name);
        }
        if (decl_t == &ty_str || decl_t->kind == TY_SLICE) {
            fatal(d->pos, "global '%s' variables are not supported yet", decl_t->name);
        }
        d->sym->type = decl_t;
        d->type = decl_t;
    }

    // Pass 4: check all function bodies.
    for (int i = 0; i < prog.fns.len; i++) {
        Node *fn = prog.fns.items[i];
        c.cur_fn = fn;
        c.ret_type = fn->type;
        c.next_slot = 0;
        c.next_var_index = 0;
        c.assigned = 0;

        push_scope(&c, true);
        for (int j = 0; j < fn->list.len; j++) {
            Node *p = fn->list.items[j];
            Symbol *ps = define(&c, SYM_PARAM, p->name, p->type, p->pos);
            ps->readonly = true;
            if (p->type->kind == TY_SLICE) {
                ps->slot = alloc_slots_bytes(&c, 16); // {ptr, len}
            } else {
                ps->slot = alloc_slot(&c); // scalar or pointer to fixed-size array
            }
            p->sym = ps;
        }
        bool returns = check_block(&c, fn->body, false);
        if (fn->type != &ty_void && !returns) {
            fatal(fn->body->end_pos, "missing return at end of function '%s' (expected '%s')",
                  fn->name, fn->type->name);
        }
        pop_scope(&c);
        fn->num_slots = c.next_slot;
    }

    // Check `main` signature (spec §5.4).
    const char *main_name = intern("main", 4);
    Symbol *m = lookup_in(c.global, main_name);
    if (!m || m->kind != SYM_FN) {
        die("missing entry point 'fn main()'");
    }
    if (m->decl->list.len != 0 || m->type != &ty_void) {
        fatal(m->pos, "'main' must have signature 'fn main()' with no parameters and no return type");
    }
    prog.main_fn = m;
    return prog;
}
