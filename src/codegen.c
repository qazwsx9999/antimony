// codegen.c - AST -> Windows x64 machine code.
//
// Representation convention:
//   Every scalar value lives in a 64-bit register or 8-byte stack/global slot,
//   normalized to its type (sign-extended for i8/i16/i32/i64, zero-extended
//   for u8/u16/u32/u64/bool). `str` is passed in (RAX = ptr, RDX = len).
#include "sb.h"

typedef struct {
    bool release;
    Buf strings;
    Buf data;
    uint32_t stdout_off;      // .data: cached stdout HANDLE

    // Built-in runtime helper labels in .text.
    int rt_write;             // (rcx=ptr, rdx=len) -> writes to stdout
    int rt_print_nl;          // () -> writes '\n' to stdout
    int rt_print_i64;         // (rcx=val)
    int rt_print_i64_raw;     // (rcx=val) without trailing '\n'
    int rt_print_u64;         // (rcx=val)
    int rt_print_u64_raw;     // (rcx=val) without trailing '\n'
    int rt_print_f64;         // (rcx=f64 bits)
    int rt_print_f64_raw;     // (rcx=f64 bits) without trailing '\n'
    int rt_print_bool;        // (rcx=0/1)
    int rt_print_bool_raw;    // (rcx=0/1) without trailing '\n'
    int rt_print_str;         // (rcx=ptr, rdx=len)
    int rt_str_eq;            // (rcx=ptr1, rdx=len1, r8=ptr2, r9=len2) -> rax=0/1
    int rt_trap;              // (rcx=msg_ptr, rdx=msg_len) -> stderr + ExitProcess(101)

    int cur_epilogue;
    Type *cur_ret_type;       // return type of current function
    int cur_ret_ptr_slot;     // slot holding hidden return buffer pointer (when cur_ret_type is TY_STRUCT)
    int temp_depth;           // expression temporary spill depth (1-based)
    int max_temp_depth;
    int scratch_depth;        // volatile scratch register depth (0..4 -> R8..R11)
    int base_slots;           // local variable slots in current function
    int max_out_stack_args;   // max args > 4 across calls in current function
    VEC(Node *) defers;       // active `defer` statement bodies in current function
    Node *cur_fn;             // outermost function currently being compiled
    Node *inline_stack[4];    // active inlined callee stack
    int inline_depth;
    Node *last_top_stmt;      // last top-level statement of current function/inline body
    Node *cur_top_block;      // top-level block of current function or inlined callee
    Node *range_loops[8];     // active enclosing N_LOOP_RANGE statements for bounds-check elimination
    int num_range_loops;
    int free_callee_regs[7];  // unused callee-saved GPRs available for inlined params/locals / loop bounds
    int num_free_callee;
    int free_xmm_regs[8];     // unused callee-saved XMMs available for inlined f64 params/locals
    int num_free_xmm;
    struct {
        uint64_t bits;
        uint32_t off;
    } fconst_cache[16];
    int fconst_cache_len;
} CG;

static uint32_t add_string(CG *cg, const char *s, int len) {
    uint32_t off = (uint32_t)cg->strings.len;
    buf_bytes(&cg->strings, s, (size_t)len);
    buf_u8(&cg->strings, 0);
    return off;
}

static uint32_t add_f64_const(CG *cg, uint64_t bits) {
    for (int i = 0; i < cg->fconst_cache_len; i++) {
        if (cg->fconst_cache[i].bits == bits) return cg->fconst_cache[i].off;
    }
    buf_align(&cg->strings, 8);
    uint32_t off = (uint32_t)cg->strings.len;
    buf_u64(&cg->strings, bits);
    if (cg->fconst_cache_len < 16) {
        cg->fconst_cache[cg->fconst_cache_len].bits = bits;
        cg->fconst_cache[cg->fconst_cache_len].off  = off;
        cg->fconst_cache_len++;
    }
    return off;
}

static int32_t slot_disp(int slot) {
    return -8 * slot;
}

// Emits a runtime trap with a pre-formatted "file:line:col: runtime panic: reason\n".
static void emit_trap_at(CG *cg, Pos pos, const char *reason) {
    int ok = x_label();
    char msg[512];
    int len = snprintf(msg, sizeof(msg), "%s:%d:%d: runtime panic: %s\n",
                       file_name_of(pos), pos.line, pos.col, reason);
    if (len < 0) len = 0;
    if (len >= (int)sizeof(msg)) len = (int)sizeof(msg) - 1;
    uint32_t off = add_string(cg, msg, len);
    x_lea_rip(RCX, SEC_RDATA, off);
    x_mov_ri(RDX, (uint64_t)len);
    x_call(cg->rt_trap);
    x_bind(ok);
}

static void emit_trap_if(CG *cg, int cc, Pos pos, const char *reason) {
    int ok = x_label();
    x_jcc(cc ^ 1, ok); // jump over trap when condition is NOT met
    emit_trap_at(cg, pos, reason);
    x_bind(ok);
}

// Truncates `reg` to `t` (sign-extending signed types, zero-extending unsigned).
static void normalize_reg(int reg, Type *t) {
    if (!t || (t->kind != TY_INT && t->kind != TY_ENUM)) return;
    if (t->is_signed) {
        if (t->size == 1)      x_movsx8(reg, reg);
        else if (t->size == 2) x_movsx16(reg, reg);
        else if (t->size == 4) x_movsxd(reg, reg);
    } else {
        if (t->size == 1)      x_movzx8(reg, reg);
        else if (t->size == 2) x_movzx16(reg, reg);
        else if (t->size == 4) x_mov_rr(0, reg, reg); // 32-bit mov zero-extends
    }
}

static void normalize_rax(Type *t) {
    normalize_reg(RAX, t);
}

// Checks that the 64-bit value in RAX (with source signedness `src_signed`)
// fits in sub-64-bit integer type `dst`; traps in debug if not, then normalizes.
static void check_and_normalize_sub64(CG *cg, Type *dst, bool src_signed, Pos pos, const char *what) {
    if (!cg->release) {
        if (dst->is_signed) {
            int64_t lo = -(1LL << (dst->size * 8 - 1));
            int64_t hi =  (1LL << (dst->size * 8 - 1)) - 1;
            if (src_signed) {
                x_alu_ri(ALU_CMP, 1, RAX, (int32_t)lo);
                emit_trap_if(cg, CC_L, pos, what);
                x_alu_ri(ALU_CMP, 1, RAX, (int32_t)hi);
                emit_trap_if(cg, CC_G, pos, what);
            } else {
                x_mov_ri(RCX, (uint64_t)hi);
                x_alu_rr(ALU_CMP, 1, RAX, RCX);
                emit_trap_if(cg, CC_A, pos, what);
            }
        } else {
            if (src_signed) {
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_S, pos, what);
            }
            if (dst->size < 8) {
                uint64_t hi = (dst->size == 4) ? UINT32_MAX : ((1ULL << (dst->size * 8)) - 1);
                x_mov_ri(RCX, hi);
                x_alu_rr(ALU_CMP, 1, RAX, RCX);
                emit_trap_if(cg, CC_A, pos, what);
            }
        }
    }
    normalize_rax(dst);
}

static uint64_t fval_bits(double v, Type *t);

static void load_var(Symbol *s, int reg) {
    if (s->is_const_alias) {
        if (s->type->kind == TY_FLOAT) {
            x_mov_ri(reg, fval_bits(s->fval, s->type));
        } else {
            x_mov_ri(reg, (uint64_t)(int64_t)s->cval);
            normalize_reg(reg, s->type);
        }
        return;
    }
    if (s->kind == SYM_FN) {
        x_lea_label(reg, s->label);
        return;
    }
    if (s->type->kind == TY_SLICE || s->type == &ty_str) {
        // Fat pointer {ptr, len} in (RAX, RDX).
        if (s->hw_reg >= 0) x_mov_rr(1, RAX, s->hw_reg);
        else                x_load(RAX, RBP, slot_disp(s->slot));
        x_load(RDX, RBP, slot_disp(s->slot) + 8);
        return;
    }
    if (s->xmm_reg >= 0) {
        x_movq_gpr_xmm(reg, s->xmm_reg);
        return;
    }
    if (s->hw_reg >= 0) {
        x_mov_rr(1, reg, s->hw_reg);
        return;
    }
    if (s->type->kind == TY_ARRAY || is_aggregate_type(s->type)) {
        // Evaluating a fixed-size array or aggregate (struct / tagged enum) produces its base pointer in `reg`.
        if (s->is_global) {
            x_lea_rip(reg, SEC_DATA, s->data_off);
        } else if (s->kind == SYM_PARAM) {
            // Fixed-size array / struct parameter is passed by pointer in its slot.
            x_load(reg, RBP, slot_disp(s->slot));
        } else {
            x_lea(reg, RBP, slot_disp(s->slot));
        }
        return;
    }
    if (s->is_global) x_load_rip(reg, SEC_DATA, s->data_off);
    else              x_load(reg, RBP, slot_disp(s->slot));
}

static void store_var(Symbol *s, int reg) {
    if (s->type->kind == TY_SLICE || s->type == &ty_str) {
        if (s->hw_reg >= 0) x_mov_rr(1, s->hw_reg, RAX);
        x_store(RBP, slot_disp(s->slot), RAX);
        x_store(RBP, slot_disp(s->slot) + 8, RDX);
        return;
    }
    if (s->xmm_reg >= 0) {
        x_movq_xmm_gpr(s->xmm_reg, reg);
        return;
    }
    if (s->hw_reg >= 0) {
        x_mov_rr(1, s->hw_reg, reg);
        return;
    }
    if (s->is_global) x_store_rip(SEC_DATA, s->data_off, reg);
    else              x_store(RBP, slot_disp(s->slot), reg);
}

static int push_temp(CG *cg) {
    cg->temp_depth++;
    if (cg->temp_depth > cg->max_temp_depth) cg->max_temp_depth = cg->temp_depth;
    int slot = cg->base_slots + cg->temp_depth;
    x_store(RBP, slot_disp(slot), RAX);
    return slot;
}

static void pop_temp(CG *cg, int slot, int reg) {
    x_load(reg, RBP, slot_disp(slot));
    cg->temp_depth--;
}

static uint64_t fval_bits(double v, Type *t) {
    if (t == &ty_f32) {
        float f = (float)v;
        uint32_t u = 0;
        memcpy(&u, &f, 4);
        return (uint64_t)u;
    }
    uint64_t u = 0;
    memcpy(&u, &v, 8);
    return u;
}

// Returns true if `n` is a compile-time constant integer/enum/bool/null whose
// 64-bit register representation equals sign-extended `int32_t *out`.
static bool fits_imm32(Node *n, int32_t *out) {
    if (!n) return false;
    if (n->kind == N_NULL) {
        *out = 0;
        return true;
    }
    bool is_c = n->is_const || (n->kind == N_IDENT && n->sym && n->sym->is_const_alias);
    if (is_c && (n->type->kind == TY_INT || n->type->kind == TY_ENUM || n->type == &ty_bool)) {
        int64_t v = (int64_t)(n->is_const ? n->cval : n->sym->cval);
        if (n->type->is_signed) {
            if (v >= INT32_MIN && v <= INT32_MAX) {
                *out = (int32_t)v;
                return true;
            }
        } else {
            if ((uint64_t)v <= (uint64_t)INT32_MAX) {
                *out = (int32_t)v;
                return true;
            }
        }
    }
    return false;
}

// Returns true if `n` can be loaded directly into an arbitrary GPR via
// `gen_simple_into_reg` without clobbering any other register or having side effects.
static bool is_simple_expr(Node *n) {
    if (!n) return false;
    if (n->is_const && (n->type->kind == TY_INT || n->type->kind == TY_ENUM ||
                        n->type->kind == TY_FLOAT || n->type == &ty_bool))
        return true;
    switch (n->kind) {
    case N_INT:
    case N_BOOL:
    case N_NULL:
    case N_FLOAT:
        return true;
    case N_IDENT: {
        if (n->sym->kind == SYM_FN) return true;
        Type *t = n->sym->type;
        return !n->implicit_slice &&
               (t->kind == TY_INT || t->kind == TY_ENUM || t->kind == TY_BOOL ||
                t->kind == TY_FLOAT || t->kind == TY_PTR || t->kind == TY_FN);
    }
    default:
        return false;
    }
}

static void gen_simple_into_reg(Node *n, int reg) {
    if (n->is_const && n->type->kind == TY_FLOAT) {
        x_mov_ri(reg, fval_bits(n->fval, n->type));
        return;
    }
    if (n->is_const && (n->type->kind == TY_INT || n->type->kind == TY_ENUM || n->type == &ty_bool)) {
        x_mov_ri(reg, (uint64_t)(int64_t)n->cval);
        normalize_reg(reg, n->type);
        return;
    }
    switch (n->kind) {
    case N_INT:
    case N_BOOL:
    case N_NULL:
        x_mov_ri(reg, (uint64_t)(int64_t)n->cval);
        return;
    case N_FLOAT:
        x_mov_ri(reg, fval_bits(n->fval, n->type));
        return;
    case N_IDENT:
        load_var(n->sym, reg);
        return;
    default:
        die("internal error: non-simple expr in gen_simple_into_reg");
    }
}

// Returns true if evaluating `n` in `release` mode is guaranteed not to issue
// a function/runtime call (and therefore preserves volatile scratch registers R8..R11).
static bool is_call_free_expr(Node *n, bool release) {
    if (!n || !release) return false;
    if (n->is_const) return true;
    switch (n->kind) {
    case N_INT:
    case N_BOOL:
    case N_NULL:
    case N_FLOAT:
        return true;
    case N_IDENT: {
        if (n->sym->kind == SYM_FN) return true;
        Type *t = n->sym->type;
        return !n->implicit_slice &&
               (t->kind == TY_INT || t->kind == TY_ENUM || t->kind == TY_BOOL ||
                t->kind == TY_FLOAT || t->kind == TY_PTR || t->kind == TY_FN);
    }
    case N_UNARY:
        return is_call_free_expr(n->lhs, release);
    case N_CONV:
        return n->type != &ty_str && is_call_free_expr(n->lhs, release);
    case N_DEREF:
    case N_FIELD:
        return !is_aggregate_type(n->type) && n->type->kind != TY_ARRAY &&
               n->type->kind != TY_SLICE && n->type != &ty_str &&
               is_call_free_expr(n->lhs, release);
    case N_INDEX: {
        if (is_aggregate_type(n->type) || n->type->kind == TY_ARRAY ||
            n->type->kind == TY_SLICE || n->type == &ty_str) {
            return false;
        }
        if (n->lhs->kind == N_IDENT &&
            (n->lhs->type->kind == TY_SLICE || n->lhs->type == &ty_str ||
             (n->lhs->type->kind == TY_ARRAY && !n->lhs->implicit_slice))) {
            return is_simple_expr(n->rhs) || is_call_free_expr(n->rhs, release);
        }
        return n->lhs->type->kind == TY_PTR &&
               is_call_free_expr(n->lhs, release) && is_call_free_expr(n->rhs, release);
    }
    case N_BINARY: {
        if (n->lhs->type == &ty_str) return false;
        if (n->op == T_SLASH || n->op == T_PERCENT) {
            int32_t imm = 0;
            if (!(fits_imm32(n->rhs, &imm) && imm > 0)) return false;
        }
        return is_call_free_expr(n->lhs, release) && is_call_free_expr(n->rhs, release);
    }
    default:
        return false;
    }
}

static bool node_reads_sym(Node *n, Symbol *s) {
    if (!n || !s) return false;
    if (n->kind == N_IDENT && n->sym == s) return true;
    if (n->kind == N_IDENT && s->alias_of && n->sym && n->sym->alias_of == s->alias_of) return true;
    if (node_reads_sym(n->lhs, s) || node_reads_sym(n->rhs, s) ||
        node_reads_sym(n->cond, s) || node_reads_sym(n->body, s) ||
        node_reads_sym(n->els, s) || node_reads_sym(n->lo, s) ||
        node_reads_sym(n->hi, s) || node_reads_sym(n->by, s) ||
        node_reads_sym(n->init, s) || node_reads_sym(n->step, s)) {
        return true;
    }
    for (int i = 0; i < n->list.len; i++) {
        if (node_reads_sym(n->list.items[i], s)) return true;
    }
    return false;
}

static bool node_reads_xmm(Node *n, int xmm) {
    if (!n || xmm < 0) return false;
    if (n->kind == N_IDENT && !n->sym->is_const_alias && n->sym->xmm_reg == xmm) return true;
    if (node_reads_xmm(n->lhs, xmm) || node_reads_xmm(n->rhs, xmm) ||
        node_reads_xmm(n->cond, xmm) || node_reads_xmm(n->body, xmm) ||
        node_reads_xmm(n->els, xmm) || node_reads_xmm(n->lo, xmm) ||
        node_reads_xmm(n->hi, xmm) || node_reads_xmm(n->by, xmm) ||
        node_reads_xmm(n->init, xmm) || node_reads_xmm(n->step, xmm)) {
        return true;
    }
    for (int i = 0; i < n->list.len; i++) {
        if (node_reads_xmm(n->list.items[i], xmm)) return true;
    }
    return false;
}

static bool is_nonneg_expr(Node *n) {
    if (!n) return false;
    if (n->is_const) {
        return (n->type && (n->type->kind == TY_INT || n->type == &ty_bool)) ? (n->cval >= 0) : false;
    }
    switch (n->kind) {
    case N_INT:
    case N_BOOL:
        return n->cval >= 0;
    case N_IDENT:
        if (!n->sym) return false;
        if (n->sym->is_const_alias) return n->sym->cval >= 0;
        return n->sym->known_nonneg ||
               (n->type && n->type->kind == TY_INT && !n->type->is_signed);
    case N_CALL:
        return n->builtin == B_LEN || n->builtin == B_SIZEOF || n->builtin == B_ALIGNOF;
    case N_CONV:
        if (!n->type || n->type->kind != TY_INT) return false;
        if (!n->type->is_signed) return true;
        if (n->lhs->type && n->lhs->type->kind == TY_INT) {
            if (!n->lhs->type->is_signed && n->type->size > n->lhs->type->size) return true;
            if (n->lhs->type->is_signed && n->type->size >= n->lhs->type->size && is_nonneg_expr(n->lhs)) return true;
        }
        return false;
    case N_BINARY:
        if (n->op == T_AMP) {
            return is_nonneg_expr(n->lhs) || is_nonneg_expr(n->rhs);
        }
        if (n->op == T_SHR) {
            return is_nonneg_expr(n->lhs);
        }
        if (n->op == T_SLASH || n->op == T_PERCENT) {
            return is_nonneg_expr(n->lhs) && is_nonneg_expr(n->rhs);
        }
        return false;
    default:
        return false;
    }
}

// Multiplies `reg` by `size`, using fast shifts for powers of two (1, 2, 4, 8, 16, 32, 64).
static void scale_reg_by_size(int reg, int size, int scratch) {
    if (size <= 1) return;
    if (size == 2)  { x_shift_imm(4, 1, reg, 1); return; }
    if (size == 4)  { x_shift_imm(4, 1, reg, 2); return; }
    if (size == 8)  { x_shift_imm(4, 1, reg, 3); return; }
    if (size == 16) { x_shift_imm(4, 1, reg, 4); return; }
    if (size == 32) { x_shift_imm(4, 1, reg, 5); return; }
    if (size == 64) { x_shift_imm(4, 1, reg, 6); return; }
    x_mov_ri(scratch, (uint64_t)size);
    x_imul_rr(1, reg, scratch);
}

// Loads float value (f32 or f64) from GPR `gpr` into `xmm` as a 64-bit double.
static void gpr_to_xmm_f64(int xmm, int gpr, Type *t) {
    x_movq_xmm_gpr(xmm, gpr);
    if (t == &ty_f32) {
        x_cvtss2sd(xmm, xmm);
    }
}

// Converts `xmm` (64-bit double) back into `gpr` in the representation of `t` (f32 or f64).
static void xmm_f64_to_gpr(int gpr, int xmm, Type *t) {
    if (t == &ty_f32) {
        x_cvtsd2ss(xmm, xmm);
    }
    x_movq_gpr_xmm(gpr, xmm);
    if (t == &ty_f32) {
        x_mov_rr(0, gpr, gpr); // zero-extend 32-bit float bits
    }
}

static bool is_direct_xmm_const(Node *n, uint64_t *bits_out) {
    if (!n || !n->type || n->type->kind != TY_FLOAT) return false;
    if (n->is_const || n->kind == N_FLOAT) {
        double d = (n->type == &ty_f32) ? (double)(float)n->fval : n->fval;
        *bits_out = fval_bits(d, &ty_f64);
        return true;
    }
    if (n->kind == N_IDENT && !n->implicit_slice && n->sym && n->sym->is_const_alias) {
        double d = (n->type == &ty_f32) ? (double)(float)n->sym->fval : n->sym->fval;
        *bits_out = fval_bits(d, &ty_f64);
        return true;
    }
    return false;
}

static bool is_direct_xmm_reg(Node *n) {
    return n && n->kind == N_IDENT && !n->implicit_slice && n->sym &&
           !n->sym->is_const_alias && n->sym->xmm_reg >= 0 && n->type == &ty_f64;
}

// Returns true if floating-point expression `n` can be evaluated directly in volatile
// XMM registers `XMM[xmm .. 5]` without function calls or stack spills.
static bool can_eval_xmm(Node *n, int xmm) {
    if (!n || xmm > 5 || !n->type || n->type->kind != TY_FLOAT) return false;
    if (is_simple_expr(n)) return true;
    if (n->kind == N_CONV && n->lhs->type->kind == TY_INT && n->lhs->type->is_signed &&
        is_simple_expr(n->lhs)) {
        return true;
    }
    if (n->kind == N_UNARY && n->op == T_MINUS && can_eval_xmm(n->lhs, xmm) && xmm < 5) {
        return true;
    }
    if (n->kind == N_BINARY &&
        (n->op == T_PLUS || n->op == T_MINUS || n->op == T_STAR || n->op == T_SLASH)) {
        uint64_t dummy = 0;
        if (is_direct_xmm_reg(n->rhs) || is_direct_xmm_const(n->rhs, &dummy)) {
            return can_eval_xmm(n->lhs, xmm);
        }
        return xmm < 5 && can_eval_xmm(n->lhs, xmm) && can_eval_xmm(n->rhs, xmm + 1);
    }
    return false;
}

// Returns true if `n` evaluates left-linearly using only `dst_xmm` (without touching `dst_xmm + 1`)
// and never reads `dst_xmm`.
static bool can_eval_xmm_in_place(Node *n, int dst_xmm) {
    if (!n || dst_xmm < 0 || !n->type || n->type != &ty_f64) return false;
    if (node_reads_xmm(n, dst_xmm)) return false;
    if (is_simple_expr(n)) return true;
    if (n->kind == N_CONV && n->lhs->type->kind == TY_INT && n->lhs->type->is_signed &&
        is_simple_expr(n->lhs)) {
        return true;
    }
    if (n->kind == N_BINARY &&
        (n->op == T_PLUS || n->op == T_MINUS || n->op == T_STAR || n->op == T_SLASH)) {
        uint64_t dummy = 0;
        if (is_direct_xmm_reg(n->rhs) || is_direct_xmm_const(n->rhs, &dummy)) {
            return can_eval_xmm_in_place(n->lhs, dst_xmm);
        }
    }
    return false;
}

// Evaluates `n` directly into `XMM[xmm]` as a 64-bit IEEE-754 double (preserving f32 rounding when `n->type == &ty_f32`).
static void gen_float_xmm(CG *cg, Node *n, int xmm) {
    uint64_t fbits = 0;
    if (is_direct_xmm_const(n, &fbits)) {
        if (fbits == 0) {
            x_xorpd(xmm, xmm);
        } else {
            x_load_sd_rip(xmm, SEC_RDATA, add_f64_const(cg, fbits));
        }
        return;
    }
    if (is_simple_expr(n)) {
        if (n->kind == N_IDENT && !n->implicit_slice) {
            if (n->sym->xmm_reg >= 0 && n->type == &ty_f64) {
                x_movsd_rr(xmm, n->sym->xmm_reg);
                return;
            }
            if (n->sym->hw_reg >= 0 && n->type == &ty_f64) {
                x_movq_xmm_gpr(xmm, n->sym->hw_reg);
                return;
            }
            if (!n->sym->is_global && n->sym->hw_reg < 0 && n->type == &ty_f64) {
                x_load_sd(xmm, RBP, slot_disp(n->sym->slot));
                return;
            }
        }
        gen_simple_into_reg(n, RAX);
        gpr_to_xmm_f64(xmm, RAX, n->type);
        return;
    }
    if (n->kind == N_CONV && n->lhs->type->kind == TY_INT && n->lhs->type->is_signed &&
        is_simple_expr(n->lhs)) {
        if (n->lhs->kind == N_IDENT && !n->lhs->implicit_slice && n->lhs->sym->hw_reg >= 0) {
            x_cvtsi2sd(xmm, n->lhs->sym->hw_reg);
        } else {
            gen_simple_into_reg(n->lhs, RAX);
            x_cvtsi2sd(xmm, RAX);
        }
        if (n->type == &ty_f32) {
            x_cvtsd2ss(xmm, xmm);
            x_cvtss2sd(xmm, xmm);
        }
        return;
    }
    if (n->kind == N_UNARY && n->op == T_MINUS) {
        x_xorpd(xmm, xmm);
        gen_float_xmm(cg, n->lhs, xmm + 1);
        x_sse_sd(0x5C, xmm, xmm + 1); // subsd 0.0, lhs
        return;
    }
    if (n->kind == N_BINARY) {
        gen_float_xmm(cg, n->lhs, xmm);
        uint8_t op = (n->op == T_PLUS)  ? 0x58
                   : (n->op == T_MINUS) ? 0x5C
                   : (n->op == T_STAR)  ? 0x59 : 0x5E;
        if (is_direct_xmm_reg(n->rhs)) {
            x_sse_sd(op, xmm, n->rhs->sym->xmm_reg);
        } else if (is_direct_xmm_const(n->rhs, &fbits)) {
            x_sse_sd_rip(op, xmm, SEC_RDATA, add_f64_const(cg, fbits));
        } else {
            gen_float_xmm(cg, n->rhs, xmm + 1);
            x_sse_sd(op, xmm, xmm + 1);
        }
        if (n->type == &ty_f32) {
            x_cvtsd2ss(xmm, xmm);
            x_cvtss2sd(xmm, xmm);
        }
        return;
    }
}

// Emits float comparison result (0 or 1 in RAX) from `XMM0` (lhs) and `XMM1` (rhs).
static void emit_xmm_cmp_rax(TokKind op) {
    if (op == T_LT) {
        x_ucomisd(1, 0);
        x_setcc(CC_A, RAX);
    } else if (op == T_LE) {
        x_ucomisd(1, 0);
        x_setcc(CC_AE, RAX);
    } else if (op == T_GT) {
        x_ucomisd(0, 1);
        x_setcc(CC_A, RAX);
    } else if (op == T_GE) {
        x_ucomisd(0, 1);
        x_setcc(CC_AE, RAX);
    } else if (op == T_EQ) {
        x_ucomisd(0, 1);
        x_setcc(CC_E, RAX);
        x_setcc(CC_NP, RCX);
        x_alu_rr(ALU_AND, 0, RAX, RCX);
    } else { // T_NE
        x_ucomisd(0, 1);
        x_setcc(CC_NE, RAX);
        x_setcc(CC_P, RCX);
        x_alu_rr(ALU_OR, 0, RAX, RCX);
    }
    x_movzx8(RAX, RAX);
}

// Combines RAX (left) and RCX (right) of float type `t` -> RAX.
static void gen_float_op(TokKind op, Type *t) {
    gpr_to_xmm_f64(0, RAX, t); // XMM0 = lhs
    gpr_to_xmm_f64(1, RCX, t); // XMM1 = rhs

    switch (op) {
    case T_PLUS:
        x_sse_sd(0x58, 0, 1); // addsd xmm0, xmm1
        xmm_f64_to_gpr(RAX, 0, t);
        return;
    case T_MINUS:
        x_sse_sd(0x5C, 0, 1); // subsd xmm0, xmm1
        xmm_f64_to_gpr(RAX, 0, t);
        return;
    case T_STAR:
        x_sse_sd(0x59, 0, 1); // mulsd xmm0, xmm1
        xmm_f64_to_gpr(RAX, 0, t);
        return;
    case T_SLASH:
        x_sse_sd(0x5E, 0, 1); // divsd xmm0, xmm1
        xmm_f64_to_gpr(RAX, 0, t);
        return;
    case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE:
        emit_xmm_cmp_rax(op);
        return;
    default:
        die("internal error: unhandled float op in codegen");
    }
}

static void gen_int_op_src_reg(CG *cg, TokKind op, Type *t, Pos pos, int src_reg) {
    bool is_wrap = (op == T_PLUSW || op == T_MINUSW || op == T_STARW);

    switch (op) {
    case T_PLUS:
    case T_PLUSW:
        if (!is_wrap && !cg->release) {
            if (t->size == 8) {
                x_alu_rr(ALU_ADD, 1, RAX, src_reg);
                emit_trap_if(cg, t->is_signed ? CC_O : CC_B, pos, "integer addition overflow");
            } else {
                x_alu_rr(ALU_ADD, 1, RAX, src_reg);
                check_and_normalize_sub64(cg, t, t->is_signed, pos, "integer addition overflow");
            }
        } else {
            x_alu_rr(ALU_ADD, 1, RAX, src_reg);
            normalize_rax(t);
        }
        return;

    case T_MINUS:
    case T_MINUSW:
        if (!is_wrap && !cg->release) {
            if (t->size == 8) {
                x_alu_rr(ALU_SUB, 1, RAX, src_reg);
                emit_trap_if(cg, t->is_signed ? CC_O : CC_B, pos, "integer subtraction overflow");
            } else {
                x_alu_rr(ALU_SUB, 1, RAX, src_reg);
                check_and_normalize_sub64(cg, t, true, pos, "integer subtraction overflow");
            }
        } else {
            x_alu_rr(ALU_SUB, 1, RAX, src_reg);
            normalize_rax(t);
        }
        return;

    case T_STAR:
    case T_STARW:
        if (!is_wrap && !cg->release) {
            if (t->size == 8) {
                if (t->is_signed) {
                    x_imul_rr(1, RAX, src_reg);
                    emit_trap_if(cg, CC_O, pos, "integer multiplication overflow");
                } else {
                    x_f7(4, 1, src_reg); // mul src_reg -> RDX:RAX, sets CF/OF if RDX != 0
                    emit_trap_if(cg, CC_O, pos, "integer multiplication overflow");
                }
            } else {
                x_imul_rr(1, RAX, src_reg);
                check_and_normalize_sub64(cg, t, t->is_signed, pos, "integer multiplication overflow");
            }
        } else {
            x_imul_rr(1, RAX, src_reg);
            normalize_rax(t);
        }
        return;

    case T_SLASH:
    case T_PERCENT: {
        // Spec §8.4: division by zero and signed MIN / -1 ALWAYS trap.
        x_test_rr(1, src_reg, src_reg);
        emit_trap_if(cg, CC_E, pos, "division by zero");

        if (t->is_signed) {
            // Check MIN / -1 for the operand width.
            int ok = x_label();
            x_alu_ri(ALU_CMP, 1, src_reg, -1);
            x_jcc(CC_NE, ok);
            int64_t minv = (t->size == 8) ? INT64_MIN : -(1LL << (t->size * 8 - 1));
            x_mov_ri(RDX, (uint64_t)minv);
            x_alu_rr(ALU_CMP, 1, RAX, RDX);
            emit_trap_if(cg, CC_E, pos, "integer division overflow (MIN / -1)");
            x_bind(ok);

            x_cqo();
            x_f7(7, 1, src_reg); // idiv src_reg
        } else {
            x_mov_ri(RDX, 0);
            x_f7(6, 1, src_reg); // div src_reg
        }
        if (op == T_PERCENT) x_mov_rr(1, RAX, RDX);
        normalize_rax(t);
        return;
    }

    case T_AMP:
        x_alu_rr(ALU_AND, 1, RAX, src_reg);
        return;
    case T_PIPE:
        x_alu_rr(ALU_OR, 1, RAX, src_reg);
        return;
    case T_CARET:
        x_alu_rr(ALU_XOR, 1, RAX, src_reg);
        return;

    case T_SHL:
    case T_SHR: {
        int bits = t->size * 8;
        if (!cg->release) {
            x_alu_ri(ALU_CMP, 1, src_reg, bits);
            emit_trap_if(cg, CC_AE, pos, "shift count >= bit width of type");
        } else if (bits < 64) {
            x_alu_ri(ALU_AND, 0, src_reg, bits - 1);
        }
        if (op == T_SHL) {
            x_shift_cl(4, 1, RAX); // shl rax, cl
            normalize_rax(t);
        } else {
            x_shift_cl(t->is_signed ? 7 : 5, 1, RAX); // sar / shr rax, cl
            normalize_rax(t);
        }
        return;
    }

    case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE: {
        x_alu_rr(ALU_CMP, 1, RAX, src_reg);
        int cc = CC_E;
        bool s = t->is_signed;
        switch (op) {
        case T_EQ: cc = CC_E; break;
        case T_NE: cc = CC_NE; break;
        case T_LT: cc = s ? CC_L  : CC_B;  break;
        case T_LE: cc = s ? CC_LE : CC_BE; break;
        case T_GT: cc = s ? CC_G  : CC_A;  break;
        case T_GE: cc = s ? CC_GE : CC_AE; break;
        default: break;
        }
        x_setcc(cc, RAX);
        x_movzx8(RAX, RAX);
        return;
    }

    default:
        die("internal error: unhandled binary op in codegen");
    }
}

static void gen_int_op(CG *cg, TokKind op, Type *t, Pos pos) {
    gen_int_op_src_reg(cg, op, t, pos, RCX);
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

static void gen_expr(CG *cg, Node *n);
static void gen_block(CG *cg, Node *b);
static bool can_inline_fn(CG *cg, Node *callee);
static void gen_inline_call(CG *cg, Node *n, Node *callee);

// Evaluates an array or slice expression `base` into (RAX = data_ptr, RDX = len).
static void gen_collection_ptr_len(CG *cg, Node *base) {
    gen_expr(cg, base);
    if (base->type->kind == TY_ARRAY && !base->implicit_slice) {
        x_mov_ri(RDX, (uint64_t)base->type->array_len);
    }
}

static bool node_modifies_or_addrs_sym(Node *n, Symbol *s);

// Returns true if `idx` is the induction variable of an active `loop i in lo..hi`
// whose range `[lo, hi)` is statically guaranteed to lie within `0 .. len(base)`.
static bool is_range_loop_in_bounds(CG *cg, Node *base, Node *idx) {
    if (!base || !idx || idx->kind != N_IDENT || !idx->sym || !idx->sym->readonly) return false;
    for (int k = cg->num_range_loops - 1; k >= 0; k--) {
        Node *L = cg->range_loops[k];
        if (L->sym != idx->sym) continue;
        if (!L->lo || !L->lo->is_const || L->lo->cval < 0) return false;
        if (base->type->kind == TY_ARRAY && !base->implicit_slice) {
            if (L->hi && L->hi->is_const && L->hi->cval <= base->type->array_len) {
                return true;
            }
        }
        if (base->kind == N_IDENT && base->sym && L->hi &&
            L->hi->kind == N_CALL && L->hi->builtin == B_LEN &&
            L->hi->list.len == 1 && L->hi->list.items[0]->kind == N_IDENT &&
            L->hi->list.items[0]->sym == base->sym &&
            !node_modifies_or_addrs_sym(L->body, base->sym)) {
            return true;
        }
    }
    return false;
}

// Matches `idx` as `reg_var` or (in release mode on 64-bit integers) `reg_var ± const`.
static bool match_reg_plus_const(CG *cg, Node *idx, int *out_reg, int64_t *out_disp_elems, Symbol **out_sym) {
    if (!idx) return false;
    if (idx->kind == N_IDENT && !idx->implicit_slice && idx->sym &&
        !idx->sym->is_const_alias && idx->sym->hw_reg >= 0) {
        *out_reg = idx->sym->hw_reg;
        *out_disp_elems = 0;
        if (out_sym) *out_sym = idx->sym;
        return true;
    }
    if (cg->release && idx->kind == N_BINARY && (idx->op == T_PLUS || idx->op == T_MINUS) &&
        idx->type && idx->type->kind == TY_INT && idx->type->size == 8) {
        if (idx->lhs->kind == N_IDENT && !idx->lhs->implicit_slice && idx->lhs->sym &&
            !idx->lhs->sym->is_const_alias && idx->lhs->sym->hw_reg >= 0 &&
            idx->lhs->type->size == 8 && idx->rhs->is_const) {
            *out_reg = idx->lhs->sym->hw_reg;
            *out_disp_elems = (idx->op == T_PLUS) ? (int64_t)idx->rhs->cval : -(int64_t)idx->rhs->cval;
            if (out_sym) *out_sym = idx->lhs->sym;
            return true;
        }
        if (idx->op == T_PLUS && idx->lhs->is_const && idx->rhs->kind == N_IDENT &&
            !idx->rhs->implicit_slice && idx->rhs->sym && !idx->rhs->sym->is_const_alias &&
            idx->rhs->sym->hw_reg >= 0 && idx->rhs->type->size == 8) {
            *out_reg = idx->rhs->sym->hw_reg;
            *out_disp_elems = (int64_t)idx->lhs->cval;
            if (out_sym) *out_sym = idx->rhs->sym;
            return true;
        }
    }
    return false;
}

// Evaluates `n` (an N_INDEX node `base[idx]`), performs bounds checking
// (`0 <= idx < len`) for arrays/slices or a debug null check for pointers,
// and returns the element address in RAX.
static void gen_index_addr(CG *cg, Node *n) {
    Type *bt = n->lhs->type;
    Type *elem_t = (bt == &ty_str) ? &ty_u8 : bt->elem;

    if (bt->kind == TY_PTR) {
        gen_expr(cg, n->lhs); // RAX = ptr
        if (!cg->release) {
            x_test_rr(1, RAX, RAX);
            emit_trap_if(cg, CC_E, n->pos, "null pointer dereference");
        }
        if (n->rhs->is_const) {
            int64_t off = (int64_t)n->rhs->cval * elem_t->size;
            if (off == 0) return;
            if (off >= INT32_MIN && off <= INT32_MAX) {
                x_alu_ri(ALU_ADD, 1, RAX, (int32_t)off);
                return;
            }
        }
        if (is_simple_expr(n->rhs)) {
            x_mov_rr(1, RCX, RAX);
            gen_simple_into_reg(n->rhs, RAX);
        } else {
            int s_ptr = push_temp(cg);
            gen_expr(cg, n->rhs); // RAX = idx
            pop_temp(cg, s_ptr, RCX); // RCX = ptr
        }
        scale_reg_by_size(RAX, elem_t->size, RDX);
        x_alu_rr(ALU_ADD, 1, RAX, RCX);
        return;
    }

    bool proven_in_bounds = is_range_loop_in_bounds(cg, n->lhs, n->rhs);

    // Fast path for fixed-size array variable `arr[idx]`: zero stack spills.
    if (n->lhs->kind == N_IDENT && bt->kind == TY_ARRAY && !n->lhs->implicit_slice) {
        Symbol *s = n->lhs->sym;
        if (n->rhs->is_const) {
            // Bounds already verified at compile time by check.c.
            int32_t off = (int32_t)((int64_t)n->rhs->cval * elem_t->size);
            if (!s->is_global && s->kind != SYM_PARAM) {
                x_lea(RAX, RBP, slot_disp(s->slot) + off);
            } else {
                load_var(s, RAX);
                if (off != 0) x_alu_ri(ALU_ADD, 1, RAX, off);
            }
            return;
        }
        gen_expr(cg, n->rhs); // RAX = idx
        if (!proven_in_bounds) {
            x_alu_ri(ALU_CMP, 1, RAX, bt->array_len);
            emit_trap_if(cg, CC_AE, n->rhs->pos, "index out of bounds");
        }
        scale_reg_by_size(RAX, elem_t->size, RDX);
        load_var(s, RCX);
        x_alu_rr(ALU_ADD, 1, RAX, RCX);
        return;
    }

    // Fast path for slice or string variable `s[idx]`: zero stack spills when `idx` doesn't mutate `s`.
    if (n->lhs->kind == N_IDENT && (bt->kind == TY_SLICE || bt == &ty_str) &&
        (is_simple_expr(n->rhs) || (!n->lhs->sym->is_global && !node_modifies_or_addrs_sym(n->rhs, n->lhs->sym)))) {
        Symbol *s = n->lhs->sym;
        int idx_reg = -1;
        int64_t disp_elems = 0;
        if ((cg->release || proven_in_bounds) && s->hw_reg >= 0 &&
            (elem_t->size == 1 || elem_t->size == 2 || elem_t->size == 4 || elem_t->size == 8) &&
            match_reg_plus_const(cg, n->rhs, &idx_reg, &disp_elems, NULL)) {
            int64_t disp_bytes = disp_elems * elem_t->size;
            if (disp_bytes >= INT32_MIN && disp_bytes <= INT32_MAX) {
                x_lea_sib_disp(RAX, s->hw_reg, idx_reg, elem_t->size, (int32_t)disp_bytes);
                return;
            }
        }
        if (is_simple_expr(n->rhs)) gen_simple_into_reg(n->rhs, RAX);
        else                        gen_expr(cg, n->rhs);
        if (!cg->release && !proven_in_bounds) {
            x_load(RDX, RBP, slot_disp(s->slot) + 8);
            x_alu_rr(ALU_CMP, 1, RAX, RDX);
            emit_trap_if(cg, CC_AE, n->rhs->pos, "index out of bounds");
        }
        if (n->rhs->is_const && n->rhs->cval == 0) {
            if (s->hw_reg >= 0) x_mov_rr(1, RAX, s->hw_reg);
            else                x_load(RAX, RBP, slot_disp(s->slot));
            return;
        }
        if (s->hw_reg >= 0 && (elem_t->size == 1 || elem_t->size == 2 || elem_t->size == 4 || elem_t->size == 8)) {
            x_lea_sib(RAX, s->hw_reg, RAX, elem_t->size);
            return;
        }
        scale_reg_by_size(RAX, elem_t->size, RDX);
        if (s->hw_reg >= 0) {
            x_alu_rr(ALU_ADD, 1, RAX, s->hw_reg);
        } else {
            x_load(RCX, RBP, slot_disp(s->slot));
            x_alu_rr(ALU_ADD, 1, RAX, RCX);
        }
        return;
    }

    gen_collection_ptr_len(cg, n->lhs);
    int s_ptr = push_temp(cg);
    x_mov_rr(1, RAX, RDX);
    int s_len = push_temp(cg);

    gen_expr(cg, n->rhs); // RAX = idx
    pop_temp(cg, s_len, RDX); // RDX = len
    pop_temp(cg, s_ptr, RCX); // RCX = base_ptr

    // Bounds check: if idx is signed and < 0, or unsigned(idx) >= len -> trap.
    if (!cg->release) {
        x_alu_rr(ALU_CMP, 1, RAX, RDX);
        emit_trap_if(cg, CC_AE, n->rhs->pos, "index out of bounds");
    }

    scale_reg_by_size(RAX, elem_t->size, RDX);
    x_alu_rr(ALU_ADD, 1, RAX, RCX); // RAX = base_ptr + idx * elem_size
}

static void gen_lvalue_addr(CG *cg, Node *n);
static void emit_defers_from(CG *cg, int limit);
static void gen_init_at_addr(CG *cg, int addr_slot, Node *rhs, Type *t);

static void gen_lvalue_addr(CG *cg, Node *n) {
    if (n->kind == N_IDENT) {
        Symbol *s = n->sym;
        if (s->is_global) {
            x_lea_rip(RAX, SEC_DATA, s->data_off);
        } else if (s->kind == SYM_PARAM && (s->type->kind == TY_ARRAY || is_aggregate_type(s->type))) {
            x_load(RAX, RBP, slot_disp(s->slot));
        } else {
            x_lea(RAX, RBP, slot_disp(s->slot));
        }
        return;
    }
    if (n->kind == N_INDEX) {
        gen_index_addr(cg, n);
        return;
    }
    if (n->kind == N_DEREF) {
        gen_expr(cg, n->lhs);
        if (!cg->release) {
            x_test_rr(1, RAX, RAX);
            emit_trap_if(cg, CC_E, n->pos, "null pointer dereference");
        }
        return;
    }
    if (n->kind == N_FIELD) {
        Type *bt = n->lhs->type;
        if (bt->kind == TY_PTR) {
            // Auto-dereference `ptr.field`!
            gen_expr(cg, n->lhs);
            if (!cg->release) {
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_E, n->pos, "null pointer dereference");
            }
        } else if (n->lhs->kind == N_STRUCT_LIT || n->lhs->kind == N_ENUM_LIT ||
                   n->lhs->kind == N_CALL || n->lhs->kind == N_TRY) {
            gen_expr(cg, n->lhs); // RAX = temp struct address
        } else {
            gen_lvalue_addr(cg, n->lhs);
        }
        if (n->field_offset != 0) {
            x_alu_ri(ALU_ADD, 1, RAX, n->field_offset);
        }
        return;
    }
    die("internal error: invalid lvalue in gen_lvalue_addr");
}

static void emit_zero_slot_addr(int addr_slot, uint64_t size) {
    if (size == 0) return;
    x_push(RDI);
    x_load(RDI, RBP, slot_disp(addr_slot));
    x_mov_ri(RAX, 0);
    x_mov_ri(RCX, size);
    x_rep_stosb();
    x_pop(RDI);
}

// Writes `rhs` (an N_STRUCT_LIT, N_ARRAY_LIT, or arbitrary expression of type `t`)
// into the memory address stored at stack temporary `addr_slot`.
static void gen_init_at_addr(CG *cg, int addr_slot, Node *rhs, Type *t) {
    if (rhs->kind == N_STRUCT_LIT) {
        emit_zero_slot_addr(addr_slot, (uint64_t)t->size);
        for (int i = 0; i < rhs->list.len; i++) {
            Node *fi = rhs->list.items[i];
            x_load(RAX, RBP, slot_disp(addr_slot));
            if (fi->field_offset != 0) {
                x_alu_ri(ALU_ADD, 1, RAX, fi->field_offset);
            }
            int f_slot = push_temp(cg);
            gen_init_at_addr(cg, f_slot, fi->rhs, fi->type);
            cg->temp_depth--;
        }
        return;
    }
    if (rhs->kind == N_ARRAY_LIT) {
        emit_zero_slot_addr(addr_slot, (uint64_t)t->size);
        Type *elem_t = t->elem;
        for (int i = 0; i < rhs->list.len; i++) {
            x_load(RAX, RBP, slot_disp(addr_slot));
            int32_t off = i * elem_t->size;
            if (off != 0) x_alu_ri(ALU_ADD, 1, RAX, off);
            int e_slot = push_temp(cg);
            gen_init_at_addr(cg, e_slot, rhs->list.items[i], elem_t);
            cg->temp_depth--;
        }
        return;
    }
    if (rhs->kind == N_ENUM_LIT) {
        emit_zero_slot_addr(addr_slot, (uint64_t)t->size);
        x_load(RCX, RBP, slot_disp(addr_slot));
        x_mov_ri(RAX, (uint64_t)(int64_t)rhs->cval);
        x_store_sized(RCX, 0, RAX, 4);
        if (rhs->lhs) {
            x_load(RAX, RBP, slot_disp(addr_slot));
            if (t->payload_offset != 0) {
                x_alu_ri(ALU_ADD, 1, RAX, t->payload_offset);
            }
            int p_slot = push_temp(cg);
            gen_init_at_addr(cg, p_slot, rhs->lhs, rhs->lhs->type);
            cg->temp_depth--;
        }
        return;
    }
    if (t->kind == TY_ARRAY || is_aggregate_type(t)) {
        gen_expr(cg, rhs); // RAX = src address
        x_push(RSI);
        x_push(RDI);
        x_mov_rr(1, RSI, RAX);
        x_load(RDI, RBP, slot_disp(addr_slot));
        x_mov_ri(RCX, (uint64_t)t->size);
        x_rep_movsb();
        x_pop(RDI);
        x_pop(RSI);
        return;
    }
    if (t == &ty_str || t->kind == TY_SLICE) {
        gen_collection_ptr_len(cg, rhs); // RAX = ptr, RDX = len
        x_load(RCX, RBP, slot_disp(addr_slot));
        x_store(RCX, 0, RAX);
        x_store(RCX, 8, RDX);
        return;
    }
    gen_expr(cg, rhs);
    x_load(RCX, RBP, slot_disp(addr_slot));
    x_store_sized(RCX, 0, RAX, t->size);
}

static void gen_expr(CG *cg, Node *n);

static void gen_print_val(CG *cg, Node *arg, bool with_nl) {
    gen_expr(cg, arg);
    if (arg->type == &ty_str) {
        x_mov_rr(1, RCX, RAX); // RDX already holds length
        x_call(with_nl ? cg->rt_print_str : cg->rt_write);
    } else if (arg->type == &ty_bool) {
        x_mov_rr(1, RCX, RAX);
        x_call(with_nl ? cg->rt_print_bool : cg->rt_print_bool_raw);
    } else if (arg->type->kind == TY_FLOAT) {
        gpr_to_xmm_f64(0, RAX, arg->type);
        x_movq_gpr_xmm(RCX, 0);
        x_call(with_nl ? cg->rt_print_f64 : cg->rt_print_f64_raw);
    } else if (arg->type->kind == TY_PTR || !arg->type->is_signed) {
        x_mov_rr(1, RCX, RAX);
        x_call(with_nl ? cg->rt_print_u64 : cg->rt_print_u64_raw);
    } else {
        x_mov_rr(1, RCX, RAX);
        x_call(with_nl ? cg->rt_print_i64 : cg->rt_print_i64_raw);
    }
}

// Evaluates (when `emit`) or checks if a simple shift/mask/power-of-two/release-mul
// binary expression can execute directly into `reg` without clobbering `RAX` or live scratch regs.
static bool eval_simple_shift_mask_impl(CG *cg, Node *n, int reg, bool emit) {
    if (!n || n->kind != N_BINARY || !n->type || n->type->kind != TY_INT) return false;
    if (!n->lhs || n->lhs->type != n->type || !is_simple_expr(n->lhs)) return false;

    if ((n->op == T_SHL || n->op == T_SHR) && n->rhs->is_const) {
        int bits = n->type->size * 8;
        if (n->rhs->cval >= 0 && n->rhs->cval < bits) {
            if (emit) {
                gen_simple_into_reg(n->lhs, reg);
                if (n->rhs->cval > 0) {
                    int ext = (n->op == T_SHL) ? 4 : (n->type->is_signed ? 7 : 5);
                    x_shift_imm(ext, 1, reg, (uint8_t)n->rhs->cval);
                }
                normalize_reg(reg, n->type);
            }
            return true;
        }
        return false;
    }

    int32_t imm32 = 0;
    if (!fits_imm32(n->rhs, &imm32)) return false;

    if (n->op == T_AMP) {
        if (emit) {
            gen_simple_into_reg(n->lhs, reg);
            x_alu_ri(ALU_AND, 1, reg, imm32);
            if (!(imm32 >= 0 && (n->type->size >= 4 ||
                                 (n->type->size == 2 && imm32 <= 0x7FFF) ||
                                 (n->type->size == 1 && imm32 <= 0x7F)))) {
                normalize_reg(reg, n->type);
            }
        }
        return true;
    }

    if ((n->op == T_SLASH || n->op == T_PERCENT) && imm32 > 0 && (imm32 & (imm32 - 1)) == 0 &&
        (!n->type->is_signed || is_nonneg_expr(n->lhs))) {
        if (emit) {
            if (imm32 == 1) {
                if (n->op == T_SLASH) gen_simple_into_reg(n->lhs, reg);
                else                  x_mov_ri(reg, 0);
                return true;
            }
            int k = 0;
            while ((1 << k) < imm32) k++;
            gen_simple_into_reg(n->lhs, reg);
            if (n->op == T_SLASH) x_shift_imm(5, 1, reg, (uint8_t)k);
            else                  x_alu_ri(ALU_AND, 1, reg, imm32 - 1);
            if (n->type->size < 8 && !(n->type->size == 4 || imm32 <= 128)) {
                normalize_reg(reg, n->type);
            }
        }
        return true;
    }

    if (n->op == T_STARW || (n->op == T_STAR && cg->release)) {
        if (emit) {
            if (imm32 == 0) {
                x_mov_ri(reg, 0);
                return true;
            }
            gen_simple_into_reg(n->lhs, reg);
            if (imm32 > 0 && (imm32 & (imm32 - 1)) == 0) {
                int k = 0;
                while ((1 << k) < imm32) k++;
                if (k > 0) x_shift_imm(4, 1, reg, (uint8_t)k);
            } else {
                x_imul_rri(1, reg, reg, imm32);
            }
            normalize_reg(reg, n->type);
        }
        return true;
    }

    return false;
}

static bool can_eval_simple_shift_mask(CG *cg, Node *n) {
    return eval_simple_shift_mask_impl(cg, n, 0, false);
}

static bool try_eval_simple_shift_mask_into(CG *cg, Node *n, int reg) {
    return eval_simple_shift_mask_impl(cg, n, reg, true);
}

// Reassociates a `--release` 64-bit integer left-linear +/- chain anchored at a
// register-allocated variable (`base +- t1 +- t2 ...`) so independent RHS terms
// combine first into `RAX` before a single loop-carried addition with `base`.
static bool try_gen_additive_spine(CG *cg, Node *n, Symbol *accum_target) {
    if (!cg->release || !n || n->kind != N_BINARY || !n->type ||
        n->type->kind != TY_INT || n->type->size != 8) {
        return false;
    }

    Node *terms[8];
    TokKind ops[8];
    int count = 0;
    Node *cur = n;
    while (count < 8 && cur && cur->kind == N_BINARY &&
           (cur->op == T_PLUS || cur->op == T_PLUSW || cur->op == T_MINUS || cur->op == T_MINUSW) &&
           cur->lhs && cur->lhs->type && cur->lhs->type->kind == TY_INT && cur->lhs->type->size == 8 &&
           cur->rhs && cur->rhs->type && cur->rhs->type->kind == TY_INT && cur->rhs->type->size == 8) {
        terms[count] = cur->rhs;
        ops[count] = cur->op;
        count++;
        cur = cur->lhs;
    }

    if (count < 2 || !cur || cur->kind != N_IDENT || cur->implicit_slice ||
        !cur->sym || cur->sym->is_const_alias || cur->sym->hw_reg < 0) {
        return false;
    }

    Symbol *base_sym = cur->sym;
    Symbol *orig_sym = base_sym->alias_of ? base_sym->alias_of : base_sym;
    for (int i = count - 1; i >= 0; i--) {
        Node *term = terms[i];
        if (!is_call_free_expr(term, true) ||
            node_modifies_or_addrs_sym(term, base_sym) ||
            node_modifies_or_addrs_sym(term, orig_sym) ||
            node_reads_sym(term, base_sym) ||
            node_reads_sym(term, orig_sym)) {
            return false;
        }
        if (accum_target && accum_target != base_sym && accum_target->hw_reg == base_sym->hw_reg &&
            (node_reads_sym(term, accum_target) || node_modifies_or_addrs_sym(term, accum_target))) {
            return false;
        }
    }

    Node *t0 = terms[count - 1];
    gen_expr(cg, t0);
    if (ops[count - 1] == T_MINUS || ops[count - 1] == T_MINUSW) {
        x_f7(3, 1, RAX); // neg rax
    }

    for (int idx = count - 2; idx >= 0; idx--) {
        Node *term = terms[idx];
        int alu = (ops[idx] == T_PLUS || ops[idx] == T_PLUSW) ? ALU_ADD : ALU_SUB;
        int32_t imm32 = 0;
        if (fits_imm32(term, &imm32)) {
            x_alu_ri(alu, 1, RAX, imm32);
        } else if (is_simple_expr(term)) {
            gen_simple_into_reg(term, RCX);
            x_alu_rr(alu, 1, RAX, RCX);
        } else if (try_eval_simple_shift_mask_into(cg, term, RCX)) {
            x_alu_rr(alu, 1, RAX, RCX);
        } else if (cg->scratch_depth < 4) {
            int sreg = R8 + cg->scratch_depth++;
            x_mov_rr(1, sreg, RAX);
            gen_expr(cg, term);
            x_mov_rr(1, RCX, RAX);
            x_mov_rr(1, RAX, sreg);
            cg->scratch_depth--;
            x_alu_rr(alu, 1, RAX, RCX);
        } else {
            int slot = push_temp(cg);
            gen_expr(cg, term);
            x_mov_rr(1, RCX, RAX);
            pop_temp(cg, slot, RAX);
            x_alu_rr(alu, 1, RAX, RCX);
        }
    }

    if (accum_target && (accum_target == base_sym || accum_target == orig_sym ||
                         accum_target->hw_reg == base_sym->hw_reg)) {
        x_alu_rr(ALU_ADD, 1, accum_target->hw_reg, RAX);
    } else if (accum_target && accum_target->hw_reg >= 0) {
        x_alu_rr(ALU_ADD, 1, RAX, base_sym->hw_reg);
        x_mov_rr(1, accum_target->hw_reg, RAX);
    } else {
        x_alu_rr(ALU_ADD, 1, RAX, base_sym->hw_reg);
        if (accum_target) store_var(accum_target, RAX);
    }
    return true;
}

static void gen_expr(CG *cg, Node *n) {
    if (n->is_const && n->type->kind == TY_FLOAT) {
        x_mov_ri(RAX, fval_bits(n->fval, n->type));
        return;
    }
    if (n->is_const && (n->type->kind == TY_INT || n->type->kind == TY_ENUM || n->type == &ty_bool)) {
        x_mov_ri(RAX, (uint64_t)(int64_t)n->cval);
        normalize_rax(n->type);
        return;
    }

    switch (n->kind) {
    case N_INT:
    case N_BOOL:
    case N_NULL:
        x_mov_ri(RAX, (uint64_t)(int64_t)n->cval);
        return;

    case N_FLOAT:
        x_mov_ri(RAX, fval_bits(n->fval, n->type));
        return;

    case N_ADDR:
        gen_lvalue_addr(cg, n->lhs);
        return;

    case N_DEREF: {
        Type *elem_t = n->type;
        gen_lvalue_addr(cg, n);
        if (elem_t->kind == TY_ARRAY || is_aggregate_type(elem_t)) {
            // Address of array/struct/tagged-enum is already in RAX.
            return;
        }
        if (elem_t == &ty_str || elem_t->kind == TY_SLICE) {
            x_load(RDX, RAX, 8);
            x_load(RAX, RAX, 0);
            return;
        }
        x_load_sized(RAX, RAX, 0, elem_t->size, elem_t->is_signed);
        return;
    }

    case N_STRUCT_LIT:
    case N_ENUM_LIT: {
        x_lea(RAX, RBP, slot_disp(n->slot_start));
        int s_addr = push_temp(cg);
        gen_init_at_addr(cg, s_addr, n, n->type);
        pop_temp(cg, s_addr, RAX);
        return;
    }

    case N_FIELD: {
        Type *ft = n->type;
        gen_lvalue_addr(cg, n);
        if (ft->kind == TY_ARRAY || is_aggregate_type(ft)) {
            return;
        }
        if (ft == &ty_str || ft->kind == TY_SLICE) {
            x_load(RDX, RAX, 8);
            x_load(RAX, RAX, 0);
            return;
        }
        x_load_sized(RAX, RAX, 0, ft->size, ft->is_signed);
        return;
    }

    case N_STR: {
        uint32_t off = add_string(cg, n->str, n->str_len);
        x_lea_rip(RAX, SEC_RDATA, off);
        x_mov_ri(RDX, (uint64_t)n->str_len);
        return;
    }

    case N_IDENT:
        load_var(n->sym, RAX);
        if (n->implicit_slice) {
            x_mov_ri(RDX, (uint64_t)n->type->array_len);
        }
        return;

    case N_INDEX: {
        Type *elem_t = n->type;
        if (elem_t->kind != TY_ARRAY && !is_aggregate_type(elem_t) &&
            elem_t != &ty_str && elem_t->kind != TY_SLICE &&
            (elem_t->size == 1 || elem_t->size == 2 || elem_t->size == 4 || elem_t->size == 8) &&
            n->lhs->kind == N_IDENT && !n->lhs->implicit_slice && n->lhs->sym->hw_reg >= 0) {
            Type *bt = n->lhs->type;
            Symbol *s = n->lhs->sym;
            int idx_reg = -1;
            int64_t disp_elems = 0;
            bool in_b = is_range_loop_in_bounds(cg, n->lhs, n->rhs);
            if (((bt->kind == TY_PTR && cg->release) ||
                 ((bt->kind == TY_SLICE || bt == &ty_str) && (cg->release || in_b))) &&
                match_reg_plus_const(cg, n->rhs, &idx_reg, &disp_elems, NULL)) {
                int64_t disp_bytes = disp_elems * elem_t->size;
                if (disp_bytes >= INT32_MIN && disp_bytes <= INT32_MAX) {
                    x_load_sib_disp_sized(RAX, s->hw_reg, idx_reg, elem_t->size, (int32_t)disp_bytes, elem_t->size, elem_t->is_signed);
                    return;
                }
            }
            if ((bt->kind == TY_SLICE || bt == &ty_str) &&
                (is_simple_expr(n->rhs) || (!s->is_global && !node_modifies_or_addrs_sym(n->rhs, s)))) {
                if (is_simple_expr(n->rhs)) gen_simple_into_reg(n->rhs, RAX);
                else                        gen_expr(cg, n->rhs);
                if (!cg->release && !in_b) {
                    x_load(RDX, RBP, slot_disp(s->slot) + 8);
                    x_alu_rr(ALU_CMP, 1, RAX, RDX);
                    emit_trap_if(cg, CC_AE, n->rhs->pos, "index out of bounds");
                }
                x_load_sib_sized(RAX, s->hw_reg, RAX, elem_t->size, elem_t->size, elem_t->is_signed);
                return;
            }
        }
        gen_index_addr(cg, n);
        if (elem_t->kind == TY_ARRAY || is_aggregate_type(elem_t)) {
            return;
        }
        if (elem_t == &ty_str || elem_t->kind == TY_SLICE) {
            x_load(RDX, RAX, 8);
            x_load(RAX, RAX, 0);
            return;
        }
        x_load_sized(RAX, RAX, 0, elem_t->size, elem_t->is_signed);
        return;
    }

    case N_SLICE: {
        Type *bt = n->lhs->type;
        Type *elem_t = (bt == &ty_str) ? &ty_u8 : n->type->elem;
        if (bt->kind == TY_PTR) {
            gen_expr(cg, n->lhs);
            if (!cg->release) {
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_E, n->pos, "null pointer dereference");
            }
            int s_ptr = push_temp(cg);
            if (n->lo) gen_expr(cg, n->lo);
            else       x_mov_ri(RAX, 0);
            int s_lo = push_temp(cg);
            gen_expr(cg, n->hi);
            x_mov_rr(1, R8, RAX);    // R8 = hi
            pop_temp(cg, s_lo, RCX); // RCX = lo
            pop_temp(cg, s_ptr, R9); // R9 = ptr
            if (n->lo && n->lo->type->is_signed) {
                x_test_rr(1, RCX, RCX);
                emit_trap_if(cg, CC_S, n->lo->pos, "slice lower bound < 0");
            }
            if (n->hi->type->is_signed) {
                x_test_rr(1, R8, R8);
                emit_trap_if(cg, CC_S, n->hi->pos, "slice upper bound < 0");
            }
            x_alu_rr(ALU_CMP, 1, RCX, R8);
            emit_trap_if(cg, CC_A, n->pos, "slice lower bound > upper bound");
            x_mov_rr(1, RDX, R8);
            x_alu_rr(ALU_SUB, 1, RDX, RCX);
            x_mov_rr(1, RAX, RCX);
            scale_reg_by_size(RAX, elem_t->size, RCX);
            x_alu_rr(ALU_ADD, 1, RAX, R9);
            return;
        }
        gen_collection_ptr_len(cg, n->lhs);
        int s_ptr = push_temp(cg);
        x_mov_rr(1, RAX, RDX);
        int s_len = push_temp(cg);

        if (n->lo) {
            gen_expr(cg, n->lo);
        } else {
            x_mov_ri(RAX, 0);
        }
        int s_lo = push_temp(cg);

        if (n->hi) {
            gen_expr(cg, n->hi);
        } else {
            x_load(RAX, RBP, slot_disp(s_len));
        }
        x_mov_rr(1, R8, RAX);      // R8 = hi
        pop_temp(cg, s_lo, RCX);   // RCX = lo
        pop_temp(cg, s_len, RDX);  // RDX = len
        pop_temp(cg, s_ptr, R9);   // R9 = base_ptr

        // Bounds check: 0 <= lo <= hi <= len.
        if (n->lo && n->lo->type->is_signed) {
            x_test_rr(1, RCX, RCX);
            emit_trap_if(cg, CC_S, n->lo->pos, "slice lower bound < 0");
        }
        if (n->hi && n->hi->type->is_signed) {
            x_test_rr(1, R8, R8);
            emit_trap_if(cg, CC_S, n->hi->pos, "slice upper bound < 0");
        }
        x_alu_rr(ALU_CMP, 1, RCX, R8);
        emit_trap_if(cg, CC_A, n->pos, "slice lower bound > upper bound");
        x_alu_rr(ALU_CMP, 1, R8, RDX);
        emit_trap_if(cg, CC_A, n->pos, "slice upper bound > length");

        // New length = hi - lo (in RDX); new ptr = base_ptr + lo * elem_size (in RAX).
        x_mov_rr(1, RDX, R8);
        x_alu_rr(ALU_SUB, 1, RDX, RCX);
        x_mov_rr(1, RAX, RCX);
        scale_reg_by_size(RAX, elem_t->size, RCX);
        x_alu_rr(ALU_ADD, 1, RAX, R9);
        return;
    }

    case N_UNARY:
        gen_expr(cg, n->lhs);
        if (n->op == T_BANG) {
            x_alu_ri(ALU_XOR, 0, RAX, 1);
        } else if (n->op == T_TILDE) {
            x_f7(2, 1, RAX); // not rax
            normalize_rax(n->type);
        } else if (n->op == T_MINUS) {
            if (n->type->kind == TY_FLOAT) {
                // Flip the IEEE-754 sign bit (bit 31 for f32, bit 63 for f64).
                uint64_t sign_mask = (n->type == &ty_f32) ? (1ULL << 31) : (1ULL << 63);
                x_mov_ri(RCX, sign_mask);
                x_alu_rr(ALU_XOR, 1, RAX, RCX);
                return;
            }
            if (!cg->release) {
                if (n->type->size == 8) {
                    x_f7(3, 1, RAX); // neg rax
                    emit_trap_if(cg, CC_O, n->pos, "integer negation overflow");
                } else {
                    x_f7(3, 1, RAX);
                    check_and_normalize_sub64(cg, n->type, true, n->pos, "integer negation overflow");
                }
            } else {
                x_f7(3, 1, RAX);
                normalize_rax(n->type);
            }
        }
        return;

    case N_CONV: {
        Type *src = n->lhs->type;
        Type *dst = n->type;
        if (dst == &ty_str) {
            gen_collection_ptr_len(cg, n->lhs);
            return;
        }
        gen_expr(cg, n->lhs);
        if (src == dst || src->kind == TY_PTR || dst->kind == TY_PTR) return;
        if (src->kind == TY_FLOAT && dst->kind == TY_FLOAT) {
            // f32 <-> f64
            gpr_to_xmm_f64(0, RAX, src);
            xmm_f64_to_gpr(RAX, 0, dst);
            return;
        }
        if (src->kind == TY_INT && dst->kind == TY_FLOAT) {
            // integer -> f32/f64
            x_cvtsi2sd(0, RAX);
            xmm_f64_to_gpr(RAX, 0, dst);
            return;
        }
        if (src->kind == TY_FLOAT && dst->kind == TY_INT) {
            // f32/f64 -> integer (truncating toward zero)
            gpr_to_xmm_f64(0, RAX, src);
            x_cvttsd2si(RAX, 0);
            if (dst->size < 8) {
                check_and_normalize_sub64(cg, dst, true, n->pos, "float to integer conversion out of range");
            }
            return;
        }
        if (dst->size == 8) {
            if (!cg->release && src->is_signed && !dst->is_signed) {
                // signed -> u64/usize: negative values are out of range
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_S, n->pos, "negative value converted to unsigned type");
            } else if (!cg->release && !src->is_signed && dst->is_signed && src->size == 8) {
                // u64 -> i64: values > INT64_MAX have sign bit set
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_S, n->pos, "value out of range for signed 64-bit conversion");
            }
            return;
        }
        check_and_normalize_sub64(cg, dst, src->is_signed, n->pos, "integer conversion out of range");
        return;
    }

    case N_BINARY: {
        if (n->op == T_ANDAND) {
            int l_false = x_label(), l_end = x_label();
            gen_expr(cg, n->lhs);
            x_test_rr(0, RAX, RAX);
            x_jcc(CC_E, l_false);
            gen_expr(cg, n->rhs);
            x_jmp(l_end);
            x_bind(l_false);
            x_mov_ri(RAX, 0);
            x_bind(l_end);
            return;
        }
        if (n->op == T_OROR) {
            int l_true = x_label(), l_end = x_label();
            gen_expr(cg, n->lhs);
            x_test_rr(0, RAX, RAX);
            x_jcc(CC_NE, l_true);
            gen_expr(cg, n->rhs);
            x_jmp(l_end);
            x_bind(l_true);
            x_mov_ri(RAX, 1);
            x_bind(l_end);
            return;
        }

        // String equality (`s1 == s2`, `s1 != s2`).
        if (n->lhs->type == &ty_str) {
            gen_expr(cg, n->lhs); // RAX = ptr1, RDX = len1
            int s_ptr1 = push_temp(cg);
            x_mov_rr(1, RAX, RDX);
            int s_len1 = push_temp(cg);
            gen_expr(cg, n->rhs); // RAX = ptr2, RDX = len2
            x_mov_rr(1, R8, RAX);
            x_mov_rr(1, R9, RDX);
            pop_temp(cg, s_len1, RDX);
            pop_temp(cg, s_ptr1, RCX);
            x_call(cg->rt_str_eq);
            if (n->op == T_NE) {
                x_alu_ri(ALU_XOR, 0, RAX, 1);
            }
            return;
        }

        // Fast XMM register-tree evaluation for floating-point arithmetic & comparisons.
        if (n->lhs->type->kind == TY_FLOAT) {
            if ((n->op == T_PLUS || n->op == T_MINUS || n->op == T_STAR || n->op == T_SLASH) &&
                can_eval_xmm(n, 0)) {
                gen_float_xmm(cg, n, 0);
                xmm_f64_to_gpr(RAX, 0, n->type);
                return;
            }
            if ((n->op == T_EQ || n->op == T_NE || n->op == T_LT ||
                 n->op == T_LE || n->op == T_GT || n->op == T_GE) &&
                can_eval_xmm(n->lhs, 0) && can_eval_xmm(n->rhs, 1)) {
                gen_float_xmm(cg, n->lhs, 0);
                gen_float_xmm(cg, n->rhs, 1);
                emit_xmm_cmp_rax(n->op);
                return;
            }
        }
        if (cg->release && try_gen_additive_spine(cg, n, NULL)) return;

        // Fast immediate specialization for shifts by a compile-time constant.
        if ((n->op == T_SHL || n->op == T_SHR) && n->rhs->is_const && n->lhs->type->kind == TY_INT) {
            int bits = n->lhs->type->size * 8;
            if (n->rhs->cval >= 0 && n->rhs->cval < bits) {
                gen_expr(cg, n->lhs);
                if (n->rhs->cval > 0) {
                    int ext = (n->op == T_SHL) ? 4 : (n->lhs->type->is_signed ? 7 : 5);
                    x_shift_imm(ext, 1, RAX, (uint8_t)n->rhs->cval);
                }
                normalize_rax(n->lhs->type);
                return;
            }
        }

        // Fast immediate specialization for 32-bit signed constant RHS.
        int32_t imm32 = 0;
        if (n->lhs->type->kind != TY_FLOAT && fits_imm32(n->rhs, &imm32)) {
            Type *lt = n->lhs->type;
            if (n->op == T_EQ || n->op == T_NE || n->op == T_LT ||
                n->op == T_LE || n->op == T_GT || n->op == T_GE) {
                gen_expr(cg, n->lhs);
                x_alu_ri(ALU_CMP, 1, RAX, imm32);
                int cc = CC_E;
                bool s = lt->is_signed;
                switch (n->op) {
                case T_EQ: cc = CC_E; break;
                case T_NE: cc = CC_NE; break;
                case T_LT: cc = s ? CC_L  : CC_B;  break;
                case T_LE: cc = s ? CC_LE : CC_BE; break;
                case T_GT: cc = s ? CC_G  : CC_A;  break;
                case T_GE: cc = s ? CC_GE : CC_AE; break;
                default: break;
                }
                x_setcc(cc, RAX);
                x_movzx8(RAX, RAX);
                return;
            }
            if (lt->kind == TY_INT && (n->op == T_AMP || n->op == T_PIPE || n->op == T_CARET)) {
                gen_expr(cg, n->lhs);
                int alu = (n->op == T_AMP) ? ALU_AND : (n->op == T_PIPE) ? ALU_OR : ALU_XOR;
                x_alu_ri(alu, 1, RAX, imm32);
                if (!(n->op == T_AMP && imm32 >= 0 &&
                      (lt->size >= 4 || (lt->size == 2 && imm32 <= 0x7FFF) ||
                       (lt->size == 1 && imm32 <= 0x7F)))) {
                    normalize_rax(lt);
                }
                return;
            }
            if (lt->kind == TY_INT &&
                (n->op == T_PLUS || n->op == T_PLUSW || n->op == T_MINUS || n->op == T_MINUSW)) {
                bool is_wrap = (n->op == T_PLUSW || n->op == T_MINUSW);
                int alu = (n->op == T_PLUS || n->op == T_PLUSW) ? ALU_ADD : ALU_SUB;
                if (is_wrap || cg->release) {
                    gen_expr(cg, n->lhs);
                    x_alu_ri(alu, 1, RAX, imm32);
                    normalize_rax(lt);
                    return;
                }
                if (lt->size == 8) {
                    gen_expr(cg, n->lhs);
                    x_alu_ri(alu, 1, RAX, imm32);
                    emit_trap_if(cg, lt->is_signed ? CC_O : CC_B, n->pos,
                                 alu == ALU_ADD ? "integer addition overflow" : "integer subtraction overflow");
                    return;
                }
            }
            if (lt->kind == TY_INT && (n->op == T_STAR || n->op == T_STARW)) {
                if (n->op == T_STARW || cg->release) {
                    gen_expr(cg, n->lhs);
                    if (imm32 > 0 && (imm32 & (imm32 - 1)) == 0) {
                        int k = 0;
                        while ((1 << k) < imm32) k++;
                        if (k > 0) x_shift_imm(4, 1, RAX, (uint8_t)k);
                    } else {
                        x_imul_rri(1, RAX, RAX, imm32);
                    }
                    normalize_rax(lt);
                    return;
                }
                if (lt->size == 8 && lt->is_signed) {
                    gen_expr(cg, n->lhs);
                    x_imul_rri(1, RAX, RAX, imm32);
                    emit_trap_if(cg, CC_O, n->pos, "integer multiplication overflow");
                    return;
                }
            }
            if (lt->kind == TY_INT && (n->op == T_SLASH || n->op == T_PERCENT) && imm32 > 0) {
                gen_expr(cg, n->lhs);
                if (imm32 == 1) {
                    if (n->op == T_PERCENT) x_mov_ri(RAX, 0);
                    return;
                }
                if ((imm32 & (imm32 - 1)) == 0) {
                    int k = 0;
                    while ((1 << k) < imm32) k++;
                    bool is_nonneg = !lt->is_signed || is_nonneg_expr(n->lhs);
                    if (is_nonneg) {
                        if (n->op == T_SLASH) x_shift_imm(5, 1, RAX, (uint8_t)k);
                        else                  x_alu_ri(ALU_AND, 1, RAX, imm32 - 1);
                        if (lt->size < 8 && !(lt->size == 4 || imm32 <= 128)) {
                            normalize_rax(lt);
                        }
                    } else if (n->op == T_SLASH) {
                        x_mov_rr(1, RCX, RAX);
                        if (k == 1) {
                            x_shift_imm(5, 1, RCX, 63);
                        } else {
                            x_shift_imm(7, 1, RCX, 63);
                            x_alu_ri(ALU_AND, 1, RCX, imm32 - 1);
                        }
                        x_alu_rr(ALU_ADD, 1, RAX, RCX);
                        x_shift_imm(7, 1, RAX, (uint8_t)k);
                        normalize_rax(lt);
                    } else {
                        x_mov_rr(1, RCX, RAX);
                        if (k == 1) {
                            x_shift_imm(5, 1, RCX, 63);
                        } else {
                            x_shift_imm(7, 1, RCX, 63);
                            x_alu_ri(ALU_AND, 1, RCX, imm32 - 1);
                        }
                        x_alu_rr(ALU_ADD, 1, RCX, RAX);
                        x_alu_ri(ALU_AND, 1, RCX, -imm32);
                        x_alu_rr(ALU_SUB, 1, RAX, RCX);
                        normalize_rax(lt);
                    }
                    return;
                }
                // Positive non-power-of-two constant divisor (> 1): cannot be 0 or -1,
                // so skip both runtime trap branches and divide directly.
                x_mov_ri(RCX, (uint64_t)(int64_t)imm32);
                if (lt->is_signed) {
                    x_cqo();
                    x_f7(7, 1, RCX);
                } else {
                    x_mov_ri(RDX, 0);
                    x_f7(6, 1, RCX);
                }
                if (n->op == T_PERCENT) x_mov_rr(1, RAX, RDX);
                normalize_rax(lt);
                return;
            }
        }

        // Evaluate left to right (spec §7.2); skip stack spill when RHS is a leaf operand
        // or when RHS is call-free and a volatile scratch register (R8..R11) is free.
        if (is_simple_expr(n->rhs)) {
            gen_expr(cg, n->lhs);
            gen_simple_into_reg(n->rhs, RCX);
        } else if (n->lhs->type->kind == TY_INT && can_eval_simple_shift_mask(cg, n->rhs)) {
            gen_expr(cg, n->lhs);
            try_eval_simple_shift_mask_into(cg, n->rhs, RCX);
        } else if (cg->scratch_depth < 4 && is_call_free_expr(n->rhs, cg->release)) {
            gen_expr(cg, n->lhs);
            int sreg = R8 + cg->scratch_depth++;
            x_mov_rr(1, sreg, RAX);
            gen_expr(cg, n->rhs);
            cg->scratch_depth--;
            if (cg->release && n->lhs->type->kind == TY_INT &&
                (n->op == T_PLUS || n->op == T_PLUSW || n->op == T_STAR || n->op == T_STARW ||
                 n->op == T_AMP || n->op == T_PIPE || n->op == T_CARET)) {
                gen_int_op_src_reg(cg, n->op, n->lhs->type, n->pos, sreg);
                return;
            }
            x_mov_rr(1, RCX, RAX);
            x_mov_rr(1, RAX, sreg);
        } else {
            gen_expr(cg, n->lhs);
            int slot = push_temp(cg);
            gen_expr(cg, n->rhs);
            x_mov_rr(1, RCX, RAX);
            pop_temp(cg, slot, RAX);
        }

        if (n->lhs->type->kind == TY_PTR && (n->op == T_PLUS || n->op == T_MINUS)) {
            int esz = n->lhs->type->elem->size;
            if (n->op == T_MINUS && n->rhs->type->kind == TY_PTR) {
                // `p - q` -> (p - q) / elem_size
                x_alu_rr(ALU_SUB, 1, RAX, RCX);
                if (esz == 2)      x_shift_imm(7, 1, RAX, 1);
                else if (esz == 4) x_shift_imm(7, 1, RAX, 2);
                else if (esz == 8) x_shift_imm(7, 1, RAX, 3);
                else if (esz > 1) {
                    x_mov_ri(RCX, (uint64_t)esz);
                    x_cqo();
                    x_f7(7, 1, RCX); // idiv rcx
                }
                return;
            }
            // `p + offset` or `p - offset`: scale RCX by elem_size.
            scale_reg_by_size(RCX, esz, RDX);
            x_alu_rr(n->op == T_PLUS ? ALU_ADD : ALU_SUB, 1, RAX, RCX);
            return;
        }

        if (n->lhs->type->kind == TY_FLOAT) {
            gen_float_op(n->op, n->lhs->type);
            return;
        }

        gen_int_op(cg, n->op, n->lhs->type, n->pos);
        return;
    }

    case N_CALL: {
        if (n->builtin == B_PRINT) {
            gen_print_val(cg, n->list.items[0], true);
            return;
        }
        if (n->builtin == B_PRINT_FMT) {
            for (int i = 0; i < n->list.len; i++) {
                gen_print_val(cg, n->list.items[i], false);
            }
            x_call(cg->rt_print_nl);
            return;
        }
        if (n->builtin == B_EXIT) {
            gen_expr(cg, n->list.items[0]);
            x_mov_rr(0, RCX, RAX);
            x_call_import(IMP_EXITPROCESS);
            return;
        }
        if (n->builtin == B_LEN) {
            Node *arg = n->list.items[0];
            gen_collection_ptr_len(cg, arg);
            x_mov_rr(1, RAX, RDX);
            return;
        }
        if (n->builtin == B_ATOMIC_LOAD) {
            gen_expr(cg, n->list.items[0]);
            if (!cg->release) {
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_E, n->pos, "null pointer in atomic_load");
            }
            x_load(RAX, RAX, 0);
            return;
        }
        if (n->builtin == B_ATOMIC_STORE) {
            gen_expr(cg, n->list.items[0]);
            if (!cg->release) {
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_E, n->pos, "null pointer in atomic_store");
            }
            int ptr_slot = push_temp(cg);
            gen_expr(cg, n->list.items[1]);
            pop_temp(cg, ptr_slot, RCX);
            x_xchg_mr(RCX, RAX);
            return;
        }
        if (n->builtin == B_ATOMIC_ADD) {
            gen_expr(cg, n->list.items[0]);
            if (!cg->release) {
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_E, n->pos, "null pointer in atomic_add");
            }
            int ptr_slot = push_temp(cg);
            gen_expr(cg, n->list.items[1]);
            pop_temp(cg, ptr_slot, RCX);
            x_lock_xadd(RCX, RAX);
            return;
        }
        if (n->builtin == B_ATOMIC_CAS) {
            gen_expr(cg, n->list.items[0]);
            if (!cg->release) {
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_E, n->pos, "null pointer in atomic_cas");
            }
            int ptr_slot = push_temp(cg);
            gen_expr(cg, n->list.items[1]);
            int exp_slot = push_temp(cg);
            gen_expr(cg, n->list.items[2]);
            x_mov_rr(1, RDX, RAX);
            pop_temp(cg, exp_slot, RAX);
            pop_temp(cg, ptr_slot, RCX);
            x_lock_cmpxchg(RCX, RDX);
            return;
        }

        // Evaluate arguments left to right into temporary slots.
        // Note: a slice argument `{ptr, len}` occupies TWO 8-byte parameter slots
        // (or a pointer to a 16-byte temporary); to keep ABI simple and fast, we
        // pass a slice by address of its 16-byte `{ptr, len}` descriptor in a
        // single register/stack argument, and the callee copies the 16 bytes into
        // its local slice slot in the prologue.
        bool is_indirect = (n->sym == NULL && n->lhs != NULL);
        if (!is_indirect && n->sym && !n->sym->is_extern && can_inline_fn(cg, n->sym->decl)) {
            gen_inline_call(cg, n, n->sym->decl);
            return;
        }
        int nargs = n->list.len;
        if (nargs > 4 && (nargs - 4) > cg->max_out_stack_args) {
            cg->max_out_stack_args = nargs - 4;
        }
        static const int arg_regs[4] = {RCX, RDX, R8, R9};
        int fn_slot = 0;
        if (is_indirect) {
            gen_expr(cg, n->lhs);
            if (!cg->release) {
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_E, n->pos, "null function pointer call");
            }
            fn_slot = push_temp(cg);
        }
        bool fast_args = (!is_indirect && nargs <= 4);
        for (int i = 0; i < nargs && fast_args; i++) {
            Type *pt = is_indirect ? n->lhs->type->params.items[i] : n->sym->decl->list.items[i]->type;
            if (pt->kind == TY_SLICE || pt == &ty_str) {
                fast_args = false;
                break;
            }
            if (i > 0 && !is_simple_expr(n->list.items[i])) {
                fast_args = false;
                break;
            }
        }
        if (fast_args) {
            if (nargs > 0) {
                gen_expr(cg, n->list.items[0]);
                x_mov_rr(1, RCX, RAX);
                if (n->sym->is_extern && n->sym->decl->list.items[0]->type->kind == TY_FLOAT) {
                    x_movq_xmm_gpr(0, RCX);
                }
                for (int i = 1; i < nargs; i++) {
                    Type *pt = n->sym->decl->list.items[i]->type;
                    gen_simple_into_reg(n->list.items[i], arg_regs[i]);
                    if (n->sym->is_extern && pt->kind == TY_FLOAT) {
                        x_movq_xmm_gpr(i, arg_regs[i]);
                    }
                }
            }
        } else {
            int *arg_slots = nargs ? xmalloc((size_t)nargs * sizeof(int)) : NULL;
            for (int i = 0; i < nargs; i++) {
                Node *arg = n->list.items[i];
                Type *pt = is_indirect ? n->lhs->type->params.items[i] : n->sym->decl->list.items[i]->type;
                if (pt->kind == TY_SLICE || pt == &ty_str) {
                    gen_collection_ptr_len(cg, arg); // RAX = ptr, RDX = len
                    int s_hi = push_temp(cg);        // stores RAX at [RBP - 8*s_hi]
                    x_mov_rr(1, RAX, RDX);
                    int s_lo = push_temp(cg);        // stores RDX at [RBP - 8*s_lo] (8 bytes below s_hi)
                    // Store {ptr, len} at `[RBP - 8*s_lo]` (ptr) and `[RBP - 8*s_lo + 8]` (len).
                    x_load(RCX, RBP, slot_disp(s_hi));
                    x_store(RBP, slot_disp(s_lo) + 8, RAX); // len at +8
                    x_store(RBP, slot_disp(s_lo), RCX);     // ptr at +0
                    x_lea(RAX, RBP, slot_disp(s_lo));       // RAX = &{ptr, len}
                    arg_slots[i] = push_temp(cg);
                } else {
                    gen_expr(cg, arg);
                    arg_slots[i] = push_temp(cg);
                }
            }
            for (int i = nargs - 1; i >= 0; i--) {
                Type *pt = is_indirect ? n->lhs->type->params.items[i] : n->sym->decl->list.items[i]->type;
                if (i < 4) {
                    pop_temp(cg, arg_slots[i], arg_regs[i]);
                    if (!is_indirect && n->sym->is_extern && pt->kind == TY_FLOAT) {
                        x_movq_xmm_gpr(i, arg_regs[i]);
                    }
                } else {
                    pop_temp(cg, arg_slots[i], RAX);
                    x_store(RSP, 32 + (i - 4) * 8, RAX);
                }
                if (pt->kind == TY_SLICE || pt == &ty_str) {
                    cg->temp_depth -= 2; // release the 16-byte temporary descriptor
                }
            }
            free(arg_slots);
        }
        if (is_indirect) {
            pop_temp(cg, fn_slot, RAX);
            if (is_aggregate_type(n->type)) {
                x_lea(R10, RBP, slot_disp(n->slot_start));
            }
            x_call_reg(RAX);
        } else if (n->sym->is_extern) {
            x_call_import(n->sym->iat_slot);
            if (n->type == &ty_f64) {
                x_movq_gpr_xmm(RAX, 0);
            } else if (n->type == &ty_f32) {
                x_movq_gpr_xmm(RAX, 0);
                x_mov_rr(0, RAX, RAX);
            } else if (n->type->kind == TY_INT) {
                normalize_rax(n->type);
            } else if (n->type == &ty_bool) {
                x_movzx8(RAX, RAX);
            }
        } else {
            if (is_aggregate_type(n->type)) {
                x_lea(R10, RBP, slot_disp(n->slot_start));
            }
            x_call(n->sym->label);
        }
        return;
    }

    case N_TRY: {
        // `expr?`: evaluate `n->lhs` (tagged union `Result`/`Option`) -> RAX = base address.
        Type *et = n->lhs->type;
        gen_expr(cg, n->lhs);
        x_store(RBP, slot_disp(n->slot_start), RAX);
        x_load_sized(RCX, RAX, 0, 4, true); // RCX = 32-bit variant tag
        int l_ok = x_label();
        x_alu_ri(ALU_CMP, 1, RCX, (int32_t)n->cval); // compare with Err/None tag
        x_jcc(CC_NE, l_ok);

        // Failure path: construct `Err(e)` / `None` in caller's return buffer and return early.
        x_load(R8, RBP, slot_disp(cg->cur_ret_ptr_slot)); // R8 = caller return buffer
        x_mov_ri(RCX, (uint64_t)(uint32_t)n->field_offset); // caller's Err/None tag
        x_store_sized(R8, 0, RCX, 4);
        StructField *err_v = (StructField *)n->sym2;
        if (err_v && err_v->has_payload) {
            x_push(RSI);
            x_push(RDI);
            x_load(RSI, RBP, slot_disp(n->slot_start));
            x_alu_ri(ALU_ADD, 1, RSI, et->payload_offset);
            x_mov_rr(1, RDI, R8);
            x_alu_ri(ALU_ADD, 1, RDI, cg->cur_ret_type->payload_offset);
            x_mov_ri(RCX, (uint64_t)err_v->type->size);
            x_rep_movsb();
            x_pop(RDI);
            x_pop(RSI);
        }
        if (cg->defers.len > 0) {
            emit_defers_from(cg, 0);
        }
        x_load(RAX, RBP, slot_disp(cg->cur_ret_ptr_slot));
        x_jmp(cg->cur_epilogue);

        // Success path (`Ok(v)` / `Some(v)`): unwrap payload of type `n->type`.
        x_bind(l_ok);
        x_load(RAX, RBP, slot_disp(n->slot_start));
        x_alu_ri(ALU_ADD, 1, RAX, et->payload_offset);
        if (is_aggregate_type(n->type)) {
            x_push(RSI);
            x_push(RDI);
            x_mov_rr(1, RSI, RAX);
            x_lea(RDI, RBP, slot_disp(n->slot_end));
            x_mov_ri(RCX, (uint64_t)n->type->size);
            x_rep_movsb();
            x_pop(RDI);
            x_pop(RSI);
            x_lea(RAX, RBP, slot_disp(n->slot_end));
        } else if (n->type == &ty_str || n->type->kind == TY_SLICE) {
            x_load(RDX, RAX, 8);
            x_load(RAX, RAX, 0);
        } else {
            x_load_sized(RAX, RAX, 0, n->type->size, n->type->is_signed);
        }
        return;
    }

    default:
        die("internal error: unhandled expression node in codegen");
    }
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

static void gen_stmt(CG *cg, Node *n);
static void gen_block(CG *cg, Node *b);

static void gen_assign(CG *cg, Node *n) {
    if (n->lhs && (n->lhs->kind == N_INDEX || n->lhs->kind == N_DEREF || n->lhs->kind == N_FIELD)) {
        Type *elem_t = n->lhs->type;
        if (n->op == T_ASSIGN && cg->release && n->lhs->kind == N_INDEX &&
            n->lhs->lhs->kind == N_IDENT && !n->lhs->lhs->implicit_slice &&
            n->lhs->lhs->sym && !n->lhs->lhs->sym->is_const_alias &&
            n->lhs->lhs->sym->hw_reg >= 0 &&
            (n->lhs->lhs->type->kind == TY_SLICE || n->lhs->lhs->type->kind == TY_PTR) &&
            (elem_t->size == 1 || elem_t->size == 2 || elem_t->size == 4 || elem_t->size == 8) &&
            !is_aggregate_type(elem_t)) {
            Symbol *base_sym = n->lhs->lhs->sym;
            int idx_reg = -1;
            int64_t disp_elems = 0;
            Symbol *idx_sym = NULL;
            if (match_reg_plus_const(cg, n->lhs->rhs, &idx_reg, &disp_elems, &idx_sym)) {
                int64_t disp_bytes = disp_elems * elem_t->size;
                Symbol *base_orig = base_sym->alias_of ? base_sym->alias_of : base_sym;
                Symbol *idx_orig = (idx_sym && idx_sym->alias_of) ? idx_sym->alias_of : idx_sym;
                if (disp_bytes >= INT32_MIN && disp_bytes <= INT32_MAX &&
                    is_call_free_expr(n->rhs, true) &&
                    !node_modifies_or_addrs_sym(n->rhs, base_sym) &&
                    !node_modifies_or_addrs_sym(n->rhs, base_orig) &&
                    (!idx_sym || (!node_modifies_or_addrs_sym(n->rhs, idx_sym) &&
                                  !node_modifies_or_addrs_sym(n->rhs, idx_orig)))) {
                    if (is_simple_expr(n->rhs)) gen_simple_into_reg(n->rhs, RAX);
                    else                        gen_expr(cg, n->rhs);
                    x_store_sib_disp_sized(base_sym->hw_reg, idx_reg, elem_t->size,
                                           (int32_t)disp_bytes, RAX, elem_t->size);
                    return;
                }
            }
        }
        if (n->op == T_ASSIGN && elem_t->kind != TY_ARRAY && !is_aggregate_type(elem_t) &&
            elem_t->kind != TY_SLICE && elem_t != &ty_str) {
            if (n->rhs->is_const && elem_t->size == 1 && (elem_t->kind == TY_INT || elem_t == &ty_bool)) {
                gen_lvalue_addr(cg, n->lhs);
                x_store8_imm(RAX, 0, (uint8_t)n->rhs->cval);
                return;
            }
            if (is_simple_expr(n->rhs)) {
                gen_lvalue_addr(cg, n->lhs);
                gen_simple_into_reg(n->rhs, RCX);
                x_store_sized(RAX, 0, RCX, elem_t->size);
                return;
            }
            if (is_call_free_expr(n->rhs, cg->release) && cg->scratch_depth < 4) {
                gen_lvalue_addr(cg, n->lhs);
                int sreg = R8 + cg->scratch_depth++;
                x_mov_rr(1, sreg, RAX);
                gen_expr(cg, n->rhs);
                cg->scratch_depth--;
                x_store_sized(sreg, 0, RAX, elem_t->size);
                return;
            }
        }
        gen_lvalue_addr(cg, n->lhs);
        int s_addr = push_temp(cg);
        if (n->op == T_ASSIGN) {
            gen_init_at_addr(cg, s_addr, n->rhs, elem_t);
            cg->temp_depth--;
            return;
        }
        gen_expr(cg, n->rhs);
        x_mov_rr(1, RCX, RAX); // RCX = rhs
        pop_temp(cg, s_addr, R8);
        x_load_sized(RAX, R8, 0, elem_t->size, elem_t->is_signed);
        int s_addr2 = push_temp(cg); // preserve R8 across gen_int_op if needed
        x_store(RBP, slot_disp(s_addr2), R8);
        if (elem_t->kind == TY_PTR) {
            scale_reg_by_size(RCX, elem_t->elem->size, RDX);
            x_alu_rr(n->op == T_PLUS_ASSIGN ? ALU_ADD : ALU_SUB, 1, RAX, RCX);
        } else if (elem_t->kind == TY_FLOAT) {
            gen_float_op(assign_to_binop(n->op), elem_t);
        } else {
            gen_int_op(cg, assign_to_binop(n->op), elem_t, n->pos);
        }
        pop_temp(cg, s_addr2, R8);
        x_store_sized(R8, 0, RAX, elem_t->size);
        return;
    }

    Symbol *s = n->sym;
    if (n->op == T_ASSIGN) {
        if (s->type->kind == TY_ARRAY || is_aggregate_type(s->type)) {
            if (s->is_global) x_lea_rip(RAX, SEC_DATA, s->data_off);
            else              x_lea(RAX, RBP, slot_disp(s->slot));
            int s_addr = push_temp(cg);
            gen_init_at_addr(cg, s_addr, n->rhs, s->type);
            cg->temp_depth--;
            return;
        }
        if (s->type->kind == TY_SLICE || s->type == &ty_str) {
            gen_collection_ptr_len(cg, n->rhs);
            store_var(s, RAX);
            return;
        }
        if (s->xmm_reg >= 0 && s->type == &ty_f64) {
            uint64_t fbits = 0;
            if (is_direct_xmm_const(n->rhs, &fbits) && fbits == 0) {
                x_xorpd(s->xmm_reg, s->xmm_reg);
                return;
            }
            if (can_eval_xmm_in_place(n->rhs, s->xmm_reg)) {
                gen_float_xmm(cg, n->rhs, s->xmm_reg);
                return;
            }
            if (can_eval_xmm(n->rhs, 0)) {
                gen_float_xmm(cg, n->rhs, 0);
                x_movsd_rr(s->xmm_reg, 0);
                return;
            }
        }
        if (s->hw_reg >= 0 && is_simple_expr(n->rhs)) {
            gen_simple_into_reg(n->rhs, s->hw_reg);
            return;
        }
        if (cg->release && s->hw_reg >= 0 && !s->is_const_alias &&
            s->type->kind == TY_INT && s->type->size == 8 &&
            try_gen_additive_spine(cg, n->rhs, s)) {
            return;
        }
        gen_expr(cg, n->rhs);
        store_var(s, RAX);
        return;
    }

    // Fast in-register compound assignment (`i += 1`, `x -= 2`, `mask |= 4`, `x <<= 1`, ...).
    int32_t imm32 = 0;
    if (s->hw_reg >= 0 && s->type->kind == TY_INT) {
        int r = s->hw_reg;
        TokKind binop = assign_to_binop(n->op);
        if ((binop == T_SHL || binop == T_SHR) && n->rhs->is_const) {
            int bits = s->type->size * 8;
            if (n->rhs->cval >= 0 && n->rhs->cval < bits) {
                if (n->rhs->cval > 0) {
                    int ext = (binop == T_SHL) ? 4 : (s->type->is_signed ? 7 : 5);
                    x_shift_imm(ext, 1, r, (uint8_t)n->rhs->cval);
                }
                normalize_reg(r, s->type);
                return;
            }
        }
        if (fits_imm32(n->rhs, &imm32)) {
            if (binop == T_AMP || binop == T_PIPE || binop == T_CARET) {
                int alu = (binop == T_AMP) ? ALU_AND : (binop == T_PIPE) ? ALU_OR : ALU_XOR;
                x_alu_ri(alu, 1, r, imm32);
                normalize_reg(r, s->type);
                return;
            }
            if (binop == T_PLUS || binop == T_PLUSW || binop == T_MINUS || binop == T_MINUSW) {
                bool is_wrap = (binop == T_PLUSW || binop == T_MINUSW);
                int alu = (binop == T_PLUS || binop == T_PLUSW) ? ALU_ADD : ALU_SUB;
                if (is_wrap || cg->release) {
                    x_alu_ri(alu, 1, r, imm32);
                    normalize_reg(r, s->type);
                    return;
                }
                if (s->type->size == 8) {
                    x_alu_ri(alu, 1, r, imm32);
                    emit_trap_if(cg, s->type->is_signed ? CC_O : CC_B, n->pos,
                                 alu == ALU_ADD ? "integer addition overflow" : "integer subtraction overflow");
                    return;
                }
            }
        }
        if ((cg->release || binop == T_PLUSW || binop == T_MINUSW || binop == T_STARW ||
             binop == T_AMP || binop == T_PIPE || binop == T_CARET) &&
            (binop == T_PLUS || binop == T_PLUSW || binop == T_MINUS || binop == T_MINUSW ||
             binop == T_STAR || binop == T_STARW || binop == T_AMP || binop == T_PIPE || binop == T_CARET)) {
            int rhs_reg = RAX;
            if (n->rhs->kind == N_IDENT && !n->rhs->implicit_slice && n->rhs->sym && n->rhs->sym->hw_reg >= 0) {
                rhs_reg = n->rhs->sym->hw_reg;
            } else {
                gen_expr(cg, n->rhs);
            }
            if (binop == T_STAR || binop == T_STARW) {
                x_imul_rr(1, r, rhs_reg);
            } else {
                int alu = (binop == T_PLUS || binop == T_PLUSW)   ? ALU_ADD
                        : (binop == T_MINUS || binop == T_MINUSW) ? ALU_SUB
                        : (binop == T_AMP)                        ? ALU_AND
                        : (binop == T_PIPE)                       ? ALU_OR : ALU_XOR;
                x_alu_rr(alu, 1, r, rhs_reg);
            }
            normalize_reg(r, s->type);
            return;
        }
    }

    // Fast XMM compound assignment (`acc += f64_expr`, `acc *= f64_expr`).
    if (s->type == &ty_f64) {
        TokKind binop = assign_to_binop(n->op);
        if (binop == T_PLUS || binop == T_MINUS || binop == T_STAR || binop == T_SLASH) {
            uint8_t op = (binop == T_PLUS)  ? 0x58
                       : (binop == T_MINUS) ? 0x5C
                       : (binop == T_STAR)  ? 0x59 : 0x5E;
            if (s->xmm_reg >= 0) {
                uint64_t fbits = 0;
                if (is_direct_xmm_reg(n->rhs)) {
                    x_sse_sd(op, s->xmm_reg, n->rhs->sym->xmm_reg);
                    return;
                }
                if (is_direct_xmm_const(n->rhs, &fbits)) {
                    x_sse_sd_rip(op, s->xmm_reg, SEC_RDATA, add_f64_const(cg, fbits));
                    return;
                }
                if (can_eval_xmm(n->rhs, 0)) {
                    gen_float_xmm(cg, n->rhs, 0);
                } else {
                    gen_expr(cg, n->rhs);
                    x_movq_xmm_gpr(0, RAX);
                }
                x_sse_sd(op, s->xmm_reg, 0);
                return;
            }
            if (can_eval_xmm(n->rhs, 1)) {
                if (s->hw_reg >= 0) {
                    x_movq_xmm_gpr(0, s->hw_reg);
                } else {
                    load_var(s, RAX);
                    x_movq_xmm_gpr(0, RAX);
                }
                gen_float_xmm(cg, n->rhs, 1);
                x_sse_sd(op, 0, 1);
                if (s->hw_reg >= 0) {
                    x_movq_gpr_xmm(s->hw_reg, 0);
                } else {
                    x_movq_gpr_xmm(RAX, 0);
                    store_var(s, RAX);
                }
                return;
            }
        }
    }

    if (is_simple_expr(n->rhs)) {
        load_var(s, RAX);
        gen_simple_into_reg(n->rhs, RCX);
    } else {
        gen_expr(cg, n->rhs);
        x_mov_rr(1, RCX, RAX);
        load_var(s, RAX);
    }
    if (s->type->kind == TY_PTR) {
        scale_reg_by_size(RCX, s->type->elem->size, RDX);
        x_alu_rr(n->op == T_PLUS_ASSIGN ? ALU_ADD : ALU_SUB, 1, RAX, RCX);
    } else if (s->type->kind == TY_FLOAT) {
        gen_float_op(assign_to_binop(n->op), s->type);
    } else {
        gen_int_op(cg, assign_to_binop(n->op), s->type, n->pos);
    }
    store_var(s, RAX);
}

// Emits a fused conditional jump to `target` when `cond` evaluates to `jump_if_true`.
static void gen_cond_jump(CG *cg, Node *cond, int target, bool jump_if_true) {
    if (cond->kind == N_UNARY && cond->op == T_BANG) {
        gen_cond_jump(cg, cond->lhs, target, !jump_if_true);
        return;
    }
    if (cond->kind == N_BINARY && cond->op == T_ANDAND) {
        if (!jump_if_true) {
            gen_cond_jump(cg, cond->lhs, target, false);
            gen_cond_jump(cg, cond->rhs, target, false);
        } else {
            int l_skip = x_label();
            gen_cond_jump(cg, cond->lhs, l_skip, false);
            gen_cond_jump(cg, cond->rhs, target, true);
            x_bind(l_skip);
        }
        return;
    }
    if (cond->kind == N_BINARY && cond->op == T_OROR) {
        if (jump_if_true) {
            gen_cond_jump(cg, cond->lhs, target, true);
            gen_cond_jump(cg, cond->rhs, target, true);
        } else {
            int l_true = x_label();
            gen_cond_jump(cg, cond->lhs, l_true, true);
            gen_cond_jump(cg, cond->rhs, target, false);
            x_bind(l_true);
        }
        return;
    }
    if (cond->kind == N_BINARY && cond->lhs->type->kind == TY_FLOAT &&
        (cond->op == T_LT || cond->op == T_LE || cond->op == T_GT || cond->op == T_GE)) {
        uint64_t fbits = 0;
        bool rhs_xmm = is_direct_xmm_reg(cond->rhs);
        bool rhs_const_gt = (cond->op == T_GT || cond->op == T_GE) && is_direct_xmm_const(cond->rhs, &fbits);
        if (can_eval_xmm(cond->lhs, 0) &&
            (rhs_xmm || rhs_const_gt || can_eval_xmm(cond->rhs, 1))) {
            int lx = 0;
            if (is_direct_xmm_reg(cond->lhs)) {
                lx = cond->lhs->sym->xmm_reg;
            } else {
                gen_float_xmm(cg, cond->lhs, 0);
            }
            if (rhs_xmm) {
                int rx = cond->rhs->sym->xmm_reg;
                if (cond->op == T_GT) {
                    x_ucomisd(lx, rx);
                    x_jcc(jump_if_true ? CC_A : CC_BE, target);
                } else if (cond->op == T_GE) {
                    x_ucomisd(lx, rx);
                    x_jcc(jump_if_true ? CC_AE : CC_B, target);
                } else if (cond->op == T_LT) {
                    x_ucomisd(rx, lx);
                    x_jcc(jump_if_true ? CC_A : CC_BE, target);
                } else { // T_LE
                    x_ucomisd(rx, lx);
                    x_jcc(jump_if_true ? CC_AE : CC_B, target);
                }
                return;
            }
            if (rhs_const_gt) {
                x_ucomisd_rip(lx, SEC_RDATA, add_f64_const(cg, fbits));
                if (cond->op == T_GT) x_jcc(jump_if_true ? CC_A : CC_BE, target);
                else                  x_jcc(jump_if_true ? CC_AE : CC_B, target);
                return;
            }
            gen_float_xmm(cg, cond->rhs, 1);
            if (cond->op == T_GT) {
                x_ucomisd(lx, 1);
                x_jcc(jump_if_true ? CC_A : CC_BE, target);
            } else if (cond->op == T_GE) {
                x_ucomisd(lx, 1);
                x_jcc(jump_if_true ? CC_AE : CC_B, target);
            } else if (cond->op == T_LT) {
                x_ucomisd(1, lx);
                x_jcc(jump_if_true ? CC_A : CC_BE, target);
            } else { // T_LE
                x_ucomisd(1, lx);
                x_jcc(jump_if_true ? CC_AE : CC_B, target);
            }
            return;
        }
    }
    if (cond->kind == N_BINARY && cond->lhs->type->kind != TY_FLOAT &&
        cond->lhs->type != &ty_str &&
        (cond->op == T_EQ || cond->op == T_NE || cond->op == T_LT ||
         cond->op == T_LE || cond->op == T_GT || cond->op == T_GE)) {
        int cc = CC_E;
        bool s = cond->lhs->type->is_signed;
        switch (cond->op) {
        case T_EQ: cc = CC_E; break;
        case T_NE: cc = CC_NE; break;
        case T_LT: cc = s ? CC_L  : CC_B;  break;
        case T_LE: cc = s ? CC_LE : CC_BE; break;
        case T_GT: cc = s ? CC_G  : CC_A;  break;
        case T_GE: cc = s ? CC_GE : CC_AE; break;
        default: break;
        }
        if (!jump_if_true) cc ^= 1;

        int32_t imm32 = 0;
        bool lhs_in_reg = (cond->lhs->kind == N_IDENT && !cond->lhs->implicit_slice &&
                           !cond->lhs->sym->is_const_alias && cond->lhs->sym->hw_reg >= 0);
        bool rhs_in_reg = (cond->rhs->kind == N_IDENT && !cond->rhs->implicit_slice &&
                           !cond->rhs->sym->is_const_alias && cond->rhs->sym->hw_reg >= 0);
        Symbol *lhs_orig = (lhs_in_reg && cond->lhs->sym->alias_of) ? cond->lhs->sym->alias_of : (lhs_in_reg ? cond->lhs->sym : NULL);
        Symbol *rhs_orig = (rhs_in_reg && cond->rhs->sym->alias_of) ? cond->rhs->sym->alias_of : (rhs_in_reg ? cond->rhs->sym : NULL);

        if (lhs_in_reg && fits_imm32(cond->rhs, &imm32)) {
            x_alu_ri(ALU_CMP, 1, cond->lhs->sym->hw_reg, imm32);
        } else if (lhs_in_reg && rhs_in_reg) {
            x_alu_rr(ALU_CMP, 1, cond->lhs->sym->hw_reg, cond->rhs->sym->hw_reg);
        } else if (lhs_in_reg && is_simple_expr(cond->rhs)) {
            gen_simple_into_reg(cond->rhs, RCX);
            x_alu_rr(ALU_CMP, 1, cond->lhs->sym->hw_reg, RCX);
        } else if (lhs_in_reg && is_call_free_expr(cond->rhs, cg->release) &&
                   !node_modifies_or_addrs_sym(cond->rhs, cond->lhs->sym) &&
                   !node_modifies_or_addrs_sym(cond->rhs, lhs_orig)) {
            gen_expr(cg, cond->rhs);
            x_alu_rr(ALU_CMP, 1, cond->lhs->sym->hw_reg, RAX);
        } else if (rhs_in_reg && is_call_free_expr(cond->lhs, cg->release) &&
                   !node_modifies_or_addrs_sym(cond->lhs, cond->rhs->sym) &&
                   !node_modifies_or_addrs_sym(cond->lhs, rhs_orig)) {
            gen_expr(cg, cond->lhs);
            x_alu_rr(ALU_CMP, 1, RAX, cond->rhs->sym->hw_reg);
        } else if (fits_imm32(cond->rhs, &imm32)) {
            gen_expr(cg, cond->lhs);
            x_alu_ri(ALU_CMP, 1, RAX, imm32);
        } else if (is_simple_expr(cond->rhs)) {
            gen_expr(cg, cond->lhs);
            gen_simple_into_reg(cond->rhs, RCX);
            x_alu_rr(ALU_CMP, 1, RAX, RCX);
        } else if (is_simple_expr(cond->lhs) && is_call_free_expr(cond->rhs, cg->release)) {
            gen_expr(cg, cond->rhs);
            gen_simple_into_reg(cond->lhs, RCX);
            x_alu_rr(ALU_CMP, 1, RCX, RAX);
        } else if (is_call_free_expr(cond->rhs, cg->release) && cg->scratch_depth < 4) {
            gen_expr(cg, cond->lhs);
            int sreg = R8 + cg->scratch_depth++;
            x_mov_rr(1, sreg, RAX);
            gen_expr(cg, cond->rhs);
            cg->scratch_depth--;
            x_alu_rr(ALU_CMP, 1, sreg, RAX);
        } else {
            gen_expr(cg, cond->lhs);
            int slot = push_temp(cg);
            gen_expr(cg, cond->rhs);
            x_mov_rr(1, RCX, RAX);
            pop_temp(cg, slot, RAX);
            x_alu_rr(ALU_CMP, 1, RAX, RCX);
        }
        x_jcc(cc, target);
        return;
    }

    if (cond->kind == N_IDENT && !cond->implicit_slice && cond->sym->hw_reg >= 0) {
        x_test_rr(0, cond->sym->hw_reg, cond->sym->hw_reg);
        x_jcc(jump_if_true ? CC_NE : CC_E, target);
        return;
    }

    gen_expr(cg, cond);
    x_test_rr(0, RAX, RAX);
    x_jcc(jump_if_true ? CC_NE : CC_E, target);
}

static bool match_unroll_mem_term(Node *t, Symbol *idx_sym, Node **out_mem,
                                  int64_t *out_mul, int *out_shift) {
    if (!t || !t->type || t->type->kind != TY_INT || t->type->size != 8) return false;
    Node *mem = NULL;
    int64_t mul = 1;
    int sh = 0;
    if (t->kind == N_INDEX) {
        mem = t;
    } else if (t->kind == N_BINARY && (t->op == T_STAR || t->op == T_STARW)) {
        int32_t imm = 0;
        if (t->lhs && t->lhs->kind == N_INDEX && fits_imm32(t->rhs, &imm)) {
            mem = t->lhs;
            mul = imm;
        } else if (t->rhs && t->rhs->kind == N_INDEX && fits_imm32(t->lhs, &imm)) {
            mem = t->rhs;
            mul = imm;
        } else {
            return false;
        }
    } else if (t->kind == N_BINARY && t->op == T_SHL && t->lhs && t->lhs->kind == N_INDEX &&
               t->rhs && t->rhs->is_const && t->rhs->cval >= 0 && t->rhs->cval < 64) {
        mem = t->lhs;
        sh = (int)t->rhs->cval;
    } else {
        return false;
    }

    if (!mem || !mem->type || mem->type->kind != TY_INT || mem->type->size != 8) return false;
    if (!mem->lhs || mem->lhs->kind != N_IDENT || mem->lhs->implicit_slice || !mem->lhs->sym) return false;
    if (mem->lhs->sym == idx_sym || mem->lhs->sym == idx_sym->alias_of) return false;
    if (!mem->rhs || mem->rhs->kind != N_IDENT || mem->rhs->implicit_slice || mem->rhs->sym != idx_sym) {
        return false;
    }

    *out_mem = mem;
    *out_mul = mul;
    *out_shift = sh;
    return true;
}

typedef struct {
    Node *inv[8];
    int inv_sign[8];
    int inv_count;
    Node *mem;
    bool mem_neg;
    int64_t mem_mul;
    int mem_shift;
    bool has_mem;
} UnrollPlan;

static bool collect_unroll_terms(Node *t, int sign, Symbol *idx_sym, Symbol *acc_sym, UnrollPlan *p) {
    if (!t || !t->type || t->type->kind != TY_INT || t->type->size != 8) return false;
    if (t->kind == N_BINARY &&
        (t->op == T_PLUS || t->op == T_PLUSW || t->op == T_MINUS || t->op == T_MINUSW)) {
        int rsign = (t->op == T_PLUS || t->op == T_PLUSW) ? sign : -sign;
        return collect_unroll_terms(t->lhs, sign, idx_sym, acc_sym, p) &&
               collect_unroll_terms(t->rhs, rsign, idx_sym, acc_sym, p);
    }

    Node *mem = NULL;
    int64_t mul = 1;
    int sh = 0;
    if (match_unroll_mem_term(t, idx_sym, &mem, &mul, &sh)) {
        if (p->has_mem) return false;
        p->mem = mem;
        p->mem_neg = (sign < 0);
        p->mem_mul = mul;
        p->mem_shift = sh;
        p->has_mem = true;
        return true;
    }

    Symbol *idx_orig = idx_sym->alias_of ? idx_sym->alias_of : idx_sym;
    Symbol *acc_orig = acc_sym->alias_of ? acc_sym->alias_of : acc_sym;
    if (p->inv_count >= 8 || !is_call_free_expr(t, true) ||
        node_reads_sym(t, idx_sym) || node_reads_sym(t, idx_orig) ||
        node_reads_sym(t, acc_sym) || node_reads_sym(t, acc_orig) ||
        node_modifies_or_addrs_sym(t, idx_sym) || node_modifies_or_addrs_sym(t, idx_orig) ||
        node_modifies_or_addrs_sym(t, acc_sym) || node_modifies_or_addrs_sym(t, acc_orig)) {
        return false;
    }
    p->inv[p->inv_count] = t;
    p->inv_sign[p->inv_count] = sign;
    p->inv_count++;
    return true;
}

// Emits an 8x unrolled inner loop with 4 parallel accumulators (RAX, RCX, RDX, R8)
// for `--release` forward unit-step range loops whose body is a single `acc += ... + base[i] * C`.
// Always leaves remaining 0..7 elements for the caller's scalar loop (or branches to `l_brk` if 0).
static bool try_gen_unrolled_slice_sum_loop(CG *cg, Node *n, bool hi_imm, int32_t end_imm,
                                            int fixed_end_reg, int32_t end_disp,
                                            int l_cond, int l_brk) {
    if (!cg->release || !n->sym || n->sym->is_const_alias || n->sym->hw_reg < 0) return false;
    if (!n->type || n->type->kind != TY_INT || n->type->size != 8) return false;
    if (!n->body || n->body->kind != N_BLOCK || n->body->list.len != 1) return false;

    Node *stmt = n->body->list.items[0];
    if (!stmt || stmt->kind != N_ASSIGN || stmt->lhs) return false;
    Symbol *acc_sym = stmt->sym;
    if (!acc_sym || acc_sym->is_const_alias || acc_sym->hw_reg < 0 ||
        !acc_sym->type || acc_sym->type->kind != TY_INT || acc_sym->type->size != 8) {
        return false;
    }

    Symbol *idx_sym = n->sym;
    if (acc_sym == idx_sym || acc_sym == idx_sym->alias_of || acc_sym->hw_reg == idx_sym->hw_reg) {
        return false;
    }

    Node *rhs = NULL;
    if (stmt->op == T_PLUS_ASSIGN) {
        rhs = stmt->rhs;
    } else if (stmt->op == T_ASSIGN && stmt->rhs && stmt->rhs->kind == N_BINARY &&
               (stmt->rhs->op == T_PLUS || stmt->rhs->op == T_PLUSW) &&
               stmt->rhs->lhs && stmt->rhs->lhs->kind == N_IDENT &&
               !stmt->rhs->lhs->implicit_slice && stmt->rhs->lhs->sym == acc_sym) {
        rhs = stmt->rhs->rhs;
    } else {
        return false;
    }

    UnrollPlan p = {0};
    if (!collect_unroll_terms(rhs, 1, idx_sym, acc_sym, &p) || !p.has_mem) return false;

    Symbol *base_sym = p.mem->lhs->sym;
    if (base_sym == acc_sym || base_sym == acc_sym->alias_of ||
        base_sym == idx_sym || base_sym == idx_sym->alias_of) {
        return false;
    }
    Type *bt = base_sym->type;
    if (bt->kind != TY_SLICE && bt->kind != TY_PTR &&
        !(bt->kind == TY_ARRAY && is_range_loop_in_bounds(cg, p.mem->lhs, p.mem->rhs))) {
        return false;
    }

    int r_i = idx_sym->hw_reg;
    int r_acc = acc_sym->hw_reg;

    if (hi_imm)                 x_mov_ri(R11, (uint64_t)(int64_t)end_imm);
    else if (fixed_end_reg >= 0) x_mov_rr(1, R11, fixed_end_reg);
    else                        x_load(R11, RBP, end_disp);

    x_alu_rr(ALU_SUB, 1, R11, r_i);
    x_shift_imm(5, 1, R11, 3); // R11 = trip_count / 8
    x_test_rr(1, R11, R11);
    x_jcc(CC_E, l_cond);

    bool has_inv = (p.inv_count > 0);
    if (has_inv) {
        x_push(R11);
        gen_expr(cg, p.inv[0]);
        if (p.inv_sign[0] < 0) x_f7(3, 1, RAX);
        for (int k = 1; k < p.inv_count; k++) {
            int sreg = R8;
            x_mov_rr(1, sreg, RAX);
            cg->scratch_depth++;
            gen_expr(cg, p.inv[k]);
            cg->scratch_depth--;
            if (p.inv_sign[k] < 0) x_alu_rr(ALU_SUB, 1, sreg, RAX);
            else                   x_alu_rr(ALU_ADD, 1, sreg, RAX);
            x_mov_rr(1, RAX, sreg);
        }
        x_shift_imm(4, 1, RAX, 3); // inv * 8 per chunk
        x_mov_rr(1, R10, RAX);
        x_pop(R11);
    }

    int breg = R9;
    if ((bt->kind == TY_SLICE || bt->kind == TY_PTR) && base_sym->hw_reg >= 0) {
        breg = base_sym->hw_reg;
    } else if (bt->kind == TY_SLICE) {
        x_load(R9, RBP, slot_disp(base_sym->slot));
    } else if (bt->kind == TY_ARRAY) {
        if (base_sym->is_global)      x_lea_rip(R9, SEC_DATA, base_sym->data_off);
        else if (base_sym->kind == SYM_PARAM) x_load(R9, RBP, slot_disp(base_sym->slot));
        else                          x_lea(R9, RBP, slot_disp(base_sym->slot));
    } else {
        load_var(base_sym, R9);
    }

    x_mov_ri(RAX, 0);
    x_mov_ri(RCX, 0);
    x_mov_ri(RDX, 0);
    x_mov_ri(R8, 0);

    int l_unroll = x_label();
    x_bind(l_unroll);
    x_alu_rm_sib_disp(ALU_ADD, 1, RAX, breg, r_i, 8, 0);
    x_alu_rm_sib_disp(ALU_ADD, 1, RCX, breg, r_i, 8, 8);
    x_alu_rm_sib_disp(ALU_ADD, 1, RDX, breg, r_i, 8, 16);
    x_alu_rm_sib_disp(ALU_ADD, 1, R8,  breg, r_i, 8, 24);
    x_alu_rm_sib_disp(ALU_ADD, 1, RAX, breg, r_i, 8, 32);
    x_alu_rm_sib_disp(ALU_ADD, 1, RCX, breg, r_i, 8, 40);
    x_alu_rm_sib_disp(ALU_ADD, 1, RDX, breg, r_i, 8, 48);
    x_alu_rm_sib_disp(ALU_ADD, 1, R8,  breg, r_i, 8, 56);
    if (has_inv) x_alu_rr(ALU_ADD, 1, r_acc, R10);
    x_alu_ri(ALU_ADD, 1, r_i, 8);
    x_alu_ri(ALU_SUB, 1, R11, 1);
    x_jcc(CC_NE, l_unroll);

    x_alu_rr(ALU_ADD, 1, RAX, RCX);
    x_alu_rr(ALU_ADD, 1, RDX, R8);
    x_alu_rr(ALU_ADD, 1, RAX, RDX);
    if (p.mem_shift > 0) x_shift_imm(4, 1, RAX, (uint8_t)p.mem_shift);
    else if (p.mem_mul != 1) x_imul_rri(1, RAX, RAX, (int32_t)p.mem_mul);
    x_alu_rr(p.mem_neg ? ALU_SUB : ALU_ADD, 1, r_acc, RAX);

    if (hi_imm)                  x_alu_ri(ALU_CMP, 1, r_i, end_imm);
    else if (fixed_end_reg >= 0) x_alu_rr(ALU_CMP, 1, r_i, fixed_end_reg);
    else                         x_alu_rm(ALU_CMP, 1, r_i, RBP, end_disp);
    x_jcc(CC_E, l_brk);
    return true;
}

// Spec §6.6: range loop `loop i in a..b [by s]` and `rev(...)`.
// Evaluates a, b, s once; checks s > 0; computes exact trip count with
// unsigned division so the loop counter arithmetic can NEVER overflow.
static void gen_range_loop(CG *cg, Node *n) {
    Type *t = n->type;
    int s_start = n->slot_start;
    int s_end   = n->slot_end;
    int s_step  = n->slot_step;

    int l_cond = x_label();
    int l_cont = x_label();
    int l_brk  = x_label();
    n->label_continue = l_cont;
    n->label_break    = l_brk;
    n->defer_depth    = cg->defers.len;

    // Fast path for forward unit-step range loops `loop i in a..b`:
    // No division needed; increments `i` by 1 until `i == end`.
    if (!n->rev && (!n->by || (n->by->is_const && n->by->cval == 1))) {
        int32_t end_imm = 0;
        bool hi_imm = fits_imm32(n->hi, &end_imm);
        bool hi_reg_var = (!hi_imm && n->hi->kind == N_IDENT && !n->hi->implicit_slice &&
                           n->hi->sym && !n->hi->sym->is_const_alias && n->hi->sym->hw_reg >= 0 &&
                           n->hi->sym != n->sym &&
                           !node_modifies_or_addrs_sym(n->body, n->hi->sym) &&
                           (!n->hi->sym->alias_of || !node_modifies_or_addrs_sym(n->body, n->hi->sym->alias_of)));
        int end_reg = -1;
        bool pushed_end_reg = false;
        if (!hi_imm && !hi_reg_var && cg->num_free_callee > 0) {
            end_reg = cg->free_callee_regs[--cg->num_free_callee];
            pushed_end_reg = true;
        }
        int fixed_end_reg = hi_reg_var ? n->hi->sym->hw_reg : end_reg;

        gen_expr(cg, n->lo);
        x_store(RBP, slot_disp(s_start), RAX);
        gen_expr(cg, n->hi);
        if (end_reg >= 0) x_mov_rr(1, end_reg, RAX);
        else if (!hi_imm && !hi_reg_var) x_store(RBP, slot_disp(s_end), RAX);
        x_mov_rr(1, RCX, RAX);
        x_load(RAX, RBP, slot_disp(s_start));
        x_alu_rr(ALU_CMP, 1, RAX, RCX);
        x_jcc(t->is_signed ? CC_GE : CC_AE, l_brk);

        store_var(n->sym, RAX);

        bool orig_nonneg = n->sym->known_nonneg;
        if (is_nonneg_expr(n->lo) && !node_modifies_or_addrs_sym(n->body, n->sym)) {
            n->sym->known_nonneg = true;
        }

        if (cg->num_range_loops < 8) cg->range_loops[cg->num_range_loops++] = n;
        try_gen_unrolled_slice_sum_loop(cg, n, hi_imm, end_imm, fixed_end_reg, slot_disp(s_end), l_cond, l_brk);

        x_bind(l_cond);
        gen_block(cg, n->body);
        if (cg->num_range_loops > 0 && cg->range_loops[cg->num_range_loops - 1] == n) cg->num_range_loops--;
        n->sym->known_nonneg = orig_nonneg;

        x_bind(l_cont);
        if (n->sym->hw_reg >= 0) {
            int r = n->sym->hw_reg;
            x_alu_ri(ALU_ADD, 1, r, 1);
            if (hi_imm)                  x_alu_ri(ALU_CMP, 1, r, end_imm);
            else if (fixed_end_reg >= 0) x_alu_rr(ALU_CMP, 1, r, fixed_end_reg);
            else                         x_alu_rm(ALU_CMP, 1, r, RBP, slot_disp(s_end));
            x_jcc(CC_NE, l_cond);
        } else {
            load_var(n->sym, RAX);
            x_alu_ri(ALU_ADD, 1, RAX, 1);
            store_var(n->sym, RAX);
            if (hi_imm)                  x_alu_ri(ALU_CMP, 1, RAX, end_imm);
            else if (fixed_end_reg >= 0) x_alu_rr(ALU_CMP, 1, RAX, fixed_end_reg);
            else                         x_alu_rm(ALU_CMP, 1, RAX, RBP, slot_disp(s_end));
            x_jcc(CC_NE, l_cond);
        }
        x_bind(l_brk);
        if (pushed_end_reg) cg->free_callee_regs[cg->num_free_callee++] = end_reg;
        return;
    }

    // 1. Evaluate `lo`, `hi`, `by` once, in order.
    gen_expr(cg, n->lo);
    x_store(RBP, slot_disp(s_start), RAX);
    gen_expr(cg, n->hi);
    x_store(RBP, slot_disp(s_end), RAX);
    if (n->by) {
        gen_expr(cg, n->by);
        if (!n->by->is_const) {
            if (t->is_signed) {
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_LE, n->by->pos, "range loop step must be > 0");
            } else {
                x_test_rr(1, RAX, RAX);
                emit_trap_if(cg, CC_E, n->by->pos, "range loop step must be > 0");
            }
        }
    } else {
        x_mov_ri(RAX, 1);
    }
    x_store(RBP, slot_disp(s_step), RAX);

    // 2. If start >= end, skip the loop entirely.
    x_load(RAX, RBP, slot_disp(s_start));
    x_load(RCX, RBP, slot_disp(s_end));
    x_alu_rr(ALU_CMP, 1, RAX, RCX);
    x_jcc(t->is_signed ? CC_GE : CC_AE, l_brk);

    // 3. Trip count = (uint64(end - start) - 1) / uint64(step) + 1.
    //    Since start < end, uint64(end - start) >= 1, so -1 never underflows,
    //    and +1 at the end can only wrap to 0 if there are 2^64 iterations.
    x_mov_rr(1, RAX, RCX);
    x_alu_rm(ALU_SUB, 1, RAX, RBP, slot_disp(s_start)); // RAX = end - start
    x_alu_ri(ALU_SUB, 1, RAX, 1);                       // RAX = (end - start) - 1
    x_mov_ri(RDX, 0);
    x_load(RCX, RBP, slot_disp(s_step));
    x_f7(6, 1, RCX);                                    // unsigned div -> RAX = (diff - 1) / step
    if (n->rev) {
        // For rev(a..b by s), initial i = start + RAX * step; cur = RAX + 1.
        x_mov_rr(1, R8, RAX);
        x_imul_rr(1, RAX, RCX);
        x_alu_rm(ALU_ADD, 1, RAX, RBP, slot_disp(s_start));
        normalize_rax(t);
        store_var(n->sym, RAX);                         // i = last element
        x_alu_ri(ALU_ADD, 1, R8, 1);
        x_store(RBP, slot_disp(s_end), R8);             // s_end holds remaining count
    } else {
        x_alu_ri(ALU_ADD, 1, RAX, 1);
        x_store(RBP, slot_disp(s_end), RAX);            // s_end holds remaining count
        x_load(RAX, RBP, slot_disp(s_start));
        store_var(n->sym, RAX);                         // i = start
    }

    // 4. Loop body & step.
    x_bind(l_cond);
    gen_block(cg, n->body);

    x_bind(l_cont);
    x_load(RAX, RBP, slot_disp(s_end));
    x_alu_ri(ALU_SUB, 1, RAX, 1);
    x_jcc(CC_E, l_brk);
    x_store(RBP, slot_disp(s_end), RAX);

    load_var(n->sym, RAX);
    if (n->rev) x_alu_rm(ALU_SUB, 1, RAX, RBP, slot_disp(s_step));
    else        x_alu_rm(ALU_ADD, 1, RAX, RBP, slot_disp(s_step));
    normalize_rax(t);
    store_var(n->sym, RAX);
    x_jmp(l_cond);

    x_bind(l_brk);
}

static void gen_iter_loop(CG *cg, Node *n) {
    Type *elem_t = n->type;
    int s_ptr = n->slot_start;
    int s_len = n->slot_end;
    int s_idx = n->slot_step;

    // 1. Evaluate collection once -> (RAX = ptr, RDX = len).
    gen_collection_ptr_len(cg, n->lhs);
    x_store(RBP, slot_disp(s_ptr), RAX);
    x_store(RBP, slot_disp(s_len), RDX);
    x_mov_ri(RAX, 0);
    x_store(RBP, slot_disp(s_idx), RAX);

    int l_cond = x_label(), l_cont = x_label(), l_brk = x_label();
    n->label_continue = l_cont;
    n->label_break    = l_brk;
    n->defer_depth    = cg->defers.len;

    x_bind(l_cond);
    x_load(RAX, RBP, slot_disp(s_idx));
    x_load(RCX, RBP, slot_disp(s_len));
    x_alu_rr(ALU_CMP, 1, RAX, RCX);
    x_jcc(CC_AE, l_brk);

    // Load current element `ptr[idx]`.
    if (n->sym2) {
        // `loop i, x in arr`: n->sym = i, n->sym2 = x
        store_var(n->sym, RAX);
    }
    scale_reg_by_size(RAX, elem_t->size, RDX);
    x_load(RCX, RBP, slot_disp(s_ptr));
    x_alu_rr(ALU_ADD, 1, RAX, RCX);
    Symbol *val_sym = n->sym2 ? n->sym2 : n->sym;
    if (is_aggregate_type(elem_t)) {
        x_push(RSI);
        x_push(RDI);
        x_mov_rr(1, RSI, RAX);
        x_lea(RDI, RBP, slot_disp(val_sym->slot));
        x_mov_ri(RCX, (uint64_t)elem_t->size);
        x_rep_movsb();
        x_pop(RDI);
        x_pop(RSI);
    } else if (elem_t == &ty_str || elem_t->kind == TY_SLICE) {
        x_load(RDX, RAX, 8);
        x_load(RAX, RAX, 0);
        store_var(val_sym, RAX);
    } else {
        x_load_sized(RAX, RAX, 0, elem_t->size, elem_t->is_signed);
        store_var(val_sym, RAX);
    }

    gen_block(cg, n->body);

    x_bind(l_cont);
    x_load(RAX, RBP, slot_disp(s_idx));
    x_alu_ri(ALU_ADD, 1, RAX, 1);
    x_store(RBP, slot_disp(s_idx), RAX);
    x_jmp(l_cond);

    x_bind(l_brk);
}

static void emit_defers_from(CG *cg, int limit) {
    int saved_len = cg->defers.len;
    for (int i = saved_len - 1; i >= limit; i--) {
        cg->defers.len = i;
        Node *dbody = cg->defers.items[i];
        if (dbody->kind == N_BLOCK) gen_block(cg, dbody);
        else                        gen_stmt(cg, dbody);
    }
    cg->defers.len = saved_len;
}

static int count_ast_nodes(Node *n, int limit) {
    if (!n || limit <= 0) return 0;
    int c = 1;
    if (c > limit) return c;
    Node *kids[] = {n->lhs, n->rhs, n->cond, n->body, n->els, n->lo, n->hi, n->by, n->init, n->step};
    for (int i = 0; i < (int)(sizeof(kids) / sizeof(kids[0])); i++) {
        if (kids[i]) {
            c += count_ast_nodes(kids[i], limit - c + 1);
            if (c > limit) return c;
        }
    }
    for (int i = 0; i < n->list.len; i++) {
        c += count_ast_nodes(n->list.items[i], limit - c + 1);
        if (c > limit) return c;
    }
    return c;
}

static void gen_stmt(CG *cg, Node *n) {
    switch (n->kind) {
    case N_VAR:
        if (n->rhs) {
            Node assign = {0};
            assign.kind = N_ASSIGN;
            assign.pos  = n->pos;
            assign.op   = T_ASSIGN;
            assign.sym  = n->sym;
            assign.type = n->type;
            assign.rhs  = n->rhs;
            gen_assign(cg, &assign);
        }
        return;

    case N_CONST:
    case N_TYPE_ALIAS:
        return;

    case N_ASSIGN:
        gen_assign(cg, n);
        return;

    case N_EXPR_STMT:
        gen_expr(cg, n->lhs);
        return;

    case N_DEFER:
        vec_push(cg->defers, n->body);
        return;

    case N_RETURN:
        if (n->lhs) {
            if (is_aggregate_type(cg->cur_ret_type)) {
                x_load(RAX, RBP, slot_disp(cg->cur_ret_ptr_slot));
                int addr_slot = push_temp(cg);
                gen_init_at_addr(cg, addr_slot, n->lhs, cg->cur_ret_type);
                cg->temp_depth--;
                if (cg->inline_depth == 0 && cg->defers.len > 0) {
                    emit_defers_from(cg, 0);
                }
                x_load(RAX, RBP, slot_disp(cg->cur_ret_ptr_slot));
            } else if (cg->cur_ret_type == &ty_str || cg->cur_ret_type->kind == TY_SLICE) {
                gen_collection_ptr_len(cg, n->lhs);
                if (cg->inline_depth == 0 && cg->defers.len > 0) {
                    int s_ptr = push_temp(cg);
                    x_mov_rr(1, RAX, RDX);
                    int s_len = push_temp(cg);
                    emit_defers_from(cg, 0);
                    pop_temp(cg, s_len, RDX);
                    pop_temp(cg, s_ptr, RAX);
                }
            } else {
                gen_expr(cg, n->lhs);
                if (cg->inline_depth == 0 && cg->defers.len > 0) {
                    int ret_slot = push_temp(cg);
                    emit_defers_from(cg, 0);
                    pop_temp(cg, ret_slot, RAX);
                }
            }
        } else if (cg->inline_depth == 0 && cg->defers.len > 0) {
            emit_defers_from(cg, 0);
        }
        if (!(n == cg->last_top_stmt && (cg->inline_depth > 0 || cg->defers.len == 0))) {
            x_jmp(cg->cur_epilogue);
        }
        return;

    case N_BREAK:
        if (cg->defers.len > n->target->defer_depth) {
            emit_defers_from(cg, n->target->defer_depth);
        }
        x_jmp(n->target->label_break);
        return;

    case N_CONTINUE:
        if (cg->defers.len > n->target->defer_depth) {
            emit_defers_from(cg, n->target->defer_depth);
        }
        x_jmp(n->target->label_continue);
        return;

    case N_IF: {
        int l_else = x_label(), l_end = x_label();
        gen_cond_jump(cg, n->cond, n->els ? l_else : l_end, false);
        gen_block(cg, n->body);
        if (n->els) {
            x_jmp(l_end);
            x_bind(l_else);
            if (n->els->kind == N_IF) gen_stmt(cg, n->els);
            else                      gen_block(cg, n->els);
        }
        x_bind(l_end);
        return;
    }

    case N_MATCH: {
        int l_end = x_label();
        int *l_arms = n->list.len ? xmalloc((size_t)n->list.len * sizeof(int)) : NULL;
        bool is_tagged_enum = (n->cond->type->kind == TY_ENUM && n->cond->type->is_tagged);
        if (n->cond->type == &ty_str) {
            gen_expr(cg, n->cond); // RAX = ptr, RDX = len
            x_store(RBP, slot_disp(n->slot_start), RAX);
            x_store(RBP, slot_disp(n->slot_end), RDX);
            for (int i = 0; i < n->list.len; i++) {
                l_arms[i] = x_label();
                Node *arm = n->list.items[i];
                for (int j = 0; j < arm->list.len; j++) {
                    Node *pat = arm->list.items[j];
                    uint32_t off = add_string(cg, pat->str, pat->str_len);
                    x_load(RCX, RBP, slot_disp(n->slot_start));
                    x_load(RDX, RBP, slot_disp(n->slot_end));
                    x_lea_rip(R8, SEC_RDATA, off);
                    x_mov_ri(R9, (uint64_t)pat->str_len);
                    x_call(cg->rt_str_eq);
                    x_test_rr(0, RAX, RAX);
                    x_jcc(CC_NE, l_arms[i]);
                }
            }
        } else if (is_tagged_enum) {
            gen_expr(cg, n->cond); // RAX = base address of tagged union
            x_store(RBP, slot_disp(n->slot_start), RAX);
            x_load_sized(RAX, RAX, 0, 4, true); // RAX = 32-bit variant tag
            for (int i = 0; i < n->list.len; i++) {
                l_arms[i] = x_label();
                Node *arm = n->list.items[i];
                for (int j = 0; j < arm->list.len; j++) {
                    Node *pat = arm->list.items[j];
                    x_alu_ri(ALU_CMP, 1, RAX, (int32_t)pat->cval);
                    x_jcc(CC_E, l_arms[i]);
                }
            }
        } else {
            int scrut_reg = RAX;
            if (n->cond->kind == N_IDENT && !n->cond->implicit_slice && n->cond->sym->hw_reg >= 0) {
                scrut_reg = n->cond->sym->hw_reg;
            } else {
                gen_expr(cg, n->cond);
            }
            for (int i = 0; i < n->list.len; i++) {
                l_arms[i] = x_label();
                Node *arm = n->list.items[i];
                for (int j = 0; j < arm->list.len; j++) {
                    Node *pat = arm->list.items[j];
                    int32_t imm32 = 0;
                    if (fits_imm32(pat, &imm32)) {
                        x_alu_ri(ALU_CMP, 1, scrut_reg, imm32);
                    } else {
                        x_mov_ri(RCX, (uint64_t)(int64_t)pat->cval);
                        x_alu_rr(ALU_CMP, 1, scrut_reg, RCX);
                    }
                    x_jcc(CC_E, l_arms[i]);
                }
            }
        }
        int l_else = n->els ? x_label() : l_end;
        x_jmp(l_else);
        for (int i = 0; i < n->list.len; i++) {
            x_bind(l_arms[i]);
            Node *arm = n->list.items[i];
            if (is_tagged_enum && arm->sym) {
                Symbol *bs = arm->sym;
                Type *bt = bs->type;
                x_load(RAX, RBP, slot_disp(n->slot_start));
                x_alu_ri(ALU_ADD, 1, RAX, n->cond->type->payload_offset);
                if (bt->kind == TY_ARRAY || is_aggregate_type(bt)) {
                    x_push(RSI);
                    x_push(RDI);
                    x_mov_rr(1, RSI, RAX);
                    x_lea(RDI, RBP, slot_disp(bs->slot));
                    x_mov_ri(RCX, (uint64_t)bt->size);
                    x_rep_movsb();
                    x_pop(RDI);
                    x_pop(RSI);
                } else if (bt == &ty_str || bt->kind == TY_SLICE) {
                    x_load(RDX, RAX, 8);
                    x_load(RAX, RAX, 0);
                    store_var(bs, RAX);
                } else {
                    x_load_sized(RAX, RAX, 0, bt->size, bt->is_signed);
                    store_var(bs, RAX);
                }
            }
            gen_block(cg, arm->body);
            x_jmp(l_end);
        }
        if (n->els) {
            x_bind(l_else);
            gen_block(cg, n->els);
        }
        x_bind(l_end);
        free(l_arms);
        return;
    }

    case N_LOOP_INF: {
        int l_top = x_label(), l_brk = x_label();
        n->label_continue = l_top;
        n->label_break    = l_brk;
        n->defer_depth    = cg->defers.len;
        x_bind(l_top);
        gen_block(cg, n->body);
        x_jmp(l_top);
        x_bind(l_brk);
        return;
    }

    case N_LOOP_COND: {
        int l_cond = x_label(), l_brk = x_label();
        n->label_continue = l_cond;
        n->label_break    = l_brk;
        n->defer_depth    = cg->defers.len;
        if (count_ast_nodes(n->cond, 16) <= 16) {
            int l_body = x_label();
            gen_cond_jump(cg, n->cond, l_brk, false);
            x_bind(l_body);
            gen_block(cg, n->body);
            x_bind(l_cond);
            gen_cond_jump(cg, n->cond, l_body, true);
            x_bind(l_brk);
        } else {
            x_bind(l_cond);
            gen_cond_jump(cg, n->cond, l_brk, false);
            gen_block(cg, n->body);
            x_jmp(l_cond);
            x_bind(l_brk);
        }
        return;
    }

    case N_LOOP_RANGE:
        gen_range_loop(cg, n);
        return;

    case N_LOOP_ITER:
        gen_iter_loop(cg, n);
        return;

    case N_LOOP_C: {
        int l_cond = x_label(), l_cont = x_label(), l_brk = x_label();
        n->label_continue = l_cont;
        n->label_break    = l_brk;
        gen_stmt(cg, n->init);
        n->defer_depth    = cg->defers.len;
        x_bind(l_cond);
        gen_cond_jump(cg, n->cond, l_brk, false);
        gen_block(cg, n->body);
        x_bind(l_cont);
        gen_assign(cg, n->step);
        x_jmp(l_cond);
        x_bind(l_brk);
        return;
    }

    case N_BLOCK:
        gen_block(cg, n);
        return;

    default:
        die("internal error: unhandled statement in codegen");
    }
}

static bool is_reg_scalar_type(Type *t);

static bool is_speculative_safe_expr(CG *cg, Node *n) {
    if (!n) return false;
    if (n->is_const) return true;
    switch (n->kind) {
    case N_INT:
    case N_BOOL:
    case N_NULL:
    case N_FLOAT:
        return true;
    case N_IDENT:
        return !n->implicit_slice && n->sym &&
               (n->sym->kind == SYM_VAR || n->sym->kind == SYM_PARAM || n->sym->kind == SYM_CONST) &&
               is_reg_scalar_type(n->sym->type);
    case N_UNARY:
        return cg->release && n->type->kind != TY_FLOAT &&
               (n->op == T_MINUS || n->op == T_TILDE || n->op == T_BANG) &&
               is_speculative_safe_expr(cg, n->lhs);
    case N_BINARY:
        if (!cg->release || !n->type || n->type->kind != TY_INT) return false;
        if (n->op == T_SLASH || n->op == T_PERCENT || n->op == T_ANDAND || n->op == T_OROR) {
            return false;
        }
        return is_speculative_safe_expr(cg, n->lhs) && is_speculative_safe_expr(cg, n->rhs);
    default:
        return false;
    }
}

static bool try_gen_branchless_return_chain(CG *cg, Node *b) {
    if (!b || cg->defers.len > 0 || !cg->cur_ret_type || !is_reg_scalar_type(cg->cur_ret_type)) {
        return false;
    }
    if (b->list.len < 2 || b->list.len > 5) return false;

    Node *last = b->list.items[b->list.len - 1];
    if (!last || last->kind != N_RETURN || !last->lhs || !is_speculative_safe_expr(cg, last->lhs)) {
        return false;
    }

    for (int i = 0; i < b->list.len - 1; i++) {
        Node *st = b->list.items[i];
        if (!st || st->kind != N_IF || st->els || !st->body || st->body->kind != N_BLOCK ||
            st->body->list.len != 1) {
            return false;
        }
        Node *ret = st->body->list.items[0];
        if (!ret || ret->kind != N_RETURN || !ret->lhs || !is_simple_expr(ret->lhs)) {
            return false;
        }
        Node *cond = st->cond;
        if (!cond || cond->kind != N_BINARY || !cond->lhs || !cond->lhs->type ||
            cond->lhs->type->kind == TY_FLOAT || cond->lhs->type == &ty_str ||
            cond->lhs->type->kind == TY_SLICE ||
            (cond->op != T_EQ && cond->op != T_NE && cond->op != T_LT &&
             cond->op != T_LE && cond->op != T_GT && cond->op != T_GE)) {
            return false;
        }
        int32_t dummy = 0;
        if (!is_simple_expr(cond->lhs) ||
            !(fits_imm32(cond->rhs, &dummy) || is_simple_expr(cond->rhs))) {
            return false;
        }
    }

    if (is_simple_expr(last->lhs)) gen_simple_into_reg(last->lhs, RAX);
    else                             gen_expr(cg, last->lhs);

    for (int i = b->list.len - 2; i >= 0; i--) {
        Node *st = b->list.items[i];
        Node *cond = st->cond;
        Node *val = st->body->list.items[0]->lhs;
        int cc = CC_E;
        bool s = cond->lhs->type->is_signed;
        switch (cond->op) {
        case T_EQ: cc = CC_E; break;
        case T_NE: cc = CC_NE; break;
        case T_LT: cc = s ? CC_L  : CC_B;  break;
        case T_LE: cc = s ? CC_LE : CC_BE; break;
        case T_GT: cc = s ? CC_G  : CC_A;  break;
        case T_GE: cc = s ? CC_GE : CC_AE; break;
        default: break;
        }

        // Load candidate value BEFORE `cmp` because `x_mov_ri(RDX, 0)` emits `xor` (clobbers EFLAGS).
        gen_simple_into_reg(val, RDX);

        int lreg = RCX;
        if (cond->lhs->kind == N_IDENT && !cond->lhs->implicit_slice &&
            !cond->lhs->sym->is_const_alias && cond->lhs->sym->hw_reg >= 0) {
            lreg = cond->lhs->sym->hw_reg;
        } else {
            gen_simple_into_reg(cond->lhs, RCX);
        }

        int32_t imm32 = 0;
        if (fits_imm32(cond->rhs, &imm32)) {
            x_alu_ri(ALU_CMP, 1, lreg, imm32);
        } else if (cond->rhs->kind == N_IDENT && !cond->rhs->implicit_slice &&
                   !cond->rhs->sym->is_const_alias && cond->rhs->sym->hw_reg >= 0) {
            x_alu_rr(ALU_CMP, 1, lreg, cond->rhs->sym->hw_reg);
        } else {
            gen_simple_into_reg(cond->rhs, R8);
            x_alu_rr(ALU_CMP, 1, lreg, R8);
        }
        x_cmovcc(cc, 1, RAX, RDX);
    }
    return true;
}

static void gen_block(CG *cg, Node *b) {
    if (b == cg->cur_top_block && try_gen_branchless_return_chain(cg, b)) {
        return;
    }
    int defer_mark = cg->defers.len;
    for (int i = 0; i < b->list.len; i++) {
        gen_stmt(cg, b->list.items[i]);
    }
    if (cg->defers.len > defer_mark) {
        bool ends_jump = false;
        if (b->list.len > 0) {
            NodeKind lk = b->list.items[b->list.len - 1]->kind;
            if (lk == N_RETURN || lk == N_BREAK || lk == N_CONTINUE) {
                ends_jump = true;
            }
        }
        if (!ends_jump) {
            emit_defers_from(cg, defer_mark);
        }
        cg->defers.len = defer_mark;
    }
}

// ---------------------------------------------------------------------------
// Native AST Function Inlining & Parameter Register/Slot Aliasing
// ---------------------------------------------------------------------------

static bool node_modifies_or_addrs_sym(Node *n, Symbol *s) {
    if (!n) return false;
    if (n->kind == N_ASSIGN) {
        if (n->sym == s) return true;
        Node *base = n->lhs;
        while (base && (base->kind == N_FIELD || base->kind == N_INDEX)) {
            base = base->lhs;
        }
        if (base && base->kind == N_IDENT && base->sym == s) return true;
    }
    if (n->kind == N_ADDR) {
        Node *base = n->lhs;
        while (base && (base->kind == N_FIELD || base->kind == N_INDEX)) {
            base = base->lhs;
        }
        if (base && base->kind == N_IDENT && base->sym == s) return true;
    }
    if (node_modifies_or_addrs_sym(n->lhs, s)) return true;
    if (node_modifies_or_addrs_sym(n->rhs, s)) return true;
    if (node_modifies_or_addrs_sym(n->cond, s)) return true;
    if (node_modifies_or_addrs_sym(n->body, s)) return true;
    if (node_modifies_or_addrs_sym(n->els, s)) return true;
    if (node_modifies_or_addrs_sym(n->lo, s)) return true;
    if (node_modifies_or_addrs_sym(n->hi, s)) return true;
    if (node_modifies_or_addrs_sym(n->by, s)) return true;
    if (node_modifies_or_addrs_sym(n->init, s)) return true;
    if (node_modifies_or_addrs_sym(n->step, s)) return true;
    for (int i = 0; i < n->list.len; i++) {
        if (node_modifies_or_addrs_sym(n->list.items[i], s)) return true;
    }
    return false;
}

static bool check_inline_body(Node *n, int *count, bool allow_loop, int *loop_count, bool *has_call) {
    if (!n) return true;
    int limit = allow_loop ? 60 : 40;
    if (++(*count) > limit) return false;
    if (n->kind == N_DEFER || n->kind == N_TRY ||
        n->kind == N_LOOP_INF || n->kind == N_LOOP_ITER || n->kind == N_LOOP_C) {
        return false;
    }
    if (n->kind == N_LOOP_COND || n->kind == N_LOOP_RANGE) {
        if (!allow_loop || ++(*loop_count) > 1) return false;
    }
    if (n->kind == N_CALL && n->builtin == 0) {
        *has_call = true;
    }
    if (!check_inline_body(n->lhs, count, allow_loop, loop_count, has_call)) return false;
    if (!check_inline_body(n->rhs, count, allow_loop, loop_count, has_call)) return false;
    if (!check_inline_body(n->cond, count, allow_loop, loop_count, has_call)) return false;
    if (!check_inline_body(n->body, count, allow_loop, loop_count, has_call)) return false;
    if (!check_inline_body(n->els, count, allow_loop, loop_count, has_call)) return false;
    if (!check_inline_body(n->lo, count, allow_loop, loop_count, has_call)) return false;
    if (!check_inline_body(n->hi, count, allow_loop, loop_count, has_call)) return false;
    if (!check_inline_body(n->by, count, allow_loop, loop_count, has_call)) return false;
    if (!check_inline_body(n->init, count, allow_loop, loop_count, has_call)) return false;
    if (!check_inline_body(n->step, count, allow_loop, loop_count, has_call)) return false;
    for (int i = 0; i < n->list.len; i++) {
        if (!check_inline_body(n->list.items[i], count, allow_loop, loop_count, has_call)) return false;
    }
    return true;
}

static bool can_inline_fn(CG *cg, Node *callee) {
    if (!callee || callee->is_extern || !callee->body) return false;
    if (cg->scratch_depth > 0) return false;
    if (is_aggregate_type(callee->type)) return false;
    if (callee == cg->cur_fn || cg->inline_depth >= 2) return false;
    for (int i = 0; i < cg->inline_depth; i++) {
        if (cg->inline_stack[i] == callee) return false;
    }
    for (int i = 0; i < callee->list.len; i++) {
        Symbol *ps = callee->list.items[i]->sym;
        if ((ps->type->kind == TY_ARRAY || is_aggregate_type(ps->type)) &&
            node_modifies_or_addrs_sym(callee->body, ps)) {
            return false;
        }
    }
    bool allow_loop = cg->release && cg->inline_depth == 0;
    int count = 0;
    int loop_count = 0;
    bool has_call = false;
    if (!check_inline_body(callee->body, &count, allow_loop, &loop_count, &has_call)) return false;
    if (loop_count > 0 && has_call) return false;
    return true;
}

// Conservatively marks every symbol whose address is taken (`&x`, `&x.f`, `&x[i]`) inside `n`.
// Needed before inlining: `addr_taken` is otherwise only computed when the callee itself is
// compiled, which may happen after its callers.
static void mark_addr_taken_syms(Node *n) {
    if (!n) return;
    if (n->kind == N_ADDR) {
        Node *base = n->lhs;
        while (base && (base->kind == N_FIELD || base->kind == N_INDEX)) {
            base = base->lhs;
        }
        if (base && base->kind == N_IDENT && base->sym) base->sym->addr_taken = true;
    }
    mark_addr_taken_syms(n->lhs);
    mark_addr_taken_syms(n->rhs);
    mark_addr_taken_syms(n->cond);
    mark_addr_taken_syms(n->body);
    mark_addr_taken_syms(n->els);
    mark_addr_taken_syms(n->lo);
    mark_addr_taken_syms(n->hi);
    mark_addr_taken_syms(n->by);
    mark_addr_taken_syms(n->init);
    mark_addr_taken_syms(n->step);
    for (int i = 0; i < n->list.len; i++) {
        mark_addr_taken_syms(n->list.items[i]);
    }
}

static bool is_reg_scalar_type(Type *t) {
    return t && (t->kind == TY_INT || (t->kind == TY_ENUM && !t->is_tagged) ||
                 t->kind == TY_BOOL || t->kind == TY_FLOAT || t->kind == TY_PTR || t->kind == TY_FN);
}

static void shift_node_slots(CG *cg, Node *n, int delta) {
    if (!n) return;
    if (n->slot_start > 0) n->slot_start += delta;
    if (n->slot_end > 0)   n->slot_end   += delta;
    if (n->slot_step > 0)  n->slot_step  += delta;

    switch (n->kind) {
    case N_VAR:
        if (n->sym) {
            n->sym->slot += delta;
            n->sym->is_const_alias = false;
            n->sym->alias_of = NULL;
            n->sym->known_nonneg = false;
            n->sym->hw_reg = -1;
            n->sym->xmm_reg = -1;
            if (delta > 0 && cg && !n->sym->addr_taken && is_reg_scalar_type(n->sym->type)) {
                if (n->sym->type == &ty_f64 && cg->num_free_xmm > 0) {
                    n->sym->xmm_reg = cg->free_xmm_regs[--cg->num_free_xmm];
                } else if (cg->num_free_callee > 0 &&
                           (n->sym->type != &ty_f64 || cg->num_free_xmm == 0)) {
                    n->sym->hw_reg = cg->free_callee_regs[--cg->num_free_callee];
                }
            }
        }
        break;
    case N_LOOP_RANGE:
        if (n->sym) {
            n->sym->slot += delta;
            n->sym->hw_reg = -1;
            n->sym->xmm_reg = -1;
            n->sym->alias_of = NULL;
            n->sym->known_nonneg = false;
            n->sym->is_const_alias = false;
            if (delta > 0 && cg && !n->sym->addr_taken && is_reg_scalar_type(n->sym->type) &&
                n->sym->type->kind != TY_FLOAT && cg->num_free_callee > 0) {
                n->sym->hw_reg = cg->free_callee_regs[--cg->num_free_callee];
            }
        }
        break;
    case N_MATCH_ARM:
        if (n->sym) {
            n->sym->slot += delta;
            n->sym->hw_reg = -1;
            n->sym->xmm_reg = -1;
            n->sym->alias_of = NULL;
            n->sym->known_nonneg = false;
            n->sym->is_const_alias = false;
        }
        break;
    case N_LOOP_ITER:
        if (n->sym) {
            n->sym->slot += delta;
            n->sym->hw_reg = -1;
            n->sym->xmm_reg = -1;
            n->sym->alias_of = NULL;
            n->sym->known_nonneg = false;
            n->sym->is_const_alias = false;
        }
        if (n->sym2) {
            n->sym2->slot += delta;
            n->sym2->hw_reg = -1;
            n->sym2->xmm_reg = -1;
            n->sym2->alias_of = NULL;
            n->sym2->known_nonneg = false;
            n->sym2->is_const_alias = false;
        }
        break;
    default:
        break;
    }

    shift_node_slots(cg, n->lhs, delta);
    shift_node_slots(cg, n->rhs, delta);
    shift_node_slots(cg, n->cond, delta);
    shift_node_slots(cg, n->body, delta);
    shift_node_slots(cg, n->els, delta);
    shift_node_slots(cg, n->lo, delta);
    shift_node_slots(cg, n->hi, delta);
    shift_node_slots(cg, n->by, delta);
    shift_node_slots(cg, n->init, delta);
    shift_node_slots(cg, n->step, delta);
    for (int i = 0; i < n->list.len; i++) {
        shift_node_slots(cg, n->list.items[i], delta);
    }
}

static void gen_inline_call(CG *cg, Node *n, Node *callee) {
    int nargs = n->list.len;
    int delta = cg->base_slots + cg->temp_depth;
    cg->temp_depth += callee->num_slots;
    if (cg->temp_depth > cg->max_temp_depth) {
        cg->max_temp_depth = cg->temp_depth;
    }
    int saved_free_callee = cg->num_free_callee;
    int saved_free_xmm    = cg->num_free_xmm;

    int *orig_slot       = nargs ? xmalloc((size_t)nargs * sizeof(int)) : NULL;
    int *orig_hw         = nargs ? xmalloc((size_t)nargs * sizeof(int)) : NULL;
    int *orig_xmm        = nargs ? xmalloc((size_t)nargs * sizeof(int)) : NULL;
    Symbol **orig_alias  = nargs ? xmalloc((size_t)nargs * sizeof(Symbol *)) : NULL;
    bool *orig_nonneg    = nargs ? xmalloc((size_t)nargs * sizeof(bool)) : NULL;
    bool *orig_const     = nargs ? xmalloc((size_t)nargs * sizeof(bool)) : NULL;
    i128 *orig_cval      = nargs ? xmalloc((size_t)nargs * sizeof(i128)) : NULL;
    double *orig_fval    = nargs ? xmalloc((size_t)nargs * sizeof(double)) : NULL;
    bool *aliased        = nargs ? xcalloc((size_t)nargs, sizeof(bool)) : NULL;
    bool *const_aliased  = nargs ? xcalloc((size_t)nargs, sizeof(bool)) : NULL;
    int *param_reg       = nargs ? xmalloc((size_t)nargs * sizeof(int)) : NULL;
    int *param_xmm       = nargs ? xmalloc((size_t)nargs * sizeof(int)) : NULL;

    // 1. Classify arguments (constant-aliased, caller-sym-aliased, or evaluated).
    for (int i = 0; i < nargs; i++) {
        Symbol *ps = callee->list.items[i]->sym;
        Node *arg  = n->list.items[i];
        orig_slot[i]   = ps->slot;
        orig_hw[i]     = ps->hw_reg;
        orig_xmm[i]    = ps->xmm_reg;
        orig_alias[i]  = ps->alias_of;
        orig_nonneg[i] = ps->known_nonneg;
        orig_const[i]  = ps->is_const_alias;
        orig_cval[i]   = ps->cval;
        orig_fval[i]   = ps->fval;
        param_reg[i]   = -1;
        param_xmm[i]   = -1;

        bool unmodified = !node_modifies_or_addrs_sym(callee->body, ps);
        if (unmodified && is_reg_scalar_type(ps->type) &&
            (arg->is_const || (arg->kind == N_IDENT && !arg->implicit_slice && arg->sym && arg->sym->is_const_alias))) {
            const_aliased[i] = true;
            continue;
        }
        if (unmodified && arg->kind == N_IDENT && !arg->implicit_slice && arg->sym) {
            Symbol *as = arg->sym;
            if ((as->kind == SYM_VAR || as->kind == SYM_PARAM) && !as->is_global && !as->addr_taken &&
                as->type == ps->type && ps->type->kind != TY_ARRAY && !is_aggregate_type(ps->type)) {
                aliased[i] = true;
                continue;
            }
        }
    }

    // Evaluate non-aliased arguments (into a free callee-saved XMM/GPR register when safe, or target_slot).
    for (int i = 0; i < nargs; i++) {
        if (aliased[i] || const_aliased[i]) continue;
        Symbol *ps = callee->list.items[i]->sym;
        Node *arg  = n->list.items[i];
        bool unmodified = !node_modifies_or_addrs_sym(callee->body, ps);
        if (unmodified && ps->type == &ty_f64 && cg->num_free_xmm > 0) {
            int xr = cg->free_xmm_regs[--cg->num_free_xmm];
            param_xmm[i] = xr;
            uint64_t fbits = 0;
            if (is_direct_xmm_const(arg, &fbits)) {
                if (fbits == 0) x_xorpd(xr, xr);
                else            x_load_sd_rip(xr, SEC_RDATA, add_f64_const(cg, fbits));
            } else if (is_direct_xmm_reg(arg)) {
                x_movsd_rr(xr, arg->sym->xmm_reg);
            } else if (can_eval_xmm(arg, 0)) {
                gen_float_xmm(cg, arg, 0);
                x_movsd_rr(xr, 0);
            } else {
                gen_expr(cg, arg);
                x_movq_xmm_gpr(xr, RAX);
            }
            continue;
        }
        if (unmodified && is_reg_scalar_type(ps->type) && cg->num_free_callee > 0) {
            int r = cg->free_callee_regs[--cg->num_free_callee];
            param_reg[i] = r;
            if (is_simple_expr(arg)) {
                gen_simple_into_reg(arg, r);
            } else {
                gen_expr(cg, arg);
                x_mov_rr(1, r, RAX);
            }
            continue;
        }
        int target_slot = delta + ps->slot;
        if (ps->type->kind == TY_SLICE || ps->type == &ty_str) {
            gen_collection_ptr_len(cg, arg);
            x_store(RBP, slot_disp(target_slot), RAX);
            x_store(RBP, slot_disp(target_slot) + 8, RDX);
        } else {
            gen_expr(cg, arg);
            x_store(RBP, slot_disp(target_slot), RAX);
        }
    }

    // 2. Shift `callee` parameter & local slots by `+delta` (or bind aliased/constant/register parameters).
    for (int i = 0; i < nargs; i++) {
        Symbol *ps = callee->list.items[i]->sym;
        Node *arg  = n->list.items[i];
        bool unmodified = !node_modifies_or_addrs_sym(callee->body, ps);
        if (const_aliased[i]) {
            ps->is_const_alias = true;
            ps->cval   = arg->is_const ? arg->cval : arg->sym->cval;
            ps->fval   = arg->is_const ? arg->fval : arg->sym->fval;
            ps->hw_reg = -1;
            ps->xmm_reg = -1;
            ps->alias_of = NULL;
            ps->known_nonneg = false;
        } else if (aliased[i]) {
            Symbol *as = arg->sym;
            ps->slot           = as->slot;
            ps->hw_reg         = as->hw_reg;
            ps->xmm_reg        = as->xmm_reg;
            ps->alias_of       = as->alias_of ? as->alias_of : as;
            ps->known_nonneg   = as->known_nonneg || is_nonneg_expr(arg);
            ps->is_const_alias = as->is_const_alias;
            ps->cval           = as->cval;
            ps->fval           = as->fval;
        } else if (param_xmm[i] >= 0) {
            ps->slot           += delta;
            ps->hw_reg          = -1;
            ps->xmm_reg         = param_xmm[i];
            ps->alias_of        = NULL;
            ps->known_nonneg    = false;
            ps->is_const_alias  = false;
        } else if (param_reg[i] >= 0) {
            ps->slot           += delta;
            ps->hw_reg          = param_reg[i];
            ps->xmm_reg         = -1;
            ps->alias_of        = NULL;
            ps->known_nonneg    = is_nonneg_expr(arg);
            ps->is_const_alias  = false;
        } else {
            ps->slot           += delta;
            ps->hw_reg          = -1;
            ps->xmm_reg         = -1;
            ps->alias_of        = NULL;
            ps->known_nonneg    = unmodified && is_nonneg_expr(arg);
            ps->is_const_alias  = false;
        }
    }
    mark_addr_taken_syms(callee->body);
    shift_node_slots(cg, callee->body, delta);

    // 3. Emit `callee->body` inline.
    cg->inline_stack[cg->inline_depth++] = callee;
    if (callee->body->list.len == 1 && callee->body->list.items[0]->kind == N_RETURN) {
        Node *ret_expr = callee->body->list.items[0]->lhs;
        if (ret_expr) {
            if (callee->type == &ty_str || callee->type->kind == TY_SLICE) {
                gen_collection_ptr_len(cg, ret_expr);
            } else {
                gen_expr(cg, ret_expr);
            }
        }
    } else {
        int saved_epilogue = cg->cur_epilogue;
        Type *saved_ret_type = cg->cur_ret_type;
        Node *saved_last_top = cg->last_top_stmt;
        Node *saved_top_block = cg->cur_top_block;
        int inline_end = x_label();
        cg->cur_epilogue = inline_end;
        cg->cur_ret_type = callee->type;
        cg->cur_top_block = callee->body;
        cg->last_top_stmt = (callee->body->list.len > 0)
            ? callee->body->list.items[callee->body->list.len - 1]
            : NULL;
        gen_block(cg, callee->body);
        x_bind(inline_end);
        cg->cur_epilogue = saved_epilogue;
        cg->cur_ret_type = saved_ret_type;
        cg->last_top_stmt = saved_last_top;
        cg->cur_top_block = saved_top_block;
    }
    cg->inline_depth--;

    // 4. Restore `callee` slots and release temporary frame space.
    shift_node_slots(NULL, callee->body, -delta);
    for (int i = 0; i < nargs; i++) {
        Symbol *ps = callee->list.items[i]->sym;
        ps->slot           = orig_slot[i];
        ps->hw_reg         = orig_hw[i];
        ps->xmm_reg        = orig_xmm[i];
        ps->alias_of       = orig_alias[i];
        ps->known_nonneg   = orig_nonneg[i];
        ps->is_const_alias = orig_const[i];
        ps->cval           = orig_cval[i];
        ps->fval           = orig_fval[i];
    }
    free(orig_slot);
    free(orig_hw);
    free(orig_xmm);
    free(orig_alias);
    free(orig_nonneg);
    free(orig_const);
    free(orig_cval);
    free(orig_fval);
    free(aliased);
    free(const_aliased);
    free(param_reg);
    free(param_xmm);
    cg->num_free_callee = saved_free_callee;
    cg->num_free_xmm    = saved_free_xmm;
    cg->temp_depth -= callee->num_slots;
}

// ---------------------------------------------------------------------------
// Loop-weighted callee-saved register allocator (GPR: RBX, R12..R15, RDI, RSI; XMM: XMM6..XMM13)
// ---------------------------------------------------------------------------

typedef VEC(Symbol *) SymVec;

static void record_local_sym(SymVec *locals, Symbol *s) {
    if (!s || s->is_global) return;
    if (s->kind != SYM_VAR && s->kind != SYM_PARAM) return;
    for (int i = 0; i < locals->len; i++) {
        if (locals->items[i] == s) return;
    }
    s->addr_taken   = false;
    s->use_score    = 0;
    s->hw_reg       = -1;
    s->xmm_reg      = -1;
    s->alias_of     = NULL;
    s->known_nonneg = false;
    vec_push(*locals, s);
}

static void add_sym_score(SymVec *locals, Symbol *s, int delta) {
    if (!s || s->is_global) return;
    if (s->kind != SYM_VAR && s->kind != SYM_PARAM) return;
    record_local_sym(locals, s);
    if (s->use_score < 100000000) {
        s->use_score += delta;
    }
}

static void scan_node_vars(SymVec *locals, Node *n, int weight) {
    if (!n) return;
    int loop_w = (weight < 100000) ? weight * 8 : weight;

    switch (n->kind) {
    case N_ADDR: {
        Node *base = n->lhs;
        while (base && (base->kind == N_FIELD || base->kind == N_INDEX)) {
            base = base->lhs;
        }
        if (base && base->kind == N_IDENT && base->sym) {
            record_local_sym(locals, base->sym);
            base->sym->addr_taken = true;
        }
        scan_node_vars(locals, n->lhs, weight);
        return;
    }
    case N_IDENT:
        add_sym_score(locals, n->sym, weight);
        return;
    case N_VAR:
        record_local_sym(locals, n->sym);
        if (n->rhs) {
            add_sym_score(locals, n->sym, weight);
            scan_node_vars(locals, n->rhs, weight);
        }
        return;
    case N_ASSIGN:
        if (n->sym) {
            add_sym_score(locals, n->sym, (n->op == T_ASSIGN ? 1 : 2) * weight);
        }
        scan_node_vars(locals, n->lhs, weight);
        scan_node_vars(locals, n->rhs, weight);
        return;
    case N_CALL: {
        int arg_w = (n->sym && !n->sym->is_extern && n->sym->decl && n->sym->decl->body) ? weight * 2 : weight;
        scan_node_vars(locals, n->lhs, weight);
        for (int i = 0; i < n->list.len; i++) {
            scan_node_vars(locals, n->list.items[i], arg_w);
        }
        return;
    }
    case N_LOOP_INF:
        scan_node_vars(locals, n->body, loop_w);
        return;
    case N_LOOP_COND:
        scan_node_vars(locals, n->cond, loop_w);
        scan_node_vars(locals, n->body, loop_w);
        return;
    case N_LOOP_RANGE:
        scan_node_vars(locals, n->lo, weight);
        scan_node_vars(locals, n->hi, loop_w);
        scan_node_vars(locals, n->by, weight);
        add_sym_score(locals, n->sym, 2 * loop_w);
        scan_node_vars(locals, n->body, loop_w);
        return;
    case N_LOOP_ITER:
        scan_node_vars(locals, n->lhs, weight);
        add_sym_score(locals, n->sym, loop_w);
        if (n->sym2) add_sym_score(locals, n->sym2, loop_w);
        scan_node_vars(locals, n->body, loop_w);
        return;
    case N_LOOP_C:
        scan_node_vars(locals, n->init, weight);
        scan_node_vars(locals, n->cond, loop_w);
        scan_node_vars(locals, n->step, loop_w);
        scan_node_vars(locals, n->body, loop_w);
        return;
    default:
        scan_node_vars(locals, n->lhs, weight);
        scan_node_vars(locals, n->rhs, weight);
        scan_node_vars(locals, n->cond, weight);
        scan_node_vars(locals, n->body, weight);
        scan_node_vars(locals, n->els, weight);
        scan_node_vars(locals, n->lo, weight);
        scan_node_vars(locals, n->hi, weight);
        for (int i = 0; i < n->list.len; i++) {
            scan_node_vars(locals, n->list.items[i], weight);
        }
        return;
    }
}

static int alloc_fn_regs(Node *fn, int out_regs[7], int out_xmms[8], int *out_num_xmm) {
    static const int callee_regs[7] = {RBX, R12, R13, R14, R15, RDI, RSI};
    static const int callee_xmms[8] = {XMM6, XMM7, XMM8, XMM9, XMM10, XMM11, XMM12, XMM13};
    SymVec locals = {0};
    for (int i = 0; i < fn->list.len; i++) {
        record_local_sym(&locals, fn->list.items[i]->sym);
    }
    scan_node_vars(&locals, fn->body, 1);

    int nxmm = 0;
    for (int k = 0; k < 8; k++) {
        Symbol *best = NULL;
        for (int i = 0; i < locals.len; i++) {
            Symbol *s = locals.items[i];
            if (s->xmm_reg >= 0 || s->addr_taken || s->use_score < 2) continue;
            if (s->type != &ty_f64) continue;
            if (!best || s->use_score > best->use_score) best = s;
        }
        if (!best) break;
        best->xmm_reg = callee_xmms[k];
        out_xmms[nxmm++] = callee_xmms[k];
    }
    *out_num_xmm = nxmm;

    int n_alloc = 0;
    for (int k = 0; k < 7; k++) {
        Symbol *best = NULL;
        for (int i = 0; i < locals.len; i++) {
            Symbol *s = locals.items[i];
            if (s->xmm_reg >= 0 || s->hw_reg >= 0 || s->addr_taken || s->use_score < 2) continue;
            Type *t = s->type;
            if (!t || (t->kind != TY_INT && !(t->kind == TY_ENUM && !t->is_tagged) &&
                       t->kind != TY_BOOL && t->kind != TY_FLOAT && t->kind != TY_PTR &&
                       t->kind != TY_FN &&
                       t->kind != TY_SLICE && t != &ty_str)) continue;
            if (!best || s->use_score > best->use_score) {
                best = s;
            }
        }
        if (!best) break;
        best->hw_reg = callee_regs[k];
        out_regs[n_alloc++] = callee_regs[k];
    }
    free(locals.items);
    return n_alloc;
}

static bool node_has_inline_call(CG *cg, Node *n) {
    if (!n) return false;
    if (n->kind == N_CALL && n->sym && !n->sym->is_extern && can_inline_fn(cg, n->sym->decl)) {
        return true;
    }
    if (node_has_inline_call(cg, n->lhs)) return true;
    if (node_has_inline_call(cg, n->rhs)) return true;
    if (node_has_inline_call(cg, n->cond)) return true;
    if (node_has_inline_call(cg, n->body)) return true;
    if (node_has_inline_call(cg, n->els)) return true;
    if (node_has_inline_call(cg, n->lo)) return true;
    if (node_has_inline_call(cg, n->hi)) return true;
    if (node_has_inline_call(cg, n->by)) return true;
    if (node_has_inline_call(cg, n->init)) return true;
    if (node_has_inline_call(cg, n->step)) return true;
    for (int i = 0; i < n->list.len; i++) {
        if (node_has_inline_call(cg, n->list.items[i])) return true;
    }
    return false;
}

static bool node_has_range_loop(Node *n) {
    if (!n) return false;
    if (n->kind == N_LOOP_RANGE) return true;
    if (node_has_range_loop(n->lhs) || node_has_range_loop(n->rhs) ||
        node_has_range_loop(n->cond) || node_has_range_loop(n->body) ||
        node_has_range_loop(n->els) || node_has_range_loop(n->lo) ||
        node_has_range_loop(n->hi) || node_has_range_loop(n->by) ||
        node_has_range_loop(n->init) || node_has_range_loop(n->step)) {
        return true;
    }
    for (int i = 0; i < n->list.len; i++) {
        if (node_has_range_loop(n->list.items[i])) return true;
    }
    return false;
}

static bool node_uses_f64(Node *n, int depth) {
    if (!n || depth > 2) return false;
    if (n->type == &ty_f64) return true;
    if (n->kind == N_VAR && n->sym && n->sym->type == &ty_f64) return true;
    if (n->kind == N_CALL && n->sym && !n->sym->is_extern && n->sym->decl && n->sym->decl->body) {
        Node *callee = n->sym->decl;
        for (int i = 0; i < callee->list.len; i++) {
            if (callee->list.items[i]->sym && callee->list.items[i]->sym->type == &ty_f64) return true;
        }
        if (node_uses_f64(callee->body, depth + 1)) return true;
    }
    if (node_uses_f64(n->lhs, depth) || node_uses_f64(n->rhs, depth) ||
        node_uses_f64(n->cond, depth) || node_uses_f64(n->body, depth) ||
        node_uses_f64(n->els, depth) || node_uses_f64(n->lo, depth) ||
        node_uses_f64(n->hi, depth) || node_uses_f64(n->by, depth) ||
        node_uses_f64(n->init, depth) || node_uses_f64(n->step, depth)) {
        return true;
    }
    for (int i = 0; i < n->list.len; i++) {
        if (node_uses_f64(n->list.items[i], depth)) return true;
    }
    return false;
}

static bool node_needs_xmm_inline(CG *cg, Node *n) {
    if (!n) return false;
    if (n->kind == N_CALL && n->sym && !n->sym->is_extern && can_inline_fn(cg, n->sym->decl)) {
        Node *callee = n->sym->decl;
        if (callee->type == &ty_f64) return true;
        for (int i = 0; i < callee->list.len; i++) {
            if (callee->list.items[i]->sym && callee->list.items[i]->sym->type == &ty_f64) return true;
        }
        if (node_uses_f64(callee->body, 0)) return true;
    }
    if (node_needs_xmm_inline(cg, n->lhs) || node_needs_xmm_inline(cg, n->rhs) ||
        node_needs_xmm_inline(cg, n->cond) || node_needs_xmm_inline(cg, n->body) ||
        node_needs_xmm_inline(cg, n->els) || node_needs_xmm_inline(cg, n->lo) ||
        node_needs_xmm_inline(cg, n->hi) || node_needs_xmm_inline(cg, n->by) ||
        node_needs_xmm_inline(cg, n->init) || node_needs_xmm_inline(cg, n->step)) {
        return true;
    }
    for (int i = 0; i < n->list.len; i++) {
        if (node_needs_xmm_inline(cg, n->list.items[i])) return true;
    }
    return false;
}

static void gen_fn(CG *cg, Node *fn) {
    static const int callee_regs[7] = {RBX, R12, R13, R14, R15, RDI, RSI};
    static const int callee_xmms[8] = {XMM6, XMM7, XMM8, XMM9, XMM10, XMM11, XMM12, XMM13};
    int saved_regs[7];
    int saved_xmms[8];
    int num_saved_xmm = 0;
    int num_saved = alloc_fn_regs(fn, saved_regs, saved_xmms, &num_saved_xmm);

    cg->cur_fn = fn;
    cg->inline_depth = 0;
    cg->scratch_depth = 0;
    cg->num_free_callee = 0;
    cg->num_free_xmm = 0;
    if (num_saved < 7 && (node_has_inline_call(cg, fn->body) || node_has_range_loop(fn->body))) {
        for (int k = num_saved; k < 7; k++) {
            saved_regs[k] = callee_regs[k];
            cg->free_callee_regs[cg->num_free_callee++] = callee_regs[k];
        }
        num_saved = 7;
    }
    if (num_saved_xmm < 8 && node_needs_xmm_inline(cg, fn->body)) {
        for (int k = num_saved_xmm; k < 8; k++) {
            saved_xmms[k] = callee_xmms[k];
            cg->free_xmm_regs[cg->num_free_xmm++] = callee_xmms[k];
        }
        num_saved_xmm = 8;
    }

    x_bind(fn->sym->label);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    uint32_t patch = x_sub_rsp_patchable();

    cg->last_top_stmt = (fn->body && fn->body->list.len > 0)
        ? fn->body->list.items[fn->body->list.len - 1]
        : NULL;
    cg->cur_top_block = fn->body;
    cg->cur_epilogue = x_label();
    cg->cur_ret_type = fn->type;
    cg->cur_ret_ptr_slot = fn->slot_start;
    cg->base_slots = fn->num_slots + num_saved + num_saved_xmm * 2;
    cg->temp_depth = 0;
    cg->max_temp_depth = 0;
    cg->max_out_stack_args = 0;
    cg->defers.len = 0;

    // Save allocated non-volatile GPRs (RBX, R12..R15, RDI, RSI) and XMMs (XMM6..XMM13).
    for (int k = 0; k < num_saved; k++) {
        x_store(RBP, slot_disp(fn->num_slots + 1 + k), saved_regs[k]);
    }
    for (int k = 0; k < num_saved_xmm; k++) {
        x_movups_store(RBP, slot_disp(fn->num_slots + num_saved + 2 * (k + 1)), saved_xmms[k]);
    }

    // If this function returns a struct or tagged union by value, save the caller's return buffer pointer (in R10).
    if (is_aggregate_type(fn->type)) {
        x_store(RBP, slot_disp(fn->slot_start), R10);
    }

    // Move incoming parameters into their allocated registers or stack slots
    // (Microsoft x64 ABI: RCX, RDX, R8, R9, then [RBP+48+...]).
    static const int arg_regs[4] = {RCX, RDX, R8, R9};
    for (int i = 0; i < fn->list.len; i++) {
        Symbol *ps = fn->list.items[i]->sym;
        if (ps->xmm_reg >= 0 && i >= 4) {
            x_load_sd(ps->xmm_reg, RBP, 48 + (i - 4) * 8);
            continue;
        }
        int reg = RAX;
        if (i < 4) {
            reg = arg_regs[i];
        } else {
            x_load(RAX, RBP, 48 + (i - 4) * 8);
        }
        if (ps->type->kind == TY_SLICE || ps->type == &ty_str) {
            // Parameter register holds pointer to 16-byte `{ptr, len}` descriptor.
            x_load(R10, reg, 0);
            x_load(R11, reg, 8);
            if (ps->hw_reg >= 0) x_mov_rr(1, ps->hw_reg, R10);
            x_store(RBP, slot_disp(ps->slot), R10);
            x_store(RBP, slot_disp(ps->slot) + 8, R11);
        } else {
            store_var(ps, reg);
        }
    }

    gen_block(cg, fn->body);
    cg->cur_top_block = NULL;

    x_bind(cg->cur_epilogue);
    for (int k = 0; k < num_saved_xmm; k++) {
        x_movups_load(saved_xmms[k], RBP, slot_disp(fn->num_slots + num_saved + 2 * (k + 1)));
    }
    for (int k = 0; k < num_saved; k++) {
        x_load(saved_regs[k], RBP, slot_disp(fn->num_slots + 1 + k));
    }
    x_leave();
    x_ret();

    // Frame size: local slots + saved callee-saved reg slots + expression temp slots +
    // 32-byte shadow space + outgoing stack arguments, rounded up to 16 bytes.
    int total_locals = (cg->base_slots + cg->max_temp_depth) * 8;
    int out_area = 32 + cg->max_out_stack_args * 8;
    int frame = total_locals + out_area;
    if (frame % 16 != 0) frame += 16 - (frame % 16);
    buf_put_u32(&A.code, patch, (uint32_t)frame);

    // Reset register/alias metadata on parameters and locals so subsequent inlining starts clean.
    for (int i = 0; i < fn->list.len; i++) {
        Symbol *ps = fn->list.items[i]->sym;
        ps->hw_reg = -1;
        ps->xmm_reg = -1;
        ps->alias_of = NULL;
        ps->known_nonneg = false;
        ps->is_const_alias = false;
    }
    shift_node_slots(NULL, fn->body, 0);
}

// ---------------------------------------------------------------------------
// Built-in Windows x64 runtime routines (no CRT, only kernel32.dll)
// ---------------------------------------------------------------------------

static void emit_print_nl_wrapper(CG *cg, int nl_lbl, int raw_lbl) {
    x_bind(nl_lbl);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 32);
    x_call(raw_lbl);
    x_call(cg->rt_print_nl);
    x_leave();
    x_ret();
}

static void emit_runtime(CG *cg) {
    cg->rt_write          = x_label();
    cg->rt_print_nl       = x_label();
    cg->rt_print_i64      = x_label();
    cg->rt_print_i64_raw  = x_label();
    cg->rt_print_u64      = x_label();
    cg->rt_print_u64_raw  = x_label();
    cg->rt_print_f64      = x_label();
    cg->rt_print_f64_raw  = x_label();
    cg->rt_print_bool     = x_label();
    cg->rt_print_bool_raw = x_label();
    cg->rt_print_str      = x_label();
    cg->rt_str_eq         = x_label();
    cg->rt_trap           = x_label();

    // rt_write(rcx = buf, rdx = len): writes to cached stdout handle.
    x_bind(cg->rt_write);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 64);
    x_store(RBP, -8, RCX);   // buf
    x_store(RBP, -16, RDX);  // len
    x_load_rip(RCX, SEC_DATA, cg->stdout_off);
    x_load(RDX, RBP, -8);
    x_load(R8, RBP, -16);
    x_lea(R9, RBP, -24);     // &bytes_written
    x_mov_ri(RAX, 0);
    x_store(RSP, 32, RAX);   // lpOverlapped = NULL
    x_call_import(IMP_WRITEFILE);
    x_leave();
    x_ret();

    // rt_print_nl(): writes '\n'.
    uint32_t s_nl = add_string(cg, "\n", 1);
    x_bind(cg->rt_print_nl);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 32);
    x_lea_rip(RCX, SEC_RDATA, s_nl);
    x_mov_ri(RDX, 1);
    x_call(cg->rt_write);
    x_leave();
    x_ret();

    // rt_print_u64_raw(rcx = unsigned 64-bit value): formats into stack buffer (no '\n').
    x_bind(cg->rt_print_u64_raw);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 64);
    x_mov_rr(1, RAX, RCX);
    x_mov_rr(1, R8, RBP);
    x_mov_ri(R9, 10);
    int u_loop = x_label();
    x_bind(u_loop);
    x_alu_ri(ALU_SUB, 1, R8, 1);
    x_mov_ri(RDX, 0);
    x_f7(6, 1, R9);          // div r9: RAX = quot, RDX = rem
    x_alu_ri(ALU_ADD, 0, RDX, '0');
    x_store8(R8, 0, RDX);
    x_test_rr(1, RAX, RAX);
    x_jcc(CC_NE, u_loop);
    x_mov_rr(1, RCX, R8);
    x_mov_rr(1, RDX, RBP);
    x_alu_rr(ALU_SUB, 1, RDX, R8);
    x_call(cg->rt_write);
    x_leave();
    x_ret();
    emit_print_nl_wrapper(cg, cg->rt_print_u64, cg->rt_print_u64_raw);

    // rt_print_i64_raw(rcx = signed 64-bit value): formats into stack buffer (no '\n').
    x_bind(cg->rt_print_i64_raw);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 64);
    x_mov_rr(1, RAX, RCX);
    x_mov_ri(R10, 0);        // sign flag
    x_test_rr(1, RAX, RAX);
    int pos_lbl = x_label();
    x_jcc(CC_NS, pos_lbl);
    x_f7(3, 1, RAX);         // neg rax (works for INT64_MIN in unsigned arithmetic)
    x_mov_ri(R10, 1);
    x_bind(pos_lbl);
    x_mov_rr(1, R8, RBP);
    x_mov_ri(R9, 10);
    int s_loop = x_label();
    x_bind(s_loop);
    x_alu_ri(ALU_SUB, 1, R8, 1);
    x_mov_ri(RDX, 0);
    x_f7(6, 1, R9);
    x_alu_ri(ALU_ADD, 0, RDX, '0');
    x_store8(R8, 0, RDX);
    x_test_rr(1, RAX, RAX);
    x_jcc(CC_NE, s_loop);
    x_test_rr(1, R10, R10);
    int no_minus = x_label();
    x_jcc(CC_E, no_minus);
    x_alu_ri(ALU_SUB, 1, R8, 1);
    x_store8_imm(R8, 0, '-');
    x_bind(no_minus);
    x_mov_rr(1, RCX, R8);
    x_mov_rr(1, RDX, RBP);
    x_alu_rr(ALU_SUB, 1, RDX, R8);
    x_call(cg->rt_write);
    x_leave();
    x_ret();
    emit_print_nl_wrapper(cg, cg->rt_print_i64, cg->rt_print_i64_raw);

    // rt_print_f64_raw(rcx = f64 IEEE-754 bits):
    // Formats with 6 decimal digits (rounded to nearest millionth), trimming trailing
    // zeros up to at least 1 digit after '.', e.g. `3.141593`, `2.5`, `-0.25`.
    x_bind(cg->rt_print_f64_raw);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 64);
    x_mov_rr(1, RAX, RCX);
    x_mov_ri(R10, 0);        // sign flag
    x_test_rr(1, RAX, RAX);  // bit 63 = IEEE-754 sign bit
    int f_pos = x_label();
    x_jcc(CC_NS, f_pos);
    x_mov_ri(RCX, 1ULL << 63);
    x_alu_rr(ALU_XOR, 1, RAX, RCX); // clear sign bit -> abs(x)
    x_mov_ri(R10, 1);
    x_bind(f_pos);

    // Multiply abs(x) by 1,000,000 and add 0.5 for rounding, then cvttsd2si -> RAX.
    x_movq_xmm_gpr(0, RAX);
    uint64_t scale_bits = fval_bits(1000000.0, &ty_f64);
    uint64_t half_bits  = fval_bits(0.5, &ty_f64);
    x_mov_ri(RCX, scale_bits);
    x_movq_xmm_gpr(1, RCX);
    x_sse_sd(0x59, 0, 1);    // mulsd xmm0, 1e6
    x_mov_ri(RCX, half_bits);
    x_movq_xmm_gpr(1, RCX);
    x_sse_sd(0x58, 0, 1);    // addsd xmm0, 0.5
    x_cvttsd2si(RAX, 0);     // RAX = integer millionths

    // Split RAX into integer part (RAX / 1_000_000) and fractional part (RAX % 1_000_000).
    x_mov_ri(RDX, 0);
    x_mov_ri(R9, 1000000);
    x_f7(6, 1, R9);          // RAX = int_part, RDX = frac_part (0..999999)
    x_mov_rr(1, R11, RAX);   // save int_part in R11
    x_mov_rr(1, RAX, RDX);   // RAX = frac_part

    x_mov_rr(1, R8, RBP);
    x_mov_ri(R9, 10);

    // Strip trailing zeros from frac_part (keeping at least 1 fractional digit).
    x_mov_ri(RCX, 6);        // number of fractional digits remaining
    int strip_loop = x_label(), strip_done = x_label();
    x_bind(strip_loop);
    x_alu_ri(ALU_CMP, 1, RCX, 1);
    x_jcc(CC_LE, strip_done);
    x_mov_ri(RDX, 0);
    x_f7(6, 1, R9);          // RAX = quot, RDX = rem
    x_test_rr(1, RDX, RDX);
    int not_zero = x_label();
    x_jcc(CC_NE, not_zero);
    x_alu_ri(ALU_SUB, 1, RCX, 1);
    x_jmp(strip_loop);
    x_bind(not_zero);
    // Restore RAX = RAX * 10 + RDX
    x_imul_rr(1, RAX, R9);
    x_alu_rr(ALU_ADD, 1, RAX, RDX);
    x_bind(strip_done);

    // Emit the `RCX` fractional digits.
    int frac_loop = x_label();
    x_bind(frac_loop);
    x_alu_ri(ALU_SUB, 1, R8, 1);
    x_mov_ri(RDX, 0);
    x_f7(6, 1, R9);
    x_alu_ri(ALU_ADD, 0, RDX, '0');
    x_store8(R8, 0, RDX);
    x_alu_ri(ALU_SUB, 1, RCX, 1);
    x_jcc(CC_NE, frac_loop);

    // Emit '.'
    x_alu_ri(ALU_SUB, 1, R8, 1);
    x_store8_imm(R8, 0, '.');

    // Emit integer part from R11.
    x_mov_rr(1, RAX, R11);
    int fi_loop = x_label();
    x_bind(fi_loop);
    x_alu_ri(ALU_SUB, 1, R8, 1);
    x_mov_ri(RDX, 0);
    x_f7(6, 1, R9);
    x_alu_ri(ALU_ADD, 0, RDX, '0');
    x_store8(R8, 0, RDX);
    x_test_rr(1, RAX, RAX);
    x_jcc(CC_NE, fi_loop);

    x_test_rr(1, R10, R10);
    int f_no_minus = x_label();
    x_jcc(CC_E, f_no_minus);
    x_alu_ri(ALU_SUB, 1, R8, 1);
    x_store8_imm(R8, 0, '-');
    x_bind(f_no_minus);
    x_mov_rr(1, RCX, R8);
    x_mov_rr(1, RDX, RBP);
    x_alu_rr(ALU_SUB, 1, RDX, R8);
    x_call(cg->rt_write);
    x_leave();
    x_ret();
    emit_print_nl_wrapper(cg, cg->rt_print_f64, cg->rt_print_f64_raw);

    // rt_print_bool_raw(rcx = 0 or 1).
    uint32_t s_true_raw  = add_string(cg, "true", 4);
    uint32_t s_false_raw = add_string(cg, "false", 5);
    x_bind(cg->rt_print_bool_raw);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 32);
    int is_false = x_label(), b_done = x_label();
    x_test_rr(0, RCX, RCX);
    x_jcc(CC_E, is_false);
    x_lea_rip(RCX, SEC_RDATA, s_true_raw);
    x_mov_ri(RDX, 4);
    x_jmp(b_done);
    x_bind(is_false);
    x_lea_rip(RCX, SEC_RDATA, s_false_raw);
    x_mov_ri(RDX, 5);
    x_bind(b_done);
    x_call(cg->rt_write);
    x_leave();
    x_ret();
    emit_print_nl_wrapper(cg, cg->rt_print_bool, cg->rt_print_bool_raw);

    // rt_print_str(rcx = ptr, rdx = len): writes string followed by '\n'.
    emit_print_nl_wrapper(cg, cg->rt_print_str, cg->rt_write);

    // rt_str_eq(rcx = ptr1, rdx = len1, r8 = ptr2, r9 = len2) -> rax = 1 if equal, 0 otherwise.
    x_bind(cg->rt_str_eq);
    int seq_true = x_label(), seq_false = x_label(), seq_loop = x_label();
    x_alu_rr(ALU_CMP, 1, RDX, R9);
    x_jcc(CC_NE, seq_false);
    x_alu_rr(ALU_CMP, 1, RCX, R8);
    x_jcc(CC_E, seq_true);
    x_bind(seq_loop);
    x_test_rr(1, RDX, RDX);
    x_jcc(CC_E, seq_true);
    x_load_sized(RAX, RCX, 0, 1, false);
    x_load_sized(R10, R8, 0, 1, false);
    x_alu_rr(ALU_CMP, 0, RAX, R10);
    x_jcc(CC_NE, seq_false);
    x_alu_ri(ALU_ADD, 1, RCX, 1);
    x_alu_ri(ALU_ADD, 1, R8, 1);
    x_alu_ri(ALU_SUB, 1, RDX, 1);
    x_jmp(seq_loop);
    x_bind(seq_true);
    x_mov_ri(RAX, 1);
    x_ret();
    x_bind(seq_false);
    x_mov_ri(RAX, 0);
    x_ret();

    // rt_trap(rcx = msg_ptr, rdx = msg_len): writes message to stderr and exits with 101.
    x_bind(cg->rt_trap);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 64);
    x_store(RBP, -8, RCX);
    x_store(RBP, -16, RDX);
    x_mov_ri(RCX, (uint64_t)(uint32_t)-12); // STD_ERROR_HANDLE
    x_call_import(IMP_GETSTDHANDLE);
    x_mov_rr(1, RCX, RAX);
    x_load(RDX, RBP, -8);
    x_load(R8, RBP, -16);
    x_lea(R9, RBP, -24);
    x_mov_ri(RAX, 0);
    x_store(RSP, 32, RAX);
    x_call_import(IMP_WRITEFILE);
    x_mov_ri(RCX, 101);
    x_call_import(IMP_EXITPROCESS);
}

Image codegen(Program *prog, bool release) {
    A = (Asm){0};
    CG cg = {0};
    cg.release = release;

    // Layout .data: [0..8) = stdout handle, followed by global variables.
    cg.stdout_off = 0;
    buf_u64(&cg.data, 0);
    for (int i = 0; i < prog->globals.len; i++) {
        Symbol *g = prog->globals.items[i];
        buf_align(&cg.data, 8);
        g->data_off = (uint32_t)cg.data.len;
        if (g->type->kind == TY_ARRAY) {
            Type *elem_t = g->type->elem;
            Node *rhs = g->decl->rhs;
            int init_n = rhs ? rhs->list.len : 0;
            for (int j = 0; j < g->type->array_len; j++) {
                uint64_t v = 0;
                if (j < init_n) {
                    Node *el = rhs->list.items[j];
                    v = (elem_t->kind == TY_FLOAT) ? fval_bits(el->fval, elem_t)
                                                   : (uint64_t)(int64_t)el->cval;
                }
                if (elem_t->size == 1)      buf_u8(&cg.data, (uint8_t)v);
                else if (elem_t->size == 2) buf_u16(&cg.data, (uint16_t)v);
                else if (elem_t->size == 4) buf_u32(&cg.data, (uint32_t)v);
                else                        buf_u64(&cg.data, v);
            }
        } else if (g->type->kind == TY_STRUCT) {
            size_t base_off = cg.data.len;
            buf_zero(&cg.data, (size_t)g->type->size);
            Node *rhs = g->decl->rhs;
            if (rhs && rhs->kind == N_STRUCT_LIT) {
                for (int j = 0; j < rhs->list.len; j++) {
                    Node *fi = rhs->list.items[j];
                    uint64_t v = (fi->type->kind == TY_FLOAT)
                        ? fval_bits(fi->rhs->fval, fi->type)
                        : (uint64_t)(int64_t)fi->rhs->cval;
                    memcpy(cg.data.data + base_off + fi->field_offset, &v, (size_t)fi->type->size);
                }
            }
        } else if (g->type->kind == TY_FLOAT) {
            buf_u64(&cg.data, fval_bits(g->fval, g->type));
        } else {
            buf_u64(&cg.data, (uint64_t)(int64_t)g->cval);
        }
    }

    emit_runtime(&cg);

    for (int i = 0; i < prog->fns.len; i++) {
        prog->fns.items[i]->sym->label = x_label();
    }
    for (int i = 0; i < prog->fns.len; i++) {
        gen_fn(&cg, prog->fns.items[i]);
    }

    // Program entry point `_start`: cache stdout handle, call `main()`, ExitProcess(0).
    uint32_t entry = x_here();
    x_alu_ri(ALU_SUB, 1, RSP, 40); // align stack to 16 bytes + 32-byte shadow space
    x_mov_ri(RCX, (uint64_t)(uint32_t)-11); // STD_OUTPUT_HANDLE
    x_call_import(IMP_GETSTDHANDLE);
    x_store_rip(SEC_DATA, cg.stdout_off, RAX);
    x_call(prog->main_fn->label);
    x_mov_ri(RCX, 0);
    x_call_import(IMP_EXITPROCESS);

    x_resolve_labels();

    Image img = {0};
    img.text            = A.code;
    img.strings         = cg.strings;
    img.data            = cg.data;
    img.data_size       = (uint32_t)cg.data.len;
    img.fixups          = A.rip.items;
    img.nfixups         = A.rip.len;
    img.entry           = entry;
    img.dll_imports     = prog->dll_imports;
    img.total_iat_slots = prog->total_iat_slots;
    return img;
}
