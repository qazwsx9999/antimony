// check.c - type checking, untyped-constant resolution, definite-assignment
// analysis, return-path checking, and stack-slot assignment.
#include "sb.h"

Type ty_void          = { .kind = TY_VOID,          .name = "void",          .size = 0,  .align = 1, .is_signed = false };
Type ty_bool          = { .kind = TY_BOOL,          .name = "bool",          .size = 1,  .align = 1, .is_signed = false };
Type ty_str           = { .kind = TY_STR,           .name = "str",           .size = 16, .align = 8, .is_signed = false };
Type ty_untyped_int   = { .kind = TY_UNTYPED_INT,   .name = "untyped int",   .size = 0,  .align = 1, .is_signed = true  };
Type ty_untyped_float = { .kind = TY_UNTYPED_FLOAT, .name = "untyped float", .size = 0,  .align = 1, .is_signed = true  };
Type ty_null          = { .kind = TY_NULL,          .name = "null",          .size = 8,  .align = 8, .is_signed = false };

Type ty_i8    = { .kind = TY_INT,   .name = "i8",    .size = 1, .align = 1, .is_signed = true  };
Type ty_i16   = { .kind = TY_INT,   .name = "i16",   .size = 2, .align = 2, .is_signed = true  };
Type ty_i32   = { .kind = TY_INT,   .name = "i32",   .size = 4, .align = 4, .is_signed = true  };
Type ty_i64   = { .kind = TY_INT,   .name = "i64",   .size = 8, .align = 8, .is_signed = true  };
Type ty_isize = { .kind = TY_INT,   .name = "isize", .size = 8, .align = 8, .is_signed = true  };
Type ty_u8    = { .kind = TY_INT,   .name = "u8",    .size = 1, .align = 1, .is_signed = false };
Type ty_u16   = { .kind = TY_INT,   .name = "u16",   .size = 2, .align = 2, .is_signed = false };
Type ty_u32   = { .kind = TY_INT,   .name = "u32",   .size = 4, .align = 4, .is_signed = false };
Type ty_u64   = { .kind = TY_INT,   .name = "u64",   .size = 8, .align = 8, .is_signed = false };
Type ty_usize = { .kind = TY_INT,   .name = "usize", .size = 8, .align = 8, .is_signed = false };
Type ty_f32   = { .kind = TY_FLOAT, .name = "f32",   .size = 4, .align = 4, .is_signed = true  };
Type ty_f64   = { .kind = TY_FLOAT, .name = "f64",   .size = 8, .align = 8, .is_signed = true  };

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
    int in_defer;
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
static void layout_enum(Checker *c, Type *et);

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
    } else if (d->ptr_depth == 0 && base->kind == TY_ENUM && base->state != 2) {
        layout_enum(c, base);
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

// If `arg` is a type expression (`T`, `*T`, `**T`, etc.) passed to `sizeof`, `alignof`,
// or `ptr_cast`, resolves and returns that `Type *`; otherwise returns NULL.
static Type *try_resolve_type_arg(Checker *c, Node *arg) {
    int ptr_depth = 0;
    Node *cur = arg;
    while (cur && cur->kind == N_DEREF) {
        ptr_depth++;
        cur = cur->lhs;
    }
    if (cur && cur->kind == N_IDENT) {
        Symbol *s = lookup(c, cur->name);
        if (s && s->kind == SYM_TYPE) {
            Type *t = s->type;
            if (ptr_depth == 0 && t->kind == TY_STRUCT && t->state != 2) {
                layout_struct(c, t);
            } else if (t->kind == TY_ENUM && t->state != 2) {
                layout_enum(c, t);
            }
            for (int i = 0; i < ptr_depth; i++) {
                t = make_ptr_type(t, arg->pos);
            }
            return t;
        }
    }
    return NULL;
}

static void layout_enum(Checker *c, Type *et) {
    if (et->state == 2) return;
    if (et->state == 1) {
        fatal(et->decl->pos, "cyclic enum definition involving '%s'", et->name);
    }
    et->state = 1;
    Node *d = et->decl;
    if (d->list.len == 0) {
        fatal(d->pos, "enum '%s' must have at least one variant", et->name);
    }
    i128 next_val = 0;
    bool any_payload = false;
    int max_payload_size = 0;
    int max_payload_align = 4;

    for (int i = 0; i < d->list.len; i++) {
        Node *vd = d->list.items[i];
        for (int j = 0; j < et->fields.len; j++) {
            if (et->fields.items[j].name == vd->name) {
                fatal(vd->pos, "duplicate variant '%s' in enum '%s'", vd->name, et->name);
            }
        }
        i128 val = next_val;
        if (vd->rhs) {
            check_expr(c, vd->rhs, &ty_i32);
            if (!vd->rhs->is_const) {
                fatal(vd->rhs->pos, "enum discriminant for '%s.%s' must be a compile-time constant",
                      et->name, vd->name);
            }
            val = vd->rhs->cval;
        }
        if (!fits(val, &ty_i32)) {
            char buf[48];
            fatal(vd->pos, "enum discriminant %s for '%s.%s' overflows 'i32'",
                  i128_str(val, buf), et->name, vd->name);
        }
        for (int j = 0; j < et->fields.len; j++) {
            if (et->fields.items[j].offset == (int)val) {
                fatal(vd->pos, "duplicate discriminant value %d for '%s.%s' (already used by '%s.%s')",
                      (int)val, et->name, vd->name, et->name, et->fields.items[j].name);
            }
        }
        Type *pt = et;
        bool has_payload = false;
        if (vd->type_name) {
            pt = resolve_decl_type(c, vd);
            if (pt == &ty_void) {
                fatal(vd->type_pos, "enum variant payload cannot have type 'void'");
            }
            has_payload = true;
            any_payload = true;
            int pa = pt->align ? pt->align : 8;
            if (pa > max_payload_align) max_payload_align = pa;
            if (pt->size > max_payload_size) max_payload_size = pt->size;
        }
        StructField sf = {vd->name, pt, (int)val, has_payload, vd->pos};
        vec_push(et->fields, sf);
        next_val = val + 1;
    }
    if (any_payload) {
        et->is_tagged = true;
        et->payload_offset = (4 % max_payload_align == 0) ? 4 : max_payload_align;
        int sz = et->payload_offset + max_payload_size;
        if (sz % max_payload_align != 0) sz += max_payload_align - (sz % max_payload_align);
        et->size = sz;
        et->align = max_payload_align;
        et->is_signed = false;
    } else {
        et->is_tagged = false;
        et->size = 4;
        et->align = 4;
        et->is_signed = true;
    }
    et->state = 2;
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
        int a = ft->align ? ft->align : 8;
        if (a > max_align) max_align = a;
        if (offset % a != 0) offset += a - (offset % a);

        StructField sf = {fd->name, ft, offset, false, fd->pos};
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

// Verifies that an lvalue (`x`, `p.x`, `arr[i]`) is not rooted at a read-only
// value parameter, range-loop variable, or immutable string without an intervening pointer dereference.
static void check_lvalue_mutable(Node *lval, Pos pos) {
    if (lval && lval->kind == N_INDEX && lval->lhs->type == &ty_str) {
        fatal(pos, "cannot mutate or take address of byte in immutable 'str'");
    }
    Node *base = lval;
    while (base && (base->kind == N_FIELD || base->kind == N_INDEX)) {
        if (base->lhs->type && (base->lhs->type->kind == TY_PTR || base->lhs->type->kind == TY_SLICE)) return;
        base = base->lhs;
    }
    if (base && (base->kind == N_STRUCT_LIT || base->kind == N_ENUM_LIT || base->kind == N_CALL)) {
        fatal(pos, "cannot mutate or take address of temporary '%s' value", base->type->name);
    }
    if (base && base->kind == N_IDENT && base->sym) {
        if (base->sym->kind == SYM_PARAM) {
            fatal(pos, "cannot mutate or take address of read-only parameter '%s' (use a pointer parameter '*%s')",
                  base->sym->name, base->sym->type->name);
        }
        if (base->sym->readonly) {
            fatal(pos, "cannot mutate or take address of read-only loop variable '%s'", base->sym->name);
        }
    }
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
    // `str` coerces to `*u8` or `*i8` (null-terminated C string pointer for FFI).
    if (target->kind == TY_PTR && (target->elem == &ty_u8 || target->elem == &ty_i8) &&
        n->type == &ty_str) {
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
            if (opd->is_const) {
                fatal(n->pos, "cannot take address '&' of constant or enum variant");
            }
            check_lvalue_mutable(opd, n->pos);
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
        // Inferred enum variant access `.Variant`
        if (n->op == T_DOT && !n->lhs) {
            if (!expected || expected->kind != TY_ENUM) {
                fatal(n->pos, "inferred enum variant '.%s' requires a known enum type context", n->name);
            }
            Type *et = expected;
            if (et->state != 2) layout_enum(c, et);
            StructField *sf = find_field(et, n->name);
            if (!sf) {
                fatal(n->pos, "enum '%s' has no variant named '%s'", et->name, n->name);
            }
            if (sf->has_payload) {
                fatal(n->pos, "enum variant '%s.%s' requires a payload of type '%s'",
                      et->name, n->name, sf->type->name);
            }
            if (et->is_tagged) {
                n->kind = N_ENUM_LIT;
                n->type = et;
                n->cval = sf->offset;
                n->lhs  = NULL;
                if (c->cur_fn) n->slot_start = alloc_slots_bytes(c, et->size);
            } else {
                n->type = et;
                n->is_const = true;
                n->cval = sf->offset;
            }
            return;
        }
        // Check for enum variant access `EnumName.Variant` before checking `n->lhs` as a value.
        if (n->lhs->kind == N_IDENT) {
            Symbol *s = lookup(c, n->lhs->name);
            if (s && s->kind == SYM_TYPE && s->type->kind == TY_ENUM) {
                Type *et = s->type;
                if (et->state != 2) layout_enum(c, et);
                StructField *sf = find_field(et, n->name);
                if (!sf) {
                    fatal(n->pos, "enum '%s' has no variant named '%s'", et->name, n->name);
                }
                if (sf->has_payload) {
                    fatal(n->pos, "enum variant '%s.%s' requires a payload of type '%s'",
                          et->name, n->name, sf->type->name);
                }
                if (et->is_tagged) {
                    n->kind = N_ENUM_LIT;
                    n->type = et;
                    n->cval = sf->offset;
                    n->lhs  = NULL;
                    if (c->cur_fn) n->slot_start = alloc_slots_bytes(c, et->size);
                } else {
                    n->type = et;
                    n->is_const = true;
                    n->cval = sf->offset;
                }
                if (expected) coerce(n, expected);
                return;
            }
            // If `lhs` is an uninitialized variable `var p: Point`, allow writing `p.x = 1`,
            // and mark `p` as assigned on first field access.
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
        if (bt->kind != TY_ARRAY && bt->kind != TY_SLICE && bt->kind != TY_PTR && bt != &ty_str) {
            fatal(n->pos, "cannot index value of type '%s' (expected array, slice, string, or pointer)", bt->name);
        }
        check_expr(c, n->rhs, NULL);
        if (!is_int(n->rhs->type)) {
            fatal(n->rhs->pos, "index must be an integer, got '%s'", n->rhs->type->name);
        }
        materialize(n->rhs);
        if (bt->kind != TY_PTR && n->rhs->is_const) {
            if (n->rhs->cval < 0) {
                char buf[48];
                fatal(n->rhs->pos, "index %s cannot be negative", i128_str(n->rhs->cval, buf));
            }
            if (bt->kind == TY_ARRAY && n->rhs->cval >= bt->array_len) {
                char buf[48];
                fatal(n->rhs->pos, "array index %s out of bounds for '%s' (length %d)",
                      i128_str(n->rhs->cval, buf), bt->name, bt->array_len);
            }
            if (bt == &ty_str && n->lhs->kind == N_STR && n->rhs->cval >= n->lhs->str_len) {
                char buf[48];
                fatal(n->rhs->pos, "string index %s out of bounds (length %d)",
                      i128_str(n->rhs->cval, buf), n->lhs->str_len);
            }
        }
        n->type = (bt == &ty_str) ? &ty_u8 : bt->elem;
        if (expected) coerce(n, expected);
        return;
    }

    case N_SLICE: {
        check_expr(c, n->lhs, NULL);
        Type *bt = n->lhs->type;
        if (bt->kind != TY_ARRAY && bt->kind != TY_SLICE && bt->kind != TY_PTR && bt != &ty_str) {
            fatal(n->pos, "cannot slice value of type '%s' (expected array, slice, string, or pointer)", bt->name);
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
            if (bt == &ty_str && n->lhs->kind == N_STR && n->lo->is_const && n->lo->cval > n->lhs->str_len) {
                fatal(n->lo->pos, "slice lower bound %d exceeds string length %d",
                      (int)n->lo->cval, n->lhs->str_len);
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
            if (bt == &ty_str && n->lhs->kind == N_STR && n->hi->is_const && n->hi->cval > n->lhs->str_len) {
                fatal(n->hi->pos, "slice upper bound %d exceeds string length %d",
                      (int)n->hi->cval, n->lhs->str_len);
            }
        }
        if (n->lo && n->hi && n->lo->is_const && n->hi->is_const && n->lo->cval > n->hi->cval) {
            fatal(n->pos, "slice lower bound (%d) > upper bound (%d)",
                  (int)n->lo->cval, (int)n->hi->cval);
        }
        n->type = (bt == &ty_str) ? &ty_str : make_slice_type(bt->elem, n->pos);
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
                if (ot->kind == TY_ENUM && ot->is_tagged) {
                    fatal(n->pos, "cannot compare tagged union enum '%s' with %s (use 'match')",
                          ot->name, tok_name(op));
                }
                if (!is_int(ot) && ot != &ty_bool && ot->kind != TY_ENUM && ot != &ty_str) {
                    fatal(n->pos, "operator %s is not defined for '%s'", tok_name(op), ot->name);
                }
            } else {
                if (!is_int(ot)) {
                    fatal(n->pos, "comparison %s requires integer operands, got '%s'", tok_name(op), ot->name);
                }
            }
            n->type = &ty_bool;
            if (ot == &ty_str && n->lhs->kind == N_STR && n->rhs->kind == N_STR) {
                bool eq = (n->lhs->str_len == n->rhs->str_len) &&
                          (memcmp(n->lhs->str, n->rhs->str, (size_t)n->lhs->str_len) == 0);
                n->is_const = true;
                n->cval = (op == T_EQ) ? eq : !eq;
            }
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
        // Inferred payload enum variant constructor: `.Variant(arg)`
        if (n->op == T_DOT && !n->lhs) {
            if (!expected || expected->kind != TY_ENUM) {
                fatal(n->pos, "inferred enum variant '.%s(...)' requires a known enum type context", n->name);
            }
            Type *et = expected;
            if (et->state != 2) layout_enum(c, et);
            StructField *sf = find_field(et, n->name);
            if (!sf) {
                fatal(n->pos, "enum '%s' has no variant named '%s'", et->name, n->name);
            }
            if (!sf->has_payload) {
                fatal(n->pos, "enum variant '%s.%s' does not take a payload", et->name, n->name);
            }
            if (n->list.len != 1) {
                fatal(n->pos, "enum variant '%s.%s' expects 1 payload argument, got %d",
                      et->name, n->name, n->list.len);
            }
            check_expr(c, n->list.items[0], sf->type);
            n->kind = N_ENUM_LIT;
            n->type = et;
            n->cval = sf->offset;
            n->sym2 = (Symbol *)sf;
            n->lhs  = n->list.items[0];
            if (c->cur_fn) n->slot_start = alloc_slots_bytes(c, et->size);
            return;
        }

        // Method call `recv.method(...)` OR explicit payload enum variant `EnumName.Variant(arg)`
        if (n->lhs) {
            if (n->lhs->kind == N_IDENT) {
                Symbol *vs = lookup(c, n->lhs->name);
                if (vs && vs->kind == SYM_TYPE && vs->type->kind == TY_ENUM) {
                    Type *et = vs->type;
                    if (et->state != 2) layout_enum(c, et);
                    StructField *sf = find_field(et, n->name);
                    if (!sf) {
                        fatal(n->pos, "enum '%s' has no variant named '%s'", et->name, n->name);
                    }
                    if (!sf->has_payload) {
                        fatal(n->pos, "enum variant '%s.%s' does not take a payload", et->name, n->name);
                    }
                    if (n->list.len != 1) {
                        fatal(n->pos, "enum variant '%s.%s' expects 1 payload argument, got %d",
                              et->name, n->name, n->list.len);
                    }
                    check_expr(c, n->list.items[0], sf->type);
                    n->kind = N_ENUM_LIT;
                    n->type = et;
                    n->cval = sf->offset;
                    n->sym2 = (Symbol *)sf;
                    n->lhs  = n->list.items[0];
                    if (c->cur_fn) n->slot_start = alloc_slots_bytes(c, et->size);
                    if (expected) coerce(n, expected);
                    return;
                }
                if (vs && vs->kind == SYM_VAR && !vs->is_global && vs->var_index >= 0 &&
                    vs->type && (vs->type->kind == TY_STRUCT || vs->type->kind == TY_ENUM)) {
                    char mname[256];
                    int mlen = snprintf(mname, sizeof(mname), "%s.%s", vs->type->name, n->name);
                    Symbol *ms = lookup_in(c->global, intern(mname, mlen));
                    if (ms && ms->kind == SYM_FN && ms->decl->list.items[0]->type->kind == TY_PTR) {
                        c->assigned |= (1ULL << vs->var_index);
                    }
                }
            }

            check_expr(c, n->lhs, NULL);
            Type *rt = n->lhs->type;
            Type *base_t = NULL;
            if (rt->kind == TY_STRUCT || rt->kind == TY_ENUM) {
                base_t = rt;
            } else if (rt->kind == TY_PTR && (rt->elem->kind == TY_STRUCT || rt->elem->kind == TY_ENUM)) {
                base_t = rt->elem;
            } else {
                fatal(n->pos, "cannot call method '.%s' on type '%s' (expected struct, enum, or pointer to struct/enum)",
                      n->name, rt->name);
            }

            char mname[256];
            int mlen = snprintf(mname, sizeof(mname), "%s.%s", base_t->name, n->name);
            Symbol *ms = lookup_in(c->global, intern(mname, mlen));
            if (!ms || ms->kind != SYM_FN) {
                fatal(n->pos, "type '%s' has no method named '%s'", base_t->name, n->name);
            }

            Node *fn = ms->decl;
            int expected_args = fn->list.len - 1;
            if (n->list.len != expected_args) {
                fatal(n->pos, "method '%s.%s' expects %d argument(s), got %d",
                      base_t->name, n->name, expected_args, n->list.len);
            }

            for (int i = 0; i < n->list.len; i++) {
                Type *pt = fn->list.items[i + 1]->type;
                check_expr(c, n->list.items[i], pt);
            }

            Type *recv_pt = fn->list.items[0]->type;
            Node *recv_arg = n->lhs;
            if (recv_pt->kind == TY_PTR && rt == base_t) {
                if (recv_arg->is_const ||
                    (recv_arg->kind != N_IDENT && recv_arg->kind != N_INDEX &&
                     recv_arg->kind != N_DEREF && recv_arg->kind != N_FIELD)) {
                    fatal(n->pos, "cannot call pointer-receiver method '%s.%s' on non-addressable '%s' value",
                          base_t->name, n->name, base_t->name);
                }
                check_lvalue_mutable(recv_arg, n->pos);
                Node *addr = xcalloc(1, sizeof(Node));
                addr->kind = N_ADDR;
                addr->pos  = recv_arg->pos;
                addr->lhs  = recv_arg;
                addr->type = recv_pt;
                recv_arg   = addr;
            } else if (recv_pt == base_t && rt->kind == TY_PTR) {
                Node *deref = xcalloc(1, sizeof(Node));
                deref->kind = N_DEREF;
                deref->pos  = recv_arg->pos;
                deref->lhs  = recv_arg;
                deref->type = base_t;
                recv_arg    = deref;
            }

            vec_push(n->list, NULL);
            for (int i = n->list.len - 1; i > 0; i--) {
                n->list.items[i] = n->list.items[i - 1];
            }
            n->list.items[0] = recv_arg;
            n->lhs  = NULL;
            n->name = ms->name;
            n->sym  = ms;
            n->type = ms->type;
            if (is_aggregate_type(n->type) && c->cur_fn) {
                n->slot_start = alloc_slots_bytes(c, n->type->size);
            }
            if (expected) {
                if (n->type == &ty_void) {
                    fatal(n->pos, "method '%s.%s' returns no value", base_t->name, fn->name2);
                }
                coerce(n, expected);
            }
            return;
        }

        Symbol *s = lookup(c, n->name);
        if (!s) fatal(n->pos, "undeclared function or type '%s'", n->name);

        // Type conversion: `i64(x)`, `f64(x)`, `f32(x)` (spec §2.2, §2.3)
        if (s->kind == SYM_TYPE) {
            if (n->list.len != 1) {
                fatal(n->pos, "type conversion '%s(...)' takes exactly 1 argument", n->name);
            }
            Node *arg = n->list.items[0];
            check_expr(c, arg, NULL);
            if ((s->type->kind == TY_ENUM && s->type->is_tagged) ||
                (arg->type->kind == TY_ENUM && arg->type->is_tagged)) {
                fatal(n->pos, "cannot convert '%s' to '%s' (tagged union enums cannot be cast as integers)",
                      arg->type->name, s->type->name);
            }
            if (s->type == &ty_str) {
                if (arg->type == &ty_str) {
                    n->kind = N_CONV;
                    n->lhs = arg;
                    n->type = &ty_str;
                    if (expected) coerce(n, expected);
                    return;
                }
                if ((arg->type->kind == TY_SLICE || arg->type->kind == TY_ARRAY) &&
                    arg->type->elem == &ty_u8) {
                    if (arg->type->kind == TY_ARRAY) {
                        arg->implicit_slice = true;
                    }
                    n->kind = N_CONV;
                    n->lhs = arg;
                    n->type = &ty_str;
                    if (expected) coerce(n, expected);
                    return;
                }
                fatal(n->pos, "cannot convert '%s' to 'str' (expected '[]u8' or '[N]u8')", arg->type->name);
            }
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
            if (arg->type == &ty_untyped_int && (is_int(s->type) || s->type->kind == TY_ENUM)) {
                coerce(arg, (s->type->kind == TY_ENUM) ? &ty_i32 : s->type);
            } else if (arg->type->kind == TY_PTR && s->type->kind == TY_INT && s->type->size == 8) {
                n->kind = N_CONV;
                n->lhs = arg;
                n->type = s->type;
                if (expected) coerce(n, expected);
                return;
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
            } else if (!((is_int(arg->type) || arg->type->kind == TY_ENUM) &&
                         (is_int(s->type)  || s->type->kind == TY_ENUM))) {
                fatal(n->pos, "cannot convert '%s' to '%s'", arg->type->name, s->type->name);
            }
            n->kind = N_CONV;
            n->lhs = arg;
            n->type = s->type;
            if (arg->is_const) {
                Type *fit_t = (s->type->kind == TY_ENUM) ? &ty_i32 : s->type;
                if (!fits(arg->cval, fit_t)) {
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

        // Built-in functions (`print`, `exit`, `rev`, `len`, `sizeof`, `alignof`, `ptr_cast`).
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
            if (s->builtin == B_SIZEOF || s->builtin == B_ALIGNOF) {
                const char *bname = (s->builtin == B_SIZEOF) ? "sizeof" : "alignof";
                if (n->list.len != 1) fatal(n->pos, "'%s' takes 1 argument (type or expression)", bname);
                Node *arg = n->list.items[0];
                Type *t = try_resolve_type_arg(c, arg);
                if (!t) {
                    check_expr(c, arg, NULL);
                    materialize(arg);
                    t = arg->type;
                }
                if (t == &ty_void) {
                    fatal(arg->pos, "'%s' is not valid on 'void'", bname);
                }
                if (t->kind == TY_STRUCT && t->state != 2) {
                    layout_struct(c, t);
                }
                n->type = &ty_untyped_int;
                n->is_const = true;
                n->cval = (s->builtin == B_SIZEOF) ? t->size : t->align;
                if (expected) coerce(n, expected);
                return;
            }
            if (s->builtin == B_PTR_CAST) {
                if (n->list.len != 2) {
                    fatal(n->pos, "'ptr_cast' takes 2 arguments: ptr_cast(*T, expr)");
                }
                Type *target_t = try_resolve_type_arg(c, n->list.items[0]);
                if (!target_t || target_t->kind != TY_PTR) {
                    fatal(n->list.items[0]->pos, "first argument to 'ptr_cast' must be a pointer type '*T'");
                }
                Node *val = n->list.items[1];
                check_expr(c, val, NULL);
                if (val->type == &ty_untyped_int) {
                    coerce(val, &ty_usize);
                } else if (val->type == &ty_null) {
                    coerce(val, target_t);
                } else if (val->type->kind != TY_PTR && val->type != &ty_str &&
                           !(val->type->kind == TY_INT && val->type->size == 8)) {
                    fatal(val->pos, "'ptr_cast' source must be a pointer, 'str', or 64-bit integer ('usize'/'i64'), got '%s'",
                          val->type->name);
                }
                n->kind = N_CONV;
                n->lhs = val;
                n->type = target_t;
                if (expected) coerce(n, expected);
                return;
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
        if (is_aggregate_type(n->type) && c->cur_fn) {
            n->slot_start = alloc_slots_bytes(c, n->type->size);
        }
        if (expected) {
            if (n->type == &ty_void) {
                fatal(n->pos, "function '%s' returns no value", n->name);
            }
            coerce(n, expected);
        }
        return;
    }

    case N_TRY: {
        if (c->in_defer > 0) {
            fatal(n->pos, "'?' operator is not allowed inside a 'defer' statement");
        }
        check_expr(c, n->lhs, NULL);
        Type *et = n->lhs->type;
        if (et->kind != TY_ENUM || !et->is_tagged) {
            fatal(n->pos, "'?' operator requires a tagged union enum ('Ok'/'Err' or 'Some'/'None'), got '%s'", et->name);
        }
        StructField *ok_v  = find_field(et, intern("Ok", 2));
        StructField *err_v = find_field(et, intern("Err", 3));
        if (!ok_v || !err_v) {
            ok_v  = find_field(et, intern("Some", 4));
            err_v = find_field(et, intern("None", 4));
        }
        if (!ok_v || !err_v || !ok_v->has_payload) {
            fatal(n->pos, "'?' operator requires an enum with 'Ok(T)'/'Err(E)' or 'Some(T)'/'None' variants (got '%s')",
                  et->name);
        }
        if (!c->ret_type || c->ret_type->kind != TY_ENUM || !c->ret_type->is_tagged) {
            fatal(n->pos, "cannot use '?' in function '%s' returning '%s' (enclosing function must return a compatible 'Result' or 'Option' enum)",
                  c->cur_fn ? c->cur_fn->name : "", c->ret_type ? c->ret_type->name : "void");
        }
        StructField *ret_err_v = find_field(c->ret_type, err_v->name);
        if (!ret_err_v || ret_err_v->has_payload != err_v->has_payload ||
            (err_v->has_payload && ret_err_v->type != err_v->type)) {
            fatal(n->pos, "cannot propagate '%s.%s' via '?' in function returning '%s'",
                  et->name, err_v->name, c->ret_type->name);
        }
        n->type = ok_v->type;
        n->cval = err_v->offset;             // failure discriminant tag on `et`
        n->field_offset = ret_err_v->offset; // failure discriminant tag on `c->ret_type`
        n->sym  = (Symbol *)ok_v;            // pointer to `StructField` of `ok_v`
        n->sym2 = (Symbol *)err_v;           // pointer to `StructField` of `err_v` (has_payload / type)
        if (c->cur_fn) {
            n->slot_start = alloc_slot(c);   // holds evaluated scrutinee pointer
            if (is_aggregate_type(n->type)) {
                n->slot_end = alloc_slots_bytes(c, n->type->size); // unwrapped struct/enum copy buffer
            }
        }
        if (expected) coerce(n, expected);
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
    case N_MATCH:
        for (int i = 0; i < stmt->list.len; i++) {
            if (loop_has_break_to(stmt->list.items[i]->body, target)) return true;
        }
        return loop_has_break_to(stmt->els, target);
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
    case N_MATCH:
        for (int i = 0; i < stmt->list.len; i++)
            if (body_assigns_var(stmt->list.items[i]->body, v)) return true;
        return body_assigns_var(stmt->els, v);
    case N_DEFER:
        return body_assigns_var(stmt->body, v);
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
        if (n->lhs->is_const) {
            fatal(n->pos, "cannot assign to constant or enum variant");
        }
        check_lvalue_mutable(n->lhs, n->pos);
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
        } else if (decl_t->kind == TY_ENUM && decl_t->is_tagged) {
            s->slot = alloc_slots_bytes(c, decl_t->size);
            if (n->rhs) {
                s->var_index = -1;
            } else {
                if (c->next_var_index >= 63) fatal(n->pos, "too many uninitialized variables");
                s->var_index = c->next_var_index++;
                c->assigned &= ~(1ULL << s->var_index);
            }
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
        if (c->in_defer > 0) {
            fatal(n->pos, "'return' is not allowed inside a 'defer' statement");
        }
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
            if (c->ret_type->kind == TY_SLICE) {
                Node *base = n->lhs;
                while (base) {
                    if (base->kind == N_SLICE || base->kind == N_FIELD || base->kind == N_INDEX) {
                        if (base->lhs && base->lhs->type &&
                            (base->lhs->type->kind == TY_PTR || base->lhs->type->kind == TY_SLICE)) {
                            break;
                        }
                        base = base->lhs;
                    } else if (base->kind == N_IDENT && base->sym &&
                               base->sym->kind == SYM_VAR && !base->sym->is_global) {
                        if (base->sym->type->kind == TY_ARRAY || base->sym->type->kind == TY_STRUCT) {
                            fatal(n->lhs->pos,
                                  "cannot return slice of local variable '%s' (stack frame is destroyed on return)",
                                  base->sym->name);
                        }
                        if (base->sym->decl && base->sym->decl->rhs) {
                            base = base->sym->decl->rhs;
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }
            }
        }
        c->assigned = ALL_ASSIGNED;
        return true;

    case N_BREAK:
    case N_CONTINUE: {
        if (c->in_defer > 0) {
            fatal(n->pos, "'%s' is not allowed inside a 'defer' statement",
                  n->kind == N_BREAK ? "break" : "continue");
        }
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

    case N_DEFER: {
        if (c->in_defer > 0) {
            fatal(n->pos, "cannot nest 'defer' inside another 'defer'");
        }
        if (n->body->kind == N_VAR || n->body->kind == N_CONST) {
            fatal(n->body->pos, "cannot declare a variable or constant directly in 'defer' (use 'defer { ... }')");
        }
        c->in_defer++;
        AssignSet saved = c->assigned;
        if (n->body->kind == N_BLOCK) {
            check_block(c, n->body, true);
        } else {
            check_stmt(c, n->body);
        }
        c->assigned = saved;
        c->in_defer--;
        return false;
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

    case N_MATCH: {
        check_expr(c, n->cond, NULL);
        materialize(n->cond);
        Type *st = n->cond->type;
        if (st->kind != TY_ENUM && st->kind != TY_INT && st != &ty_bool && st != &ty_str) {
            fatal(n->cond->pos, "'match' expression must be an enum, integer, bool, or str (got '%s')", st->name);
        }
        if (n->list.len == 0 && !n->els) {
            fatal(n->pos, "'match' statement must have at least one arm");
        }
        if (st == &ty_str) {
            n->slot_start = alloc_slot(c); // scrutinee ptr
            n->slot_end   = alloc_slot(c); // scrutinee len
        } else if (st->kind == TY_ENUM && st->is_tagged) {
            n->slot_start = alloc_slot(c); // scrutinee base address
        }

        VEC(i128)   seen_vals = {0};
        VEC(Node *) seen_strs = {0};
        VEC(Pos)    seen_pos  = {0};
        AssignSet before = c->assigned;
        AssignSet after_all = ALL_ASSIGNED;
        bool all_ret = true;

        for (int i = 0; i < n->list.len; i++) {
            Node *arm = n->list.items[i];
            arm->name = NULL;
            arm->sym  = NULL;
            for (int j = 0; j < arm->list.len; j++) {
                Node *pat = arm->list.items[j];
                if (st->kind == TY_ENUM && st->is_tagged) {
                    // Tagged union pattern: `.Variant(bind)`, `Enum.Variant(bind)`, `.Variant`, or `Enum.Variant`
                    bool is_dot_pat = (pat->op == T_DOT && !pat->lhs);
                    bool is_explicit_pat = (!is_dot_pat && pat->lhs && pat->lhs->kind == N_IDENT);
                    if (!is_dot_pat && !is_explicit_pat) {
                        fatal(pat->pos, "expected enum variant pattern for '%s'", st->name);
                    }
                    if (is_explicit_pat) {
                        Symbol *ts = lookup(c, pat->lhs->name);
                        if (!ts || ts->kind != SYM_TYPE || ts->type != st) {
                            fatal(pat->pos, "expected variant of enum '%s'", st->name);
                        }
                    }
                    StructField *sf = find_field(st, pat->name);
                    if (!sf) {
                        fatal(pat->pos, "enum '%s' has no variant named '%s'", st->name, pat->name);
                    }
                    if (pat->kind == N_CALL) {
                        if (!sf->has_payload) {
                            fatal(pat->pos, "enum variant '%s.%s' has no payload to destructure",
                                  st->name, pat->name);
                        }
                        if (pat->list.len != 1 || pat->list.items[0]->kind != N_IDENT) {
                            fatal(pat->pos, "expected a single identifier binding in pattern '%s.%s(name)'",
                                  st->name, pat->name);
                        }
                        if (arm->list.len > 1) {
                            fatal(pat->pos, "cannot combine payload-binding pattern '%s.%s(%s)' with multiple patterns in one arm",
                                  st->name, pat->name, pat->list.items[0]->name);
                        }
                        arm->name = pat->list.items[0]->name;
                        arm->type = sf->type;
                    } else if (pat->kind != N_FIELD) {
                        fatal(pat->pos, "invalid match pattern for enum '%s'", st->name);
                    }
                    pat->type = st;
                    pat->sym2 = (Symbol *)sf;
                    pat->is_const = true;
                    pat->cval = sf->offset;
                    for (int k = 0; k < seen_vals.len; k++) {
                        if (seen_vals.items[k] == pat->cval) {
                            fatal(pat->pos, "duplicate match pattern (already matched at line %d)",
                                  seen_pos.items[k].line);
                        }
                    }
                    vec_push(seen_vals, pat->cval);
                    vec_push(seen_pos, pat->pos);
                    continue;
                }
                check_expr(c, pat, st);
                if (st == &ty_str) {
                    if (pat->kind != N_STR) {
                        fatal(pat->pos, "string match pattern must be a string literal");
                    }
                    for (int k = 0; k < seen_strs.len; k++) {
                        Node *prev = seen_strs.items[k];
                        if (prev->str_len == pat->str_len &&
                            memcmp(prev->str, pat->str, (size_t)pat->str_len) == 0) {
                            fatal(pat->pos, "duplicate match pattern (already matched at line %d)",
                                  seen_pos.items[k].line);
                        }
                    }
                    vec_push(seen_strs, pat);
                    vec_push(seen_pos, pat->pos);
                } else {
                    if (!pat->is_const) {
                        fatal(pat->pos, "match pattern must be a compile-time constant");
                    }
                    for (int k = 0; k < seen_vals.len; k++) {
                        if (seen_vals.items[k] == pat->cval) {
                            fatal(pat->pos, "duplicate match pattern (already matched at line %d)",
                                  seen_pos.items[k].line);
                        }
                    }
                    vec_push(seen_vals, pat->cval);
                    vec_push(seen_pos, pat->pos);
                }
            }
            c->assigned = before;
            push_scope(c, false);
            if (arm->name && strcmp(arm->name, "_") != 0) {
                Type *pt = arm->type;
                Symbol *bs = define(c, SYM_VAR, arm->name, pt, arm->pos);
                bs->readonly = true;
                bs->var_index = -1;
                if (pt->kind == TY_ARRAY || is_aggregate_type(pt)) {
                    bs->slot = alloc_slots_bytes(c, pt->size);
                } else if (pt->kind == TY_SLICE || pt == &ty_str) {
                    bs->slot = alloc_slots_bytes(c, 16);
                } else {
                    bs->slot = alloc_slot(c);
                }
                arm->sym = bs;
            }
            bool arm_ret = check_block(c, arm->body, false);
            pop_scope(c);
            after_all &= c->assigned;
            if (!arm_ret) all_ret = false;
        }

        bool exhaustive = false;
        if (n->els) {
            if (st->kind == TY_ENUM && seen_vals.len == st->fields.len) {
                fatal(n->els->pos, "unreachable 'else' arm: all variants of enum '%s' are already covered", st->name);
            }
            if (st == &ty_bool && seen_vals.len == 2) {
                fatal(n->els->pos, "unreachable 'else' arm: both 'true' and 'false' are already covered");
            }
            c->assigned = before;
            bool els_ret = check_block(c, n->els, true);
            after_all &= c->assigned;
            if (!els_ret) all_ret = false;
            exhaustive = true;
        } else if (st->kind == TY_ENUM) {
            for (int i = 0; i < st->fields.len; i++) {
                int v = st->fields.items[i].offset;
                bool covered = false;
                for (int k = 0; k < seen_vals.len; k++) {
                    if (seen_vals.items[k] == v) {
                        covered = true;
                        break;
                    }
                }
                if (!covered) {
                    free(seen_vals.items);
                    free(seen_strs.items);
                    free(seen_pos.items);
                    fatal(n->pos, "non-exhaustive 'match': variant '%s.%s' is not covered (add an arm or 'else' branch)",
                          st->name, st->fields.items[i].name);
                }
            }
            exhaustive = true;
        } else if (st == &ty_bool) {
            bool has_true = false, has_false = false;
            for (int k = 0; k < seen_vals.len; k++) {
                if (seen_vals.items[k] != 0) has_true = true;
                else has_false = true;
            }
            if (!has_true || !has_false) {
                free(seen_vals.items);
                free(seen_strs.items);
                free(seen_pos.items);
                fatal(n->pos, "non-exhaustive 'match' on 'bool': missing '%s' arm (or 'else' branch)",
                      !has_true ? "true" : "false");
            }
            exhaustive = true;
        } else if (st == &ty_str) {
            free(seen_vals.items);
            free(seen_strs.items);
            free(seen_pos.items);
            fatal(n->pos, "non-exhaustive 'match' on 'str': an 'else' branch is required");
        } else {
            free(seen_vals.items);
            free(seen_strs.items);
            free(seen_pos.items);
            fatal(n->pos, "non-exhaustive 'match' on integer type '%s': an 'else' branch is required", st->name);
        }

        free(seen_vals.items);
        free(seen_strs.items);
        free(seen_pos.items);
        c->assigned = exhaustive ? after_all : before;
        return exhaustive && all_ret;
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
        if (ct->kind != TY_ARRAY && ct->kind != TY_SLICE && ct != &ty_str) {
            fatal(n->lhs->pos, "cannot iterate over '%s' (expected array, slice, or string)", ct->name);
        }
        Type *elem_t = (ct == &ty_str) ? &ty_u8 : ct->elem;
        AssignSet before = c->assigned;
        push_scope(c, false);
        if (n->name2) {
            // `loop i, x in arr`
            Symbol *idx_sym = define(c, SYM_VAR, n->name, &ty_i64, n->pos);
            idx_sym->readonly = true;
            idx_sym->slot = alloc_slot(c);
            Symbol *val_sym = define(c, SYM_VAR, n->name2, elem_t, n->pos);
            val_sym->readonly = true;
            val_sym->slot = is_aggregate_type(elem_t) ? alloc_slots_bytes(c, elem_t->size) : alloc_slot(c);
            n->sym  = idx_sym;
            n->sym2 = val_sym;
        } else {
            // `loop x in arr`
            Symbol *val_sym = define(c, SYM_VAR, n->name, elem_t, n->pos);
            val_sym->readonly = true;
            val_sym->slot = is_aggregate_type(elem_t) ? alloc_slots_bytes(c, elem_t->size) : alloc_slot(c);
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
        Symbol *s = define(c, SYM_TYPE, t->name, t, (Pos){0, 0, 0});
        (void)s;
    }
    struct { const char *name; int id; } builtins[] = {
        {"print", B_PRINT}, {"exit", B_EXIT}, {"rev", B_REV}, {"len", B_LEN},
        {"sizeof", B_SIZEOF}, {"alignof", B_ALIGNOF}, {"ptr_cast", B_PTR_CAST}
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        const char *nm = intern(builtins[i].name, (int)strlen(builtins[i].name));
        Symbol *s = define(c, SYM_BUILTIN, nm, &ty_void, (Pos){0, 0, 0});
        s->builtin = builtins[i].id;
    }
}

static void register_dll_import(Program *prog, const char *dll_name, const char *fn_name) {
    int d_idx = -1;
    for (int i = 0; i < prog->dll_imports.len; i++) {
        if (_stricmp(prog->dll_imports.items[i].dll_name, dll_name) == 0) {
            d_idx = i;
            break;
        }
    }
    if (d_idx < 0) {
        DllImport di = {0};
        di.dll_name = dll_name;
        vec_push(prog->dll_imports, di);
        d_idx = prog->dll_imports.len - 1;
    }
    DllImport *di = &prog->dll_imports.items[d_idx];
    for (int j = 0; j < di->funcs.len; j++) {
        if (strcmp(di->funcs.items[j], fn_name) == 0) return;
    }
    vec_push(di->funcs, fn_name);
}

static int find_iat_slot(Program *prog, const char *dll_name, const char *fn_name) {
    for (int i = 0; i < prog->dll_imports.len; i++) {
        DllImport *di = &prog->dll_imports.items[i];
        if (_stricmp(di->dll_name, dll_name) == 0) {
            for (int j = 0; j < di->funcs.len; j++) {
                if (strcmp(di->funcs.items[j], fn_name) == 0) {
                    return di->first_iat_slot + j;
                }
            }
        }
    }
    die("internal error: extern function '%s' not found in DLL import table", fn_name);
}

Program check(NodeVec decls) {
    Checker c = {0};
    c.global = push_scope(&c, false);
    register_builtins(&c);

    // Seed built-in KERNEL32.dll imports at IAT slots 0, 1, 2.
    Program prog = {0};
    const char *k32 = intern("KERNEL32.dll", 12);
    register_dll_import(&prog, k32, intern("GetStdHandle", 12));
    register_dll_import(&prog, k32, intern("WriteFile", 9));
    register_dll_import(&prog, k32, intern("ExitProcess", 11));

    // Pass 1: register all top-level names so structs, enums, constants, and functions
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
        } else if (d->kind == N_ENUM) {
            Type *et = xcalloc(1, sizeof(Type));
            et->kind = TY_ENUM;
            et->name = d->name;
            et->decl = d;
            et->size = 4;
            et->align = 4;
            et->is_signed = true;
            Symbol *s = define(&c, SYM_TYPE, d->name, et, d->pos);
            d->sym = s;
            d->type = et;
        } else if (d->kind == N_CONST) {
            Symbol *s = define(&c, SYM_CONST, d->name, &ty_untyped_int, d->pos);
            s->decl = d;
            d->sym = s;
        }
    }
    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        if (d->kind == N_ENUM) layout_enum(&c, d->type);
    }
    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        if (d->kind == N_CONST) resolve_global_const(&c, d->sym);
    }
    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        if (d->kind == N_STRUCT) layout_struct(&c, d->type);
    }

    VEC(Symbol *) extern_syms = {0};
    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        if (d->kind == N_FN) {
            Type *ret = d->type_name ? resolve_decl_type(&c, d) : &ty_void;
            if (ret->kind == TY_ARRAY) {
                fatal(d->type_pos, "returning fixed-size array '%s' by value is not allowed (return a slice '[]%s' or pass a pointer)",
                      ret->name, ret->elem->name);
            }
            if (d->is_extern && (ret == &ty_str || ret->kind == TY_SLICE || ret->kind == TY_STRUCT)) {
                fatal(d->type_pos, "extern function '%s' cannot return '%s' by value (return a pointer instead)",
                      d->name, ret->name);
            }
            for (int j = 0; j < d->list.len; j++) {
                Node *p = d->list.items[j];
                p->type = resolve_decl_type(&c, p);
                if (d->is_extern && (p->type == &ty_str || p->type->kind == TY_SLICE ||
                                     p->type->kind == TY_ARRAY || p->type->kind == TY_STRUCT)) {
                    fatal(p->type_pos, "extern function parameter '%s' cannot have type '%s' by value; pass a pointer (e.g. '*u8') instead",
                          p->name, p->type->name);
                }
            }
            if (d->name2) {
                Type *rt = d->list.items[0]->type;
                Type *base_t = (rt->kind == TY_PTR) ? rt->elem : rt;
                if (base_t->kind != TY_STRUCT && base_t->kind != TY_ENUM) {
                    fatal(d->list.items[0]->type_pos,
                          "methods can only be defined on struct or enum types (got '%s')", rt->name);
                }
                if (find_field(base_t, d->name2)) {
                    fatal(d->pos, "method '%s' conflicts with %s '%s.%s'",
                          d->name2, base_t->kind == TY_STRUCT ? "field" : "variant",
                          base_t->name, d->name2);
                }
            }
            if (d->is_extern) {
                Symbol *existing = lookup_in(c.global, d->name);
                if (existing && existing->kind == SYM_FN && existing->is_extern &&
                    existing->type == ret && existing->decl->list.len == d->list.len &&
                    _stricmp(existing->decl->dll_name, d->dll_name) == 0) {
                    bool same_params = true;
                    for (int j = 0; j < d->list.len; j++) {
                        if (existing->decl->list.items[j]->type != d->list.items[j]->type) {
                            same_params = false;
                            break;
                        }
                    }
                    if (same_params) {
                        d->sym = existing;
                        d->type = ret;
                        continue;
                    }
                }
                Symbol *s = define(&c, SYM_FN, d->name, ret, d->pos);
                s->is_extern = true;
                s->decl = d;
                d->sym = s;
                d->type = ret;
                register_dll_import(&prog, d->dll_name, d->name);
                vec_push(extern_syms, s);
            } else {
                Symbol *s = define(&c, SYM_FN, d->name, ret, d->pos);
                s->decl = d;
                d->sym = s;
                d->type = ret;
                vec_push(prog.fns, d);
            }
        } else if (d->kind == N_VAR) {
            Symbol *s = define(&c, SYM_VAR, d->name, NULL, d->pos);
            s->decl = d;
            d->sym = s;
            vec_push(prog.globals, s);
        }
    }

    // Assign IAT slot indices for all imported DLL functions.
    int cur_iat_slot = 0;
    for (int i = 0; i < prog.dll_imports.len; i++) {
        prog.dll_imports.items[i].first_iat_slot = cur_iat_slot;
        cur_iat_slot += prog.dll_imports.items[i].funcs.len + 1; // +1 for null terminator entry
    }
    prog.total_iat_slots = cur_iat_slot;
    for (int i = 0; i < extern_syms.len; i++) {
        Symbol *es = extern_syms.items[i];
        es->iat_slot = find_iat_slot(&prog, es->decl->dll_name, es->name);
    }
    free(extern_syms.items);

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
        if (is_aggregate_type(fn->type)) {
            fn->slot_start = alloc_slot(&c); // hidden return buffer pointer passed in R10
        }
        for (int j = 0; j < fn->list.len; j++) {
            Node *p = fn->list.items[j];
            Symbol *ps = define(&c, SYM_PARAM, p->name, p->type, p->pos);
            ps->readonly = true;
            if (p->type->kind == TY_SLICE || p->type == &ty_str) {
                ps->slot = alloc_slots_bytes(&c, 16); // {ptr, len}
            } else {
                ps->slot = alloc_slot(&c); // scalar or pointer to fixed-size array/struct
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
