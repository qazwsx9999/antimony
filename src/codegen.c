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
    int rt_print_i64;         // (rcx=val)
    int rt_print_u64;         // (rcx=val)
    int rt_print_f64;         // (rcx=f64 bits)
    int rt_print_bool;        // (rcx=0/1)
    int rt_print_str;         // (rcx=ptr, rdx=len)
    int rt_trap;              // (rcx=msg_ptr, rdx=msg_len) -> stderr + ExitProcess(101)

    int cur_epilogue;
    int temp_depth;           // expression temporary spill depth (1-based)
    int max_temp_depth;
    int base_slots;           // local variable slots in current function
    int max_out_stack_args;   // max args > 4 across calls in current function
} CG;

static uint32_t add_string(CG *cg, const char *s, int len) {
    uint32_t off = (uint32_t)cg->strings.len;
    buf_bytes(&cg->strings, s, (size_t)len);
    buf_u8(&cg->strings, 0);
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

static void load_var(Symbol *s, int reg) {
    if (s->hw_reg >= 0) {
        x_mov_rr(1, reg, s->hw_reg);
        return;
    }
    if (s->type->kind == TY_ARRAY || s->type->kind == TY_STRUCT) {
        // Evaluating a fixed-size array or struct produces its base pointer in `reg`.
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
    if (s->type->kind == TY_SLICE || s->type == &ty_str) {
        // Fat pointer {ptr, len} in (RAX, RDX).
        x_load(RAX, RBP, slot_disp(s->slot));
        x_load(RDX, RBP, slot_disp(s->slot) + 8);
        return;
    }
    if (s->is_global) x_load_rip(reg, SEC_DATA, s->data_off);
    else              x_load(reg, RBP, slot_disp(s->slot));
}

static void store_var(Symbol *s, int reg) {
    if (s->hw_reg >= 0) {
        x_mov_rr(1, s->hw_reg, reg);
        return;
    }
    if (s->type->kind == TY_SLICE || s->type == &ty_str) {
        x_store(RBP, slot_disp(s->slot), RAX);
        x_store(RBP, slot_disp(s->slot) + 8, RDX);
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
    if (n->is_const && (n->type->kind == TY_INT || n->type->kind == TY_ENUM || n->type == &ty_bool)) {
        int64_t v = (int64_t)n->cval;
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
        Type *t = n->sym->type;
        return !n->implicit_slice &&
               (t->kind == TY_INT || t->kind == TY_ENUM || t->kind == TY_BOOL ||
                t->kind == TY_FLOAT || t->kind == TY_PTR);
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
    case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE: {
        // ucomisd sets ZF/PF/CF; for `<` and `<=`, swap operands and use `>` / `>=`
        // so unordered NaN comparisons return false properly.
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
        return;
    }
    default:
        die("internal error: unhandled float op in codegen");
    }
}

static void gen_int_op(CG *cg, TokKind op, Type *t, Pos pos) {
    bool is_wrap = (op == T_PLUSW || op == T_MINUSW || op == T_STARW);

    switch (op) {
    case T_PLUS:
    case T_PLUSW:
        if (!is_wrap && !cg->release) {
            if (t->size == 8) {
                x_alu_rr(ALU_ADD, 1, RAX, RCX);
                emit_trap_if(cg, t->is_signed ? CC_O : CC_B, pos, "integer addition overflow");
            } else {
                x_alu_rr(ALU_ADD, 1, RAX, RCX);
                check_and_normalize_sub64(cg, t, t->is_signed, pos, "integer addition overflow");
            }
        } else {
            x_alu_rr(ALU_ADD, 1, RAX, RCX);
            normalize_rax(t);
        }
        return;

    case T_MINUS:
    case T_MINUSW:
        if (!is_wrap && !cg->release) {
            if (t->size == 8) {
                x_alu_rr(ALU_SUB, 1, RAX, RCX);
                emit_trap_if(cg, t->is_signed ? CC_O : CC_B, pos, "integer subtraction overflow");
            } else {
                x_alu_rr(ALU_SUB, 1, RAX, RCX);
                check_and_normalize_sub64(cg, t, true, pos, "integer subtraction overflow");
            }
        } else {
            x_alu_rr(ALU_SUB, 1, RAX, RCX);
            normalize_rax(t);
        }
        return;

    case T_STAR:
    case T_STARW:
        if (!is_wrap && !cg->release) {
            if (t->size == 8) {
                if (t->is_signed) {
                    x_imul_rr(1, RAX, RCX);
                    emit_trap_if(cg, CC_O, pos, "integer multiplication overflow");
                } else {
                    x_f7(4, 1, RCX); // mul rcx -> RDX:RAX, sets CF/OF if RDX != 0
                    emit_trap_if(cg, CC_O, pos, "integer multiplication overflow");
                }
            } else {
                x_imul_rr(1, RAX, RCX);
                check_and_normalize_sub64(cg, t, t->is_signed, pos, "integer multiplication overflow");
            }
        } else {
            x_imul_rr(1, RAX, RCX);
            normalize_rax(t);
        }
        return;

    case T_SLASH:
    case T_PERCENT: {
        // Spec §8.4: division by zero and signed MIN / -1 ALWAYS trap.
        x_test_rr(1, RCX, RCX);
        emit_trap_if(cg, CC_E, pos, "division by zero");

        if (t->is_signed) {
            // Check MIN / -1 for the operand width.
            int ok = x_label();
            x_alu_ri(ALU_CMP, 1, RCX, -1);
            x_jcc(CC_NE, ok);
            int64_t minv = (t->size == 8) ? INT64_MIN : -(1LL << (t->size * 8 - 1));
            x_mov_ri(RDX, (uint64_t)minv);
            x_alu_rr(ALU_CMP, 1, RAX, RDX);
            emit_trap_if(cg, CC_E, pos, "integer division overflow (MIN / -1)");
            x_bind(ok);

            x_cqo();
            x_f7(7, 1, RCX); // idiv rcx
        } else {
            x_mov_ri(RDX, 0);
            x_f7(6, 1, RCX); // div rcx
        }
        if (op == T_PERCENT) x_mov_rr(1, RAX, RDX);
        normalize_rax(t);
        return;
    }

    case T_AMP:
        x_alu_rr(ALU_AND, 1, RAX, RCX);
        return;
    case T_PIPE:
        x_alu_rr(ALU_OR, 1, RAX, RCX);
        return;
    case T_CARET:
        x_alu_rr(ALU_XOR, 1, RAX, RCX);
        return;

    case T_SHL:
    case T_SHR: {
        int bits = t->size * 8;
        if (!cg->release) {
            x_alu_ri(ALU_CMP, 1, RCX, bits);
            emit_trap_if(cg, CC_AE, pos, "shift count >= bit width of type");
        } else if (bits < 64) {
            x_alu_ri(ALU_AND, 0, RCX, bits - 1);
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
        x_alu_rr(ALU_CMP, 1, RAX, RCX);
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

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

static void gen_expr(CG *cg, Node *n);

// Evaluates an array or slice expression `base` into (RAX = data_ptr, RDX = len).
static void gen_collection_ptr_len(CG *cg, Node *base) {
    gen_expr(cg, base);
    if (base->type->kind == TY_ARRAY && !base->implicit_slice) {
        x_mov_ri(RDX, (uint64_t)base->type->array_len);
    }
}

// Evaluates `n` (an N_INDEX node `base[idx]`), performs bounds checking
// (`0 <= idx < len`) for arrays/slices or a debug null check for pointers,
// and returns the element address in RAX.
static void gen_index_addr(CG *cg, Node *n) {
    Type *bt = n->lhs->type;
    Type *elem_t = bt->elem;

    if (bt->kind == TY_PTR) {
        gen_expr(cg, n->lhs); // RAX = ptr
        if (!cg->release) {
            x_test_rr(1, RAX, RAX);
            emit_trap_if(cg, CC_E, n->pos, "null pointer dereference");
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
        x_alu_ri(ALU_CMP, 1, RAX, bt->array_len);
        emit_trap_if(cg, CC_AE, n->rhs->pos, "index out of bounds");
        scale_reg_by_size(RAX, elem_t->size, RDX);
        load_var(s, RCX);
        x_alu_rr(ALU_ADD, 1, RAX, RCX);
        return;
    }

    // Fast path for slice variable `s[idx]` with simple index: zero stack spills.
    if (n->lhs->kind == N_IDENT && bt->kind == TY_SLICE && is_simple_expr(n->rhs)) {
        Symbol *s = n->lhs->sym;
        gen_simple_into_reg(n->rhs, RAX);
        x_load(RDX, RBP, slot_disp(s->slot) + 8);
        x_alu_rr(ALU_CMP, 1, RAX, RDX);
        emit_trap_if(cg, CC_AE, n->rhs->pos, "index out of bounds");
        scale_reg_by_size(RAX, elem_t->size, RDX);
        x_load(RCX, RBP, slot_disp(s->slot));
        x_alu_rr(ALU_ADD, 1, RAX, RCX);
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
    x_alu_rr(ALU_CMP, 1, RAX, RDX);
    emit_trap_if(cg, CC_AE, n->rhs->pos, "index out of bounds");

    scale_reg_by_size(RAX, elem_t->size, RDX);
    x_alu_rr(ALU_ADD, 1, RAX, RCX); // RAX = base_ptr + idx * elem_size
}

static void gen_lvalue_addr(CG *cg, Node *n);
static void gen_init_at_addr(CG *cg, int addr_slot, Node *rhs, Type *t);

static void gen_lvalue_addr(CG *cg, Node *n) {
    if (n->kind == N_IDENT) {
        Symbol *s = n->sym;
        if (s->is_global) {
            x_lea_rip(RAX, SEC_DATA, s->data_off);
        } else if (s->kind == SYM_PARAM && (s->type->kind == TY_ARRAY || s->type->kind == TY_STRUCT)) {
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
        } else if (n->lhs->kind == N_STRUCT_LIT) {
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

// Writes `rhs` (an N_STRUCT_LIT, N_ARRAY_LIT, or arbitrary expression of type `t`)
// into the memory address stored at stack temporary `addr_slot`.
static void gen_init_at_addr(CG *cg, int addr_slot, Node *rhs, Type *t) {
    if (rhs->kind == N_STRUCT_LIT) {
        x_load(RDI, RBP, slot_disp(addr_slot));
        x_mov_ri(RAX, 0);
        x_mov_ri(RCX, (uint64_t)t->size);
        x_rep_stosb();
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
        x_load(RDI, RBP, slot_disp(addr_slot));
        x_mov_ri(RAX, 0);
        x_mov_ri(RCX, (uint64_t)t->size);
        x_rep_stosb();
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
    if (t->kind == TY_ARRAY || t->kind == TY_STRUCT) {
        gen_expr(cg, rhs); // RAX = src address
        x_mov_rr(1, RSI, RAX);
        x_load(RDI, RBP, slot_disp(addr_slot));
        x_mov_ri(RCX, (uint64_t)t->size);
        x_rep_movsb();
        return;
    }
    gen_expr(cg, rhs);
    x_load(RCX, RBP, slot_disp(addr_slot));
    x_store_sized(RCX, 0, RAX, t->size);
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
        if (elem_t->kind == TY_ARRAY || elem_t->kind == TY_STRUCT) {
            // Address of array/struct is already in RAX.
            return;
        }
        x_load_sized(RAX, RAX, 0, elem_t->size, elem_t->is_signed);
        return;
    }

    case N_STRUCT_LIT: {
        x_lea(RAX, RBP, slot_disp(n->slot_start));
        int s_addr = push_temp(cg);
        gen_init_at_addr(cg, s_addr, n, n->type);
        pop_temp(cg, s_addr, RAX);
        return;
    }

    case N_FIELD: {
        Type *ft = n->type;
        gen_lvalue_addr(cg, n);
        if (ft->kind == TY_ARRAY || ft->kind == TY_STRUCT) {
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
        gen_index_addr(cg, n);
        if (elem_t->kind == TY_ARRAY || elem_t->kind == TY_STRUCT) {
            return;
        }
        x_load_sized(RAX, RAX, 0, elem_t->size, elem_t->is_signed);
        return;
    }

    case N_SLICE: {
        Type *bt = n->lhs->type;
        Type *elem_t = n->type->elem;
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
        gen_expr(cg, n->lhs);
        if (src == dst) return;
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
                normalize_rax(lt);
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
        }

        // Evaluate left to right (spec §7.2); skip stack spill when RHS is a leaf operand.
        if (is_simple_expr(n->rhs)) {
            gen_expr(cg, n->lhs);
            gen_simple_into_reg(n->rhs, RCX);
        } else {
            gen_expr(cg, n->lhs);
            int slot = push_temp(cg);
            gen_expr(cg, n->rhs);
            x_mov_rr(1, RCX, RAX);
            pop_temp(cg, slot, RAX);
        }

        if (n->lhs->type->kind == TY_PTR && (n->op == T_PLUS || n->op == T_MINUS)) {
            int esz = n->lhs->type->elem->size;
            if (n->op == T_MINUS && n->rhs->type == n->lhs->type) {
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
            Node *arg = n->list.items[0];
            gen_expr(cg, arg);
            if (arg->type == &ty_str) {
                x_mov_rr(1, RCX, RAX); // RDX already holds length
                x_call(cg->rt_print_str);
            } else if (arg->type == &ty_bool) {
                x_mov_rr(1, RCX, RAX);
                x_call(cg->rt_print_bool);
            } else if (arg->type->kind == TY_FLOAT) {
                gpr_to_xmm_f64(0, RAX, arg->type);
                x_movq_gpr_xmm(RCX, 0);
                x_call(cg->rt_print_f64);
            } else if (arg->type->kind == TY_PTR) {
                x_mov_rr(1, RCX, RAX);
                x_call(cg->rt_print_u64);
            } else if (arg->type->is_signed) {
                x_mov_rr(1, RCX, RAX);
                x_call(cg->rt_print_i64);
            } else {
                x_mov_rr(1, RCX, RAX);
                x_call(cg->rt_print_u64);
            }
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

        // Evaluate arguments left to right into temporary slots.
        // Note: a slice argument `{ptr, len}` occupies TWO 8-byte parameter slots
        // (or a pointer to a 16-byte temporary); to keep ABI simple and fast, we
        // pass a slice by address of its 16-byte `{ptr, len}` descriptor in a
        // single register/stack argument, and the callee copies the 16 bytes into
        // its local slice slot in the prologue.
        int nargs = n->list.len;
        if (nargs > 4 && (nargs - 4) > cg->max_out_stack_args) {
            cg->max_out_stack_args = nargs - 4;
        }
        int *arg_slots = nargs ? xmalloc((size_t)nargs * sizeof(int)) : NULL;
        for (int i = 0; i < nargs; i++) {
            Node *arg = n->list.items[i];
            Type *pt = n->sym->decl->list.items[i]->type;
            if (pt->kind == TY_SLICE) {
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
        static const int arg_regs[4] = {RCX, RDX, R8, R9};
        for (int i = nargs - 1; i >= 0; i--) {
            Type *pt = n->sym->decl->list.items[i]->type;
            if (i < 4) {
                pop_temp(cg, arg_slots[i], arg_regs[i]);
                if (n->sym->is_extern && pt->kind == TY_FLOAT) {
                    x_movq_xmm_gpr(i, arg_regs[i]);
                }
            } else {
                pop_temp(cg, arg_slots[i], RAX);
                x_store(RSP, 32 + (i - 4) * 8, RAX);
            }
            if (pt->kind == TY_SLICE) {
                cg->temp_depth -= 2; // release the 16-byte temporary descriptor
            }
        }
        free(arg_slots);
        if (n->sym->is_extern) {
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
            x_call(n->sym->label);
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
        if (s->type->kind == TY_ARRAY || s->type->kind == TY_STRUCT) {
            if (s->is_global) x_lea_rip(RAX, SEC_DATA, s->data_off);
            else              x_lea(RAX, RBP, slot_disp(s->slot));
            int s_addr = push_temp(cg);
            gen_init_at_addr(cg, s_addr, n->rhs, s->type);
            cg->temp_depth--;
            return;
        }
        if (s->type->kind == TY_SLICE) {
            gen_collection_ptr_len(cg, n->rhs);
            store_var(s, RAX);
            return;
        }
        if (s->hw_reg >= 0 && is_simple_expr(n->rhs)) {
            gen_simple_into_reg(n->rhs, s->hw_reg);
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
    if (cond->kind == N_BINARY && cond->lhs->type->kind != TY_FLOAT &&
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
                           cond->lhs->sym->hw_reg >= 0);
        bool rhs_in_reg = (cond->rhs->kind == N_IDENT && !cond->rhs->implicit_slice &&
                           cond->rhs->sym->hw_reg >= 0);

        if (lhs_in_reg && fits_imm32(cond->rhs, &imm32)) {
            x_alu_ri(ALU_CMP, 1, cond->lhs->sym->hw_reg, imm32);
        } else if (lhs_in_reg && rhs_in_reg) {
            x_alu_rr(ALU_CMP, 1, cond->lhs->sym->hw_reg, cond->rhs->sym->hw_reg);
        } else if (fits_imm32(cond->rhs, &imm32)) {
            gen_expr(cg, cond->lhs);
            x_alu_ri(ALU_CMP, 1, RAX, imm32);
        } else if (is_simple_expr(cond->rhs)) {
            gen_expr(cg, cond->lhs);
            gen_simple_into_reg(cond->rhs, RCX);
            x_alu_rr(ALU_CMP, 1, RAX, RCX);
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

    // Fast path for forward unit-step range loops `loop i in a..b`:
    // No division needed; increments `i` by 1 until `i == end`.
    if (!n->rev && (!n->by || (n->by->is_const && n->by->cval == 1))) {
        gen_expr(cg, n->lo);
        x_store(RBP, slot_disp(s_start), RAX);
        gen_expr(cg, n->hi);
        x_store(RBP, slot_disp(s_end), RAX);
        x_mov_rr(1, RCX, RAX);
        x_load(RAX, RBP, slot_disp(s_start));
        x_alu_rr(ALU_CMP, 1, RAX, RCX);
        x_jcc(t->is_signed ? CC_GE : CC_AE, l_brk);

        store_var(n->sym, RAX);

        x_bind(l_cond);
        gen_block(cg, n->body);

        x_bind(l_cont);
        int32_t end_imm = 0;
        if (n->sym->hw_reg >= 0) {
            int r = n->sym->hw_reg;
            x_alu_ri(ALU_ADD, 1, r, 1);
            if (fits_imm32(n->hi, &end_imm)) {
                x_alu_ri(ALU_CMP, 1, r, end_imm);
            } else {
                x_alu_rm(ALU_CMP, 1, r, RBP, slot_disp(s_end));
            }
            x_jcc(CC_NE, l_cond);
        } else {
            load_var(n->sym, RAX);
            x_alu_ri(ALU_ADD, 1, RAX, 1);
            store_var(n->sym, RAX);
            if (fits_imm32(n->hi, &end_imm)) {
                x_alu_ri(ALU_CMP, 1, RAX, end_imm);
            } else {
                x_alu_rm(ALU_CMP, 1, RAX, RBP, slot_disp(s_end));
            }
            x_jcc(CC_NE, l_cond);
        }
        x_bind(l_brk);
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
    if (elem_t->kind == TY_STRUCT) {
        x_mov_rr(1, RSI, RAX);
        x_lea(RDI, RBP, slot_disp(val_sym->slot));
        x_mov_ri(RCX, (uint64_t)elem_t->size);
        x_rep_movsb();
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
        return;

    case N_ASSIGN:
        gen_assign(cg, n);
        return;

    case N_EXPR_STMT:
        gen_expr(cg, n->lhs);
        return;

    case N_RETURN:
        if (n->lhs) gen_expr(cg, n->lhs);
        x_jmp(cg->cur_epilogue);
        return;

    case N_BREAK:
        x_jmp(n->target->label_break);
        return;

    case N_CONTINUE:
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
        int l_else = n->els ? x_label() : l_end;
        x_jmp(l_else);
        for (int i = 0; i < n->list.len; i++) {
            x_bind(l_arms[i]);
            gen_block(cg, n->list.items[i]->body);
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
        x_bind(l_cond);
        gen_cond_jump(cg, n->cond, l_brk, false);
        gen_block(cg, n->body);
        x_jmp(l_cond);
        x_bind(l_brk);
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

static void gen_block(CG *cg, Node *b) {
    for (int i = 0; i < b->list.len; i++) {
        gen_stmt(cg, b->list.items[i]);
    }
}

// ---------------------------------------------------------------------------
// Loop-weighted callee-saved register allocator (RBX, R12, R13, R14, R15)
// ---------------------------------------------------------------------------

typedef VEC(Symbol *) SymVec;

static void record_local_sym(SymVec *locals, Symbol *s) {
    if (!s || s->is_global) return;
    if (s->kind != SYM_VAR && s->kind != SYM_PARAM) return;
    for (int i = 0; i < locals->len; i++) {
        if (locals->items[i] == s) return;
    }
    s->addr_taken = false;
    s->use_score  = 0;
    s->hw_reg     = -1;
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
    case N_LOOP_INF:
        scan_node_vars(locals, n->body, loop_w);
        return;
    case N_LOOP_COND:
        scan_node_vars(locals, n->cond, loop_w);
        scan_node_vars(locals, n->body, loop_w);
        return;
    case N_LOOP_RANGE:
        scan_node_vars(locals, n->lo, weight);
        scan_node_vars(locals, n->hi, weight);
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

static int alloc_fn_regs(Node *fn, int out_regs[5]) {
    static const int callee_regs[5] = {RBX, R12, R13, R14, R15};
    SymVec locals = {0};
    for (int i = 0; i < fn->list.len; i++) {
        record_local_sym(&locals, fn->list.items[i]->sym);
    }
    scan_node_vars(&locals, fn->body, 1);

    int n_alloc = 0;
    for (int k = 0; k < 5; k++) {
        Symbol *best = NULL;
        for (int i = 0; i < locals.len; i++) {
            Symbol *s = locals.items[i];
            if (s->hw_reg >= 0 || s->addr_taken || s->use_score < 2) continue;
            Type *t = s->type;
            if (!t || (t->kind != TY_INT && t->kind != TY_ENUM && t->kind != TY_BOOL &&
                       t->kind != TY_FLOAT && t->kind != TY_PTR)) continue;
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

static void gen_fn(CG *cg, Node *fn) {
    int saved_regs[5];
    int num_saved = alloc_fn_regs(fn, saved_regs);

    x_bind(fn->sym->label);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    uint32_t patch = x_sub_rsp_patchable();

    cg->cur_epilogue = x_label();
    cg->base_slots = fn->num_slots + num_saved;
    cg->temp_depth = 0;
    cg->max_temp_depth = 0;
    cg->max_out_stack_args = 0;

    // Save allocated non-volatile registers (RBX, R12..R15).
    for (int k = 0; k < num_saved; k++) {
        x_store(RBP, slot_disp(fn->num_slots + 1 + k), saved_regs[k]);
    }

    // Move incoming parameters into their allocated registers or stack slots
    // (Microsoft x64 ABI: RCX, RDX, R8, R9, then [RBP+48+...]).
    static const int arg_regs[4] = {RCX, RDX, R8, R9};
    for (int i = 0; i < fn->list.len; i++) {
        Symbol *ps = fn->list.items[i]->sym;
        int reg = RAX;
        if (i < 4) {
            reg = arg_regs[i];
        } else {
            x_load(RAX, RBP, 48 + (i - 4) * 8);
        }
        if (ps->type->kind == TY_SLICE) {
            // Parameter register holds pointer to 16-byte `{ptr, len}` descriptor.
            x_load(R10, reg, 0);
            x_load(R11, reg, 8);
            x_store(RBP, slot_disp(ps->slot), R10);
            x_store(RBP, slot_disp(ps->slot) + 8, R11);
        } else {
            store_var(ps, reg);
        }
    }

    gen_block(cg, fn->body);

    x_bind(cg->cur_epilogue);
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
}

// ---------------------------------------------------------------------------
// Built-in Windows x64 runtime routines (no CRT, only kernel32.dll)
// ---------------------------------------------------------------------------

static void emit_runtime(CG *cg) {
    cg->rt_write      = x_label();
    cg->rt_print_i64  = x_label();
    cg->rt_print_u64  = x_label();
    cg->rt_print_f64  = x_label();
    cg->rt_print_bool = x_label();
    cg->rt_print_str  = x_label();
    cg->rt_trap       = x_label();

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

    // rt_print_u64(rcx = unsigned 64-bit value): formats into stack buffer + '\n'.
    x_bind(cg->rt_print_u64);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 64);
    x_mov_rr(1, RAX, RCX);
    x_lea(R8, RBP, -1);
    x_store8_imm(R8, 0, '\n');
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

    // rt_print_i64(rcx = signed 64-bit value).
    x_bind(cg->rt_print_i64);
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
    x_lea(R8, RBP, -1);
    x_store8_imm(R8, 0, '\n');
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

    // rt_print_f64(rcx = f64 IEEE-754 bits):
    // Formats with 6 decimal digits (rounded to nearest millionth), trimming trailing
    // zeros up to at least 1 digit after '.', e.g. `3.141593`, `2.5`, `-0.25`.
    x_bind(cg->rt_print_f64);
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

    x_lea(R8, RBP, -1);
    x_store8_imm(R8, 0, '\n');
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

    // rt_print_bool(rcx = 0 or 1).
    uint32_t s_true  = add_string(cg, "true\n", 5);
    uint32_t s_false = add_string(cg, "false\n", 6);
    x_bind(cg->rt_print_bool);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 32);
    int is_false = x_label(), b_done = x_label();
    x_test_rr(0, RCX, RCX);
    x_jcc(CC_E, is_false);
    x_lea_rip(RCX, SEC_RDATA, s_true);
    x_mov_ri(RDX, 5);
    x_jmp(b_done);
    x_bind(is_false);
    x_lea_rip(RCX, SEC_RDATA, s_false);
    x_mov_ri(RDX, 6);
    x_bind(b_done);
    x_call(cg->rt_write);
    x_leave();
    x_ret();

    // rt_print_str(rcx = ptr, rdx = len): writes string followed by '\n'.
    uint32_t s_nl = add_string(cg, "\n", 1);
    x_bind(cg->rt_print_str);
    x_push(RBP);
    x_mov_rr(1, RBP, RSP);
    x_alu_ri(ALU_SUB, 1, RSP, 32);
    x_call(cg->rt_write);
    x_lea_rip(RCX, SEC_RDATA, s_nl);
    x_mov_ri(RDX, 1);
    x_call(cg->rt_write);
    x_leave();
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
