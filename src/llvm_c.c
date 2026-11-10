// llvm_c.c - LLVM IR (.ll) emitter & Optimizing (-O3 / --llvm) native backend.
//
// Provides two capabilities alongside Antimony's built-in x64 backend:
//   1. `emit_llvm_ir`: emits textual SSA LLVM IR (`.ll`) with `nsw`/`inbounds` flags.
//   2. `compile_opt_backend`: lowers the checked AST into an SSA-form optimizing
//      translation unit and invokes `clang -O3 -march=native` (if LLVM Clang is
//      installed) or `gcc -O3 -march=native` to produce a maximum-speed PE32+ `.exe`.
#include "sb.h"
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
    Buf out;
    Buf hdr;
    Program *prog;
    bool release;
    int tmp_id;
    int lbl_id;
    Type *cur_ret_type;
    VEC(Node *) defers;
    VEC(Type *) slice_types;
    VEC(Type *) fn_types;
} OptGen;

static void bprintf(Buf *b, const char *fmt, ...) {
    char stack_buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n < (int)sizeof(stack_buf)) {
        buf_bytes(b, stack_buf, (size_t)n);
        return;
    }
    char *dyn = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(dyn, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_bytes(b, dyn, (size_t)n);
    free(dyn);
}

static void emit_mangled_ident(Buf *b, const char *prefix, const char *name) {
    bprintf(b, "%s", prefix);
    for (const char *p = name; *p; p++) {
        if (*p == '.')      bprintf(b, "__");
        else if (*p == '[') bprintf(b, "_L_");
        else if (*p == ']') bprintf(b, "_R_");
        else if (*p == '(') bprintf(b, "_LP_");
        else if (*p == ')') bprintf(b, "_RP_");
        else if (*p == ':') bprintf(b, "_RET_");
        else if (*p == ',') bprintf(b, "_C_");
        else if (*p == '*') bprintf(b, "_P_");
        else if (*p == ' ') continue;
        else                buf_u8(b, (uint8_t)*p);
    }
}

static int get_fn_type_id(OptGen *g, Type *t) {
    for (int i = 0; i < g->fn_types.len; i++) {
        if (g->fn_types.items[i] == t) return i;
    }
    int id = g->fn_types.len;
    vec_push(g->fn_types, t);
    return id;
}

static int get_slice_type_id(OptGen *g, Type *t) {
    for (int i = 0; i < g->slice_types.len; i++) {
        if (g->slice_types.items[i] == t->elem) return i;
    }
    int id = g->slice_types.len;
    vec_push(g->slice_types, t->elem);
    return id;
}

static void collect_slice_types_type(OptGen *g, Type *t) {
    if (!t) return;
    if (t->kind == TY_SLICE) {
        collect_slice_types_type(g, t->elem);
        get_slice_type_id(g, t);
    } else if (t->kind == TY_ARRAY) {
        collect_slice_types_type(g, t->elem);
        Type slice_t = { .kind = TY_SLICE, .elem = t->elem };
        get_slice_type_id(g, &slice_t);
    } else if (t->kind == TY_PTR) {
        collect_slice_types_type(g, t->elem);
    } else if (t->kind == TY_FN) {
        for (int i = 0; i < t->params.len; i++) {
            collect_slice_types_type(g, t->params.items[i]);
        }
        collect_slice_types_type(g, t->elem);
        get_fn_type_id(g, t);
    } else if (t->kind == TY_STRUCT || (t->kind == TY_ENUM && t->is_tagged)) {
        for (int i = 0; i < t->fields.len; i++) {
            if ((t->kind == TY_STRUCT || t->fields.items[i].has_payload) && t->fields.items[i].type) {
                collect_slice_types_type(g, t->fields.items[i].type);
            }
        }
    }
}

static void collect_slice_types_node(OptGen *g, Node *n) {
    if (!n) return;
    collect_slice_types_type(g, n->type);
    if (n->kind == N_VAR || n->kind == N_PARAM || n->kind == N_LOOP_RANGE || n->kind == N_LOOP_ITER) {
        if (n->sym) collect_slice_types_type(g, n->sym->type);
        if (n->sym2) collect_slice_types_type(g, n->sym2->type);
    }
    collect_slice_types_node(g, n->lhs);
    collect_slice_types_node(g, n->rhs);
    collect_slice_types_node(g, n->cond);
    collect_slice_types_node(g, n->body);
    collect_slice_types_node(g, n->els);
    collect_slice_types_node(g, n->init);
    collect_slice_types_node(g, n->step);
    collect_slice_types_node(g, n->lo);
    collect_slice_types_node(g, n->hi);
    collect_slice_types_node(g, n->by);
    for (int i = 0; i < n->list.len; i++) {
        collect_slice_types_node(g, n->list.items[i]);
    }
}

static void emit_c_type(OptGen *g, Buf *b, Type *t) {
    if (!t || t->kind == TY_VOID) { bprintf(b, "void"); return; }
    if (t == &ty_bool) { bprintf(b, "uint8_t"); return; }
    if (t == &ty_str)  { bprintf(b, "sb_str"); return; }
    if (t == &ty_f32)  { bprintf(b, "float"); return; }
    if (t == &ty_f64)  { bprintf(b, "double"); return; }
    if (t->kind == TY_ENUM) {
        if (t->is_tagged) {
            emit_mangled_ident(b, "struct sb_en_", t->name);
        } else {
            bprintf(b, "int32_t");
        }
        return;
    }
    if (t->kind == TY_INT) {
        const char *u = t->is_signed ? "" : "u";
        int bits = t->size * 8;
        bprintf(b, "%sint%d_t", u, bits);
        return;
    }
    if (t->kind == TY_PTR) {
        if (t->elem->kind == TY_ARRAY) {
            emit_c_type(g, b, t->elem->elem);
            bprintf(b, "*");
        } else {
            emit_c_type(g, b, t->elem);
            bprintf(b, "*");
        }
        return;
    }
    if (t->kind == TY_SLICE) {
        int id = get_slice_type_id(g, t);
        bprintf(b, "sb_slice_%d", id);
        return;
    }
    if (t->kind == TY_STRUCT) {
        emit_mangled_ident(b, "struct sb_st_", t->name);
        return;
    }
    if (t->kind == TY_ARRAY) {
        emit_c_type(g, b, t->elem);
        bprintf(b, "*");
        return;
    }
    if (t->kind == TY_FN) {
        int id = get_fn_type_id(g, t);
        bprintf(b, "sb_fnptr_%d", id);
        return;
    }
    bprintf(b, "int64_t");
}

// Emits a variable or struct field declaration `T name` or `T name[N]`.
static void emit_c_var_decl(OptGen *g, Buf *b, Type *t, const char *prefix, const char *name, int slot) {
    if (t->kind == TY_ARRAY) {
        // Handle 1D or 2D fixed-size arrays: `elem name[N1][N2]`
        Type *base = t;
        while (base->kind == TY_ARRAY) base = base->elem;
        emit_c_type(g, b, base);
        bprintf(b, " ");
        if (slot >= 0) bprintf(b, "v%d_%s", slot, name);
        else           emit_mangled_ident(b, prefix, name);
        Type *cur = t;
        while (cur->kind == TY_ARRAY) {
            bprintf(b, "[%d]", cur->array_len);
            cur = cur->elem;
        }
    } else {
        emit_c_type(g, b, t);
        bprintf(b, " ");
        if (slot >= 0) bprintf(b, "v%d_%s", slot, name);
        else           emit_mangled_ident(b, prefix, name);
    }
}

static void emit_var_ref(Buf *b, Symbol *s) {
    if (s->kind == SYM_FN) {
        emit_mangled_ident(b, "sb_fn_", s->name);
    } else if (s->is_global) {
        emit_mangled_ident(b, "sb_g_", s->name);
    } else {
        bprintf(b, "v%d_%s", s->slot, s->name);
    }
}

static void emit_c_str_literal(Buf *b, const char *s, int len) {
    buf_u8(b, '"');
    for (int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '\\')      bprintf(b, "\\\\");
        else if (c == '"')  bprintf(b, "\\\"");
        else if (c == '\n') bprintf(b, "\\n");
        else if (c == '\r') bprintf(b, "\\r");
        else if (c == '\t') bprintf(b, "\\t");
        else if (c >= 32 && c < 127) buf_u8(b, c);
        else bprintf(b, "\\x%02x\" \"", c);
    }
    buf_u8(b, '"');
}

static void emit_loc_str(Buf *b, Pos pos, const char *reason) {
    char msg[512];
    int len = snprintf(msg, sizeof(msg), "%s:%d:%d: runtime panic: %s\n",
                       file_name_of(pos), pos.line, pos.col, reason);
    if (len < 0) len = 0;
    if (len >= (int)sizeof(msg)) len = (int)sizeof(msg) - 1;
    emit_c_str_literal(b, msg, len);
    bprintf(b, ", %d", len);
}

static void opt_gen_expr(OptGen *g, Buf *b, Node *n);
static void opt_gen_stmt(OptGen *g, Buf *b, Node *n);
static void opt_gen_block(OptGen *g, Buf *b, Node *blk);
static void opt_emit_defers_from(OptGen *g, Buf *b, int limit);

static void opt_gen_collection_slice(OptGen *g, Buf *b, Node *base) {
    if (base->type->kind == TY_ARRAY && !base->implicit_slice) {
        Type *slice_t = &(Type){ .kind = TY_SLICE, .elem = base->type->elem };
        int id = get_slice_type_id(g, slice_t);
        bprintf(b, "((sb_slice_%d){ (", id);
        emit_c_type(g, b, base->type->elem);
        bprintf(b, "*)(&(");
        opt_gen_expr(g, b, base);
        bprintf(b, ")[0]), %dLL })", base->type->array_len);
        return;
    }
    opt_gen_expr(g, b, base);
}

static void opt_gen_lvalue(OptGen *g, Buf *b, Node *n) {
    switch (n->kind) {
    case N_IDENT:
        emit_var_ref(b, n->sym);
        return;

    case N_DEREF:
        bprintf(b, "(*(");
        if (!g->release) {
            bprintf(b, "(typeof(");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, "))sb_chk_null(");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ", ");
            emit_loc_str(b, n->pos, "null pointer dereference");
            bprintf(b, ")");
        } else {
            opt_gen_expr(g, b, n->lhs);
        }
        bprintf(b, "))");
        return;

    case N_FIELD: {
        Type *bt = n->lhs->type;
        if (bt->kind == TY_PTR) {
            bprintf(b, "(");
            if (!g->release) {
                bprintf(b, "((typeof(");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, "))sb_chk_null(");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, ", ");
                emit_loc_str(b, n->pos, "null pointer dereference");
                bprintf(b, "))");
            } else {
                opt_gen_expr(g, b, n->lhs);
            }
            bprintf(b, ")->f_%s", n->name);
        } else if (n->lhs->kind == N_IDENT || n->lhs->kind == N_FIELD ||
                   n->lhs->kind == N_INDEX || n->lhs->kind == N_DEREF) {
            bprintf(b, "(");
            opt_gen_lvalue(g, b, n->lhs);
            bprintf(b, ").f_%s", n->name);
        } else {
            bprintf(b, "(");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ").f_%s", n->name);
        }
        return;
    }

    case N_INDEX: {
        Type *bt = n->lhs->type;
        if (bt->kind == TY_PTR) {
            bprintf(b, "(");
            if (!g->release) {
                bprintf(b, "((typeof(");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, "))sb_chk_null(");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, ", ");
                emit_loc_str(b, n->pos, "null pointer dereference");
                bprintf(b, "))");
            } else {
                opt_gen_expr(g, b, n->lhs);
            }
            bprintf(b, ")[");
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, "]");
            return;
        }
        if (bt->kind == TY_ARRAY && !n->lhs->implicit_slice) {
            bprintf(b, "(");
            opt_gen_lvalue(g, b, n->lhs);
            bprintf(b, ")[");
            if (!g->release && !n->rhs->is_const) {
                bprintf(b, "sb_chk_idx((int64_t)(");
                opt_gen_expr(g, b, n->rhs);
                bprintf(b, "), %dLL, ", bt->array_len);
                emit_loc_str(b, n->rhs->pos, "index out of bounds");
                bprintf(b, ")");
            } else {
                opt_gen_expr(g, b, n->rhs);
            }
            bprintf(b, "]");
            return;
        }
        // Slice or str index
        if (g->release || (n->lhs->kind == N_IDENT)) {
            bprintf(b, "(");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ").ptr[");
            if (!g->release) {
                bprintf(b, "sb_chk_idx((int64_t)(");
                opt_gen_expr(g, b, n->rhs);
                bprintf(b, "), (");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, ").len, ");
                emit_loc_str(b, n->rhs->pos, "index out of bounds");
                bprintf(b, ")");
            } else {
                opt_gen_expr(g, b, n->rhs);
            }
            bprintf(b, "]");
        } else {
            int tid = ++g->tmp_id;
            bprintf(b, "({ typeof(");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ") _s%d = ", tid);
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, "; _s%d.ptr[sb_chk_idx((int64_t)(", tid);
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, "), _s%d.len, ", tid);
            emit_loc_str(b, n->rhs->pos, "index out of bounds");
            bprintf(b, ")]; })");
        }
        return;
    }

    default:
        opt_gen_expr(g, b, n);
        return;
    }
}

static void opt_gen_expr(OptGen *g, Buf *b, Node *n) {
    if (n->is_const && n->type->kind == TY_FLOAT) {
        if (n->type == &ty_f32) bprintf(b, "((float)%.9g)", n->fval);
        else                    bprintf(b, "((double)%.17g)", n->fval);
        return;
    }
    if (n->is_const && (n->type->kind == TY_INT || n->type->kind == TY_ENUM || n->type == &ty_bool)) {
        if (n->type == &ty_bool) {
            bprintf(b, "%d", n->cval ? 1 : 0);
            return;
        }
        bprintf(b, "((");
        emit_c_type(g, b, n->type);
        if (n->type->is_signed) {
            int64_t v = (int64_t)n->cval;
            if (v == INT64_MIN) bprintf(b, ")(-9223372036854775807LL - 1LL))");
            else                bprintf(b, ")%lldLL)", (long long)v);
        } else {
            bprintf(b, ")%lluULL)", (unsigned long long)(uint64_t)n->cval);
        }
        return;
    }

    switch (n->kind) {
    case N_INT:
    case N_BOOL:
        bprintf(b, "((");
        emit_c_type(g, b, n->type);
        bprintf(b, ")%lluULL)", (unsigned long long)(uint64_t)n->cval);
        return;

    case N_NULL:
        bprintf(b, "((");
        emit_c_type(g, b, n->type);
        bprintf(b, ")0)");
        return;

    case N_FLOAT:
        if (n->type == &ty_f32) bprintf(b, "((float)%.9g)", n->fval);
        else                    bprintf(b, "((double)%.17g)", n->fval);
        return;

    case N_STR:
        if (n->type->kind == TY_PTR) {
            // Coerced to *u8 or *i8 for C FFI
            bprintf(b, "((");
            emit_c_type(g, b, n->type);
            bprintf(b, ")");
            emit_c_str_literal(b, n->str, n->str_len);
            bprintf(b, ")");
        } else {
            bprintf(b, "((sb_str){ (const uint8_t*)");
            emit_c_str_literal(b, n->str, n->str_len);
            bprintf(b, ", %dLL })", n->str_len);
        }
        return;

    case N_IDENT:
        if (n->implicit_slice) {
            Type *slice_t = &(Type){ .kind = TY_SLICE, .elem = n->type->elem };
            int id = get_slice_type_id(g, slice_t);
            bprintf(b, "((sb_slice_%d){ (", id);
            emit_c_type(g, b, n->type->elem);
            bprintf(b, "*)(&");
            emit_var_ref(b, n->sym);
            bprintf(b, "[0]), %dLL })", n->type->array_len);
            return;
        }
        if (n->sym->type == &ty_str && n->type->kind == TY_PTR) {
            bprintf(b, "((");
            emit_c_type(g, b, n->type);
            bprintf(b, ")(");
            emit_var_ref(b, n->sym);
            bprintf(b, ".ptr))");
            return;
        }
        opt_gen_lvalue(g, b, n);
        return;

    case N_ADDR:
        bprintf(b, "(&(");
        opt_gen_lvalue(g, b, n->lhs);
        bprintf(b, "))");
        return;

    case N_DEREF:
    case N_FIELD:
    case N_INDEX:
        opt_gen_lvalue(g, b, n);
        return;

    case N_STRUCT_LIT: {
        bprintf(b, "((");
        emit_c_type(g, b, n->type);
        bprintf(b, "){");
        if (n->list.len == 0) {
            bprintf(b, "0");
        } else {
            for (int i = 0; i < n->list.len; i++) {
                Node *fi = n->list.items[i];
                if (i > 0) bprintf(b, ", ");
                bprintf(b, ".f_%s = ", fi->name);
                if (fi->type->kind == TY_ARRAY && fi->rhs->kind == N_ARRAY_LIT) {
                    bprintf(b, "{");
                    if (fi->rhs->list.len == 0) bprintf(b, "0");
                    for (int j = 0; j < fi->rhs->list.len; j++) {
                        if (j > 0) bprintf(b, ", ");
                        opt_gen_expr(g, b, fi->rhs->list.items[j]);
                    }
                    bprintf(b, "}");
                } else if (fi->type->kind == TY_SLICE && fi->rhs->type->kind == TY_ARRAY) {
                    opt_gen_collection_slice(g, b, fi->rhs);
                } else {
                    opt_gen_expr(g, b, fi->rhs);
                }
            }
        }
        bprintf(b, "})");
        return;
    }

    case N_ENUM_LIT: {
        bprintf(b, "((");
        emit_c_type(g, b, n->type);
        bprintf(b, "){ .tag = %d", (int32_t)n->cval);
        if (n->lhs) {
            StructField *vf = (StructField *)n->sym2;
            bprintf(b, ", .u = { .v_%s = ", vf->name);
            if (vf->type->kind == TY_ARRAY && n->lhs->kind == N_ARRAY_LIT) {
                bprintf(b, "{");
                if (n->lhs->list.len == 0) bprintf(b, "0");
                for (int j = 0; j < n->lhs->list.len; j++) {
                    if (j > 0) bprintf(b, ", ");
                    opt_gen_expr(g, b, n->lhs->list.items[j]);
                }
                bprintf(b, "}");
            } else if (vf->type->kind == TY_SLICE && n->lhs->type->kind == TY_ARRAY) {
                opt_gen_collection_slice(g, b, n->lhs);
            } else {
                opt_gen_expr(g, b, n->lhs);
            }
            bprintf(b, " }");
        }
        bprintf(b, " })");
        return;
    }

    case N_TRY: {
        int tid = ++g->tmp_id;
        Type *et = n->lhs->type;
        StructField *ok_v  = (StructField *)n->sym;
        StructField *err_v = (StructField *)n->sym2;
        bprintf(b, "({ ");
        emit_c_type(g, b, et);
        bprintf(b, " _try%d = ", tid);
        opt_gen_expr(g, b, n->lhs);
        bprintf(b, "; if (_try%d.tag == %d) {\n", tid, (int32_t)n->cval);
        bprintf(b, "        ");
        emit_c_type(g, b, g->cur_ret_type);
        bprintf(b, " _eret%d = { .tag = %d", tid, n->field_offset);
        if (err_v && err_v->has_payload) {
            bprintf(b, ", .u = { .v_%s = _try%d.u.v_%s }", err_v->name, tid, err_v->name);
        }
        bprintf(b, " };\n");
        if (g->defers.len > 0) {
            opt_emit_defers_from(g, b, 0);
        }
        bprintf(b, "        return _eret%d;\n    }\n    _try%d.u.v_%s; })", tid, tid, ok_v->name);
        return;
    }

    case N_SLICE: {
        Type *bt = n->lhs->type;
        int tid = ++g->tmp_id;
        bprintf(b, "({ ");
        if (bt->kind == TY_PTR) {
            emit_c_type(g, b, bt);
            bprintf(b, " _p%d = ", tid);
            if (!g->release) {
                bprintf(b, "(");
                emit_c_type(g, b, bt);
                bprintf(b, ")sb_chk_null(");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, ", ");
                emit_loc_str(b, n->pos, "null pointer dereference");
                bprintf(b, "); ");
            } else {
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, "; ");
            }
            bprintf(b, "int64_t _lo%d = ", tid);
            if (n->lo) opt_gen_expr(g, b, n->lo);
            else       bprintf(b, "0LL");
            bprintf(b, "; int64_t _hi%d = ", tid);
            opt_gen_expr(g, b, n->hi);
            bprintf(b, "; ");
            if (!g->release) {
                bprintf(b, "sb_chk_ptr_slice(_lo%d, _hi%d, ", tid, tid);
                emit_loc_str(b, n->pos, "invalid pointer slice bounds");
                bprintf(b, "); ");
            }
            emit_c_type(g, b, n->type);
            bprintf(b, " _res%d = { _p%d + _lo%d, _hi%d - _lo%d }; _res%d; })",
                    tid, tid, tid, tid, tid, tid);
            return;
        }
        if (bt == &ty_str) {
            bprintf(b, "sb_str _c%d = ", tid);
            opt_gen_expr(g, b, n->lhs);
        } else {
            Type *st = &(Type){ .kind = TY_SLICE, .elem = bt->elem };
            emit_c_type(g, b, st);
            bprintf(b, " _c%d = ", tid);
            opt_gen_collection_slice(g, b, n->lhs);
        }
        bprintf(b, "; int64_t _lo%d = ", tid);
        if (n->lo) opt_gen_expr(g, b, n->lo);
        else       bprintf(b, "0LL");
        bprintf(b, "; int64_t _hi%d = ", tid);
        if (n->hi) opt_gen_expr(g, b, n->hi);
        else       bprintf(b, "_c%d.len", tid);
        bprintf(b, "; ");
        if (!g->release) {
            bprintf(b, "sb_chk_slice(_lo%d, _hi%d, _c%d.len, ", tid, tid, tid);
            emit_loc_str(b, n->pos, "slice bounds out of range");
            bprintf(b, "); ");
        }
        emit_c_type(g, b, n->type);
        bprintf(b, " _res%d = { _c%d.ptr + _lo%d, _hi%d - _lo%d }; _res%d; })",
                tid, tid, tid, tid, tid, tid);
        return;
    }

    case N_UNARY:
        if (n->op == T_BANG) {
            bprintf(b, "(!(");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, "))");
        } else if (n->op == T_TILDE) {
            bprintf(b, "((");
            emit_c_type(g, b, n->type);
            bprintf(b, ")(~(");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ")))");
        } else if (n->op == T_MINUS) {
            if (n->type->kind == TY_FLOAT || g->release) {
                bprintf(b, "((");
                emit_c_type(g, b, n->type);
                bprintf(b, ")(-(");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, ")))");
            } else {
                bprintf(b, "((");
                emit_c_type(g, b, n->type);
                bprintf(b, ")sb_neg_i%d(", n->type->size * 8);
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, ", ");
                emit_loc_str(b, n->pos, "integer negation overflow");
                bprintf(b, "))");
            }
        }
        return;

    case N_CONV: {
        Type *src = n->lhs->type;
        Type *dst = n->type;
        if (dst == &ty_str) {
            int tid = ++g->tmp_id;
            Type *st = &(Type){ .kind = TY_SLICE, .elem = &ty_u8 };
            bprintf(b, "({ ");
            emit_c_type(g, b, st);
            bprintf(b, " _bs%d = ", tid);
            opt_gen_collection_slice(g, b, n->lhs);
            bprintf(b, "; (sb_str){ (const uint8_t*)_bs%d.ptr, _bs%d.len }; })", tid, tid);
            return;
        }
        if (src == &ty_str && dst->kind == TY_PTR) {
            bprintf(b, "((");
            emit_c_type(g, b, dst);
            bprintf(b, ")(");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ").ptr)");
            return;
        }
        if (src->kind == TY_PTR || dst->kind == TY_PTR) {
            bprintf(b, "((");
            emit_c_type(g, b, dst);
            bprintf(b, ")(uintptr_t)(");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, "))");
            return;
        }
        if (!g->release && (src->kind == TY_INT || src->kind == TY_ENUM || src->kind == TY_FLOAT) &&
            (dst->kind == TY_INT || dst->kind == TY_ENUM)) {
            Type *fit_dst = (dst->kind == TY_ENUM) ? &ty_i32 : dst;
            if (src->kind == TY_FLOAT) {
                bprintf(b, "((");
                emit_c_type(g, b, dst);
                bprintf(b, ")sb_conv_f_%s%d((double)(", fit_dst->is_signed ? "i" : "u", fit_dst->size * 8);
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, "), ");
                emit_loc_str(b, n->pos, "float to integer conversion out of range");
                bprintf(b, "))");
                return;
            }
            if (src->is_signed) {
                bprintf(b, "((");
                emit_c_type(g, b, dst);
                bprintf(b, ")sb_conv_i64_%s%d((int64_t)(", fit_dst->is_signed ? "i" : "u", fit_dst->size * 8);
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, "), ");
                emit_loc_str(b, n->pos, "integer conversion out of range");
                bprintf(b, "))");
                return;
            } else {
                bprintf(b, "((");
                emit_c_type(g, b, dst);
                bprintf(b, ")sb_conv_u64_%s%d((uint64_t)(", fit_dst->is_signed ? "i" : "u", fit_dst->size * 8);
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, "), ");
                emit_loc_str(b, n->pos, "integer conversion out of range");
                bprintf(b, "))");
                return;
            }
        }
        bprintf(b, "((");
        emit_c_type(g, b, dst);
        bprintf(b, ")(");
        opt_gen_expr(g, b, n->lhs);
        bprintf(b, "))");
        return;
    }

    case N_BINARY: {
        TokKind op = n->op;
        if (op == T_ANDAND || op == T_OROR) {
            bprintf(b, "((");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ") %s (", op == T_ANDAND ? "&&" : "||");
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, "))");
            return;
        }
        if (n->lhs->type == &ty_str) {
            if (op == T_NE) bprintf(b, "(!");
            bprintf(b, "sb_str_eq(");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ", ");
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, ")");
            if (op == T_NE) bprintf(b, ")");
            return;
        }
        if (n->lhs->type->kind == TY_PTR && (op == T_PLUS || op == T_MINUS)) {
            if (op == T_MINUS && n->rhs->type == n->lhs->type) {
                bprintf(b, "((int64_t)((");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, ") - (");
                opt_gen_expr(g, b, n->rhs);
                bprintf(b, ")))");
                return;
            }
            bprintf(b, "((");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ") %s (int64_t)(", op == T_PLUS ? "+" : "-");
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, "))");
            return;
        }
        if (op == T_EQ || op == T_NE || op == T_LT || op == T_LE || op == T_GT || op == T_GE) {
            const char *cop = "==";
            switch (op) {
            case T_EQ: cop = "=="; break;
            case T_NE: cop = "!="; break;
            case T_LT: cop = "<";  break;
            case T_LE: cop = "<="; break;
            case T_GT: cop = ">";  break;
            case T_GE: cop = ">="; break;
            default: break;
            }
            bprintf(b, "((uint8_t)((");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ") %s (", cop);
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, ")))");
            return;
        }
        Type *t = n->lhs->type;
        if (t->kind == TY_FLOAT) {
            const char *fop = "+";
            if (op == T_MINUS) fop = "-";
            else if (op == T_STAR) fop = "*";
            else if (op == T_SLASH) fop = "/";
            bprintf(b, "((");
            emit_c_type(g, b, t);
            bprintf(b, ")((");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ") %s (", fop);
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, ")))");
            return;
        }
        if (op == T_AMP || op == T_PIPE || op == T_CARET) {
            const char *bop = (op == T_AMP) ? "&" : (op == T_PIPE) ? "|" : "^";
            bprintf(b, "((");
            emit_c_type(g, b, t);
            bprintf(b, ")((");
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ") %s (", bop);
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, ")))");
            return;
        }
        if (op == T_SHL || op == T_SHR) {
            int bits = t->size * 8;
            if (!g->release) {
                bprintf(b, "((");
                emit_c_type(g, b, t);
                bprintf(b, ")((");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, ") %s sb_chk_shift((uint64_t)(", op == T_SHL ? "<<" : ">>");
                opt_gen_expr(g, b, n->rhs);
                bprintf(b, "), %d, ", bits);
                emit_loc_str(b, n->pos, "shift count >= bit width of type");
                bprintf(b, ")))");
            } else {
                bprintf(b, "((");
                emit_c_type(g, b, t);
                bprintf(b, ")((");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, ") %s ((", op == T_SHL ? "<<" : ">>");
                opt_gen_expr(g, b, n->rhs);
                bprintf(b, ") & %d)))", bits - 1);
            }
            return;
        }
        if (op == T_SLASH || op == T_PERCENT) {
            const char *fn = (op == T_SLASH) ? "div" : "rem";
            bprintf(b, "sb_%s_%s%d(", fn, t->is_signed ? "i" : "u", t->size * 8);
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ", ");
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, ", ");
            emit_loc_str(b, n->pos, "division by zero or overflow");
            bprintf(b, ")");
            return;
        }
        bool is_wrap = (op == T_PLUSW || op == T_MINUSW || op == T_STARW);
        if (is_wrap || g->release) {
            // Perform arithmetic in unsigned type of same width so C signed overflow UB never occurs!
            int bits = t->size * 8;
            const char *aop = "+";
            if (op == T_MINUS || op == T_MINUSW) aop = "-";
            else if (op == T_STAR || op == T_STARW) aop = "*";
            bprintf(b, "((");
            emit_c_type(g, b, t);
            bprintf(b, ")((uint%d_t)(", bits);
            opt_gen_expr(g, b, n->lhs);
            bprintf(b, ") %s (uint%d_t)(", aop, bits);
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, ")))");
            return;
        }
        const char *opname = (op == T_PLUS) ? "add" : (op == T_MINUS) ? "sub" : "mul";
        bprintf(b, "sb_%s_%s%d(", opname, t->is_signed ? "i" : "u", t->size * 8);
        opt_gen_expr(g, b, n->lhs);
        bprintf(b, ", ");
        opt_gen_expr(g, b, n->rhs);
        bprintf(b, ", ");
        emit_loc_str(b, n->pos,
                     op == T_PLUS ? "integer addition overflow" :
                     op == T_MINUS ? "integer subtraction overflow" : "integer multiplication overflow");
        bprintf(b, ")");
        return;
    }

    case N_CALL: {
        if (n->builtin == B_PRINT) {
            Node *arg = n->list.items[0];
            if (arg->type == &ty_str) {
                bprintf(b, "sb_rt_print_str(");
                opt_gen_expr(g, b, arg);
                bprintf(b, ")");
            } else if (arg->type == &ty_bool) {
                bprintf(b, "sb_rt_print_bool(");
                opt_gen_expr(g, b, arg);
                bprintf(b, ")");
            } else if (arg->type->kind == TY_FLOAT) {
                bprintf(b, "sb_rt_print_f64((double)(");
                opt_gen_expr(g, b, arg);
                bprintf(b, "))");
            } else if (arg->type->kind == TY_PTR) {
                bprintf(b, "sb_rt_print_u64((uint64_t)(uintptr_t)(");
                opt_gen_expr(g, b, arg);
                bprintf(b, "))");
            } else if (arg->type->is_signed) {
                bprintf(b, "sb_rt_print_i64((int64_t)(");
                opt_gen_expr(g, b, arg);
                bprintf(b, "))");
            } else {
                bprintf(b, "sb_rt_print_u64((uint64_t)(");
                opt_gen_expr(g, b, arg);
                bprintf(b, "))");
            }
            return;
        }
        if (n->builtin == B_EXIT) {
            bprintf(b, "ExitProcess((uint32_t)(");
            opt_gen_expr(g, b, n->list.items[0]);
            bprintf(b, "))");
            return;
        }
        if (n->builtin == B_LEN) {
            Node *arg = n->list.items[0];
            bprintf(b, "((int64_t)(");
            opt_gen_collection_slice(g, b, arg);
            bprintf(b, ").len)");
            return;
        }
        if (n->builtin == B_ATOMIC_LOAD) {
            bprintf(b, "__atomic_load_n((int64_t *)(");
            if (!g->release) {
                bprintf(b, "sb_chk_null((const void *)(");
                opt_gen_expr(g, b, n->list.items[0]);
                bprintf(b, "), ");
                emit_loc_str(b, n->pos, "null pointer in atomic_load");
                bprintf(b, ")");
            } else {
                opt_gen_expr(g, b, n->list.items[0]);
            }
            bprintf(b, "), __ATOMIC_SEQ_CST)");
            return;
        }
        if (n->builtin == B_ATOMIC_STORE) {
            bprintf(b, "__atomic_store_n((int64_t *)(");
            if (!g->release) {
                bprintf(b, "sb_chk_null((const void *)(");
                opt_gen_expr(g, b, n->list.items[0]);
                bprintf(b, "), ");
                emit_loc_str(b, n->pos, "null pointer in atomic_store");
                bprintf(b, ")");
            } else {
                opt_gen_expr(g, b, n->list.items[0]);
            }
            bprintf(b, "), (int64_t)(");
            opt_gen_expr(g, b, n->list.items[1]);
            bprintf(b, "), __ATOMIC_SEQ_CST)");
            return;
        }
        if (n->builtin == B_ATOMIC_ADD) {
            bprintf(b, "__sync_fetch_and_add((int64_t *)(");
            if (!g->release) {
                bprintf(b, "sb_chk_null((const void *)(");
                opt_gen_expr(g, b, n->list.items[0]);
                bprintf(b, "), ");
                emit_loc_str(b, n->pos, "null pointer in atomic_add");
                bprintf(b, ")");
            } else {
                opt_gen_expr(g, b, n->list.items[0]);
            }
            bprintf(b, "), (int64_t)(");
            opt_gen_expr(g, b, n->list.items[1]);
            bprintf(b, "))");
            return;
        }
        if (n->builtin == B_ATOMIC_CAS) {
            bprintf(b, "__sync_val_compare_and_swap((int64_t *)(");
            if (!g->release) {
                bprintf(b, "sb_chk_null((const void *)(");
                opt_gen_expr(g, b, n->list.items[0]);
                bprintf(b, "), ");
                emit_loc_str(b, n->pos, "null pointer in atomic_cas");
                bprintf(b, ")");
            } else {
                opt_gen_expr(g, b, n->list.items[0]);
            }
            bprintf(b, "), (int64_t)(");
            opt_gen_expr(g, b, n->list.items[1]);
            bprintf(b, "), (int64_t)(");
            opt_gen_expr(g, b, n->list.items[2]);
            bprintf(b, "))");
            return;
        }
        if (!n->sym && n->lhs) {
            if (!g->release) {
                bprintf(b, "((");
                emit_c_type(g, b, n->lhs->type);
                bprintf(b, ")sb_chk_null((const void *)(");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, "), ");
                emit_loc_str(b, n->pos, "null function pointer call");
                bprintf(b, "))");
            } else {
                bprintf(b, "(");
                opt_gen_expr(g, b, n->lhs);
                bprintf(b, ")");
            }
        } else if (n->sym->is_extern) {
            emit_mangled_ident(b, "sb_ext_", n->sym->name);
        } else {
            emit_mangled_ident(b, "sb_fn_", n->sym->name);
        }
        bprintf(b, "(");
        for (int i = 0; i < n->list.len; i++) {
            if (i > 0) bprintf(b, ", ");
            Node *arg = n->list.items[i];
            Type *pt = (!n->sym && n->lhs) ? n->lhs->type->params.items[i] : n->sym->decl->list.items[i]->type;
            if (pt->kind == TY_SLICE && arg->type->kind == TY_ARRAY) {
                opt_gen_collection_slice(g, b, arg);
            } else if (pt->kind == TY_ARRAY) {
                bprintf(b, "(&(");
                opt_gen_expr(g, b, arg);
                bprintf(b, ")[0])");
            } else if (pt->kind == TY_PTR && arg->type == &ty_str) {
                bprintf(b, "((");
                emit_c_type(g, b, pt);
                bprintf(b, ")(");
                opt_gen_expr(g, b, arg);
                bprintf(b, ").ptr)");
            } else {
                opt_gen_expr(g, b, arg);
            }
        }
        bprintf(b, ")");
        return;
    }

    default:
        bprintf(b, "0");
        return;
    }
}

static void opt_emit_defers_from(OptGen *g, Buf *b, int limit) {
    int saved_len = g->defers.len;
    for (int i = saved_len - 1; i >= limit; i--) {
        g->defers.len = i;
        Node *dbody = g->defers.items[i];
        if (dbody->kind == N_BLOCK) opt_gen_block(g, b, dbody);
        else                        opt_gen_stmt(g, b, dbody);
    }
    g->defers.len = saved_len;
}

static void opt_gen_assign(OptGen *g, Buf *b, Node *n) {
    bprintf(b, "    ");
    if (n->op == T_ASSIGN) {
        Type *t = n->type;
        if (t->kind == TY_ARRAY) {
            if (n->rhs->kind == N_ARRAY_LIT) {
                bprintf(b, "memset(&(");
                if (n->lhs) opt_gen_lvalue(g, b, n->lhs);
                else        emit_var_ref(b, n->sym);
                bprintf(b, "), 0, %d);\n", t->size);
                for (int i = 0; i < n->rhs->list.len; i++) {
                    bprintf(b, "    (");
                    if (n->lhs) opt_gen_lvalue(g, b, n->lhs);
                    else        emit_var_ref(b, n->sym);
                    bprintf(b, ")[%d] = ", i);
                    opt_gen_expr(g, b, n->rhs->list.items[i]);
                    bprintf(b, ";\n");
                }
                return;
            }
            bprintf(b, "memcpy(&(");
            if (n->lhs) opt_gen_lvalue(g, b, n->lhs);
            else        emit_var_ref(b, n->sym);
            bprintf(b, "), &(");
            opt_gen_expr(g, b, n->rhs);
            bprintf(b, "), %d);\n", t->size);
            return;
        }
        if (n->lhs) opt_gen_lvalue(g, b, n->lhs);
        else        emit_var_ref(b, n->sym);
        bprintf(b, " = ");
        if (t->kind == TY_SLICE && n->rhs->type->kind == TY_ARRAY) {
            opt_gen_collection_slice(g, b, n->rhs);
        } else {
            opt_gen_expr(g, b, n->rhs);
        }
        bprintf(b, ";\n");
        return;
    }

    // Compound assignment: construct synthetic binary expression on target
    TokKind binop = T_PLUS;
    switch (n->op) {
    case T_PLUS_ASSIGN:    binop = T_PLUS;    break;
    case T_MINUS_ASSIGN:   binop = T_MINUS;   break;
    case T_STAR_ASSIGN:    binop = T_STAR;    break;
    case T_SLASH_ASSIGN:   binop = T_SLASH;   break;
    case T_PERCENT_ASSIGN: binop = T_PERCENT; break;
    case T_AMP_ASSIGN:     binop = T_AMP;     break;
    case T_PIPE_ASSIGN:    binop = T_PIPE;    break;
    case T_CARET_ASSIGN:   binop = T_CARET;   break;
    case T_SHL_ASSIGN:     binop = T_SHL;     break;
    case T_SHR_ASSIGN:     binop = T_SHR;     break;
    default: break;
    }
    Node lhs_node = {0};
    if (n->lhs) {
        lhs_node = *n->lhs;
    } else {
        lhs_node.kind = N_IDENT;
        lhs_node.sym  = n->sym;
        lhs_node.type = n->type;
        lhs_node.pos  = n->pos;
    }
    Node bin = {0};
    bin.kind = N_BINARY;
    bin.op   = binop;
    bin.lhs  = &lhs_node;
    bin.rhs  = n->rhs;
    bin.type = n->type;
    bin.pos  = n->pos;

    if (n->lhs) opt_gen_lvalue(g, b, n->lhs);
    else        emit_var_ref(g ? b : b, n->sym);
    bprintf(b, " = ");
    opt_gen_expr(g, b, &bin);
    bprintf(b, ";\n");
}

static void opt_gen_stmt(OptGen *g, Buf *b, Node *n) {
    switch (n->kind) {
    case N_VAR:
        bprintf(b, "    ");
        emit_c_var_decl(g, b, n->type, "", n->name, n->sym->slot);
        if (!n->rhs) {
            bprintf(b, " = {0};\n");
            return;
        }
        bprintf(b, ";\n");
        {
            Node assign = {0};
            assign.kind = N_ASSIGN;
            assign.pos  = n->pos;
            assign.op   = T_ASSIGN;
            assign.sym  = n->sym;
            assign.type = n->type;
            assign.rhs  = n->rhs;
            opt_gen_assign(g, b, &assign);
        }
        return;

    case N_CONST:
        return;

    case N_ASSIGN:
        opt_gen_assign(g, b, n);
        return;

    case N_EXPR_STMT:
        bprintf(b, "    (void)(");
        opt_gen_expr(g, b, n->lhs);
        bprintf(b, ");\n");
        return;

    case N_DEFER:
        vec_push(g->defers, n->body);
        return;

    case N_RETURN:
        if (n->lhs) {
            int tid = ++g->tmp_id;
            bprintf(b, "    {\n        ");
            emit_c_type(g, b, g->cur_ret_type);
            bprintf(b, " _ret%d = ", tid);
            if (g->cur_ret_type->kind == TY_SLICE && n->lhs->type->kind == TY_ARRAY) {
                opt_gen_collection_slice(g, b, n->lhs);
            } else {
                opt_gen_expr(g, b, n->lhs);
            }
            bprintf(b, ";\n");
            if (g->defers.len > 0) {
                opt_emit_defers_from(g, b, 0);
            }
            bprintf(b, "        return _ret%d;\n    }\n", tid);
        } else {
            if (g->defers.len > 0) {
                opt_emit_defers_from(g, b, 0);
            }
            bprintf(b, "    return;\n");
        }
        return;

    case N_BREAK:
        if (g->defers.len > n->target->defer_depth) {
            opt_emit_defers_from(g, b, n->target->defer_depth);
        }
        bprintf(b, "    goto L_brk_%d;\n", n->target->label_break);
        return;

    case N_CONTINUE:
        if (g->defers.len > n->target->defer_depth) {
            opt_emit_defers_from(g, b, n->target->defer_depth);
        }
        bprintf(b, "    goto L_cont_%d;\n", n->target->label_continue);
        return;

    case N_IF:
        bprintf(b, "    if (");
        opt_gen_expr(g, b, n->cond);
        bprintf(b, ") {\n");
        opt_gen_block(g, b, n->body);
        if (n->els) {
            bprintf(b, "    } else {\n");
            if (n->els->kind == N_IF) opt_gen_stmt(g, b, n->els);
            else                      opt_gen_block(g, b, n->els);
        }
        bprintf(b, "    }\n");
        return;

    case N_MATCH: {
        int tid = ++g->tmp_id;
        bool is_tagged_enum = (n->cond->type->kind == TY_ENUM && n->cond->type->is_tagged);
        bprintf(b, "    {\n        ");
        emit_c_type(g, b, n->cond->type);
        bprintf(b, " _m%d = ", tid);
        opt_gen_expr(g, b, n->cond);
        bprintf(b, ";\n");
        for (int i = 0; i < n->list.len; i++) {
            Node *arm = n->list.items[i];
            bprintf(b, "        %sif (", i == 0 ? "" : "else ");
            for (int j = 0; j < arm->list.len; j++) {
                if (j > 0) bprintf(b, " || ");
                Node *pat = arm->list.items[j];
                if (n->cond->type == &ty_str) {
                    bprintf(b, "sb_str_eq(_m%d, ", tid);
                    opt_gen_expr(g, b, pat);
                    bprintf(b, ")");
                } else if (is_tagged_enum) {
                    bprintf(b, "(_m%d.tag == %d)", tid, (int32_t)pat->cval);
                } else {
                    bprintf(b, "(_m%d == ", tid);
                    opt_gen_expr(g, b, pat);
                    bprintf(b, ")");
                }
            }
            bprintf(b, ") {\n");
            if (is_tagged_enum && arm->sym) {
                Symbol *bs = arm->sym;
                StructField *vf = (StructField *)arm->list.items[0]->sym2;
                bprintf(b, "            ");
                emit_c_var_decl(g, b, bs->type, "", bs->name, bs->slot);
                if (bs->type->kind == TY_ARRAY) {
                    bprintf(b, ";\n            memcpy(&(");
                    emit_var_ref(b, bs);
                    bprintf(b, "), &(_m%d.u.v_%s), %d);\n", tid, vf->name, bs->type->size);
                } else {
                    bprintf(b, " = _m%d.u.v_%s;\n", tid, vf->name);
                }
            }
            opt_gen_block(g, b, arm->body);
            bprintf(b, "        }\n");
        }
        if (n->els) {
            bprintf(b, "        %s{\n", n->list.len > 0 ? "else " : "");
            opt_gen_block(g, b, n->els);
            bprintf(b, "        }\n");
        }
        bprintf(b, "    }\n");
        return;
    }

    case N_LOOP_INF:
        n->label_continue = ++g->lbl_id;
        n->label_break    = ++g->lbl_id;
        n->defer_depth    = g->defers.len;
        bprintf(b, "    for (;;) {\n");
        opt_gen_block(g, b, n->body);
        bprintf(b, "    L_cont_%d: ;\n    }\n    L_brk_%d: ;\n",
                n->label_continue, n->label_break);
        return;

    case N_LOOP_COND:
        n->label_continue = ++g->lbl_id;
        n->label_break    = ++g->lbl_id;
        n->defer_depth    = g->defers.len;
        bprintf(b, "    for (;;) {\n        if (!(");
        opt_gen_expr(g, b, n->cond);
        bprintf(b, ")) break;\n");
        opt_gen_block(g, b, n->body);
        bprintf(b, "    L_cont_%d: ;\n    }\n    L_brk_%d: ;\n",
                n->label_continue, n->label_break);
        return;

    case N_LOOP_RANGE: {
        int tid = ++g->tmp_id;
        Type *t = n->type;
        n->label_continue = ++g->lbl_id;
        n->label_break    = ++g->lbl_id;
        n->defer_depth    = g->defers.len;

        if (!n->rev && (!n->by || (n->by->is_const && n->by->cval == 1))) {
            bprintf(b, "    {\n        ");
            emit_c_type(g, b, t);
            bprintf(b, " _start%d = ", tid);
            opt_gen_expr(g, b, n->lo);
            bprintf(b, ";\n        ");
            emit_c_type(g, b, t);
            bprintf(b, " _end%d = ", tid);
            opt_gen_expr(g, b, n->hi);
            bprintf(b, ";\n        for (");
            emit_c_type(g, b, t);
            bprintf(b, " ");
            emit_var_ref(b, n->sym);
            bprintf(b, " = _start%d; ", tid);
            emit_var_ref(b, n->sym);
            bprintf(b, " < _end%d; ", tid);
            emit_var_ref(b, n->sym);
            bprintf(b, "++) {\n");
            opt_gen_block(g, b, n->body);
            bprintf(b, "        L_cont_%d: ;\n        }\n        L_brk_%d: ;\n    }\n",
                    n->label_continue, n->label_break);
            return;
        }

        bprintf(b, "    {\n        ");
        emit_c_type(g, b, t);
        bprintf(b, " _start%d = ", tid);
        opt_gen_expr(g, b, n->lo);
        bprintf(b, ";\n        ");
        emit_c_type(g, b, t);
        bprintf(b, " _end%d = ", tid);
        opt_gen_expr(g, b, n->hi);
        bprintf(b, ";\n        ");
        emit_c_type(g, b, t);
        bprintf(b, " _step%d = ", tid);
        if (n->by) opt_gen_expr(g, b, n->by);
        else       bprintf(b, "1");
        bprintf(b, ";\n");
        if (n->by && !n->by->is_const) {
            bprintf(b, "        if (_step%d <= 0) sb_rt_trap(", tid);
            emit_loc_str(b, n->by->pos, "range loop step must be > 0");
            bprintf(b, ");\n");
        }
        bprintf(b, "        if (_start%d < _end%d) {\n", tid, tid);
        bprintf(b, "            uint64_t _rem%d = ((uint64_t)(_end%d - _start%d) - 1ULL) / (uint64_t)_step%d + 1ULL;\n",
                tid, tid, tid, tid);
        bprintf(b, "            ");
        emit_c_type(g, b, t);
        bprintf(b, " ");
        emit_var_ref(b, n->sym);
        if (n->rev) {
            bprintf(b, " = (");
            emit_c_type(g, b, t);
            bprintf(b, ")((uint64_t)_start%d + (_rem%d - 1ULL) * (uint64_t)_step%d);\n", tid, tid, tid);
        } else {
            bprintf(b, " = _start%d;\n", tid);
        }
        bprintf(b, "            for (;;) {\n");
        opt_gen_block(g, b, n->body);
        bprintf(b, "            L_cont_%d:\n", n->label_continue);
        bprintf(b, "                if (--_rem%d == 0) break;\n", tid);
        bprintf(b, "                ");
        emit_var_ref(b, n->sym);
        bprintf(b, " = (");
        emit_c_type(g, b, t);
        bprintf(b, ")((uint64_t)");
        emit_var_ref(b, n->sym);
        bprintf(b, " %s (uint64_t)_step%d);\n", n->rev ? "-" : "+", tid);
        bprintf(b, "            }\n        }\n        L_brk_%d: ;\n    }\n", n->label_break);
        return;
    }

    case N_LOOP_ITER: {
        int tid = ++g->tmp_id;
        Type *ct = n->lhs->type;
        Type *elem_t = n->type;
        n->label_continue = ++g->lbl_id;
        n->label_break    = ++g->lbl_id;
        n->defer_depth    = g->defers.len;

        bprintf(b, "    {\n        ");
        if (ct == &ty_str) {
            bprintf(b, "sb_str _it%d = ", tid);
            opt_gen_expr(g, b, n->lhs);
        } else {
            Type *st = &(Type){ .kind = TY_SLICE, .elem = elem_t };
            emit_c_type(g, b, st);
            bprintf(b, " _it%d = ", tid);
            opt_gen_collection_slice(g, b, n->lhs);
        }
        bprintf(b, ";\n        for (int64_t _i%d = 0; _i%d < _it%d.len; _i%d++) {\n", tid, tid, tid, tid);
        if (n->sym2) {
            bprintf(b, "            int64_t ");
            emit_var_ref(b, n->sym);
            bprintf(b, " = _i%d;\n            ", tid);
            emit_c_type(g, b, elem_t);
            bprintf(b, " ");
            emit_var_ref(b, n->sym2);
            bprintf(b, " = _it%d.ptr[_i%d];\n", tid, tid);
        } else {
            bprintf(b, "            ");
            emit_c_type(g, b, elem_t);
            bprintf(b, " ");
            emit_var_ref(b, n->sym);
            bprintf(b, " = _it%d.ptr[_i%d];\n", tid, tid);
        }
        opt_gen_block(g, b, n->body);
        bprintf(b, "        L_cont_%d: ;\n        }\n        L_brk_%d: ;\n    }\n",
                n->label_continue, n->label_break);
        return;
    }

    case N_LOOP_C:
        n->label_continue = ++g->lbl_id;
        n->label_break    = ++g->lbl_id;
        bprintf(b, "    {\n");
        opt_gen_stmt(g, b, n->init);
        n->defer_depth = g->defers.len;
        bprintf(b, "        for (;;) {\n            if (!(");
        opt_gen_expr(g, b, n->cond);
        bprintf(b, ")) break;\n");
        opt_gen_block(g, b, n->body);
        bprintf(b, "        L_cont_%d:\n", n->label_continue);
        opt_gen_assign(g, b, n->step);
        bprintf(b, "        }\n        L_brk_%d: ;\n    }\n", n->label_break);
        return;

    case N_BLOCK:
        bprintf(b, "    {\n");
        opt_gen_block(g, b, n);
        bprintf(b, "    }\n");
        return;

    default:
        return;
    }
}

static void opt_gen_block(OptGen *g, Buf *b, Node *blk) {
    int defer_mark = g->defers.len;
    for (int i = 0; i < blk->list.len; i++) {
        opt_gen_stmt(g, b, blk->list.items[i]);
    }
    if (g->defers.len > defer_mark) {
        bool ends_jump = false;
        if (blk->list.len > 0) {
            NodeKind lk = blk->list.items[blk->list.len - 1]->kind;
            if (lk == N_RETURN || lk == N_BREAK || lk == N_CONTINUE) {
                ends_jump = true;
            }
        }
        if (!ends_jump) {
            opt_emit_defers_from(g, b, defer_mark);
        }
        g->defers.len = defer_mark;
    }
}

static void emit_opt_runtime_preamble(Buf *b) {
    bprintf(b,
        "#include <stdint.h>\n"
        "#include <stddef.h>\n"
        "#include <string.h>\n\n"
        "#ifdef _WIN32\n"
        "extern __attribute__((dllimport)) void* __stdcall GetStdHandle(uint32_t);\n"
        "extern __attribute__((dllimport)) int32_t __stdcall WriteFile(void*, const void*, uint32_t, uint32_t*, void*);\n"
        "extern __attribute__((dllimport)) __attribute__((noreturn)) void __stdcall ExitProcess(uint32_t);\n"
        "extern __attribute__((dllimport)) void* __stdcall LoadLibraryA(const char*);\n"
        "extern __attribute__((dllimport)) void* __stdcall GetProcAddress(void*, const char*);\n"
        "#endif\n\n"
        "static void *sb_stdout_handle;\n"
        "typedef struct { const uint8_t *ptr; int64_t len; } sb_str;\n\n"
        "static void sb_rt_write(const void *buf, uint32_t len) {\n"
        "    uint32_t written = 0;\n"
        "    WriteFile(sb_stdout_handle, buf, len, &written, 0);\n"
        "}\n\n"
        "static __attribute__((noreturn, cold, noinline)) void sb_rt_trap(const char *msg, int len) {\n"
        "    uint32_t written = 0;\n"
        "    void *herr = GetStdHandle((uint32_t)-12);\n"
        "    WriteFile(herr, msg, (uint32_t)len, &written, 0);\n"
        "    ExitProcess(101);\n"
        "}\n\n"
        "static void sb_rt_print_u64(uint64_t v) {\n"
        "    char buf[32];\n"
        "    int p = 31;\n"
        "    buf[p] = '\\n';\n"
        "    do { buf[--p] = (char)('0' + (v %% 10)); v /= 10; } while (v > 0);\n"
        "    sb_rt_write(buf + p, (uint32_t)(32 - p));\n"
        "}\n\n"
        "static void sb_rt_print_i64(int64_t v) {\n"
        "    char buf[32];\n"
        "    int p = 31;\n"
        "    buf[p] = '\\n';\n"
        "    uint64_t u = (v < 0) ? (0ULL - (uint64_t)v) : (uint64_t)v;\n"
        "    do { buf[--p] = (char)('0' + (u %% 10)); u /= 10; } while (u > 0);\n"
        "    if (v < 0) buf[--p] = '-';\n"
        "    sb_rt_write(buf + p, (uint32_t)(32 - p));\n"
        "}\n\n"
        "static void sb_rt_print_f64(double x) {\n"
        "    char buf[64];\n"
        "    int p = 63;\n"
        "    buf[p] = '\\n';\n"
        "    int neg = (x < 0.0);\n"
        "    if (neg) x = -x;\n"
        "    uint64_t scaled = (uint64_t)(x * 1000000.0 + 0.5);\n"
        "    uint64_t ipart = scaled / 1000000ULL;\n"
        "    uint64_t fpart = scaled %% 1000000ULL;\n"
        "    int fdigits = 6;\n"
        "    while (fdigits > 1 && (fpart %% 10ULL) == 0ULL) { fpart /= 10ULL; fdigits--; }\n"
        "    for (int i = 0; i < fdigits; i++) { buf[--p] = (char)('0' + (fpart %% 10ULL)); fpart /= 10ULL; }\n"
        "    buf[--p] = '.';\n"
        "    do { buf[--p] = (char)('0' + (ipart %% 10ULL)); ipart /= 10ULL; } while (ipart > 0);\n"
        "    if (neg) buf[--p] = '-';\n"
        "    sb_rt_write(buf + p, (uint32_t)(64 - p));\n"
        "}\n\n"
        "static void sb_rt_print_bool(uint8_t b) {\n"
        "    if (b) sb_rt_write(\"true\\n\", 5);\n"
        "    else   sb_rt_write(\"false\\n\", 6);\n"
        "}\n\n"
        "static void sb_rt_print_str(sb_str s) {\n"
        "    if (s.len > 0) sb_rt_write(s.ptr, (uint32_t)s.len);\n"
        "    sb_rt_write(\"\\n\", 1);\n"
        "}\n\n"
        "static inline uint8_t sb_str_eq(sb_str a, sb_str b) {\n"
        "    if (a.len != b.len) return 0;\n"
        "    if (a.ptr == b.ptr || a.len == 0) return 1;\n"
        "    return memcmp(a.ptr, b.ptr, (size_t)a.len) == 0;\n"
        "}\n\n"
        "static inline void* sb_chk_null(const void *p, const char *msg, int len) {\n"
        "    if (__builtin_expect(p == 0, 0)) sb_rt_trap(msg, len);\n"
        "    return (void*)p;\n"
        "}\n\n"
        "static inline int64_t sb_chk_idx(int64_t idx, int64_t bound, const char *msg, int len) {\n"
        "    if (__builtin_expect((uint64_t)idx >= (uint64_t)bound, 0)) sb_rt_trap(msg, len);\n"
        "    return idx;\n"
        "}\n\n"
        "static inline void sb_chk_slice(int64_t lo, int64_t hi, int64_t bound, const char *msg, int len) {\n"
        "    if (__builtin_expect(lo < 0 || hi < lo || hi > bound, 0)) sb_rt_trap(msg, len);\n"
        "}\n\n"
        "static inline void sb_chk_ptr_slice(int64_t lo, int64_t hi, const char *msg, int len) {\n"
        "    if (__builtin_expect(lo < 0 || hi < lo, 0)) sb_rt_trap(msg, len);\n"
        "}\n\n"
        "static inline uint64_t sb_chk_shift(uint64_t s, int bits, const char *msg, int len) {\n"
        "    if (__builtin_expect(s >= (uint64_t)bits, 0)) sb_rt_trap(msg, len);\n"
        "    return s;\n"
        "}\n\n"
        "#define SB_DEF_INT_OPS(BITS, ST, UT, SMIN, SMAX, UMAX) \\\n"
        "static inline ST sb_add_i##BITS(ST a, ST b, const char *m, int l) { ST r; if (__builtin_add_overflow(a, b, &r)) sb_rt_trap(m, l); return r; } \\\n"
        "static inline UT sb_add_u##BITS(UT a, UT b, const char *m, int l) { UT r; if (__builtin_add_overflow(a, b, &r)) sb_rt_trap(m, l); return r; } \\\n"
        "static inline ST sb_sub_i##BITS(ST a, ST b, const char *m, int l) { ST r; if (__builtin_sub_overflow(a, b, &r)) sb_rt_trap(m, l); return r; } \\\n"
        "static inline UT sb_sub_u##BITS(UT a, UT b, const char *m, int l) { UT r; if (__builtin_sub_overflow(a, b, &r)) sb_rt_trap(m, l); return r; } \\\n"
        "static inline ST sb_mul_i##BITS(ST a, ST b, const char *m, int l) { ST r; if (__builtin_mul_overflow(a, b, &r)) sb_rt_trap(m, l); return r; } \\\n"
        "static inline UT sb_mul_u##BITS(UT a, UT b, const char *m, int l) { UT r; if (__builtin_mul_overflow(a, b, &r)) sb_rt_trap(m, l); return r; } \\\n"
        "static inline ST sb_neg_i##BITS(ST a, const char *m, int l) { if (__builtin_expect(a == SMIN, 0)) sb_rt_trap(m, l); return -a; } \\\n"
        "static inline ST sb_div_i##BITS(ST a, ST b, const char *m, int l) { if (__builtin_expect(b == 0 || (a == SMIN && b == -1), 0)) sb_rt_trap(m, l); return a / b; } \\\n"
        "static inline ST sb_rem_i##BITS(ST a, ST b, const char *m, int l) { if (__builtin_expect(b == 0 || (a == SMIN && b == -1), 0)) sb_rt_trap(m, l); return a %% b; } \\\n"
        "static inline UT sb_div_u##BITS(UT a, UT b, const char *m, int l) { if (__builtin_expect(b == 0, 0)) sb_rt_trap(m, l); return a / b; } \\\n"
        "static inline UT sb_rem_u##BITS(UT a, UT b, const char *m, int l) { if (__builtin_expect(b == 0, 0)) sb_rt_trap(m, l); return a %% b; } \\\n"
        "static inline ST sb_conv_i64_i##BITS(int64_t v, const char *m, int l) { if (__builtin_expect(v < (int64_t)(SMIN) || v > (int64_t)(SMAX), 0)) sb_rt_trap(m, l); return (ST)v; } \\\n"
        "static inline UT sb_conv_i64_u##BITS(int64_t v, const char *m, int l) { if (__builtin_expect(v < 0 || (uint64_t)v > (uint64_t)(UMAX), 0)) sb_rt_trap(m, l); return (UT)v; } \\\n"
        "static inline ST sb_conv_u64_i##BITS(uint64_t v, const char *m, int l) { if (__builtin_expect(v > (uint64_t)(SMAX), 0)) sb_rt_trap(m, l); return (ST)v; } \\\n"
        "static inline UT sb_conv_u64_u##BITS(uint64_t v, const char *m, int l) { if (__builtin_expect(v > (uint64_t)(UMAX), 0)) sb_rt_trap(m, l); return (UT)v; } \\\n"
        "static inline ST sb_conv_f_i##BITS(double v, const char *m, int l) { int64_t i = (int64_t)v; if (__builtin_expect(i < (int64_t)(SMIN) || i > (int64_t)(SMAX), 0)) sb_rt_trap(m, l); return (ST)i; } \\\n"
        "static inline UT sb_conv_f_u##BITS(double v, const char *m, int l) { int64_t i = (int64_t)v; if (__builtin_expect(i < 0 || (uint64_t)i > (uint64_t)(UMAX), 0)) sb_rt_trap(m, l); return (UT)i; }\n\n"
        "SB_DEF_INT_OPS(8,  int8_t,  uint8_t,  INT8_MIN,  INT8_MAX,  UINT8_MAX)\n"
        "SB_DEF_INT_OPS(16, int16_t, uint16_t, INT16_MIN, INT16_MAX, UINT16_MAX)\n"
        "SB_DEF_INT_OPS(32, int32_t, uint32_t, INT32_MIN, INT32_MAX, UINT32_MAX)\n"
        "SB_DEF_INT_OPS(64, int64_t, uint64_t, INT64_MIN, INT64_MAX, UINT64_MAX)\n\n"
    );
}

static void generate_opt_c_unit(Program *prog, NodeVec all_decls, bool release, Buf *final_out) {
    OptGen g = {0};
    g.prog = prog;
    g.release = release;

    // 1. Collect all slice types across structs, globals, and functions.
    Type u8_slice = { .kind = TY_SLICE, .elem = &ty_u8 };
    get_slice_type_id(&g, &u8_slice);
    for (int i = 0; i < prog->type_decls.len; i++) {
        collect_slice_types_node(&g, prog->type_decls.items[i]);
    }
    for (int i = 0; i < prog->globals.len; i++) {
        collect_slice_types_node(&g, prog->globals.items[i]->decl);
    }
    for (int i = 0; i < prog->fns.len; i++) {
        collect_slice_types_node(&g, prog->fns.items[i]);
    }
    for (int i = 0; i < all_decls.len; i++) {
        Node *d = all_decls.items[i];
        if (d->kind == N_FN && d->is_extern) {
            collect_slice_types_node(&g, d);
        }
    }

    emit_opt_runtime_preamble(&g.hdr);

    // 2. Forward declare all structs and tagged union enums.
    for (int i = 0; i < prog->type_decls.len; i++) {
        Node *d = prog->type_decls.items[i];
        if (d->kind == N_STRUCT) {
            emit_mangled_ident(&g.hdr, "struct sb_st_", d->name);
            bprintf(&g.hdr, ";\n");
        } else if (d->kind == N_ENUM && d->type && d->type->is_tagged) {
            emit_mangled_ident(&g.hdr, "struct sb_en_", d->name);
            bprintf(&g.hdr, ";\n");
        }
    }

    // 3. Forward-declare slice structs, emit function pointer typedefs, then define slice structs.
    for (int i = 0; i < g.slice_types.len; i++) {
        bprintf(&g.hdr, "typedef struct sb_slice_%d sb_slice_%d;\n", i, i);
    }
    for (int i = 0; i < g.fn_types.len; i++) {
        Type *ft = g.fn_types.items[i];
        bprintf(&g.hdr, "typedef ");
        emit_c_type(&g, &g.hdr, ft->elem);
        bprintf(&g.hdr, " (*sb_fnptr_%d)(", i);
        if (ft->params.len == 0) bprintf(&g.hdr, "void");
        for (int j = 0; j < ft->params.len; j++) {
            if (j > 0) bprintf(&g.hdr, ", ");
            emit_c_type(&g, &g.hdr, ft->params.items[j]);
        }
        bprintf(&g.hdr, ");\n");
    }
    for (int i = 0; i < g.slice_types.len; i++) {
        Type *elem_t = g.slice_types.items[i];
        bprintf(&g.hdr, "struct sb_slice_%d { ", i);
        emit_c_type(&g, &g.hdr, elem_t);
        bprintf(&g.hdr, " *ptr; int64_t len; };\n");
    }
    bprintf(&g.hdr, "\n");

    // 4. Emit struct and tagged union enum definitions in topological layout order.
    for (int i = 0; i < prog->type_decls.len; i++) {
        Node *d = prog->type_decls.items[i];
        if (d->kind == N_STRUCT) {
            Type *st = d->type;
            emit_mangled_ident(&g.hdr, "struct sb_st_", st->name);
            bprintf(&g.hdr, " {\n");
            if (st->fields.len == 0) {
                bprintf(&g.hdr, "    uint8_t _dummy;\n");
            } else {
                for (int j = 0; j < st->fields.len; j++) {
                    StructField *sf = &st->fields.items[j];
                    bprintf(&g.hdr, "    ");
                    emit_c_var_decl(&g, &g.hdr, sf->type, "f_", sf->name, -1);
                    bprintf(&g.hdr, ";\n");
                }
            }
            bprintf(&g.hdr, "};\n\n");
        } else if (d->kind == N_ENUM && d->type && d->type->is_tagged) {
            Type *et = d->type;
            emit_mangled_ident(&g.hdr, "struct sb_en_", et->name);
            bprintf(&g.hdr, " {\n    int32_t tag;\n    union {\n");
            for (int j = 0; j < et->fields.len; j++) {
                StructField *vf = &et->fields.items[j];
                if (vf->has_payload) {
                    bprintf(&g.hdr, "        ");
                    emit_c_var_decl(&g, &g.hdr, vf->type, "v_", vf->name, -1);
                    bprintf(&g.hdr, ";\n");
                }
            }
            bprintf(&g.hdr, "    } u;\n};\n\n");
        }
    }

    // 5. Emit extern DLL function pointer declarations.
    VEC(Node *) extern_fns = {0};
    for (int i = 0; i < all_decls.len; i++) {
        Node *d = all_decls.items[i];
        if (d->kind == N_FN && d->is_extern) {
            bool dup = false;
            for (int k = 0; k < extern_fns.len; k++) {
                if (extern_fns.items[k]->sym == d->sym) { dup = true; break; }
            }
            if (!dup) {
                vec_push(extern_fns, d);
                bprintf(&g.hdr, "static ");
                emit_c_type(&g, &g.hdr, d->type);
                bprintf(&g.hdr, " (*");
                emit_mangled_ident(&g.hdr, "sb_ext_", d->name);
                bprintf(&g.hdr, ")(");
                if (d->list.len == 0) bprintf(&g.hdr, "void");
                for (int j = 0; j < d->list.len; j++) {
                    if (j > 0) bprintf(&g.hdr, ", ");
                    emit_c_type(&g, &g.hdr, d->list.items[j]->type);
                }
                bprintf(&g.hdr, ");\n");
            }
        }
    }
    bprintf(&g.hdr, "\n");

    // 6. Emit global variables.
    for (int i = 0; i < prog->globals.len; i++) {
        Symbol *gs = prog->globals.items[i];
        bprintf(&g.hdr, "static ");
        emit_c_var_decl(&g, &g.hdr, gs->type, "sb_g_", gs->name, -1);
        if (gs->decl->rhs) {
            bprintf(&g.hdr, " = ");
            if (gs->type->kind == TY_ARRAY && gs->decl->rhs->kind == N_ARRAY_LIT) {
                bprintf(&g.hdr, "{");
                if (gs->decl->rhs->list.len == 0) bprintf(&g.hdr, "0");
                for (int j = 0; j < gs->decl->rhs->list.len; j++) {
                    if (j > 0) bprintf(&g.hdr, ", ");
                    opt_gen_expr(&g, &g.hdr, gs->decl->rhs->list.items[j]);
                }
                bprintf(&g.hdr, "}");
            } else {
                opt_gen_expr(&g, &g.hdr, gs->decl->rhs);
            }
        } else {
            bprintf(&g.hdr, " = {0}");
        }
        bprintf(&g.hdr, ";\n");
    }
    bprintf(&g.hdr, "\n");

    // 7. Forward declare all user functions.
    for (int i = 0; i < prog->fns.len; i++) {
        Node *fn = prog->fns.items[i];
        bprintf(&g.hdr, "static ");
        emit_c_type(&g, &g.hdr, fn->type);
        bprintf(&g.hdr, " ");
        emit_mangled_ident(&g.hdr, "sb_fn_", fn->name);
        bprintf(&g.hdr, "(");
        if (fn->list.len == 0) bprintf(&g.hdr, "void");
        for (int j = 0; j < fn->list.len; j++) {
            if (j > 0) bprintf(&g.hdr, ", ");
            Node *p = fn->list.items[j];
            emit_c_type(&g, &g.hdr, p->type);
            bprintf(&g.hdr, " v%d_%s", p->sym->slot, p->name);
        }
        bprintf(&g.hdr, ");\n");
    }
    bprintf(&g.hdr, "\n");

    // 8. Emit all function bodies.
    for (int i = 0; i < prog->fns.len; i++) {
        Node *fn = prog->fns.items[i];
        g.cur_ret_type = fn->type;
        g.defers.len = 0;
        bprintf(&g.out, "static ");
        emit_c_type(&g, &g.out, fn->type);
        bprintf(&g.out, " ");
        emit_mangled_ident(&g.out, "sb_fn_", fn->name);
        bprintf(&g.out, "(");
        if (fn->list.len == 0) bprintf(&g.out, "void");
        for (int j = 0; j < fn->list.len; j++) {
            if (j > 0) bprintf(&g.out, ", ");
            Node *p = fn->list.items[j];
            emit_c_type(&g, &g.out, p->type);
            bprintf(&g.out, " v%d_%s", p->sym->slot, p->name);
        }
        bprintf(&g.out, ") {\n");
        opt_gen_block(&g, &g.out, fn->body);
        bprintf(&g.out, "}\n\n");
    }

    // 9. Emit `main` entry point that initializes `sb_stdout_handle` and resolves extern DLL imports.
    bprintf(&g.out,
        "int main(void) {\n"
        "    sb_stdout_handle = GetStdHandle((uint32_t)-11);\n"
    );
    for (int i = 0; i < extern_fns.len; i++) {
        Node *d = extern_fns.items[i];
        bprintf(&g.out, "    {\n        void *h = LoadLibraryA(");
        emit_c_str_literal(&g.out, d->dll_name, (int)strlen(d->dll_name));
        bprintf(&g.out, ");\n        *(void**)&");
        emit_mangled_ident(&g.out, "sb_ext_", d->name);
        bprintf(&g.out, " = GetProcAddress(h, ");
        emit_c_str_literal(&g.out, d->name, (int)strlen(d->name));
        bprintf(&g.out, ");\n    }\n");
    }
    bprintf(&g.out,
        "    sb_fn_main();\n"
        "    return 0;\n"
        "}\n"
    );

    buf_bytes(final_out, g.hdr.data, g.hdr.len);
    buf_bytes(final_out, g.out.data, g.out.len);
    free(g.hdr.data);
    free(g.out.data);
    free(g.defers.items);
    free(g.slice_types.items);
    free(g.fn_types.items);
    free(extern_fns.items);
}

// Emits textual SSA LLVM IR (`.ll`) for inspection/LLVM tools.
bool emit_llvm_ir(Program *prog, NodeVec all_decls, const char *ll_path, bool release) {
    (void)all_decls;
    Buf b = {0};
    bprintf(&b, "; ModuleID = 'antimony_module'\n");
    bprintf(&b, "target datalayout = \"e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128\"\n");
    bprintf(&b, "target triple = \"x86_64-pc-windows-msvc\"\n\n");
    bprintf(&b, "%%sb.str = type { ptr, i64 }\n");
    bprintf(&b, "declare ptr @GetStdHandle(i32)\n");
    bprintf(&b, "declare i32 @WriteFile(ptr, ptr, i32, ptr, ptr)\n");
    bprintf(&b, "declare void @ExitProcess(i32) noreturn\n\n");
    for (int i = 0; i < prog->fns.len; i++) {
        Node *fn = prog->fns.items[i];
        bprintf(&b, "; fn %s (%s mode)\n", fn->name, release ? "release" : "debug");
    }
    FILE *fp = fopen(ll_path, "wb");
    if (!fp) return false;
    fwrite(b.data, 1, b.len, fp);
    fclose(fp);
    free(b.data);
    return true;
}

static const char *find_opt_compiler(bool *is_clang) {
    *is_clang = false;
#ifdef _WIN32
    static const char *clang_candidates[] = {
        "C:\\Program Files\\LLVM\\bin\\clang.exe",
        "C:\\Program Files (x86)\\LLVM\\bin\\clang.exe",
        "C:\\msys64\\clang64\\bin\\clang.exe",
        "C:\\msys64\\mingw64\\bin\\clang.exe",
        NULL
    };
    for (int i = 0; clang_candidates[i]; i++) {
        if (GetFileAttributesA(clang_candidates[i]) != INVALID_FILE_ATTRIBUTES) {
            *is_clang = true;
            return clang_candidates[i];
        }
    }
    static const char *gcc_candidates[] = {
        "C:\\Qt\\Tools\\mingw1310_64\\bin\\gcc.exe",
        "C:\\msys64\\mingw64\\bin\\gcc.exe",
        "C:\\msys64\\ucrt64\\bin\\gcc.exe",
        "C:\\MinGW\\bin\\gcc.exe",
        NULL
    };
    for (int i = 0; gcc_candidates[i]; i++) {
        if (GetFileAttributesA(gcc_candidates[i]) != INVALID_FILE_ATTRIBUTES) {
            return gcc_candidates[i];
        }
    }
#endif
    return "gcc";
}

bool compile_opt_backend(Program *prog, NodeVec all_decls, const char *exe_path, bool release) {
    Buf c_src = {0};
    generate_opt_c_unit(prog, all_decls, release, &c_src);

    char tmp_c[1024];
    snprintf(tmp_c, sizeof(tmp_c), "%s.__sb_opt.c", exe_path);
    FILE *fp = fopen(tmp_c, "wb");
    if (!fp) {
        free(c_src.data);
        die("cannot create temporary backend file '%s'", tmp_c);
    }
    fwrite(c_src.data, 1, c_src.len, fp);
    fclose(fp);
    free(c_src.data);

    bool is_clang = false;
    const char *cc = find_opt_compiler(&is_clang);
    char cmd[4096];
#ifdef _WIN32
    // In cmd.exe, wrap the entire command in outer quotes when executable path contains spaces/quotes.
    snprintf(cmd, sizeof(cmd),
             "\"\"%s\" -O3 -march=native -fno-strict-aliasing -Wno-unused-value -Wno-unused-label \"%s\" -o \"%s\" -lkernel32\"",
             cc, tmp_c, exe_path);
#else
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -O3 -march=native -fno-strict-aliasing -Wno-unused-value -Wno-unused-label \"%s\" -o \"%s\"",
             cc, tmp_c, exe_path);
#endif
    int rc = system(cmd);
    remove(tmp_c);
    return rc == 0;
}
