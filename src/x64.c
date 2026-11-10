// x64.c - native x86-64 machine-code encoder.
#include "sb.h"

Asm A;

int x_label(void) {
    int id = A.label_pos.len;
    vec_push(A.label_pos, -1);
    return id;
}

void x_bind(int label) {
    A.label_pos.items[label] = (int)A.code.len;
}

uint32_t x_here(void) {
    return (uint32_t)A.code.len;
}

static void emit8(uint8_t b)   { buf_u8(&A.code, b); }
static void emit32(uint32_t v) { buf_u32(&A.code, v); }
static void emit64(uint64_t v) { buf_u64(&A.code, v); }

static void emit_rex(int w, int r, int x, int b) {
    uint8_t rex = 0x40 | ((w & 1) << 3) | (((r >> 3) & 1) << 2) |
                  (((x >> 3) & 1) << 1) | ((b >> 3) & 1);
    if (rex != 0x40) emit8(rex);
}

static void emit_rex_byte(int r, int b) {
    uint8_t rex = 0x40 | (((r >> 3) & 1) << 2) | ((b >> 3) & 1);
    if (rex != 0x40 || (r & 7) >= 4 || (b & 7) >= 4) emit8(rex);
}

static void modrm(int mod, int reg, int rm) {
    emit8((uint8_t)(((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7)));
}

static void mem_disp(int reg, int base, int32_t disp) {
    int b = base & 7;
    if (disp == 0 && b != RBP) {
        modrm(0, reg, b);
        if (b == RSP) emit8(0x24);
    } else if (disp >= -128 && disp <= 127) {
        modrm(1, reg, b);
        if (b == RSP) emit8(0x24);
        emit8((uint8_t)(int8_t)disp);
    } else {
        modrm(2, reg, b);
        if (b == RSP) emit8(0x24);
        emit32((uint32_t)disp);
    }
}

static void mem_sib(int reg, int base, int index, int scale) {
    int ss = (scale == 8) ? 3 : (scale == 4) ? 2 : (scale == 2) ? 1 : 0;
    int b = base & 7;
    int x = index & 7;
    if (b == RBP) {
        modrm(1, reg, 4);
        emit8((uint8_t)((ss << 6) | (x << 3) | b));
        emit8(0);
    } else {
        modrm(0, reg, 4);
        emit8((uint8_t)((ss << 6) | (x << 3) | b));
    }
}

static void rip_rel(int reg, int sec, uint32_t off, int addend) {
    modrm(0, reg, 5); // [RIP + disp32]
    uint32_t pos = x_here();
    emit32(0);
    RipFixup fx = {pos, sec, off, addend};
    vec_push(A.rip, fx);
}

void x_mov_rr(int w, int dst, int src) {
    if (w && dst == src) return;
    emit_rex(w, src, 0, dst);
    emit8(0x89);
    modrm(3, src, dst);
}

void x_mov_ri(int dst, uint64_t imm) {
    if (imm == 0) {
        emit_rex(0, dst, 0, dst);
        emit8(0x31);
        modrm(3, dst, dst);
        return;
    }
    if (imm <= UINT32_MAX) {
        emit_rex(0, 0, 0, dst);
        emit8((uint8_t)(0xB8 + (dst & 7)));
        emit32((uint32_t)imm);
        return;
    }
    if ((int64_t)imm >= INT32_MIN && (int64_t)imm <= INT32_MAX) {
        emit_rex(1, 0, 0, dst);
        emit8(0xC7);
        modrm(3, 0, dst);
        emit32((uint32_t)(int32_t)(int64_t)imm);
        return;
    }
    emit_rex(1, 0, 0, dst);
    emit8((uint8_t)(0xB8 + (dst & 7)));
    emit64(imm);
}

void x_load(int dst, int base, int32_t disp) {
    emit_rex(1, dst, 0, base);
    emit8(0x8B);
    mem_disp(dst, base, disp);
}

void x_load_sized(int dst, int base, int32_t disp, int size, bool is_signed) {
    if (size == 8) {
        x_load(dst, base, disp);
    } else if (size == 4) {
        if (is_signed) {
            emit_rex(1, dst, 0, base);
            emit8(0x63); // movsxd r64, m32
            mem_disp(dst, base, disp);
        } else {
            emit_rex(0, dst, 0, base);
            emit8(0x8B); // mov r32, m32 (zero-extends to 64-bit)
            mem_disp(dst, base, disp);
        }
    } else if (size == 2) {
        emit_rex(1, dst, 0, base);
        emit8(0x0F);
        emit8(is_signed ? 0xBF : 0xB7); // movsx/movzx r64, m16
        mem_disp(dst, base, disp);
    } else {
        emit_rex(1, dst, 0, base);
        emit8(0x0F);
        emit8(is_signed ? 0xBE : 0xB6); // movsx/movzx r64, m8
        mem_disp(dst, base, disp);
    }
}

void x_load_sib_sized(int dst, int base, int index, int scale, int size, bool is_signed) {
    if (size == 8) {
        emit_rex(1, dst, index, base);
        emit8(0x8B);
        mem_sib(dst, base, index, scale);
    } else if (size == 4) {
        if (is_signed) {
            emit_rex(1, dst, index, base);
            emit8(0x63); // movsxd r64, [base+index*scale]
            mem_sib(dst, base, index, scale);
        } else {
            emit_rex(0, dst, index, base);
            emit8(0x8B); // mov r32, [base+index*scale]
            mem_sib(dst, base, index, scale);
        }
    } else if (size == 2) {
        emit_rex(1, dst, index, base);
        emit8(0x0F);
        emit8(is_signed ? 0xBF : 0xB7);
        mem_sib(dst, base, index, scale);
    } else {
        emit_rex(1, dst, index, base);
        emit8(0x0F);
        emit8(is_signed ? 0xBE : 0xB6);
        mem_sib(dst, base, index, scale);
    }
}

void x_store(int base, int32_t disp, int src) {
    emit_rex(1, src, 0, base);
    emit8(0x89);
    mem_disp(src, base, disp);
}

void x_store_sized(int base, int32_t disp, int src, int size) {
    if (size == 8) {
        x_store(base, disp, src);
    } else if (size == 4) {
        emit_rex(0, src, 0, base);
        emit8(0x89); // mov m32, r32
        mem_disp(src, base, disp);
    } else if (size == 2) {
        emit8(0x66); // 16-bit operand size prefix
        emit_rex(0, src, 0, base);
        emit8(0x89); // mov m16, r16
        mem_disp(src, base, disp);
    } else {
        x_store8(base, disp, src);
    }
}

void x_store8(int base, int32_t disp, int src) {
    emit_rex_byte(src, base);
    emit8(0x88);
    mem_disp(src, base, disp);
}

void x_store8_imm(int base, int32_t disp, uint8_t imm) {
    emit_rex(0, 0, 0, base);
    emit8(0xC6);
    mem_disp(0, base, disp);
    emit8(imm);
}

void x_load_rip(int dst, int sec, uint32_t off) {
    emit_rex(1, dst, 0, 0);
    emit8(0x8B);
    rip_rel(dst, sec, off, 0);
}

void x_store_rip(int sec, uint32_t off, int src) {
    emit_rex(1, src, 0, 0);
    emit8(0x89);
    rip_rel(src, sec, off, 0);
}

void x_lea_rip(int dst, int sec, uint32_t off) {
    emit_rex(1, dst, 0, 0);
    emit8(0x8D);
    rip_rel(dst, sec, off, 0);
}

void x_lea_label(int dst, int label) {
    emit_rex(1, dst, 0, 0);
    emit8(0x8D);
    modrm(0, dst, 5); // [RIP + disp32]
    uint32_t pos = x_here();
    emit32(0);
    LabelFixup fx = {pos, label};
    vec_push(A.fixups, fx);
}

void x_lea(int dst, int base, int32_t disp) {
    emit_rex(1, dst, 0, base);
    emit8(0x8D);
    mem_disp(dst, base, disp);
}

void x_lea_sib(int dst, int base, int index, int scale) {
    emit_rex(1, dst, index, base);
    emit8(0x8D);
    mem_sib(dst, base, index, scale);
}

// alu: ADD=0 OR=1 AND=4 SUB=5 XOR=6 CMP=7
void x_alu_rr(int alu, int w, int dst, int src) {
    emit_rex(w, src, 0, dst);
    emit8((uint8_t)((alu << 3) | 1));
    modrm(3, src, dst);
}

void x_alu_ri(int alu, int w, int dst, int32_t imm) {
    emit_rex(w, 0, 0, dst);
    if (imm >= -128 && imm <= 127) {
        emit8(0x83);
        modrm(3, alu, dst);
        emit8((uint8_t)(int8_t)imm);
    } else {
        emit8(0x81);
        modrm(3, alu, dst);
        emit32((uint32_t)imm);
    }
}

void x_alu_rm(int alu, int w, int dst, int base, int32_t disp) {
    emit_rex(w, dst, 0, base);
    emit8((uint8_t)((alu << 3) | 3));
    mem_disp(dst, base, disp);
}

void x_alu_mr_rip(int alu, int sec, uint32_t off, int src) {
    emit_rex(1, src, 0, 0);
    emit8((uint8_t)((alu << 3) | 1));
    rip_rel(src, sec, off, 0);
}

void x_test_rr(int w, int a, int b) {
    emit_rex(w, b, 0, a);
    emit8(0x85);
    modrm(3, b, a);
}

void x_imul_rr(int w, int dst, int src) {
    emit_rex(w, dst, 0, src);
    emit8(0x0F);
    emit8(0xAF);
    modrm(3, dst, src);
}

void x_imul_rri(int w, int dst, int src, int32_t imm) {
    emit_rex(w, dst, 0, src);
    if (imm >= -128 && imm <= 127) {
        emit8(0x6B);
        modrm(3, dst, src);
        emit8((uint8_t)(int8_t)imm);
    } else {
        emit8(0x69);
        modrm(3, dst, src);
        emit32((uint32_t)imm);
    }
}

void x_f7(int ext, int w, int r) {
    emit_rex(w, 0, 0, r);
    emit8(0xF7);
    modrm(3, ext, r);
}

void x_cqo(void) { emit8(0x48); emit8(0x99); }
void x_cdq(void) { emit8(0x99); }

void x_shift_cl(int ext, int w, int r) {
    emit_rex(w, 0, 0, r);
    emit8(0xD3);
    modrm(3, ext, r);
}

void x_shift_imm(int ext, int w, int r, uint8_t imm) {
    emit_rex(w, 0, 0, r);
    emit8(0xC1);
    modrm(3, ext, r);
    emit8(imm);
}

void x_movsx8(int dst, int src) {
    uint8_t rex = 0x48 | (((dst >> 3) & 1) << 2) | ((src >> 3) & 1);
    emit8(rex);
    emit8(0x0F);
    emit8(0xBE);
    modrm(3, dst, src);
}

void x_movsx16(int dst, int src) {
    emit_rex(1, dst, 0, src);
    emit8(0x0F);
    emit8(0xBF);
    modrm(3, dst, src);
}

void x_movsxd(int dst, int src) {
    emit_rex(1, dst, 0, src);
    emit8(0x63);
    modrm(3, dst, src);
}

void x_movzx8(int dst, int src) {
    emit_rex_byte(dst, src);
    emit8(0x0F);
    emit8(0xB6);
    modrm(3, dst, src);
}

void x_movzx16(int dst, int src) {
    emit_rex(0, dst, 0, src);
    emit8(0x0F);
    emit8(0xB7);
    modrm(3, dst, src);
}

void x_setcc(int cc, int r) {
    emit_rex_byte(0, r);
    emit8(0x0F);
    emit8((uint8_t)(0x90 + cc));
    modrm(3, 0, r);
}

static void emit_rel32_to(int label) {
    uint32_t pos = x_here();
    emit32(0);
    LabelFixup fx = {pos, label};
    vec_push(A.fixups, fx);
}

void x_jmp(int label) {
    emit8(0xE9);
    emit_rel32_to(label);
}

void x_jcc(int cc, int label) {
    emit8(0x0F);
    emit8((uint8_t)(0x80 + cc));
    emit_rel32_to(label);
}

void x_call(int label) {
    emit8(0xE8);
    emit_rel32_to(label);
}

void x_call_reg(int reg) {
    if (reg >= 8) emit8(0x41);
    emit8(0xFF);
    modrm(3, 2, reg);
}

void x_xchg_mr(int base, int src) {
    emit_rex(1, src, 0, base);
    emit8(0x87); // xchg [base], src (implicitly LOCKed on x86-64)
    mem_disp(src, base, 0);
}

void x_lock_xadd(int base, int src_dst) {
    emit8(0xF0); // LOCK prefix
    emit_rex(1, src_dst, 0, base);
    emit8(0x0F);
    emit8(0xC1); // lock xadd [base], src_dst
    mem_disp(src_dst, base, 0);
}

void x_lock_cmpxchg(int base, int desired_reg) {
    emit8(0xF0); // LOCK prefix
    emit_rex(1, desired_reg, 0, base);
    emit8(0x0F);
    emit8(0xB1); // lock cmpxchg [base], desired_reg (compares RAX with [base])
    mem_disp(desired_reg, base, 0);
}

void x_call_import(int imp) {
    emit8(0xFF);
    rip_rel(2, SEC_IAT, (uint32_t)imp * 8, 0);
}

void x_push(int r) {
    if (r >= 8) emit8(0x41);
    emit8((uint8_t)(0x50 + (r & 7)));
}

void x_pop(int r) {
    if (r >= 8) emit8(0x41);
    emit8((uint8_t)(0x58 + (r & 7)));
}

void x_ret(void)   { emit8(0xC3); }
void x_leave(void) { emit8(0xC9); }

uint32_t x_sub_rsp_patchable(void) {
    emit8(0x48);
    emit8(0x81);
    emit8(0xEC);
    uint32_t pos = x_here();
    emit32(0);
    return pos;
}

void x_rep_movsb(void) {
    emit8(0xF3);
    emit8(0xA4);
}

void x_rep_stosb(void) {
    emit8(0xF3);
    emit8(0xAA);
}

// movq xmm, gpr: 66 REX.W 0F 6E /r
void x_movq_xmm_gpr(int xmm, int gpr) {
    emit8(0x66);
    emit_rex(1, xmm, 0, gpr);
    emit8(0x0F);
    emit8(0x6E);
    modrm(3, xmm, gpr);
}

// movq gpr, xmm: 66 REX.W 0F 7E /r
void x_movq_gpr_xmm(int gpr, int xmm) {
    emit8(0x66);
    emit_rex(1, xmm, 0, gpr);
    emit8(0x0F);
    emit8(0x7E);
    modrm(3, xmm, gpr);
}

// cvtss2sd xmm_dst, xmm_src: F3 0F 5A /r
void x_cvtss2sd(int xmm_dst, int xmm_src) {
    emit8(0xF3);
    emit_rex(0, xmm_dst, 0, xmm_src);
    emit8(0x0F);
    emit8(0x5A);
    modrm(3, xmm_dst, xmm_src);
}

// cvtsd2ss xmm_dst, xmm_src: F2 0F 5A /r
void x_cvtsd2ss(int xmm_dst, int xmm_src) {
    emit8(0xF2);
    emit_rex(0, xmm_dst, 0, xmm_src);
    emit8(0x0F);
    emit8(0x5A);
    modrm(3, xmm_dst, xmm_src);
}

// cvtsi2sd xmm_dst, gpr_src (64-bit signed int -> double): F2 REX.W 0F 2A /r
void x_cvtsi2sd(int xmm_dst, int gpr_src) {
    emit8(0xF2);
    emit_rex(1, xmm_dst, 0, gpr_src);
    emit8(0x0F);
    emit8(0x2A);
    modrm(3, xmm_dst, gpr_src);
}

// cvttsd2si gpr_dst, xmm_src (double -> 64-bit signed int, truncating): F2 REX.W 0F 2C /r
void x_cvttsd2si(int gpr_dst, int xmm_src) {
    emit8(0xF2);
    emit_rex(1, gpr_dst, 0, xmm_src);
    emit8(0x0F);
    emit8(0x2C);
    modrm(3, gpr_dst, xmm_src);
}

// Scalar double arithmetic: F2 0F <op> /r (addsd=0x58, mulsd=0x59, subsd=0x5C, divsd=0x5E)
void x_sse_sd(uint8_t op, int xmm_dst, int xmm_src) {
    emit8(0xF2);
    emit_rex(0, xmm_dst, 0, xmm_src);
    emit8(0x0F);
    emit8(op);
    modrm(3, xmm_dst, xmm_src);
}

// ucomisd xmm_a, xmm_b: 66 0F 2E /r
void x_ucomisd(int xmm_a, int xmm_b) {
    emit8(0x66);
    emit_rex(0, xmm_a, 0, xmm_b);
    emit8(0x0F);
    emit8(0x2E);
    modrm(3, xmm_a, xmm_b);
}

void x_resolve_labels(void) {
    for (int i = 0; i < A.fixups.len; i++) {
        uint32_t pos = A.fixups.items[i].pos;
        int lbl = A.fixups.items[i].label;
        int target = A.label_pos.items[lbl];
        if (target < 0) die("internal error: unbound code label %d", lbl);
        int32_t rel = (int32_t)(target - (int)(pos + 4));
        buf_put_u32(&A.code, pos, (uint32_t)rel);
    }
}
