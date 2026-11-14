// llvm_c.c - Direct AST-to-LLVM-IR (.ll) & LLVM Optimizing (-O3 / --llvm) backend.
//
// Lowers the type-checked Antimony AST directly into LLVM 23 textual IR (.ll)
// targeting x86_64-pc-windows-msvc (zero intermediate C translation):
//   1. `emit_llvm_ir`: writes the full textual LLVM IR module (.ll) to disk.
//   2. `compile_opt_backend`: writes a temporary `.ll` module and invokes LLVM
//      `clang -O3 -march=native` directly on that IR to produce a PE32+ `.exe`.
#include "sb.h"
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
    const char *str;
    int len;
    const char *sym_name;
} LlvmStrLit;

typedef struct {
    const char *op;
    const char *ty;
} LlvmVal;

typedef struct {
    Buf hdr;
    Buf mod_fns;
    Buf fn_entry;
    Buf out;
    Program *prog;
    bool release;
    int tmp_id;
    int lbl_id;
    int str_id;
    Type *cur_ret_type;
    bool block_terminated;
    bool *slot_emitted;
    int slot_cap;
    VEC(Node *) defers;
    VEC(LlvmStrLit) str_lits;
    VEC(char *) pool;
} LlvmGen;

// ---------------------------------------------------------------------------
// Formatting & pool helpers
// ---------------------------------------------------------------------------

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

static const char *llvm_vfmt(LlvmGen *g, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) n = 0;
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    vec_push(g->pool, s);
    return s;
}

static const char *llvm_fmt(LlvmGen *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const char *s = llvm_vfmt(g, fmt, ap);
    va_end(ap);
    return s;
}

static const char *llvm_tmp(LlvmGen *g) {
    return llvm_fmt(g, "%%t%d", ++g->tmp_id);
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

static const char *llvm_mangled(LlvmGen *g, const char *prefix, const char *name) {
    Buf b = {0};
    emit_mangled_ident(&b, prefix, name);
    buf_u8(&b, '\0');
    const char *res = llvm_fmt(g, "%s", (const char *)b.data);
    free(b.data);
    return res;
}

// ---------------------------------------------------------------------------
// Type & constant mapping
// ---------------------------------------------------------------------------

static int llvm_type_align(Type *t) {
    if (!t || t->align <= 0) return 8;
    return t->align;
}

static const char *llvm_type_str(LlvmGen *g, Type *t) {
    if (!t || t->kind == TY_VOID) return "void";
    if (t == &ty_bool) return "i8";
    if (t == &ty_f32)  return "float";
    if (t == &ty_f64)  return "double";
    if (t == &ty_str || t->kind == TY_SLICE) return "%sb.slice";
    if (t->kind == TY_INT) {
        return llvm_fmt(g, "i%d", t->size * 8);
    }
    if (t->kind == TY_ENUM) {
        return t->is_tagged ? llvm_mangled(g, "%sb.en.", t->name) : "i32";
    }
    if (t->kind == TY_STRUCT) {
        return llvm_mangled(g, "%sb.st.", t->name);
    }
    if (t->kind == TY_ARRAY) {
        return llvm_fmt(g, "[%d x %s]", t->array_len, llvm_type_str(g, t->elem));
    }
    if (t->kind == TY_PTR || t->kind == TY_FN || t->kind == TY_NULL) {
        return "ptr";
    }
    return "i64";
}

static const char *llvm_zero_const(Type *t) {
    if (!t) return "0";
    if (t->kind == TY_PTR || t->kind == TY_FN || t->kind == TY_NULL) return "null";
    if (t->kind == TY_FLOAT) return "0.0";
    if (t == &ty_str || t->kind == TY_SLICE || t->kind == TY_STRUCT ||
        t->kind == TY_ARRAY || (t->kind == TY_ENUM && t->is_tagged)) {
        return "zeroinitializer";
    }
    return "0";
}

static const char *llvm_float_const(LlvmGen *g, double val, Type *t) {
    double d = (t == &ty_f32) ? (double)(float)val : val;
    uint64_t bits = 0;
    memcpy(&bits, &d, 8);
    return llvm_fmt(g, "0x%016llX", (unsigned long long)bits);
}

static const char *llvm_int_const(LlvmGen *g, i128 val, Type *t) {
    if (t == &ty_bool) return val ? "1" : "0";
    if (t->is_signed) return llvm_fmt(g, "%lld", (long long)(int64_t)val);
    return llvm_fmt(g, "%llu", (unsigned long long)(uint64_t)val);
}

static void emit_llvm_str_bytes(Buf *b, const char *s, int len) {
    bprintf(b, "c\"");
    for (int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c >= 32 && c < 127 && c != '"' && c != '\\') {
            buf_u8(b, c);
        } else {
            bprintf(b, "\\%02X", c);
        }
    }
    bprintf(b, "\\00\"");
}

static const char *llvm_intern_str(LlvmGen *g, const char *s, int len) {
    for (int i = 0; i < g->str_lits.len; i++) {
        LlvmStrLit *lit = &g->str_lits.items[i];
        if (lit->len == len && (len == 0 || memcmp(lit->str, s, (size_t)len) == 0)) {
            return lit->sym_name;
        }
    }
    const char *sym = llvm_fmt(g, "@.str.%d", ++g->str_id);
    bprintf(&g->hdr, "%s = private unnamed_addr constant [%d x i8] ", sym, len + 1);
    emit_llvm_str_bytes(&g->hdr, s, len);
    bprintf(&g->hdr, ", align 1\n");
    LlvmStrLit lit = { .str = s, .len = len, .sym_name = sym };
    vec_push(g->str_lits, lit);
    return sym;
}

static void llvm_loc_str(LlvmGen *g, Pos pos, const char *reason, const char **sym_out, int *len_out) {
    char msg[512];
    int len = snprintf(msg, sizeof(msg), "%s:%d:%d: runtime panic: %s\n",
                       file_name_of(pos), pos.line, pos.col, reason);
    if (len < 0) len = 0;
    if (len >= (int)sizeof(msg)) len = (int)sizeof(msg) - 1;
    char *copy = xmalloc((size_t)len + 1);
    memcpy(copy, msg, (size_t)len + 1);
    vec_push(g->pool, copy);
    *sym_out = llvm_intern_str(g, copy, len);
    *len_out = len;
}

static void emit_llvm_const_init(LlvmGen *g, Buf *b, Node *expr, Type *t) {
    if (!expr) {
        bprintf(b, "%s", llvm_zero_const(t));
        return;
    }
    if (expr->kind == N_NULL) {
        bprintf(b, "null");
        return;
    }
    if (expr->is_const) {
        if (t->kind == TY_FLOAT) bprintf(b, "%s", llvm_float_const(g, expr->fval, t));
        else                     bprintf(b, "%s", llvm_int_const(g, expr->cval, t));
        return;
    }
    if (t->kind == TY_ARRAY && expr->kind == N_ARRAY_LIT) {
        const char *elem_ty = llvm_type_str(g, t->elem);
        bprintf(b, "[ ");
        for (int i = 0; i < t->array_len; i++) {
            if (i > 0) bprintf(b, ", ");
            bprintf(b, "%s ", elem_ty);
            if (i < expr->list.len) emit_llvm_const_init(g, b, expr->list.items[i], t->elem);
            else                    bprintf(b, "%s", llvm_zero_const(t->elem));
        }
        bprintf(b, " ]");
        return;
    }
    if (t->kind == TY_STRUCT && expr->kind == N_STRUCT_LIT) {
        bprintf(b, "{ ");
        for (int i = 0; i < t->fields.len; i++) {
            StructField *sf = &t->fields.items[i];
            Node *init_rhs = NULL;
            for (int j = 0; j < expr->list.len; j++) {
                if (expr->list.items[j]->name == sf->name) {
                    init_rhs = expr->list.items[j]->rhs;
                    break;
                }
            }
            if (i > 0) bprintf(b, ", ");
            bprintf(b, "%s ", llvm_type_str(g, sf->type));
            emit_llvm_const_init(g, b, init_rhs, sf->type);
        }
        bprintf(b, " }");
        return;
    }
    bprintf(b, "%s", llvm_zero_const(t));
}

// ---------------------------------------------------------------------------
// Basic block, slot, and SSA value helpers
// ---------------------------------------------------------------------------

static void llvm_ensure_block(LlvmGen *g) {
    if (g->block_terminated) {
        bprintf(&g->out, "L_unreach_%d:\n", ++g->lbl_id);
        g->block_terminated = false;
    }
}

static void llvm_emit_br(LlvmGen *g, int lbl) {
    if (!g->block_terminated) {
        bprintf(&g->out, "  br label %%L_%d\n", lbl);
        g->block_terminated = true;
    }
}

static void llvm_emit_label(LlvmGen *g, int lbl) {
    if (!g->block_terminated) {
        bprintf(&g->out, "  br label %%L_%d\n", lbl);
    }
    bprintf(&g->out, "L_%d:\n", lbl);
    g->block_terminated = false;
}

static const char *llvm_alloca_ty(LlvmGen *g, const char *ty_str, int align) {
    const char *tmp = llvm_tmp(g);
    bprintf(&g->fn_entry, "  %s = alloca %s, align %d\n", tmp, ty_str, align > 0 ? align : 8);
    return tmp;
}

static const char *llvm_sym_ptr(LlvmGen *g, Symbol *s) {
    if (s->kind == SYM_FN) {
        return llvm_mangled(g, "@sb_fn_", s->name);
    }
    if (s->is_global) {
        return llvm_mangled(g, "@sb_g_", s->name);
    }
    const char *name = llvm_fmt(g, "%%v%d", s->slot);
    if (!g->slot_emitted[s->slot]) {
        g->slot_emitted[s->slot] = true;
        if (s->kind == SYM_PARAM && s->type->kind == TY_ARRAY) {
            bprintf(&g->fn_entry, "  %s = alloca ptr, align 8\n", name);
        } else {
            bprintf(&g->fn_entry, "  %s = alloca %s, align %d\n",
                    name, llvm_type_str(g, s->type), llvm_type_align(s->type));
        }
    }
    return name;
}

static const char *llvm_gep_bytes(LlvmGen *g, const char *base_ptr, int byte_off) {
    if (byte_off == 0) return base_ptr;
    llvm_ensure_block(g);
    const char *tmp = llvm_tmp(g);
    bprintf(&g->out, "  %s = getelementptr inbounds i8, ptr %s, i64 %d\n",
            tmp, base_ptr, byte_off);
    return tmp;
}

static LlvmVal llvm_load_ty(LlvmGen *g, const char *ptr, const char *ty_str, int align) {
    llvm_ensure_block(g);
    const char *tmp = llvm_tmp(g);
    bprintf(&g->out, "  %s = load %s, ptr %s, align %d\n",
            tmp, ty_str, ptr, align > 0 ? align : 8);
    return (LlvmVal){ tmp, ty_str };
}

static void llvm_store_val(LlvmGen *g, LlvmVal val, const char *ptr, int align) {
    llvm_ensure_block(g);
    bprintf(&g->out, "  store %s %s, ptr %s, align %d\n",
            val.ty, val.op, ptr, align > 0 ? align : 8);
}

static LlvmVal llvm_to_i1(LlvmGen *g, LlvmVal v) {
    if (strcmp(v.ty, "i1") == 0) return v;
    llvm_ensure_block(g);
    const char *tmp = llvm_tmp(g);
    bprintf(&g->out, "  %s = icmp ne %s %s, 0\n", tmp, v.ty, v.op);
    return (LlvmVal){ tmp, "i1" };
}

static LlvmVal llvm_to_i64(LlvmGen *g, LlvmVal v, Type *t) {
    if (strcmp(v.ty, "i64") == 0) return v;
    llvm_ensure_block(g);
    const char *tmp = llvm_tmp(g);
    if (t->kind == TY_PTR || t->kind == TY_FN) {
        bprintf(&g->out, "  %s = ptrtoint ptr %s to i64\n", tmp, v.op);
    } else if (t->is_signed && t != &ty_bool) {
        bprintf(&g->out, "  %s = sext %s %s to i64\n", tmp, v.ty, v.op);
    } else {
        bprintf(&g->out, "  %s = zext %s %s to i64\n", tmp, v.ty, v.op);
    }
    return (LlvmVal){ tmp, "i64" };
}

static LlvmVal llvm_resize_int(LlvmGen *g, LlvmVal v, int src_bits, int dst_bits, bool is_signed) {
    if (src_bits == dst_bits) return v;
    llvm_ensure_block(g);
    const char *dst_ty = llvm_fmt(g, "i%d", dst_bits);
    const char *tmp = llvm_tmp(g);
    if (dst_bits < src_bits) {
        bprintf(&g->out, "  %s = trunc %s %s to %s\n", tmp, v.ty, v.op, dst_ty);
    } else {
        bprintf(&g->out, "  %s = %s %s %s to %s\n",
                tmp, is_signed ? "sext" : "zext", v.ty, v.op, dst_ty);
    }
    return (LlvmVal){ tmp, dst_ty };
}

static LlvmVal llvm_chk_null(LlvmGen *g, LlvmVal ptr_v, Pos pos, const char *reason) {
    if (g->release) return ptr_v;
    llvm_ensure_block(g);
    const char *msg_sym = NULL;
    int msg_len = 0;
    llvm_loc_str(g, pos, reason, &msg_sym, &msg_len);
    const char *tmp = llvm_tmp(g);
    bprintf(&g->out, "  %s = call ptr @sb_chk_null(ptr %s, ptr %s, i32 %d)\n",
            tmp, ptr_v.op, msg_sym, msg_len);
    return (LlvmVal){ tmp, "ptr" };
}

static LlvmVal llvm_make_slice(LlvmGen *g, const char *ptr_op, const char *len_op) {
    llvm_ensure_block(g);
    const char *t1 = llvm_tmp(g);
    const char *t2 = llvm_tmp(g);
    bprintf(&g->out, "  %s = insertvalue %%sb.slice poison, ptr %s, 0\n", t1, ptr_op);
    bprintf(&g->out, "  %s = insertvalue %%sb.slice %s, i64 %s, 1\n", t2, t1, len_op);
    return (LlvmVal){ t2, "%sb.slice" };
}

// ---------------------------------------------------------------------------
// LLVM Runtime Preamble (pure LLVM IR)
// ---------------------------------------------------------------------------

static void emit_llvm_int_ops_for_width(Buf *b, int bits, const char *smin, const char *smax, const char *umax) {
    // Signed & unsigned checked add/sub/mul using LLVM overflow intrinsics
    const char *ops[3][2] = {
        { "add", "add" },
        { "sub", "sub" },
        { "mul", "mul" }
    };
    for (int i = 0; i < 3; i++) {
        for (int sg = 0; sg < 2; sg++) {
            const char *sname = sg ? "i" : "u";
            const char *iname = sg ? "s" : "u";
            bprintf(b,
                "define internal i%d @sb_%s_%s%d(i%d %%a, i%d %%b, ptr %%m, i32 %%l) #0 {\n"
                "entry:\n"
                "  %%res = call { i%d, i1 } @llvm.%s%s.with.overflow.i%d(i%d %%a, i%d %%b)\n"
                "  %%ov0 = extractvalue { i%d, i1 } %%res, 1\n"
                "  %%ov = call i1 @llvm.expect.i1(i1 %%ov0, i1 false)\n"
                "  br i1 %%ov, label %%trap, label %%ok\n"
                "trap:\n"
                "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
                "  unreachable\n"
                "ok:\n"
                "  %%val = extractvalue { i%d, i1 } %%res, 0\n"
                "  ret i%d %%val\n"
                "}\n",
                bits, ops[i][0], sname, bits, bits, bits,
                bits, iname, ops[i][1], bits, bits, bits,
                bits, bits, bits);
        }
    }

    // Checked signed negation
    bprintf(b,
        "define internal i%d @sb_neg_i%d(i%d %%a, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%ov0 = icmp eq i%d %%a, %s\n"
        "  %%ov = call i1 @llvm.expect.i1(i1 %%ov0, i1 false)\n"
        "  br i1 %%ov, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n"
        "  %%val = sub i%d 0, %%a\n"
        "  ret i%d %%val\n"
        "}\n",
        bits, bits, bits, bits, smin, bits, bits);

    // Checked signed div/rem
    for (int is_rem = 0; is_rem < 2; is_rem++) {
        const char *fn = is_rem ? "rem" : "div";
        const char *ins = is_rem ? "srem" : "sdiv";
        bprintf(b,
            "define internal i%d @sb_%s_i%d(i%d %%a, i%d %%b, ptr %%m, i32 %%l) #0 {\n"
            "entry:\n"
            "  %%z = icmp eq i%d %%b, 0\n"
            "  %%mn = icmp eq i%d %%a, %s\n"
            "  %%m1 = icmp eq i%d %%b, -1\n"
            "  %%ov1 = and i1 %%mn, %%m1\n"
            "  %%bad0 = or i1 %%z, %%ov1\n"
            "  %%bad = call i1 @llvm.expect.i1(i1 %%bad0, i1 false)\n"
            "  br i1 %%bad, label %%trap, label %%ok\n"
            "trap:\n"
            "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
            "  unreachable\n"
            "ok:\n"
            "  %%val = %s i%d %%a, %%b\n"
            "  ret i%d %%val\n"
            "}\n",
            bits, fn, bits, bits, bits,
            bits, bits, smin, bits, ins, bits, bits);
    }

    // Checked unsigned div/rem
    for (int is_rem = 0; is_rem < 2; is_rem++) {
        const char *fn = is_rem ? "rem" : "div";
        const char *ins = is_rem ? "urem" : "udiv";
        bprintf(b,
            "define internal i%d @sb_%s_u%d(i%d %%a, i%d %%b, ptr %%m, i32 %%l) #0 {\n"
            "entry:\n"
            "  %%z = icmp eq i%d %%b, 0\n"
            "  %%bad = call i1 @llvm.expect.i1(i1 %%z, i1 false)\n"
            "  br i1 %%bad, label %%trap, label %%ok\n"
            "trap:\n"
            "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
            "  unreachable\n"
            "ok:\n"
            "  %%val = %s i%d %%a, %%b\n"
            "  ret i%d %%val\n"
            "}\n",
            bits, fn, bits, bits, bits, bits, ins, bits, bits);
    }

    // Checked i64 -> i<B> / u<B>
    bprintf(b,
        "define internal i%d @sb_conv_i64_i%d(i64 %%v, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%lo = icmp slt i64 %%v, %s\n"
        "  %%hi = icmp sgt i64 %%v, %s\n"
        "  %%bad0 = or i1 %%lo, %%hi\n"
        "  %%bad = call i1 @llvm.expect.i1(i1 %%bad0, i1 false)\n"
        "  br i1 %%bad, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n",
        bits, bits, smin, smax);
    if (bits < 64) {
        bprintf(b, "  %%val = trunc i64 %%v to i%d\n  ret i%d %%val\n}\n", bits, bits);
    } else {
        bprintf(b, "  ret i64 %%v\n}\n");
    }

    bprintf(b,
        "define internal i%d @sb_conv_i64_u%d(i64 %%v, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%lo = icmp slt i64 %%v, 0\n",
        bits, bits);
    if (bits < 64) {
        bprintf(b,
            "  %%hi = icmp ugt i64 %%v, %s\n"
            "  %%bad0 = or i1 %%lo, %%hi\n",
            umax);
    } else {
        bprintf(b, "  %%bad0 = xor i1 %%lo, false\n");
    }
    bprintf(b,
        "  %%bad = call i1 @llvm.expect.i1(i1 %%bad0, i1 false)\n"
        "  br i1 %%bad, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n");
    if (bits < 64) {
        bprintf(b, "  %%val = trunc i64 %%v to i%d\n  ret i%d %%val\n}\n", bits, bits);
    } else {
        bprintf(b, "  ret i64 %%v\n}\n");
    }

    // Checked u64 -> i<B> / u<B>
    bprintf(b,
        "define internal i%d @sb_conv_u64_i%d(i64 %%v, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%bad0 = icmp ugt i64 %%v, %s\n"
        "  %%bad = call i1 @llvm.expect.i1(i1 %%bad0, i1 false)\n"
        "  br i1 %%bad, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n",
        bits, bits, smax);
    if (bits < 64) {
        bprintf(b, "  %%val = trunc i64 %%v to i%d\n  ret i%d %%val\n}\n", bits, bits);
    } else {
        bprintf(b, "  ret i64 %%v\n}\n");
    }

    bprintf(b,
        "define internal i%d @sb_conv_u64_u%d(i64 %%v, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n",
        bits, bits);
    if (bits < 64) {
        bprintf(b,
            "  %%bad0 = icmp ugt i64 %%v, %s\n"
            "  %%bad = call i1 @llvm.expect.i1(i1 %%bad0, i1 false)\n"
            "  br i1 %%bad, label %%trap, label %%ok\n"
            "trap:\n"
            "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
            "  unreachable\n"
            "ok:\n"
            "  %%val = trunc i64 %%v to i%d\n"
            "  ret i%d %%val\n}\n",
            umax, bits, bits);
    } else {
        bprintf(b, "  ret i64 %%v\n}\n");
    }

    // Checked double -> i<B> / u<B>
    bprintf(b,
        "define internal i%d @sb_conv_f_i%d(double %%v, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%i = fptosi double %%v to i64\n"
        "  %%lo = icmp slt i64 %%i, %s\n"
        "  %%hi = icmp sgt i64 %%i, %s\n"
        "  %%bad0 = or i1 %%lo, %%hi\n"
        "  %%bad = call i1 @llvm.expect.i1(i1 %%bad0, i1 false)\n"
        "  br i1 %%bad, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n",
        bits, bits, smin, smax);
    if (bits < 64) {
        bprintf(b, "  %%val = trunc i64 %%i to i%d\n  ret i%d %%val\n}\n", bits, bits);
    } else {
        bprintf(b, "  ret i64 %%i\n}\n");
    }

    bprintf(b,
        "define internal i%d @sb_conv_f_u%d(double %%v, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%i = fptosi double %%v to i64\n"
        "  %%lo = icmp slt i64 %%i, 0\n",
        bits, bits);
    if (bits < 64) {
        bprintf(b,
            "  %%hi = icmp ugt i64 %%i, %s\n"
            "  %%bad0 = or i1 %%lo, %%hi\n",
            umax);
    } else {
        bprintf(b, "  %%bad0 = xor i1 %%lo, false\n");
    }
    bprintf(b,
        "  %%bad = call i1 @llvm.expect.i1(i1 %%bad0, i1 false)\n"
        "  br i1 %%bad, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n");
    if (bits < 64) {
        bprintf(b, "  %%val = trunc i64 %%i to i%d\n  ret i%d %%val\n}\n", bits, bits);
    } else {
        bprintf(b, "  ret i64 %%i\n}\n");
    }
}

static void emit_llvm_runtime_preamble(Buf *b) {
    bprintf(b,
        "; ModuleID = 'antimony_module'\n"
        "target datalayout = \"e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128\"\n"
        "target triple = \"x86_64-pc-windows-msvc\"\n\n"
        "%%sb.slice = type { ptr, i64 }\n"
        "@sb_stdout_handle = internal global ptr null, align 8\n"
        "@.str.nl    = private unnamed_addr constant [2 x i8] c\"\\0A\\00\", align 1\n"
        "@.str.true  = private unnamed_addr constant [6 x i8] c\"true\\0A\\00\", align 1\n"
        "@.str.false = private unnamed_addr constant [7 x i8] c\"false\\0A\\00\", align 1\n\n"
        "declare ptr @GetStdHandle(i32)\n"
        "declare i32 @WriteFile(ptr, ptr, i32, ptr, ptr)\n"
        "declare void @ExitProcess(i32) #1\n"
        "declare ptr @LoadLibraryA(ptr)\n"
        "declare ptr @GetProcAddress(ptr, ptr)\n"
        "declare i32 @memcmp(ptr, ptr, i64)\n"
        "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n"
        "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)\n"
        "declare i1 @llvm.expect.i1(i1, i1)\n\n"
    );

    const int widths[4] = { 8, 16, 32, 64 };
    for (int i = 0; i < 4; i++) {
        int bits = widths[i];
        bprintf(b,
            "declare { i%d, i1 } @llvm.sadd.with.overflow.i%d(i%d, i%d)\n"
            "declare { i%d, i1 } @llvm.uadd.with.overflow.i%d(i%d, i%d)\n"
            "declare { i%d, i1 } @llvm.ssub.with.overflow.i%d(i%d, i%d)\n"
            "declare { i%d, i1 } @llvm.usub.with.overflow.i%d(i%d, i%d)\n"
            "declare { i%d, i1 } @llvm.smul.with.overflow.i%d(i%d, i%d)\n"
            "declare { i%d, i1 } @llvm.umul.with.overflow.i%d(i%d, i%d)\n",
            bits, bits, bits, bits, bits, bits, bits, bits,
            bits, bits, bits, bits, bits, bits, bits, bits,
            bits, bits, bits, bits, bits, bits, bits, bits);
    }

    bprintf(b,
        "\ndefine internal void @sb_rt_write(ptr %%buf, i32 %%len) #2 {\n"
        "entry:\n"
        "  %%written = alloca i32, align 4\n"
        "  store i32 0, ptr %%written, align 4\n"
        "  %%h = load ptr, ptr @sb_stdout_handle, align 8\n"
        "  %%ok = call i32 @WriteFile(ptr %%h, ptr %%buf, i32 %%len, ptr %%written, ptr null)\n"
        "  ret void\n"
        "}\n\n"
        "define internal void @sb_rt_trap(ptr %%msg, i32 %%len) #1 {\n"
        "entry:\n"
        "  %%written = alloca i32, align 4\n"
        "  store i32 0, ptr %%written, align 4\n"
        "  %%herr = call ptr @GetStdHandle(i32 -12)\n"
        "  %%ok = call i32 @WriteFile(ptr %%herr, ptr %%msg, i32 %%len, ptr %%written, ptr null)\n"
        "  call void @ExitProcess(i32 101)\n"
        "  unreachable\n"
        "}\n\n"
        "define internal void @sb_rt_print_u64(i64 %%v0) #2 {\n"
        "entry:\n"
        "  %%buf = alloca [32 x i8], align 1\n"
        "  %%p31 = getelementptr inbounds [32 x i8], ptr %%buf, i64 0, i64 31\n"
        "  store i8 10, ptr %%p31, align 1\n"
        "  br label %%loop\n"
        "loop:\n"
        "  %%v = phi i64 [ %%v0, %%entry ], [ %%q, %%loop ]\n"
        "  %%pos = phi i64 [ 31, %%entry ], [ %%pos1, %%loop ]\n"
        "  %%pos1 = sub nsw i64 %%pos, 1\n"
        "  %%rem = urem i64 %%v, 10\n"
        "  %%q = udiv i64 %%v, 10\n"
        "  %%d8 = trunc i64 %%rem to i8\n"
        "  %%ch = add i8 %%d8, 48\n"
        "  %%ptr = getelementptr inbounds [32 x i8], ptr %%buf, i64 0, i64 %%pos1\n"
        "  store i8 %%ch, ptr %%ptr, align 1\n"
        "  %%more = icmp ugt i64 %%q, 0\n"
        "  br i1 %%more, label %%loop, label %%done\n"
        "done:\n"
        "  %%sptr = getelementptr inbounds [32 x i8], ptr %%buf, i64 0, i64 %%pos1\n"
        "  %%len64 = sub i64 32, %%pos1\n"
        "  %%len32 = trunc i64 %%len64 to i32\n"
        "  call void @sb_rt_write(ptr %%sptr, i32 %%len32)\n"
        "  ret void\n"
        "}\n\n"
        "define internal void @sb_rt_print_i64(i64 %%v) #2 {\n"
        "entry:\n"
        "  %%buf = alloca [32 x i8], align 1\n"
        "  %%p31 = getelementptr inbounds [32 x i8], ptr %%buf, i64 0, i64 31\n"
        "  store i8 10, ptr %%p31, align 1\n"
        "  %%is_neg = icmp slt i64 %%v, 0\n"
        "  %%neg_v = sub i64 0, %%v\n"
        "  %%u0 = select i1 %%is_neg, i64 %%neg_v, i64 %%v\n"
        "  br label %%loop\n"
        "loop:\n"
        "  %%u = phi i64 [ %%u0, %%entry ], [ %%q, %%loop ]\n"
        "  %%pos = phi i64 [ 31, %%entry ], [ %%pos1, %%loop ]\n"
        "  %%pos1 = sub nsw i64 %%pos, 1\n"
        "  %%rem = urem i64 %%u, 10\n"
        "  %%q = udiv i64 %%u, 10\n"
        "  %%d8 = trunc i64 %%rem to i8\n"
        "  %%ch = add i8 %%d8, 48\n"
        "  %%ptr = getelementptr inbounds [32 x i8], ptr %%buf, i64 0, i64 %%pos1\n"
        "  store i8 %%ch, ptr %%ptr, align 1\n"
        "  %%more = icmp ugt i64 %%q, 0\n"
        "  br i1 %%more, label %%loop, label %%sign\n"
        "sign:\n"
        "  br i1 %%is_neg, label %%emit_minus, label %%done\n"
        "emit_minus:\n"
        "  %%pos2 = sub nsw i64 %%pos1, 1\n"
        "  %%mptr = getelementptr inbounds [32 x i8], ptr %%buf, i64 0, i64 %%pos2\n"
        "  store i8 45, ptr %%mptr, align 1\n"
        "  br label %%done\n"
        "done:\n"
        "  %%fpos = phi i64 [ %%pos1, %%sign ], [ %%pos2, %%emit_minus ]\n"
        "  %%sptr = getelementptr inbounds [32 x i8], ptr %%buf, i64 0, i64 %%fpos\n"
        "  %%len64 = sub i64 32, %%fpos\n"
        "  %%len32 = trunc i64 %%len64 to i32\n"
        "  call void @sb_rt_write(ptr %%sptr, i32 %%len32)\n"
        "  ret void\n"
        "}\n\n"
        "define internal void @sb_rt_print_f64(double %%x) #2 {\n"
        "entry:\n"
        "  %%buf = alloca [64 x i8], align 1\n"
        "  %%p63 = getelementptr inbounds [64 x i8], ptr %%buf, i64 0, i64 63\n"
        "  store i8 10, ptr %%p63, align 1\n"
        "  %%neg = fcmp olt double %%x, 0.0\n"
        "  %%nx = fneg double %%x\n"
        "  %%ax = select i1 %%neg, double %%nx, double %%x\n"
        "  %%m = fmul double %%ax, 1.000000e+06\n"
        "  %%r = fadd double %%m, 5.000000e-01\n"
        "  %%scaled = fptoui double %%r to i64\n"
        "  %%ipart0 = udiv i64 %%scaled, 1000000\n"
        "  %%fpart0 = urem i64 %%scaled, 1000000\n"
        "  br label %%strip\n"
        "strip:\n"
        "  %%fd = phi i32 [ 6, %%entry ], [ %%fd1, %%strip_step ]\n"
        "  %%fp = phi i64 [ %%fpart0, %%entry ], [ %%fp1, %%strip_step ]\n"
        "  %%gt1 = icmp sgt i32 %%fd, 1\n"
        "  %%mod10 = urem i64 %%fp, 10\n"
        "  %%z = icmp eq i64 %%mod10, 0\n"
        "  %%cont = and i1 %%gt1, %%z\n"
        "  br i1 %%cont, label %%strip_step, label %%floop_pre\n"
        "strip_step:\n"
        "  %%fp1 = udiv i64 %%fp, 10\n"
        "  %%fd1 = sub i32 %%fd, 1\n"
        "  br label %%strip\n"
        "floop_pre:\n"
        "  br label %%floop\n"
        "floop:\n"
        "  %%fi = phi i32 [ 0, %%floop_pre ], [ %%fi1, %%floop ]\n"
        "  %%fcur = phi i64 [ %%fp, %%floop_pre ], [ %%fq, %%floop ]\n"
        "  %%pos = phi i64 [ 63, %%floop_pre ], [ %%pos1, %%floop ]\n"
        "  %%pos1 = sub nsw i64 %%pos, 1\n"
        "  %%frem = urem i64 %%fcur, 10\n"
        "  %%fq = udiv i64 %%fcur, 10\n"
        "  %%fd8 = trunc i64 %%frem to i8\n"
        "  %%fch = add i8 %%fd8, 48\n"
        "  %%fptr = getelementptr inbounds [64 x i8], ptr %%buf, i64 0, i64 %%pos1\n"
        "  store i8 %%fch, ptr %%fptr, align 1\n"
        "  %%fi1 = add i32 %%fi, 1\n"
        "  %%fmore = icmp slt i32 %%fi1, %%fd\n"
        "  br i1 %%fmore, label %%floop, label %%dot\n"
        "dot:\n"
        "  %%dotpos = sub nsw i64 %%pos1, 1\n"
        "  %%dptr = getelementptr inbounds [64 x i8], ptr %%buf, i64 0, i64 %%dotpos\n"
        "  store i8 46, ptr %%dptr, align 1\n"
        "  br label %%iloop\n"
        "iloop:\n"
        "  %%icur = phi i64 [ %%ipart0, %%dot ], [ %%iq, %%iloop ]\n"
        "  %%ipos = phi i64 [ %%dotpos, %%dot ], [ %%ipos1, %%iloop ]\n"
        "  %%ipos1 = sub nsw i64 %%ipos, 1\n"
        "  %%irem = urem i64 %%icur, 10\n"
        "  %%iq = udiv i64 %%icur, 10\n"
        "  %%id8 = trunc i64 %%irem to i8\n"
        "  %%ich = add i8 %%id8, 48\n"
        "  %%iptr = getelementptr inbounds [64 x i8], ptr %%buf, i64 0, i64 %%ipos1\n"
        "  store i8 %%ich, ptr %%iptr, align 1\n"
        "  %%imore = icmp ugt i64 %%iq, 0\n"
        "  br i1 %%imore, label %%iloop, label %%fsign\n"
        "fsign:\n"
        "  br i1 %%neg, label %%emit_minus, label %%done\n"
        "emit_minus:\n"
        "  %%mpos = sub nsw i64 %%ipos1, 1\n"
        "  %%mptr = getelementptr inbounds [64 x i8], ptr %%buf, i64 0, i64 %%mpos\n"
        "  store i8 45, ptr %%mptr, align 1\n"
        "  br label %%done\n"
        "done:\n"
        "  %%finalpos = phi i64 [ %%ipos1, %%fsign ], [ %%mpos, %%emit_minus ]\n"
        "  %%sptr = getelementptr inbounds [64 x i8], ptr %%buf, i64 0, i64 %%finalpos\n"
        "  %%len64 = sub i64 64, %%finalpos\n"
        "  %%len32 = trunc i64 %%len64 to i32\n"
        "  call void @sb_rt_write(ptr %%sptr, i32 %%len32)\n"
        "  ret void\n"
        "}\n\n"
        "define internal void @sb_rt_print_bool(i8 %%b) #2 {\n"
        "entry:\n"
        "  %%z = icmp eq i8 %%b, 0\n"
        "  br i1 %%z, label %%f, label %%t\n"
        "t:\n"
        "  call void @sb_rt_write(ptr @.str.true, i32 5)\n"
        "  ret void\n"
        "f:\n"
        "  call void @sb_rt_write(ptr @.str.false, i32 6)\n"
        "  ret void\n"
        "}\n\n"
        "define internal void @sb_rt_print_str(%%sb.slice %%s) #2 {\n"
        "entry:\n"
        "  %%sp = extractvalue %%sb.slice %%s, 0\n"
        "  %%sl = extractvalue %%sb.slice %%s, 1\n"
        "  %%pos = icmp sgt i64 %%sl, 0\n"
        "  br i1 %%pos, label %%write_s, label %%write_nl\n"
        "write_s:\n"
        "  %%l32 = trunc i64 %%sl to i32\n"
        "  call void @sb_rt_write(ptr %%sp, i32 %%l32)\n"
        "  br label %%write_nl\n"
        "write_nl:\n"
        "  call void @sb_rt_write(ptr @.str.nl, i32 1)\n"
        "  ret void\n"
        "}\n\n"
        "define internal i8 @sb_str_eq(%%sb.slice %%a, %%sb.slice %%b) #0 {\n"
        "entry:\n"
        "  %%ap = extractvalue %%sb.slice %%a, 0\n"
        "  %%al = extractvalue %%sb.slice %%a, 1\n"
        "  %%bp = extractvalue %%sb.slice %%b, 0\n"
        "  %%bl = extractvalue %%sb.slice %%b, 1\n"
        "  %%len_eq = icmp eq i64 %%al, %%bl\n"
        "  br i1 %%len_eq, label %%chk_ptr, label %%ret_false\n"
        "chk_ptr:\n"
        "  %%ptr_eq = icmp eq ptr %%ap, %%bp\n"
        "  %%len_z = icmp eq i64 %%al, 0\n"
        "  %%fast_ok = or i1 %%ptr_eq, %%len_z\n"
        "  br i1 %%fast_ok, label %%ret_true, label %%do_cmp\n"
        "do_cmp:\n"
        "  %%c = call i32 @memcmp(ptr %%ap, ptr %%bp, i64 %%al)\n"
        "  %%eq = icmp eq i32 %%c, 0\n"
        "  %%r = zext i1 %%eq to i8\n"
        "  ret i8 %%r\n"
        "ret_true:\n"
        "  ret i8 1\n"
        "ret_false:\n"
        "  ret i8 0\n"
        "}\n\n"
        "define internal ptr @sb_chk_null(ptr %%p, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%z = icmp eq ptr %%p, null\n"
        "  %%bad = call i1 @llvm.expect.i1(i1 %%z, i1 false)\n"
        "  br i1 %%bad, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n"
        "  ret ptr %%p\n"
        "}\n\n"
        "define internal i64 @sb_chk_idx(i64 %%idx, i64 %%bound, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%ov = icmp uge i64 %%idx, %%bound\n"
        "  %%bad = call i1 @llvm.expect.i1(i1 %%ov, i1 false)\n"
        "  br i1 %%bad, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n"
        "  ret i64 %%idx\n"
        "}\n\n"
        "define internal void @sb_chk_slice(i64 %%lo, i64 %%hi, i64 %%bound, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%c1 = icmp slt i64 %%lo, 0\n"
        "  %%c2 = icmp slt i64 %%hi, %%lo\n"
        "  %%c3 = icmp sgt i64 %%hi, %%bound\n"
        "  %%c12 = or i1 %%c1, %%c2\n"
        "  %%bad0 = or i1 %%c12, %%c3\n"
        "  %%bad = call i1 @llvm.expect.i1(i1 %%bad0, i1 false)\n"
        "  br i1 %%bad, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n"
        "  ret void\n"
        "}\n\n"
        "define internal void @sb_chk_ptr_slice(i64 %%lo, i64 %%hi, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%c1 = icmp slt i64 %%lo, 0\n"
        "  %%c2 = icmp slt i64 %%hi, %%lo\n"
        "  %%bad0 = or i1 %%c1, %%c2\n"
        "  %%bad = call i1 @llvm.expect.i1(i1 %%bad0, i1 false)\n"
        "  br i1 %%bad, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n"
        "  ret void\n"
        "}\n\n"
        "define internal i64 @sb_chk_shift(i64 %%s, i32 %%bits, ptr %%m, i32 %%l) #0 {\n"
        "entry:\n"
        "  %%b64 = zext i32 %%bits to i64\n"
        "  %%ov = icmp uge i64 %%s, %%b64\n"
        "  %%bad = call i1 @llvm.expect.i1(i1 %%ov, i1 false)\n"
        "  br i1 %%bad, label %%trap, label %%ok\n"
        "trap:\n"
        "  call void @sb_rt_trap(ptr %%m, i32 %%l)\n"
        "  unreachable\n"
        "ok:\n"
        "  ret i64 %%s\n"
        "}\n\n"
    );

    emit_llvm_int_ops_for_width(b,  8, "-128", "127", "255");
    emit_llvm_int_ops_for_width(b, 16, "-32768", "32767", "65535");
    emit_llvm_int_ops_for_width(b, 32, "-2147483648", "2147483647", "4294967295");
    emit_llvm_int_ops_for_width(b, 64, "-9223372036854775808", "9223372036854775807", "18446744073709551615");
    bprintf(b, "\n");
}

// ---------------------------------------------------------------------------
// Forward declarations for recursive AST lowering
// ---------------------------------------------------------------------------

static LlvmVal llvm_gen_expr(LlvmGen *g, Node *n);
static const char *llvm_gen_lvalue(LlvmGen *g, Node *n);
static const char *llvm_gen_addr(LlvmGen *g, Node *n);
static void llvm_gen_init_at_ptr(LlvmGen *g, const char *dst_ptr, Node *expr, Type *target_t);
static void llvm_gen_stmt(LlvmGen *g, Node *n);
static void llvm_gen_block(LlvmGen *g, Node *blk);
static void llvm_emit_defers_from(LlvmGen *g, int limit);

// Evaluates any slice/array/string expression to a `%sb.slice` value.
static LlvmVal llvm_gen_collection_slice(LlvmGen *g, Node *base) {
    if (base->type->kind == TY_ARRAY && !base->implicit_slice) {
        const char *arr_ptr = llvm_gen_addr(g, base);
        return llvm_make_slice(g, arr_ptr, llvm_fmt(g, "%d", base->type->array_len));
    }
    return llvm_gen_expr(g, base);
}

// Returns a `ptr` holding the storage address of `n`.
static const char *llvm_gen_addr(LlvmGen *g, Node *n) {
    if (n->kind == N_IDENT || n->kind == N_DEREF || n->kind == N_FIELD || n->kind == N_INDEX) {
        if (n->kind == N_IDENT && n->implicit_slice) {
            LlvmVal sl = llvm_gen_expr(g, n);
            const char *p = llvm_alloca_ty(g, "%sb.slice", 8);
            llvm_store_val(g, sl, p, 8);
            return p;
        }
        return llvm_gen_lvalue(g, n);
    }
    if (n->type->kind == TY_ARRAY) {
        const char *tmp = llvm_alloca_ty(g, llvm_type_str(g, n->type), llvm_type_align(n->type));
        llvm_gen_init_at_ptr(g, tmp, n, n->type);
        return tmp;
    }
    LlvmVal v = llvm_gen_expr(g, n);
    const char *tmp = llvm_alloca_ty(g, v.ty, llvm_type_align(n->type));
    llvm_store_val(g, v, tmp, llvm_type_align(n->type));
    return tmp;
}

static const char *llvm_gen_lvalue(LlvmGen *g, Node *n) {
    llvm_ensure_block(g);
    switch (n->kind) {
    case N_IDENT:
        if (n->sym->kind == SYM_PARAM && n->sym->type->kind == TY_ARRAY) {
            LlvmVal pv = llvm_load_ty(g, llvm_sym_ptr(g, n->sym), "ptr", 8);
            return pv.op;
        }
        return llvm_sym_ptr(g, n->sym);

    case N_DEREF: {
        LlvmVal pv = llvm_gen_expr(g, n->lhs);
        LlvmVal cp = llvm_chk_null(g, pv, n->pos, "null pointer dereference");
        return cp.op;
    }

    case N_FIELD: {
        const char *base_ptr = NULL;
        Type *bt = n->lhs->type;
        if (bt->kind == TY_PTR) {
            LlvmVal pv = llvm_gen_expr(g, n->lhs);
            base_ptr = llvm_chk_null(g, pv, n->pos, "null pointer dereference").op;
        } else {
            base_ptr = llvm_gen_addr(g, n->lhs);
        }
        return llvm_gep_bytes(g, base_ptr, n->field_offset);
    }

    case N_INDEX: {
        Type *bt = n->lhs->type;
        const char *elem_ty = llvm_type_str(g, n->type);
        if (bt->kind == TY_PTR) {
            LlvmVal pv = llvm_gen_expr(g, n->lhs);
            const char *bp = llvm_chk_null(g, pv, n->pos, "null pointer dereference").op;
            LlvmVal iv = llvm_gen_expr(g, n->rhs);
            LlvmVal i64 = llvm_to_i64(g, iv, n->rhs->type);
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = getelementptr %s, ptr %s, i64 %s\n", tmp, elem_ty, bp, i64.op);
            return tmp;
        }
        if (bt->kind == TY_ARRAY && !n->lhs->implicit_slice) {
            const char *bp = llvm_gen_addr(g, n->lhs);
            LlvmVal iv = llvm_gen_expr(g, n->rhs);
            LlvmVal i64 = llvm_to_i64(g, iv, n->rhs->type);
            if (!g->release && !n->rhs->is_const) {
                const char *msg_sym = NULL;
                int msg_len = 0;
                llvm_loc_str(g, n->rhs->pos, "index out of bounds", &msg_sym, &msg_len);
                const char *chk = llvm_tmp(g);
                bprintf(&g->out, "  %s = call i64 @sb_chk_idx(i64 %s, i64 %d, ptr %s, i32 %d)\n",
                        chk, i64.op, bt->array_len, msg_sym, msg_len);
                i64.op = chk;
            }
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = getelementptr inbounds %s, ptr %s, i64 %s\n",
                    tmp, elem_ty, bp, i64.op);
            return tmp;
        }
        // Slice or str index
        LlvmVal sl = llvm_gen_collection_slice(g, n->lhs);
        const char *bp = llvm_tmp(g);
        bprintf(&g->out, "  %s = extractvalue %%sb.slice %s, 0\n", bp, sl.op);
        const char *blen = llvm_tmp(g);
        bprintf(&g->out, "  %s = extractvalue %%sb.slice %s, 1\n", blen, sl.op);
        LlvmVal iv = llvm_gen_expr(g, n->rhs);
        LlvmVal i64 = llvm_to_i64(g, iv, n->rhs->type);
        if (!g->release) {
            const char *msg_sym = NULL;
            int msg_len = 0;
            llvm_loc_str(g, n->rhs->pos, "index out of bounds", &msg_sym, &msg_len);
            const char *chk = llvm_tmp(g);
            bprintf(&g->out, "  %s = call i64 @sb_chk_idx(i64 %s, i64 %s, ptr %s, i32 %d)\n",
                    chk, i64.op, blen, msg_sym, msg_len);
            i64.op = chk;
        }
        const char *tmp = llvm_tmp(g);
        bprintf(&g->out, "  %s = getelementptr inbounds %s, ptr %s, i64 %s\n",
                tmp, elem_ty, bp, i64.op);
        return tmp;
    }

    default:
        return llvm_gen_addr(g, n);
    }
}

static void llvm_gen_init_at_ptr(LlvmGen *g, const char *dst_ptr, Node *expr, Type *target_t) {
    llvm_ensure_block(g);
    if (!expr) {
        if (target_t->kind == TY_ARRAY || is_aggregate_type(target_t)) {
            bprintf(&g->out, "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 %d, i1 false)\n",
                    dst_ptr, target_t->size);
        } else {
            LlvmVal zv = { llvm_zero_const(target_t), llvm_type_str(g, target_t) };
            llvm_store_val(g, zv, dst_ptr, llvm_type_align(target_t));
        }
        return;
    }
    if (target_t->kind == TY_ARRAY) {
        if (expr->kind == N_ARRAY_LIT) {
            if (expr->list.len < target_t->array_len) {
                bprintf(&g->out, "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 %d, i1 false)\n",
                        dst_ptr, target_t->size);
            }
            const char *arr_ty = llvm_type_str(g, target_t);
            for (int i = 0; i < expr->list.len; i++) {
                const char *ep = llvm_tmp(g);
                bprintf(&g->out, "  %s = getelementptr inbounds %s, ptr %s, i64 0, i64 %d\n",
                        ep, arr_ty, dst_ptr, i);
                llvm_gen_init_at_ptr(g, ep, expr->list.items[i], target_t->elem);
            }
            return;
        }
        const char *src_ptr = llvm_gen_addr(g, expr);
        bprintf(&g->out, "  call void @llvm.memcpy.p0.p0.i64(ptr %s, ptr %s, i64 %d, i1 false)\n",
                dst_ptr, src_ptr, target_t->size);
        return;
    }
    if (target_t->kind == TY_SLICE && expr->type->kind == TY_ARRAY) {
        LlvmVal sv = llvm_gen_collection_slice(g, expr);
        llvm_store_val(g, sv, dst_ptr, 8);
        return;
    }
    LlvmVal v = llvm_gen_expr(g, expr);
    llvm_store_val(g, v, dst_ptr, llvm_type_align(target_t));
}

// Performs an integer/float/pointer binary operation (shared by N_BINARY & compound N_ASSIGN).
static LlvmVal llvm_emit_arith_op(LlvmGen *g, TokKind op, LlvmVal lhs, LlvmVal rhs,
                                    Type *lhs_t, Type *rhs_t, Pos pos) {
    llvm_ensure_block(g);

    if (lhs_t->kind == TY_PTR && (op == T_PLUS || op == T_MINUS)) {
        if (op == T_MINUS && rhs_t == lhs_t) {
            const char *p1 = llvm_tmp(g);
            const char *p2 = llvm_tmp(g);
            const char *diff = llvm_tmp(g);
            bprintf(&g->out, "  %s = ptrtoint ptr %s to i64\n", p1, lhs.op);
            bprintf(&g->out, "  %s = ptrtoint ptr %s to i64\n", p2, rhs.op);
            bprintf(&g->out, "  %s = sub i64 %s, %s\n", diff, p1, p2);
            int esz = lhs_t->elem->size;
            if (esz > 1) {
                const char *q = llvm_tmp(g);
                bprintf(&g->out, "  %s = sdiv i64 %s, %d\n", q, diff, esz);
                return (LlvmVal){ q, "i64" };
            }
            return (LlvmVal){ diff, "i64" };
        }
        LlvmVal off = llvm_to_i64(g, rhs, rhs_t);
        if (op == T_MINUS) {
            const char *neg = llvm_tmp(g);
            bprintf(&g->out, "  %s = sub i64 0, %s\n", neg, off.op);
            off.op = neg;
        }
        const char *elem_ty = llvm_type_str(g, lhs_t->elem);
        const char *tmp = llvm_tmp(g);
        bprintf(&g->out, "  %s = getelementptr %s, ptr %s, i64 %s\n",
                tmp, elem_ty, lhs.op, off.op);
        return (LlvmVal){ tmp, "ptr" };
    }

    if (lhs_t->kind == TY_FLOAT) {
        const char *fop = "fadd";
        if (op == T_MINUS)      fop = "fsub";
        else if (op == T_STAR)  fop = "fmul";
        else if (op == T_SLASH) fop = "fdiv";
        const char *tmp = llvm_tmp(g);
        bprintf(&g->out, "  %s = %s %s %s, %s\n", tmp, fop, lhs.ty, lhs.op, rhs.op);
        return (LlvmVal){ tmp, lhs.ty };
    }

    int bits = lhs_t->size * 8;
    const char *lty = lhs.ty;

    if (op == T_AMP || op == T_PIPE || op == T_CARET) {
        const char *bop = (op == T_AMP) ? "and" : (op == T_PIPE) ? "or" : "xor";
        const char *tmp = llvm_tmp(g);
        bprintf(&g->out, "  %s = %s %s %s, %s\n", tmp, bop, lty, lhs.op, rhs.op);
        return (LlvmVal){ tmp, lty };
    }

    if (op == T_SHL || op == T_SHR) {
        LlvmVal sh_amt = rhs;
        if (!g->release) {
            LlvmVal s64 = llvm_to_i64(g, rhs, rhs_t);
            const char *msg_sym = NULL;
            int msg_len = 0;
            llvm_loc_str(g, pos, "shift count >= bit width of type", &msg_sym, &msg_len);
            const char *chk = llvm_tmp(g);
            bprintf(&g->out, "  %s = call i64 @sb_chk_shift(i64 %s, i32 %d, ptr %s, i32 %d)\n",
                    chk, s64.op, bits, msg_sym, msg_len);
            sh_amt = llvm_resize_int(g, (LlvmVal){ chk, "i64" }, 64, bits, false);
        } else {
            sh_amt = llvm_resize_int(g, rhs, rhs_t->size * 8, bits, false);
            const char *masked = llvm_tmp(g);
            bprintf(&g->out, "  %s = and %s %s, %d\n", masked, lty, sh_amt.op, bits - 1);
            sh_amt.op = masked;
        }
        const char *sop = (op == T_SHL) ? "shl" : (lhs_t->is_signed ? "ashr" : "lshr");
        const char *tmp = llvm_tmp(g);
        bprintf(&g->out, "  %s = %s %s %s, %s\n", tmp, sop, lty, lhs.op, sh_amt.op);
        return (LlvmVal){ tmp, lty };
    }

    if (op == T_SLASH || op == T_PERCENT) {
        const char *fn = (op == T_SLASH) ? "div" : "rem";
        const char *msg_sym = NULL;
        int msg_len = 0;
        llvm_loc_str(g, pos, "division by zero or overflow", &msg_sym, &msg_len);
        const char *tmp = llvm_tmp(g);
        bprintf(&g->out, "  %s = call %s @sb_%s_%s%d(%s %s, %s %s, ptr %s, i32 %d)\n",
                tmp, lty, fn, lhs_t->is_signed ? "i" : "u", bits,
                lty, lhs.op, lty, rhs.op, msg_sym, msg_len);
        return (LlvmVal){ tmp, lty };
    }

    bool is_wrap = (op == T_PLUSW || op == T_MINUSW || op == T_STARW);
    if (is_wrap || g->release) {
        const char *aop = (op == T_MINUS || op == T_MINUSW) ? "sub"
                        : (op == T_STAR  || op == T_STARW)  ? "mul" : "add";
        const char *tmp = llvm_tmp(g);
        bprintf(&g->out, "  %s = %s %s %s, %s\n", tmp, aop, lty, lhs.op, rhs.op);
        return (LlvmVal){ tmp, lty };
    }

    const char *opname = (op == T_PLUS) ? "add" : (op == T_MINUS) ? "sub" : "mul";
    const char *reason = (op == T_PLUS)  ? "integer addition overflow"
                       : (op == T_MINUS) ? "integer subtraction overflow"
                                         : "integer multiplication overflow";
    const char *msg_sym = NULL;
    int msg_len = 0;
    llvm_loc_str(g, pos, reason, &msg_sym, &msg_len);
    const char *tmp = llvm_tmp(g);
    bprintf(&g->out, "  %s = call %s @sb_%s_%s%d(%s %s, %s %s, ptr %s, i32 %d)\n",
            tmp, lty, opname, lhs_t->is_signed ? "i" : "u", bits,
            lty, lhs.op, lty, rhs.op, msg_sym, msg_len);
    return (LlvmVal){ tmp, lty };
}

static LlvmVal llvm_gen_expr(LlvmGen *g, Node *n) {
    llvm_ensure_block(g);

    if (n->is_const && n->type->kind == TY_FLOAT) {
        return (LlvmVal){ llvm_float_const(g, n->fval, n->type), llvm_type_str(g, n->type) };
    }
    if (n->is_const && (n->type->kind == TY_INT || n->type->kind == TY_ENUM || n->type == &ty_bool)) {
        return (LlvmVal){ llvm_int_const(g, n->cval, n->type), llvm_type_str(g, n->type) };
    }

    switch (n->kind) {
    case N_INT:
    case N_BOOL:
        return (LlvmVal){ llvm_int_const(g, n->cval, n->type), llvm_type_str(g, n->type) };

    case N_NULL:
        return (LlvmVal){ "null", "ptr" };

    case N_FLOAT:
        return (LlvmVal){ llvm_float_const(g, n->fval, n->type), llvm_type_str(g, n->type) };

    case N_STR: {
        const char *sym = llvm_intern_str(g, n->str, n->str_len);
        if (n->type->kind == TY_PTR) {
            return (LlvmVal){ sym, "ptr" };
        }
        return llvm_make_slice(g, sym, llvm_fmt(g, "%d", n->str_len));
    }

    case N_IDENT:
        if (n->sym->kind == SYM_FN) {
            return (LlvmVal){ llvm_sym_ptr(g, n->sym), "ptr" };
        }
        if (n->implicit_slice) {
            const char *arr_ptr = llvm_gen_lvalue(g, n);
            return llvm_make_slice(g, arr_ptr, llvm_fmt(g, "%d", n->type->array_len));
        }
        if (n->sym->type == &ty_str && n->type->kind == TY_PTR) {
            LlvmVal sv = llvm_load_ty(g, llvm_sym_ptr(g, n->sym), "%sb.slice", 8);
            const char *p = llvm_tmp(g);
            bprintf(&g->out, "  %s = extractvalue %%sb.slice %s, 0\n", p, sv.op);
            return (LlvmVal){ p, "ptr" };
        }
        return llvm_load_ty(g, llvm_gen_lvalue(g, n), llvm_type_str(g, n->type), llvm_type_align(n->type));

    case N_ADDR:
        return (LlvmVal){ llvm_gen_lvalue(g, n->lhs), "ptr" };

    case N_DEREF:
    case N_FIELD:
    case N_INDEX: {
        const char *addr = llvm_gen_lvalue(g, n);
        return llvm_load_ty(g, addr, llvm_type_str(g, n->type), llvm_type_align(n->type));
    }

    case N_STRUCT_LIT: {
        const char *sty = llvm_type_str(g, n->type);
        int al = llvm_type_align(n->type);
        const char *tmp = llvm_alloca_ty(g, sty, al);
        bprintf(&g->out, "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 %d, i1 false)\n",
                tmp, n->type->size);
        for (int i = 0; i < n->list.len; i++) {
            Node *fi = n->list.items[i];
            const char *fp = llvm_gep_bytes(g, tmp, fi->field_offset);
            llvm_gen_init_at_ptr(g, fp, fi->rhs, fi->type);
        }
        return llvm_load_ty(g, tmp, sty, al);
    }

    case N_ENUM_LIT: {
        const char *ety = llvm_type_str(g, n->type);
        int al = llvm_type_align(n->type);
        const char *tmp = llvm_alloca_ty(g, ety, al);
        bprintf(&g->out, "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 %d, i1 false)\n",
                tmp, n->type->size);
        llvm_store_val(g, (LlvmVal){ llvm_fmt(g, "%d", (int32_t)n->cval), "i32" }, tmp, al);
        if (n->lhs) {
            StructField *vf = (StructField *)n->sym2;
            const char *pp = llvm_gep_bytes(g, tmp, n->type->payload_offset);
            llvm_gen_init_at_ptr(g, pp, n->lhs, vf->type);
        }
        return llvm_load_ty(g, tmp, ety, al);
    }

    case N_TRY: {
        Type *et = n->lhs->type;
        const char *ety = llvm_type_str(g, et);
        int eal = llvm_type_align(et);
        const char *try_ptr = llvm_alloca_ty(g, ety, eal);
        LlvmVal ev = llvm_gen_expr(g, n->lhs);
        llvm_store_val(g, ev, try_ptr, eal);

        LlvmVal tag_v = llvm_load_ty(g, try_ptr, "i32", 4);
        const char *is_err = llvm_tmp(g);
        bprintf(&g->out, "  %s = icmp eq i32 %s, %d\n", is_err, tag_v.op, (int32_t)n->cval);
        int l_err = ++g->lbl_id;
        int l_ok  = ++g->lbl_id;
        bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n", is_err, l_err, l_ok);
        g->block_terminated = true;

        llvm_emit_label(g, l_err);
        const char *rty = llvm_type_str(g, g->cur_ret_type);
        int ral = llvm_type_align(g->cur_ret_type);
        const char *eret = llvm_alloca_ty(g, rty, ral);
        bprintf(&g->out, "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 %d, i1 false)\n",
                eret, g->cur_ret_type->size);
        llvm_store_val(g, (LlvmVal){ llvm_fmt(g, "%d", n->field_offset), "i32" }, eret, ral);
        StructField *err_v = (StructField *)n->sym2;
        if (err_v && err_v->has_payload) {
            const char *src_p = llvm_gep_bytes(g, try_ptr, et->payload_offset);
            const char *dst_p = llvm_gep_bytes(g, eret, g->cur_ret_type->payload_offset);
            bprintf(&g->out, "  call void @llvm.memcpy.p0.p0.i64(ptr %s, ptr %s, i64 %d, i1 false)\n",
                    dst_p, src_p, err_v->type->size);
        }
        if (g->defers.len > 0) {
            llvm_emit_defers_from(g, 0);
        }
        if (!g->block_terminated) {
            LlvmVal rv = llvm_load_ty(g, eret, rty, ral);
            bprintf(&g->out, "  ret %s %s\n", rv.ty, rv.op);
            g->block_terminated = true;
        }

        llvm_emit_label(g, l_ok);
        const char *ok_p = llvm_gep_bytes(g, try_ptr, et->payload_offset);
        return llvm_load_ty(g, ok_p, llvm_type_str(g, n->type), llvm_type_align(n->type));
    }

    case N_SLICE: {
        Type *bt = n->lhs->type;
        Type *elem_t = (bt == &ty_str) ? &ty_u8 : n->type->elem;
        const char *elem_ty = llvm_type_str(g, elem_t);
        if (bt->kind == TY_PTR) {
            LlvmVal pv = llvm_gen_expr(g, n->lhs);
            const char *bp = llvm_chk_null(g, pv, n->pos, "null pointer dereference").op;
            LlvmVal lo = n->lo ? llvm_to_i64(g, llvm_gen_expr(g, n->lo), n->lo->type)
                               : (LlvmVal){ "0", "i64" };
            LlvmVal hi = llvm_to_i64(g, llvm_gen_expr(g, n->hi), n->hi->type);
            if (!g->release) {
                const char *msg_sym = NULL;
                int msg_len = 0;
                llvm_loc_str(g, n->pos, "invalid pointer slice bounds", &msg_sym, &msg_len);
                bprintf(&g->out, "  call void @sb_chk_ptr_slice(i64 %s, i64 %s, ptr %s, i32 %d)\n",
                        lo.op, hi.op, msg_sym, msg_len);
            }
            const char *np = llvm_tmp(g);
            bprintf(&g->out, "  %s = getelementptr %s, ptr %s, i64 %s\n", np, elem_ty, bp, lo.op);
            const char *nl = llvm_tmp(g);
            bprintf(&g->out, "  %s = sub i64 %s, %s\n", nl, hi.op, lo.op);
            return llvm_make_slice(g, np, nl);
        }
        LlvmVal sl = llvm_gen_collection_slice(g, n->lhs);
        const char *bp = llvm_tmp(g);
        bprintf(&g->out, "  %s = extractvalue %%sb.slice %s, 0\n", bp, sl.op);
        const char *blen = llvm_tmp(g);
        bprintf(&g->out, "  %s = extractvalue %%sb.slice %s, 1\n", blen, sl.op);
        LlvmVal lo = n->lo ? llvm_to_i64(g, llvm_gen_expr(g, n->lo), n->lo->type)
                           : (LlvmVal){ "0", "i64" };
        LlvmVal hi = n->hi ? llvm_to_i64(g, llvm_gen_expr(g, n->hi), n->hi->type)
                           : (LlvmVal){ blen, "i64" };
        if (!g->release) {
            const char *msg_sym = NULL;
            int msg_len = 0;
            llvm_loc_str(g, n->pos, "slice bounds out of range", &msg_sym, &msg_len);
            bprintf(&g->out, "  call void @sb_chk_slice(i64 %s, i64 %s, i64 %s, ptr %s, i32 %d)\n",
                    lo.op, hi.op, blen, msg_sym, msg_len);
        }
        const char *np = llvm_tmp(g);
        bprintf(&g->out, "  %s = getelementptr inbounds %s, ptr %s, i64 %s\n", np, elem_ty, bp, lo.op);
        const char *nl = llvm_tmp(g);
        bprintf(&g->out, "  %s = sub i64 %s, %s\n", nl, hi.op, lo.op);
        return llvm_make_slice(g, np, nl);
    }

    case N_UNARY: {
        LlvmVal lv = llvm_gen_expr(g, n->lhs);
        if (n->op == T_BANG) {
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = xor i8 %s, 1\n", tmp, lv.op);
            return (LlvmVal){ tmp, "i8" };
        }
        if (n->op == T_TILDE) {
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = xor %s %s, -1\n", tmp, lv.ty, lv.op);
            return (LlvmVal){ tmp, lv.ty };
        }
        if (n->type->kind == TY_FLOAT) {
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = fneg %s %s\n", tmp, lv.ty, lv.op);
            return (LlvmVal){ tmp, lv.ty };
        }
        if (g->release) {
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = sub %s 0, %s\n", tmp, lv.ty, lv.op);
            return (LlvmVal){ tmp, lv.ty };
        }
        const char *msg_sym = NULL;
        int msg_len = 0;
        llvm_loc_str(g, n->pos, "integer negation overflow", &msg_sym, &msg_len);
        const char *tmp = llvm_tmp(g);
        bprintf(&g->out, "  %s = call %s @sb_neg_i%d(%s %s, ptr %s, i32 %d)\n",
                tmp, lv.ty, n->type->size * 8, lv.ty, lv.op, msg_sym, msg_len);
        return (LlvmVal){ tmp, lv.ty };
    }

    case N_CONV: {
        Type *src = n->lhs->type;
        Type *dst = n->type;
        if (dst == &ty_str) {
            return llvm_gen_collection_slice(g, n->lhs);
        }
        LlvmVal v = llvm_gen_expr(g, n->lhs);
        if (src == &ty_str && dst->kind == TY_PTR) {
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = extractvalue %%sb.slice %s, 0\n", tmp, v.op);
            return (LlvmVal){ tmp, "ptr" };
        }
        if ((src->kind == TY_PTR || src->kind == TY_FN) && dst->kind == TY_PTR) {
            return (LlvmVal){ v.op, "ptr" };
        }
        if (src->kind == TY_PTR && dst->kind == TY_INT) {
            const char *dty = llvm_type_str(g, dst);
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = ptrtoint ptr %s to %s\n", tmp, v.op, dty);
            return (LlvmVal){ tmp, dty };
        }
        if (src->kind == TY_INT && dst->kind == TY_PTR) {
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = inttoptr %s %s to ptr\n", tmp, v.ty, v.op);
            return (LlvmVal){ tmp, "ptr" };
        }
        if (src == dst) return v;
        const char *dty = llvm_type_str(g, dst);
        if (src->kind == TY_FLOAT && dst->kind == TY_FLOAT) {
            const char *ins = (dst == &ty_f64) ? "fpext" : "fptrunc";
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = %s %s %s to %s\n", tmp, ins, v.ty, v.op, dty);
            return (LlvmVal){ tmp, dty };
        }
        if ((src->kind == TY_INT || src->kind == TY_ENUM) && dst->kind == TY_FLOAT) {
            const char *ins = src->is_signed ? "sitofp" : "uitofp";
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = %s %s %s to %s\n", tmp, ins, v.ty, v.op, dty);
            return (LlvmVal){ tmp, dty };
        }
        if (src->kind == TY_FLOAT && (dst->kind == TY_INT || dst->kind == TY_ENUM)) {
            Type *fit_dst = (dst->kind == TY_ENUM) ? &ty_i32 : dst;
            if (!g->release) {
                LlvmVal dval = v;
                if (src == &ty_f32) {
                    const char *dtmp = llvm_tmp(g);
                    bprintf(&g->out, "  %s = fpext float %s to double\n", dtmp, v.op);
                    dval = (LlvmVal){ dtmp, "double" };
                }
                const char *msg_sym = NULL;
                int msg_len = 0;
                llvm_loc_str(g, n->pos, "float to integer conversion out of range", &msg_sym, &msg_len);
                const char *tmp = llvm_tmp(g);
                bprintf(&g->out, "  %s = call %s @sb_conv_f_%s%d(double %s, ptr %s, i32 %d)\n",
                        tmp, dty, fit_dst->is_signed ? "i" : "u", fit_dst->size * 8,
                        dval.op, msg_sym, msg_len);
                return (LlvmVal){ tmp, dty };
            }
            const char *ins = fit_dst->is_signed ? "fptosi" : "fptoui";
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = %s %s %s to %s\n", tmp, ins, v.ty, v.op, dty);
            return (LlvmVal){ tmp, dty };
        }
        if ((src->kind == TY_INT || src->kind == TY_ENUM) &&
            (dst->kind == TY_INT || dst->kind == TY_ENUM)) {
            Type *fit_dst = (dst->kind == TY_ENUM) ? &ty_i32 : dst;
            if (!g->release) {
                LlvmVal v64 = llvm_to_i64(g, v, src);
                const char *msg_sym = NULL;
                int msg_len = 0;
                llvm_loc_str(g, n->pos, "integer conversion out of range", &msg_sym, &msg_len);
                const char *tmp = llvm_tmp(g);
                bprintf(&g->out, "  %s = call %s @sb_conv_%s_%s%d(i64 %s, ptr %s, i32 %d)\n",
                        tmp, dty, src->is_signed ? "i64" : "u64",
                        fit_dst->is_signed ? "i" : "u", fit_dst->size * 8,
                        v64.op, msg_sym, msg_len);
                return (LlvmVal){ tmp, dty };
            }
            return llvm_resize_int(g, v, src->size * 8, dst->size * 8, src->is_signed);
        }
        return v;
    }

    case N_BINARY: {
        TokKind op = n->op;
        if (op == T_ANDAND || op == T_OROR) {
            const char *res_slot = llvm_alloca_ty(g, "i8", 1);
            LlvmVal lv = llvm_gen_expr(g, n->lhs);
            LlvmVal b1 = llvm_to_i1(g, lv);
            int l_rhs   = ++g->lbl_id;
            int l_short = ++g->lbl_id;
            int l_end   = ++g->lbl_id;
            if (op == T_ANDAND) {
                bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n", b1.op, l_rhs, l_short);
            } else {
                bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n", b1.op, l_short, l_rhs);
            }
            g->block_terminated = true;
            llvm_emit_label(g, l_short);
            llvm_store_val(g, (LlvmVal){ op == T_ANDAND ? "0" : "1", "i8" }, res_slot, 1);
            llvm_emit_br(g, l_end);
            llvm_emit_label(g, l_rhs);
            LlvmVal rv = llvm_gen_expr(g, n->rhs);
            llvm_store_val(g, rv, res_slot, 1);
            llvm_emit_br(g, l_end);
            llvm_emit_label(g, l_end);
            return llvm_load_ty(g, res_slot, "i8", 1);
        }

        LlvmVal lv = llvm_gen_expr(g, n->lhs);
        LlvmVal rv = llvm_gen_expr(g, n->rhs);

        if (n->lhs->type == &ty_str) {
            const char *eq = llvm_tmp(g);
            bprintf(&g->out, "  %s = call i8 @sb_str_eq(%%sb.slice %s, %%sb.slice %s)\n",
                    eq, lv.op, rv.op);
            if (op == T_NE) {
                const char *ne = llvm_tmp(g);
                bprintf(&g->out, "  %s = xor i8 %s, 1\n", ne, eq);
                return (LlvmVal){ ne, "i8" };
            }
            return (LlvmVal){ eq, "i8" };
        }

        if (op == T_EQ || op == T_NE || op == T_LT || op == T_LE || op == T_GT || op == T_GE) {
            Type *lt = n->lhs->type;
            const char *cmp_res = llvm_tmp(g);
            if (lt->kind == TY_FLOAT) {
                const char *pred = "oeq";
                switch (op) {
                case T_EQ: pred = "oeq"; break;
                case T_NE: pred = "une"; break;
                case T_LT: pred = "olt"; break;
                case T_LE: pred = "ole"; break;
                case T_GT: pred = "ogt"; break;
                case T_GE: pred = "oge"; break;
                default: break;
                }
                bprintf(&g->out, "  %s = fcmp %s %s %s, %s\n", cmp_res, pred, lv.ty, lv.op, rv.op);
            } else {
                bool is_s = lt->is_signed && lt != &ty_bool && lt->kind != TY_PTR && lt->kind != TY_FN;
                const char *pred = "eq";
                switch (op) {
                case T_EQ: pred = "eq"; break;
                case T_NE: pred = "ne"; break;
                case T_LT: pred = is_s ? "slt" : "ult"; break;
                case T_LE: pred = is_s ? "sle" : "ule"; break;
                case T_GT: pred = is_s ? "sgt" : "ugt"; break;
                case T_GE: pred = is_s ? "sge" : "uge"; break;
                default: break;
                }
                bprintf(&g->out, "  %s = icmp %s %s %s, %s\n", cmp_res, pred, lv.ty, lv.op, rv.op);
            }
            const char *z = llvm_tmp(g);
            bprintf(&g->out, "  %s = zext i1 %s to i8\n", z, cmp_res);
            return (LlvmVal){ z, "i8" };
        }

        if ((op == T_SLASH || op == T_PERCENT) && n->lhs->type->kind == TY_INT &&
            n->rhs->is_const && n->rhs->cval > 0) {
            const char *ins = (op == T_SLASH) ? (n->lhs->type->is_signed ? "sdiv" : "udiv")
                                              : (n->lhs->type->is_signed ? "srem" : "urem");
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = %s %s %s, %s\n", tmp, ins, lv.ty, lv.op, rv.op);
            return (LlvmVal){ tmp, lv.ty };
        }

        return llvm_emit_arith_op(g, op, lv, rv, n->lhs->type, n->rhs->type, n->pos);
    }

    case N_CALL: {
        if (n->builtin == B_PRINT) {
            Node *arg = n->list.items[0];
            LlvmVal av = llvm_gen_expr(g, arg);
            if (arg->type == &ty_str) {
                bprintf(&g->out, "  call void @sb_rt_print_str(%%sb.slice %s)\n", av.op);
            } else if (arg->type == &ty_bool) {
                bprintf(&g->out, "  call void @sb_rt_print_bool(i8 %s)\n", av.op);
            } else if (arg->type->kind == TY_FLOAT) {
                if (arg->type == &ty_f32) {
                    const char *d = llvm_tmp(g);
                    bprintf(&g->out, "  %s = fpext float %s to double\n", d, av.op);
                    av.op = d;
                }
                bprintf(&g->out, "  call void @sb_rt_print_f64(double %s)\n", av.op);
            } else if (arg->type->kind == TY_PTR || arg->type->kind == TY_FN) {
                LlvmVal u64 = llvm_to_i64(g, av, arg->type);
                bprintf(&g->out, "  call void @sb_rt_print_u64(i64 %s)\n", u64.op);
            } else if (arg->type->is_signed) {
                LlvmVal i64 = llvm_to_i64(g, av, arg->type);
                bprintf(&g->out, "  call void @sb_rt_print_i64(i64 %s)\n", i64.op);
            } else {
                LlvmVal u64 = llvm_to_i64(g, av, arg->type);
                bprintf(&g->out, "  call void @sb_rt_print_u64(i64 %s)\n", u64.op);
            }
            return (LlvmVal){ NULL, "void" };
        }
        if (n->builtin == B_EXIT) {
            LlvmVal av = llvm_gen_expr(g, n->list.items[0]);
            bprintf(&g->out, "  call void @ExitProcess(i32 %s)\n", av.op);
            bprintf(&g->out, "  unreachable\n");
            g->block_terminated = true;
            return (LlvmVal){ NULL, "void" };
        }
        if (n->builtin == B_LEN) {
            LlvmVal sl = llvm_gen_collection_slice(g, n->list.items[0]);
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = extractvalue %%sb.slice %s, 1\n", tmp, sl.op);
            return (LlvmVal){ tmp, "i64" };
        }
        if (n->builtin == B_ATOMIC_LOAD) {
            LlvmVal p = llvm_chk_null(g, llvm_gen_expr(g, n->list.items[0]),
                                      n->pos, "null pointer in atomic_load");
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = load atomic i64, ptr %s seq_cst, align 8\n", tmp, p.op);
            return (LlvmVal){ tmp, "i64" };
        }
        if (n->builtin == B_ATOMIC_STORE) {
            LlvmVal p = llvm_chk_null(g, llvm_gen_expr(g, n->list.items[0]),
                                      n->pos, "null pointer in atomic_store");
            LlvmVal v = llvm_gen_expr(g, n->list.items[1]);
            bprintf(&g->out, "  store atomic i64 %s, ptr %s seq_cst, align 8\n", v.op, p.op);
            return (LlvmVal){ NULL, "void" };
        }
        if (n->builtin == B_ATOMIC_ADD) {
            LlvmVal p = llvm_chk_null(g, llvm_gen_expr(g, n->list.items[0]),
                                      n->pos, "null pointer in atomic_add");
            LlvmVal v = llvm_gen_expr(g, n->list.items[1]);
            const char *tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = atomicrmw add ptr %s, i64 %s seq_cst, align 8\n",
                    tmp, p.op, v.op);
            return (LlvmVal){ tmp, "i64" };
        }
        if (n->builtin == B_ATOMIC_CAS) {
            LlvmVal p = llvm_chk_null(g, llvm_gen_expr(g, n->list.items[0]),
                                      n->pos, "null pointer in atomic_cas");
            LlvmVal exp_v = llvm_gen_expr(g, n->list.items[1]);
            LlvmVal des_v = llvm_gen_expr(g, n->list.items[2]);
            const char *pair = llvm_tmp(g);
            const char *res  = llvm_tmp(g);
            bprintf(&g->out, "  %s = cmpxchg ptr %s, i64 %s, i64 %s seq_cst seq_cst, align 8\n",
                    pair, p.op, exp_v.op, des_v.op);
            bprintf(&g->out, "  %s = extractvalue { i64, i1 } %s, 0\n", res, pair);
            return (LlvmVal){ res, "i64" };
        }

        const char *callee_op = NULL;
        bool is_indirect = (!n->sym && n->lhs);
        if (is_indirect) {
            LlvmVal fp = llvm_gen_expr(g, n->lhs);
            callee_op = llvm_chk_null(g, fp, n->pos, "null function pointer call").op;
        } else if (n->sym->is_extern) {
            const char *ep = llvm_mangled(g, "@sb_ext_", n->sym->name);
            callee_op = llvm_load_ty(g, ep, "ptr", 8).op;
        } else {
            callee_op = llvm_mangled(g, "@sb_fn_", n->sym->name);
        }

        int nargs = n->list.len;
        LlvmVal *args = nargs ? xmalloc((size_t)nargs * sizeof(LlvmVal)) : NULL;
        for (int i = 0; i < nargs; i++) {
            Node *arg = n->list.items[i];
            Type *pt = is_indirect ? n->lhs->type->params.items[i] : n->sym->decl->list.items[i]->type;
            if (pt->kind == TY_SLICE && arg->type->kind == TY_ARRAY) {
                args[i] = llvm_gen_collection_slice(g, arg);
            } else if (pt->kind == TY_ARRAY) {
                args[i] = (LlvmVal){ llvm_gen_addr(g, arg), "ptr" };
            } else if (pt->kind == TY_PTR && arg->type == &ty_str) {
                LlvmVal sv = llvm_gen_expr(g, arg);
                const char *p = llvm_tmp(g);
                bprintf(&g->out, "  %s = extractvalue %%sb.slice %s, 0\n", p, sv.op);
                args[i] = (LlvmVal){ p, "ptr" };
            } else {
                args[i] = llvm_gen_expr(g, arg);
            }
        }

        const char *rty = llvm_type_str(g, n->type);
        const char *res_tmp = NULL;
        if (n->type == &ty_void) {
            bprintf(&g->out, "  call void %s(", callee_op);
        } else {
            res_tmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = call %s %s(", res_tmp, rty, callee_op);
        }
        for (int i = 0; i < nargs; i++) {
            if (i > 0) bprintf(&g->out, ", ");
            bprintf(&g->out, "%s %s", args[i].ty, args[i].op);
        }
        bprintf(&g->out, ")\n");
        free(args);
        return (LlvmVal){ res_tmp, rty };
    }

    default:
        return (LlvmVal){ llvm_zero_const(n->type), llvm_type_str(g, n->type) };
    }
}

// ---------------------------------------------------------------------------
// Statements, defers, and control flow
// ---------------------------------------------------------------------------

static void llvm_emit_defers_from(LlvmGen *g, int limit) {
    int saved_len = g->defers.len;
    for (int i = saved_len - 1; i >= limit; i--) {
        g->defers.len = i;
        Node *dbody = g->defers.items[i];
        if (dbody->kind == N_BLOCK) llvm_gen_block(g, dbody);
        else                        llvm_gen_stmt(g, dbody);
    }
    g->defers.len = saved_len;
}

static void llvm_gen_assign(LlvmGen *g, Node *n) {
    llvm_ensure_block(g);
    const char *dst_ptr = n->lhs ? llvm_gen_lvalue(g, n->lhs) : llvm_sym_ptr(g, n->sym);
    Type *t = n->type;
    if (n->op == T_ASSIGN) {
        if (t->kind == TY_ARRAY) {
            if (n->rhs->kind == N_ARRAY_LIT) {
                bprintf(&g->out, "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 %d, i1 false)\n",
                        dst_ptr, t->size);
                const char *arr_ty = llvm_type_str(g, t);
                LlvmVal *vals = n->rhs->list.len ? xmalloc((size_t)n->rhs->list.len * sizeof(LlvmVal)) : NULL;
                for (int i = 0; i < n->rhs->list.len; i++) {
                    vals[i] = llvm_gen_expr(g, n->rhs->list.items[i]);
                }
                for (int i = 0; i < n->rhs->list.len; i++) {
                    const char *ep = llvm_tmp(g);
                    bprintf(&g->out, "  %s = getelementptr inbounds %s, ptr %s, i64 0, i64 %d\n",
                            ep, arr_ty, dst_ptr, i);
                    llvm_store_val(g, vals[i], ep, llvm_type_align(t->elem));
                }
                free(vals);
                return;
            }
            const char *src_ptr = llvm_gen_addr(g, n->rhs);
            bprintf(&g->out, "  call void @llvm.memcpy.p0.p0.i64(ptr %s, ptr %s, i64 %d, i1 false)\n",
                    dst_ptr, src_ptr, t->size);
            return;
        }
        llvm_gen_init_at_ptr(g, dst_ptr, n->rhs, t);
        return;
    }

    int al = llvm_type_align(t);
    LlvmVal cur = llvm_load_ty(g, dst_ptr, llvm_type_str(g, t), al);
    LlvmVal rv  = llvm_gen_expr(g, n->rhs);
    LlvmVal res = llvm_emit_arith_op(g, assign_to_binop(n->op), cur, rv, t, n->rhs->type, n->pos);
    llvm_store_val(g, res, dst_ptr, al);
}

static void llvm_gen_stmt(LlvmGen *g, Node *n) {
    llvm_ensure_block(g);
    switch (n->kind) {
    case N_VAR: {
        const char *slot_ptr = llvm_sym_ptr(g, n->sym);
        llvm_gen_init_at_ptr(g, slot_ptr, n->rhs, n->type);
        return;
    }

    case N_CONST:
        return;

    case N_ASSIGN:
        llvm_gen_assign(g, n);
        return;

    case N_EXPR_STMT:
        (void)llvm_gen_expr(g, n->lhs);
        return;

    case N_DEFER:
        vec_push(g->defers, n->body);
        return;

    case N_RETURN:
        if (n->lhs) {
            LlvmVal rv = (g->cur_ret_type->kind == TY_SLICE && n->lhs->type->kind == TY_ARRAY)
                             ? llvm_gen_collection_slice(g, n->lhs)
                             : llvm_gen_expr(g, n->lhs);
            if (g->defers.len > 0) {
                llvm_emit_defers_from(g, 0);
            }
            if (!g->block_terminated) {
                bprintf(&g->out, "  ret %s %s\n", rv.ty, rv.op);
                g->block_terminated = true;
            }
        } else {
            if (g->defers.len > 0) {
                llvm_emit_defers_from(g, 0);
            }
            if (!g->block_terminated) {
                bprintf(&g->out, "  ret void\n");
                g->block_terminated = true;
            }
        }
        return;

    case N_BREAK:
        if (g->defers.len > n->target->defer_depth) {
            llvm_emit_defers_from(g, n->target->defer_depth);
        }
        llvm_emit_br(g, n->target->label_break);
        return;

    case N_CONTINUE:
        if (g->defers.len > n->target->defer_depth) {
            llvm_emit_defers_from(g, n->target->defer_depth);
        }
        llvm_emit_br(g, n->target->label_continue);
        return;

    case N_IF: {
        int l_then = ++g->lbl_id;
        int l_else = n->els ? ++g->lbl_id : 0;
        int l_end  = ++g->lbl_id;
        LlvmVal cv = llvm_gen_expr(g, n->cond);
        LlvmVal b1 = llvm_to_i1(g, cv);
        bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n",
                b1.op, l_then, n->els ? l_else : l_end);
        g->block_terminated = true;

        llvm_emit_label(g, l_then);
        llvm_gen_block(g, n->body);
        bool then_term = g->block_terminated;
        if (!then_term) llvm_emit_br(g, l_end);

        bool else_term = false;
        if (n->els) {
            llvm_emit_label(g, l_else);
            if (n->els->kind == N_IF) llvm_gen_stmt(g, n->els);
            else                      llvm_gen_block(g, n->els);
            else_term = g->block_terminated;
            if (!else_term) llvm_emit_br(g, l_end);
        }
        if (!n->els || !then_term || !else_term) {
            llvm_emit_label(g, l_end);
        }
        return;
    }

    case N_MATCH: {
        Type *st = n->cond->type;
        bool is_tagged_enum = (st->kind == TY_ENUM && st->is_tagged);
        LlvmVal mv = llvm_gen_expr(g, n->cond);
        const char *scrut_ptr = NULL;
        LlvmVal tag_v = mv;
        if (is_tagged_enum) {
            int al = llvm_type_align(st);
            scrut_ptr = llvm_alloca_ty(g, mv.ty, al);
            llvm_store_val(g, mv, scrut_ptr, al);
            tag_v = llvm_load_ty(g, scrut_ptr, "i32", 4);
        }

        int n_arms = n->list.len;
        int *arm_lbls = n_arms ? xmalloc((size_t)n_arms * sizeof(int)) : NULL;
        for (int i = 0; i < n_arms; i++) arm_lbls[i] = ++g->lbl_id;
        int l_els = n->els ? ++g->lbl_id : 0;
        int l_end = ++g->lbl_id;
        bool any_fallthrough = false;

        for (int i = 0; i < n_arms; i++) {
            Node *arm = n->list.items[i];
            int l_next_arm = (i + 1 < n_arms) ? ++g->lbl_id : (n->els ? l_els : l_end);
            if (!n->els && i + 1 == n_arms) any_fallthrough = true;
            for (int j = 0; j < arm->list.len; j++) {
                Node *pat = arm->list.items[j];
                const char *cond_op = NULL;
                if (st == &ty_str) {
                    LlvmVal pv = llvm_gen_expr(g, pat);
                    const char *eq = llvm_tmp(g);
                    bprintf(&g->out, "  %s = call i8 @sb_str_eq(%%sb.slice %s, %%sb.slice %s)\n",
                            eq, mv.op, pv.op);
                    cond_op = llvm_to_i1(g, (LlvmVal){ eq, "i8" }).op;
                } else if (is_tagged_enum) {
                    cond_op = llvm_tmp(g);
                    bprintf(&g->out, "  %s = icmp eq i32 %s, %d\n",
                            cond_op, tag_v.op, (int32_t)pat->cval);
                } else {
                    LlvmVal pv = llvm_gen_expr(g, pat);
                    cond_op = llvm_tmp(g);
                    bprintf(&g->out, "  %s = icmp eq %s %s, %s\n",
                            cond_op, mv.ty, mv.op, pv.op);
                }
                if (j + 1 < arm->list.len) {
                    int l_next_pat = ++g->lbl_id;
                    bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n",
                            cond_op, arm_lbls[i], l_next_pat);
                    g->block_terminated = true;
                    llvm_emit_label(g, l_next_pat);
                } else {
                    bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n",
                            cond_op, arm_lbls[i], l_next_arm);
                    g->block_terminated = true;
                }
            }
            if (i + 1 < n_arms) {
                llvm_emit_label(g, l_next_arm);
            }
        }

        for (int i = 0; i < n_arms; i++) {
            Node *arm = n->list.items[i];
            llvm_emit_label(g, arm_lbls[i]);
            if (is_tagged_enum && arm->sym) {
                Symbol *bs = arm->sym;
                const char *bind_ptr = llvm_sym_ptr(g, bs);
                const char *pay_ptr  = llvm_gep_bytes(g, scrut_ptr, st->payload_offset);
                if (bs->type->kind == TY_ARRAY) {
                    bprintf(&g->out, "  call void @llvm.memcpy.p0.p0.i64(ptr %s, ptr %s, i64 %d, i1 false)\n",
                            bind_ptr, pay_ptr, bs->type->size);
                } else {
                    const char *bty = llvm_type_str(g, bs->type);
                    int bal = llvm_type_align(bs->type);
                    LlvmVal bval = llvm_load_ty(g, pay_ptr, bty, bal);
                    llvm_store_val(g, bval, bind_ptr, bal);
                }
            }
            llvm_gen_block(g, arm->body);
            if (!g->block_terminated) {
                any_fallthrough = true;
                llvm_emit_br(g, l_end);
            }
        }
        if (n->els) {
            llvm_emit_label(g, l_els);
            llvm_gen_block(g, n->els);
            if (!g->block_terminated) {
                any_fallthrough = true;
                llvm_emit_br(g, l_end);
            }
        }
        free(arm_lbls);
        if (any_fallthrough) {
            llvm_emit_label(g, l_end);
        }
        return;
    }

    case N_LOOP_INF: {
        int l_hdr = ++g->lbl_id;
        n->label_continue = l_hdr;
        n->label_break    = ++g->lbl_id;
        n->defer_depth    = g->defers.len;
        llvm_emit_label(g, l_hdr);
        llvm_gen_block(g, n->body);
        llvm_emit_br(g, l_hdr);
        bprintf(&g->out, "L_%d:\n", n->label_break);
        g->block_terminated = false;
        return;
    }

    case N_LOOP_COND: {
        int l_hdr  = ++g->lbl_id;
        int l_body = ++g->lbl_id;
        n->label_continue = l_hdr;
        n->label_break    = ++g->lbl_id;
        n->defer_depth    = g->defers.len;
        llvm_emit_label(g, l_hdr);
        LlvmVal cv = llvm_gen_expr(g, n->cond);
        LlvmVal b1 = llvm_to_i1(g, cv);
        bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n", b1.op, l_body, n->label_break);
        g->block_terminated = true;
        llvm_emit_label(g, l_body);
        llvm_gen_block(g, n->body);
        llvm_emit_br(g, l_hdr);
        bprintf(&g->out, "L_%d:\n", n->label_break);
        g->block_terminated = false;
        return;
    }

    case N_LOOP_RANGE: {
        Type *t = n->type;
        const char *ty_s = llvm_type_str(g, t);
        int al = llvm_type_align(t);
        const char *iv_ptr = llvm_sym_ptr(g, n->sym);
        n->label_continue = ++g->lbl_id;
        n->label_break    = ++g->lbl_id;
        n->defer_depth    = g->defers.len;

        LlvmVal start_v = llvm_gen_expr(g, n->lo);
        LlvmVal end_v   = llvm_gen_expr(g, n->hi);

        if (!n->rev && (!n->by || (n->by->is_const && n->by->cval == 1))) {
            llvm_store_val(g, start_v, iv_ptr, al);
            int l_hdr  = ++g->lbl_id;
            int l_body = ++g->lbl_id;
            llvm_emit_label(g, l_hdr);
            LlvmVal cur = llvm_load_ty(g, iv_ptr, ty_s, al);
            const char *cmp = llvm_tmp(g);
            bprintf(&g->out, "  %s = icmp %s %s %s, %s\n",
                    cmp, t->is_signed ? "slt" : "ult", ty_s, cur.op, end_v.op);
            bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n", cmp, l_body, n->label_break);
            g->block_terminated = true;
            llvm_emit_label(g, l_body);
            llvm_gen_block(g, n->body);
            llvm_emit_label(g, n->label_continue);
            LlvmVal cur2 = llvm_load_ty(g, iv_ptr, ty_s, al);
            const char *nxt = llvm_tmp(g);
            bprintf(&g->out, "  %s = add %s %s, 1\n", nxt, ty_s, cur2.op);
            llvm_store_val(g, (LlvmVal){ nxt, ty_s }, iv_ptr, al);
            llvm_emit_br(g, l_hdr);
            bprintf(&g->out, "L_%d:\n", n->label_break);
            g->block_terminated = false;
            return;
        }

        LlvmVal step_v = n->by ? llvm_gen_expr(g, n->by) : (LlvmVal){ "1", ty_s };
        if (n->by && !n->by->is_const) {
            const char *bad_step = llvm_tmp(g);
            bprintf(&g->out, "  %s = icmp %s %s %s, 0\n",
                    bad_step, t->is_signed ? "sle" : "eq", ty_s, step_v.op);
            int l_trap = ++g->lbl_id;
            int l_step_ok = ++g->lbl_id;
            bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n", bad_step, l_trap, l_step_ok);
            g->block_terminated = true;
            llvm_emit_label(g, l_trap);
            const char *msg_sym = NULL;
            int msg_len = 0;
            llvm_loc_str(g, n->by->pos, "range loop step must be > 0", &msg_sym, &msg_len);
            bprintf(&g->out, "  call void @sb_rt_trap(ptr %s, i32 %d)\n", msg_sym, msg_len);
            bprintf(&g->out, "  unreachable\n");
            g->block_terminated = true;
            llvm_emit_label(g, l_step_ok);
        }

        const char *has_iters = llvm_tmp(g);
        bprintf(&g->out, "  %s = icmp %s %s %s, %s\n",
                has_iters, t->is_signed ? "slt" : "ult", ty_s, start_v.op, end_v.op);
        int l_init = ++g->lbl_id;
        int l_body = ++g->lbl_id;
        int l_next = ++g->lbl_id;
        bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n", has_iters, l_init, n->label_break);
        g->block_terminated = true;

        llvm_emit_label(g, l_init);
        LlvmVal s64 = llvm_to_i64(g, start_v, t);
        LlvmVal e64 = llvm_to_i64(g, end_v, t);
        LlvmVal st64 = llvm_to_i64(g, step_v, t);
        const char *diff  = llvm_tmp(g);
        const char *diff1 = llvm_tmp(g);
        const char *quot  = llvm_tmp(g);
        const char *rem0  = llvm_tmp(g);
        bprintf(&g->out, "  %s = sub i64 %s, %s\n", diff, e64.op, s64.op);
        bprintf(&g->out, "  %s = sub i64 %s, 1\n", diff1, diff);
        bprintf(&g->out, "  %s = udiv i64 %s, %s\n", quot, diff1, st64.op);
        bprintf(&g->out, "  %s = add i64 %s, 1\n", rem0, quot);
        const char *rem_ptr = llvm_alloca_ty(g, "i64", 8);
        llvm_store_val(g, (LlvmVal){ rem0, "i64" }, rem_ptr, 8);
        if (n->rev) {
            const char *rcm1 = llvm_tmp(g);
            const char *span = llvm_tmp(g);
            const char *iv64 = llvm_tmp(g);
            bprintf(&g->out, "  %s = sub i64 %s, 1\n", rcm1, rem0);
            bprintf(&g->out, "  %s = mul i64 %s, %s\n", span, rcm1, st64.op);
            bprintf(&g->out, "  %s = add i64 %s, %s\n", iv64, s64.op, span);
            LlvmVal iv0 = llvm_resize_int(g, (LlvmVal){ iv64, "i64" }, 64, t->size * 8, false);
            llvm_store_val(g, iv0, iv_ptr, al);
        } else {
            llvm_store_val(g, start_v, iv_ptr, al);
        }
        llvm_emit_br(g, l_body);

        llvm_emit_label(g, l_body);
        llvm_gen_block(g, n->body);
        llvm_emit_label(g, n->label_continue);
        LlvmVal rem_cur = llvm_load_ty(g, rem_ptr, "i64", 8);
        const char *rem_nxt = llvm_tmp(g);
        bprintf(&g->out, "  %s = sub i64 %s, 1\n", rem_nxt, rem_cur.op);
        llvm_store_val(g, (LlvmVal){ rem_nxt, "i64" }, rem_ptr, 8);
        const char *done = llvm_tmp(g);
        bprintf(&g->out, "  %s = icmp eq i64 %s, 0\n", done, rem_nxt);
        bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n", done, n->label_break, l_next);
        g->block_terminated = true;

        llvm_emit_label(g, l_next);
        LlvmVal iv_cur = llvm_load_ty(g, iv_ptr, ty_s, al);
        const char *iv_nxt = llvm_tmp(g);
        bprintf(&g->out, "  %s = %s %s %s, %s\n",
                iv_nxt, n->rev ? "sub" : "add", ty_s, iv_cur.op, step_v.op);
        llvm_store_val(g, (LlvmVal){ iv_nxt, ty_s }, iv_ptr, al);
        llvm_emit_br(g, l_body);

        bprintf(&g->out, "L_%d:\n", n->label_break);
        g->block_terminated = false;
        return;
    }

    case N_LOOP_ITER: {
        Type *elem_t = n->type;
        const char *elem_ty = llvm_type_str(g, elem_t);
        int eal = llvm_type_align(elem_t);
        n->label_continue = ++g->lbl_id;
        n->label_break    = ++g->lbl_id;
        n->defer_depth    = g->defers.len;

        LlvmVal sl = llvm_gen_collection_slice(g, n->lhs);
        const char *bp = llvm_tmp(g);
        bprintf(&g->out, "  %s = extractvalue %%sb.slice %s, 0\n", bp, sl.op);
        const char *blen = llvm_tmp(g);
        bprintf(&g->out, "  %s = extractvalue %%sb.slice %s, 1\n", blen, sl.op);

        const char *idx_ptr = n->sym2 ? llvm_sym_ptr(g, n->sym) : llvm_alloca_ty(g, "i64", 8);
        const char *val_ptr = llvm_sym_ptr(g, n->sym2 ? n->sym2 : n->sym);
        llvm_store_val(g, (LlvmVal){ "0", "i64" }, idx_ptr, 8);

        int l_hdr  = ++g->lbl_id;
        int l_body = ++g->lbl_id;
        llvm_emit_label(g, l_hdr);
        LlvmVal cur_i = llvm_load_ty(g, idx_ptr, "i64", 8);
        const char *cond = llvm_tmp(g);
        bprintf(&g->out, "  %s = icmp slt i64 %s, %s\n", cond, cur_i.op, blen);
        bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n", cond, l_body, n->label_break);
        g->block_terminated = true;

        llvm_emit_label(g, l_body);
        const char *ep = llvm_tmp(g);
        bprintf(&g->out, "  %s = getelementptr inbounds %s, ptr %s, i64 %s\n", ep, elem_ty, bp, cur_i.op);
        if (elem_t->kind == TY_ARRAY) {
            bprintf(&g->out, "  call void @llvm.memcpy.p0.p0.i64(ptr %s, ptr %s, i64 %d, i1 false)\n",
                    val_ptr, ep, elem_t->size);
        } else {
            LlvmVal ev = llvm_load_ty(g, ep, elem_ty, eal);
            llvm_store_val(g, ev, val_ptr, eal);
        }
        llvm_gen_block(g, n->body);

        llvm_emit_label(g, n->label_continue);
        LlvmVal ci2 = llvm_load_ty(g, idx_ptr, "i64", 8);
        const char *nxt_i = llvm_tmp(g);
        bprintf(&g->out, "  %s = add i64 %s, 1\n", nxt_i, ci2.op);
        llvm_store_val(g, (LlvmVal){ nxt_i, "i64" }, idx_ptr, 8);
        llvm_emit_br(g, l_hdr);

        bprintf(&g->out, "L_%d:\n", n->label_break);
        g->block_terminated = false;
        return;
    }

    case N_LOOP_C: {
        int l_hdr  = ++g->lbl_id;
        int l_body = ++g->lbl_id;
        n->label_continue = ++g->lbl_id;
        n->label_break    = ++g->lbl_id;
        llvm_gen_stmt(g, n->init);
        n->defer_depth = g->defers.len;
        llvm_emit_label(g, l_hdr);
        LlvmVal cv = llvm_gen_expr(g, n->cond);
        LlvmVal b1 = llvm_to_i1(g, cv);
        bprintf(&g->out, "  br i1 %s, label %%L_%d, label %%L_%d\n", b1.op, l_body, n->label_break);
        g->block_terminated = true;
        llvm_emit_label(g, l_body);
        llvm_gen_block(g, n->body);
        llvm_emit_label(g, n->label_continue);
        llvm_gen_assign(g, n->step);
        llvm_emit_br(g, l_hdr);
        bprintf(&g->out, "L_%d:\n", n->label_break);
        g->block_terminated = false;
        return;
    }

    case N_BLOCK:
        llvm_gen_block(g, n);
        return;

    default:
        return;
    }
}

static void llvm_gen_block(LlvmGen *g, Node *blk) {
    int defer_mark = g->defers.len;
    for (int i = 0; i < blk->list.len; i++) {
        llvm_gen_stmt(g, blk->list.items[i]);
    }
    if (g->defers.len > defer_mark) {
        if (!g->block_terminated) {
            llvm_emit_defers_from(g, defer_mark);
        }
        g->defers.len = defer_mark;
    }
}

// ---------------------------------------------------------------------------
// Module generation & LLVM Clang driver
// ---------------------------------------------------------------------------

static void generate_llvm_ir_unit(Program *prog, NodeVec all_decls, bool release, Buf *final_out) {
    LlvmGen g = {0};
    g.prog = prog;
    g.release = release;

    emit_llvm_runtime_preamble(&g.hdr);

    // 1. Struct and tagged union enum type definitions (in topological layout order).
    for (int i = 0; i < prog->type_decls.len; i++) {
        Node *d = prog->type_decls.items[i];
        if (d->kind == N_STRUCT) {
            Type *st = d->type;
            bprintf(&g.hdr, "%s = type { ", llvm_mangled(&g, "%sb.st.", st->name));
            if (st->fields.len == 0) {
                bprintf(&g.hdr, "i8");
            } else {
                for (int j = 0; j < st->fields.len; j++) {
                    if (j > 0) bprintf(&g.hdr, ", ");
                    bprintf(&g.hdr, "%s", llvm_type_str(&g, st->fields.items[j].type));
                }
            }
            bprintf(&g.hdr, " }\n");
        } else if (d->kind == N_ENUM && d->type && d->type->is_tagged) {
            Type *et = d->type;
            const char *ename = llvm_mangled(&g, "%sb.en.", et->name);
            if (et->align == 8) {
                int qwords = (et->size - 8) / 8;
                if (qwords < 1) qwords = 1;
                bprintf(&g.hdr, "%s = type { i32, [4 x i8], [%d x i64] }\n", ename, qwords);
            } else {
                int dwords = (et->size - 4) / 4;
                if (dwords < 1) dwords = 1;
                bprintf(&g.hdr, "%s = type { i32, [%d x i32] }\n", ename, dwords);
            }
        }
    }
    bprintf(&g.hdr, "\n");

    // 2. Extern DLL function pointer globals (resolved at startup in @main).
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
                bprintf(&g.hdr, "%s = internal global ptr null, align 8\n",
                        llvm_mangled(&g, "@sb_ext_", d->name));
            }
        }
    }

    // 3. Global variables.
    for (int i = 0; i < prog->globals.len; i++) {
        Symbol *gs = prog->globals.items[i];
        bprintf(&g.hdr, "%s = internal global %s ",
                llvm_mangled(&g, "@sb_g_", gs->name), llvm_type_str(&g, gs->type));
        emit_llvm_const_init(&g, &g.hdr, gs->decl->rhs, gs->type);
        bprintf(&g.hdr, ", align %d\n", llvm_type_align(gs->type));
    }
    bprintf(&g.hdr, "\n");

    // 4. User function definitions.
    for (int i = 0; i < prog->fns.len; i++) {
        Node *fn = prog->fns.items[i];
        g.cur_ret_type = fn->type;
        g.defers.len = 0;
        g.tmp_id = 0;
        g.block_terminated = false;
        g.fn_entry.len = 0;
        g.out.len = 0;

        int need_slots = fn->num_slots + 2;
        if (need_slots > g.slot_cap) {
            g.slot_cap = need_slots;
            g.slot_emitted = xrealloc(g.slot_emitted, (size_t)g.slot_cap * sizeof(bool));
        }
        memset(g.slot_emitted, 0, (size_t)need_slots * sizeof(bool));

        bprintf(&g.mod_fns, "define internal %s %s(",
                llvm_type_str(&g, fn->type), llvm_mangled(&g, "@sb_fn_", fn->name));
        for (int j = 0; j < fn->list.len; j++) {
            if (j > 0) bprintf(&g.mod_fns, ", ");
            Node *p = fn->list.items[j];
            const char *pty = (p->type->kind == TY_ARRAY) ? "ptr" : llvm_type_str(&g, p->type);
            bprintf(&g.mod_fns, "%s %%p%d", pty, j);
            const char *sp = llvm_sym_ptr(&g, p->sym);
            int pal = (p->type->kind == TY_ARRAY) ? 8 : llvm_type_align(p->type);
            bprintf(&g.out, "  store %s %%p%d, ptr %s, align %d\n", pty, j, sp, pal);
        }
        bprintf(&g.mod_fns, ") #2 {\nentry:\n");

        llvm_gen_block(&g, fn->body);
        if (!g.block_terminated) {
            if (fn->type == &ty_void) bprintf(&g.out, "  ret void\n");
            else                      bprintf(&g.out, "  unreachable\n");
        }

        buf_bytes(&g.mod_fns, g.fn_entry.data, g.fn_entry.len);
        buf_bytes(&g.mod_fns, g.out.data, g.out.len);
        bprintf(&g.mod_fns, "}\n\n");
    }

    // 5. Program entry point `@main`: init stdout handle, resolve extern DLLs, call `@sb_fn_main`.
    bprintf(&g.mod_fns,
        "define i32 @main() #2 {\n"
        "entry:\n"
        "  %%hstd = call ptr @GetStdHandle(i32 -11)\n"
        "  store ptr %%hstd, ptr @sb_stdout_handle, align 8\n");
    for (int i = 0; i < extern_fns.len; i++) {
        Node *d = extern_fns.items[i];
        const char *dll_sym = llvm_intern_str(&g, d->dll_name, (int)strlen(d->dll_name));
        const char *fn_sym  = llvm_intern_str(&g, d->name, (int)strlen(d->name));
        const char *ext_ptr = llvm_mangled(&g, "@sb_ext_", d->name);
        bprintf(&g.mod_fns,
            "  %%hdll_%d = call ptr @LoadLibraryA(ptr %s)\n"
            "  %%fp_%d = call ptr @GetProcAddress(ptr %%hdll_%d, ptr %s)\n"
            "  store ptr %%fp_%d, ptr %s, align 8\n",
            i, dll_sym, i, i, fn_sym, i, ext_ptr);
    }
    bprintf(&g.mod_fns,
        "  call void @sb_fn_main()\n"
        "  ret i32 0\n"
        "}\n\n"
        "attributes #0 = { alwaysinline nounwind }\n"
        "attributes #1 = { noreturn cold noinline nounwind }\n"
        "attributes #2 = { nounwind }\n");

    buf_bytes(final_out, g.hdr.data, g.hdr.len);
    buf_bytes(final_out, g.mod_fns.data, g.mod_fns.len);

    free(g.hdr.data);
    free(g.mod_fns.data);
    free(g.fn_entry.data);
    free(g.out.data);
    free(g.slot_emitted);
    free(g.defers.items);
    free(g.str_lits.items);
    for (int i = 0; i < g.pool.len; i++) free(g.pool.items[i]);
    free(g.pool.items);
    free(extern_fns.items);
}

bool emit_llvm_ir(Program *prog, NodeVec all_decls, const char *ll_path, bool release) {
    Buf ll_src = {0};
    generate_llvm_ir_unit(prog, all_decls, release, &ll_src);
    FILE *fp = fopen(ll_path, "wb");
    if (!fp) {
        free(ll_src.data);
        return false;
    }
    fwrite(ll_src.data, 1, ll_src.len, fp);
    fclose(fp);
    free(ll_src.data);
    return true;
}

static const char *find_llvm_clang(void) {
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
            return clang_candidates[i];
        }
    }
#endif
    return "clang";
}

bool compile_opt_backend(Program *prog, NodeVec all_decls, const char *exe_path, bool release) {
    Buf ll_src = {0};
    generate_llvm_ir_unit(prog, all_decls, release, &ll_src);

    char tmp_ll[1024];
    snprintf(tmp_ll, sizeof(tmp_ll), "%s.__sb_opt.ll", exe_path);
    FILE *fp = fopen(tmp_ll, "wb");
    if (!fp) {
        free(ll_src.data);
        die("cannot create temporary LLVM IR file '%s'", tmp_ll);
    }
    fwrite(ll_src.data, 1, ll_src.len, fp);
    fclose(fp);
    free(ll_src.data);

    const char *clang = find_llvm_clang();
    char cmd[4096];
#ifdef _WIN32
    // In cmd.exe, wrap the entire command in outer quotes when executable path contains spaces/quotes.
    snprintf(cmd, sizeof(cmd),
             "\"\"%s\" -O3 -march=native -Wno-override-module \"%s\" -o \"%s\" -lkernel32\"",
             clang, tmp_ll, exe_path);
#else
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -O3 -march=native -Wno-override-module \"%s\" -o \"%s\"",
             clang, tmp_ll, exe_path);
#endif
    int rc = system(cmd);
    remove(tmp_ll);
    return rc == 0;
}
