// sb.h - shared declarations for the Antimony compiler (sbc).
#ifndef SB_H
#define SB_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef __int128 i128;

// ---------------------------------------------------------------------------
// Utilities (util.c)
// ---------------------------------------------------------------------------

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);

#define VEC(T) struct { T *items; int len, cap; }
#define vec_push(v, x)                                                        \
    do {                                                                      \
        if ((v).len == (v).cap) {                                             \
            (v).cap = (v).cap ? (v).cap * 2 : 8;                              \
            (v).items = xrealloc((v).items, (size_t)(v).cap * sizeof(*(v).items)); \
        }                                                                     \
        (v).items[(v).len++] = (x);                                           \
    } while (0)

typedef struct {
    uint8_t *data;
    size_t len, cap;
} Buf;

void buf_reserve(Buf *b, size_t extra);
void buf_u8(Buf *b, uint8_t v);
void buf_u16(Buf *b, uint16_t v);
void buf_u32(Buf *b, uint32_t v);
void buf_u64(Buf *b, uint64_t v);
void buf_bytes(Buf *b, const void *p, size_t n);
void buf_zero(Buf *b, size_t n);
void buf_align(Buf *b, size_t a);
void buf_put_u32(Buf *b, size_t off, uint32_t v);

typedef struct {
    int line, col, file_id;
} Pos;

extern const char *g_filename;
extern const char *g_source;
extern int g_file_id;

int register_source_file(const char *filename, const char *source);
const char *file_name_of(Pos pos);
const char *file_source_of(Pos pos);

_Noreturn void fatal(Pos pos, const char *fmt, ...);
_Noreturn void die(const char *fmt, ...);

// Returns a unique, NUL-terminated copy; equal strings give equal pointers.
const char *intern(const char *s, int len);

// Formats an i128 as decimal into buf (at least 48 bytes); returns buf.
char *i128_str(i128 v, char *buf);

// ---------------------------------------------------------------------------
// Lexer (lexer.c)
// ---------------------------------------------------------------------------

typedef enum {
    T_EOF, T_TERM, T_SEMI, T_IDENT, T_INT, T_FLOAT, T_STR,
    // keywords
    T_VAR, T_CONST, T_FN, T_STRUCT, T_ENUM, T_MATCH, T_DEFER, T_IMPORT, T_EXTERN,
    T_RETURN, T_IF, T_ELSE, T_LOOP, T_IN, T_BY,
    T_BREAK, T_CONTINUE, T_TRUE, T_FALSE, T_NULL,
    // punctuation
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_COLON, T_DOT, T_DOTDOT, T_QUESTION,
    // operators
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_PLUSW, T_MINUSW, T_STARW,
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_BANG, T_SHL, T_SHR,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_ANDAND, T_OROR,
    // assignment
    T_ASSIGN, T_PLUS_ASSIGN, T_MINUS_ASSIGN, T_STAR_ASSIGN, T_SLASH_ASSIGN,
    T_PERCENT_ASSIGN, T_AMP_ASSIGN, T_PIPE_ASSIGN, T_CARET_ASSIGN,
    T_SHL_ASSIGN, T_SHR_ASSIGN, T_PLUSW_ASSIGN, T_MINUSW_ASSIGN, T_STARW_ASSIGN,
    T_COUNT
} TokKind;

typedef struct {
    TokKind kind;
    Pos pos;
    const char *name;   // T_IDENT: interned name
    uint64_t ival;      // T_INT
    double fval;        // T_FLOAT
    const char *str;    // T_STR: decoded bytes
    int str_len;
} Token;

Token *lex(const char *src, int *count);
const char *tok_name(TokKind k);
bool is_assign_op(TokKind k);
TokKind assign_to_binop(TokKind k);   // T_PLUS_ASSIGN -> T_PLUS, ...

// ---------------------------------------------------------------------------
// Types and symbols
// ---------------------------------------------------------------------------

typedef struct Node Node;

typedef enum {
    TY_VOID, TY_BOOL, TY_INT, TY_FLOAT, TY_STR, TY_UNTYPED_INT, TY_UNTYPED_FLOAT,
    TY_ARRAY, TY_SLICE, TY_PTR, TY_NULL, TY_STRUCT, TY_ENUM, TY_FN
} TypeKind;

typedef struct {
    const char *name;
    struct Type *type;
    int offset;
    bool has_payload;
    Pos pos;
} StructField;

typedef struct Type {
    TypeKind kind;
    const char *name;
    int size;          // bytes
    int align;         // alignment in bytes (1, 2, 4, 8)
    bool is_signed;
    bool is_tagged;    // TY_ENUM: true if any variant carries a payload
    int payload_offset;// TY_ENUM: byte offset of union payload when is_tagged
    struct Type *elem; // TY_ARRAY / TY_SLICE / TY_PTR element type, or TY_FN return type
    int array_len;     // TY_ARRAY element count
    Node *decl;        // TY_STRUCT / TY_ENUM: AST declaration node
    VEC(StructField) fields; // TY_STRUCT fields or TY_ENUM variants
    VEC(struct Type *) params; // TY_FN parameter types
    int state;         // TY_STRUCT / TY_ENUM layout state: 0=unresolved, 1=in-progress, 2=done
} Type;

static inline bool is_aggregate_type(Type *t) {
    return t && (t->kind == TY_STRUCT || (t->kind == TY_ENUM && t->is_tagged));
}

extern Type ty_void, ty_bool, ty_str, ty_untyped_int, ty_untyped_float, ty_null;
extern Type ty_i8, ty_i16, ty_i32, ty_i64, ty_u8, ty_u16, ty_u32, ty_u64;
extern Type ty_isize, ty_usize, ty_f32, ty_f64;

typedef struct Node Node;

typedef enum { SYM_VAR, SYM_PARAM, SYM_CONST, SYM_FN, SYM_TYPE, SYM_BUILTIN } SymKind;
typedef enum { B_PRINT = 1, B_EXIT, B_REV, B_LEN, B_SIZEOF, B_ALIGNOF, B_PTR_CAST } BuiltinId;

typedef struct Symbol {
    SymKind kind;
    const char *name;
    Type *type;          // variable/param/const type; fn return type
    Node *decl;
    Pos pos;
    bool is_global;
    bool readonly;       // range-loop variable
    int slot;            // local: stack slot (1-based); param: index
    int var_index;       // definite-assignment index, -1 = always assigned
    uint32_t data_off;   // global variable: offset in .data
    i128 cval;           // constant value
    int state;           // global const resolution: 0 todo, 1 in progress, 2 done
    int builtin;         // BuiltinId
    int label;           // fn: code label
    double fval;         // SYM_CONST float value
    bool addr_taken;     // local var: address taken (&x) -> cannot live in register
    int use_score;       // loop-weighted reference count for register allocation
    int hw_reg;          // allocated callee-saved register (RBX, R12..R15), or -1 if stack
    bool is_extern;      // fn: imported from Windows DLL
    int iat_slot;        // extern fn: slot index in PE32+ Import Address Table
} Symbol;

// ---------------------------------------------------------------------------
// AST (parser.c)
// ---------------------------------------------------------------------------

typedef enum {
    // expressions
    N_INT, N_FLOAT, N_STR, N_BOOL, N_NULL, N_IDENT, N_CALL, N_CONV, N_UNARY, N_BINARY,
    N_ADDR, N_DEREF, N_ARRAY_LIT, N_STRUCT_LIT, N_ENUM_LIT, N_TRY, N_FIELD_INIT, N_INDEX, N_SLICE, N_FIELD,
    // statements
    N_VAR, N_CONST, N_ASSIGN, N_EXPR_STMT, N_IF, N_MATCH, N_MATCH_ARM, N_DEFER, N_BLOCK, N_RETURN,
    N_BREAK, N_CONTINUE, N_LOOP_INF, N_LOOP_COND, N_LOOP_RANGE, N_LOOP_ITER, N_LOOP_C,
    // declarations
    N_FN, N_PARAM, N_STRUCT, N_FIELD_DECL, N_ENUM, N_ENUM_VAR, N_IMPORT
} NodeKind;

typedef enum { DECL_SCALAR = 0, DECL_ARRAY, DECL_SLICE } DeclKind;

typedef VEC(Node *) NodeVec;
typedef VEC(const char *) StrVec;
typedef VEC(Type *) TypeVec;

struct Node {
    NodeKind kind;
    Pos pos;
    Pos end_pos;            // N_BLOCK: position of closing '}'

    Type *type;             // expression type / declared type / fn return type
    bool is_const;
    i128 cval;

    int op;                 // TokKind of operator
    const char *name;       // identifier, callee, declared name, label target
    const char *name2;      // N_LOOP_ITER: second identifier in `loop i, x in arr`
    const char *type_name;  // type annotation, NULL if absent
    Pos type_pos;
    DeclKind decl_kind;     // DECL_SCALAR / DECL_ARRAY (`x[N]`) / DECL_SLICE (`x[]`)
    int ptr_depth;          // number of leading `*` in type annotation (`*i32` = 1, `**i32` = 2)
    Node *len_expr;         // DECL_ARRAY: N expression

    Node *lhs, *rhs;        // operands; var/const/assign value in rhs; return value in lhs
    NodeVec list;           // call args, block statements, fn params, array literal elements

    Node *init, *cond, *step, *body, *els;
    Node *lo, *hi, *by;     // range loop / slice bounds
    bool rev;
    bool implicit_slice;    // coercion from fixed array `[N]T` to slice `[]T`
    bool is_extern;         // N_FN: declared with `extern "dll"`
    const char *dll_name;   // N_FN: interned DLL name for extern functions
    const char *label;      // loop label

    const char *str;        // string literal
    int str_len;

    Symbol *sym;            // resolved symbol
    Symbol *sym2;           // N_LOOP_ITER: value symbol when both index and value are present
    Node *target;           // break/continue: loop node
    int builtin;            // call to builtin
    double fval;            // constant float value

    // filled by checker for codegen
    int field_offset;                     // N_FIELD / N_FIELD_INIT: byte offset inside struct
    int slot_start, slot_end, slot_step;  // range/iter loop hidden slots
    int num_slots;                        // fn: total stack slots
    int label_break, label_continue;      // codegen
    int defer_depth;                      // codegen: active defer count at loop entry
    StrVec tparams;                       // generic declaration type parameters (`[T, U]`)
};

NodeVec parse(Token *toks, int count);

// ---------------------------------------------------------------------------
// Checker (check.c)
// ---------------------------------------------------------------------------

typedef struct {
    const char *dll_name;
    VEC(const char *) funcs;
    int first_iat_slot;
} DllImport;

typedef VEC(DllImport) DllImportVec;

typedef struct {
    NodeVec fns;            // checked function declarations
    NodeVec type_decls;     // concrete & instantiated N_STRUCT / N_ENUM declarations in layout order
    VEC(Symbol *) globals;  // global variables, in declaration order
    Symbol *main_fn;
    DllImportVec dll_imports;
    int total_iat_slots;
} Program;

Program check(NodeVec decls);
bool is_int(Type *t);
bool is_float(Type *t);
bool fits(i128 v, Type *t);

// ---------------------------------------------------------------------------
// x86-64 encoder (x64.c)
// ---------------------------------------------------------------------------

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A,
       CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
enum { ALU_ADD = 0, ALU_OR = 1, ALU_AND = 4, ALU_SUB = 5, ALU_XOR = 6, ALU_CMP = 7 };
enum { SEC_RDATA, SEC_DATA, SEC_IAT };
enum { IMP_GETSTDHANDLE, IMP_WRITEFILE, IMP_EXITPROCESS, IMP_COUNT };

typedef struct {
    uint32_t pos;     // offset of disp32 in .text
    int section;      // SEC_*
    uint32_t off;     // offset within the target (strings buffer / .data / import index)
    int addend;       // bytes between the end of disp32 and the end of the instruction
} RipFixup;

typedef struct {
    uint32_t pos;
    int label;
} LabelFixup;

typedef struct {
    Buf code;
    VEC(int) label_pos;                       // -1 = unbound
    VEC(LabelFixup) fixups;
    VEC(RipFixup) rip;
} Asm;

extern Asm A;

int  x_label(void);
void x_bind(int label);
uint32_t x_here(void);

void x_mov_rr(int w, int dst, int src);
void x_mov_ri(int dst, uint64_t imm);
void x_load(int dst, int base, int32_t disp);          // mov dst, [base+disp] (64-bit)
void x_load_sized(int dst, int base, int32_t disp, int size, bool is_signed);
void x_store(int base, int32_t disp, int src);         // mov [base+disp], src (64-bit)
void x_store_sized(int base, int32_t disp, int src, int size);
void x_store8(int base, int32_t disp, int src);        // mov byte [base+disp], src8
void x_store8_imm(int base, int32_t disp, uint8_t imm);
void x_load_rip(int dst, int sec, uint32_t off);
void x_store_rip(int sec, uint32_t off, int src);
void x_lea_rip(int dst, int sec, uint32_t off);
void x_lea_label(int dst, int label);
void x_lea(int dst, int base, int32_t disp);
void x_rep_stosb(void);
void x_alu_rr(int alu, int w, int dst, int src);
void x_alu_ri(int alu, int w, int dst, int32_t imm);
void x_alu_rm(int alu, int w, int dst, int base, int32_t disp);   // dst = dst op [base+disp]
void x_alu_mr_rip(int alu, int sec, uint32_t off, int src);       // [rip+x] op= src (64-bit)
void x_test_rr(int w, int a, int b);
void x_imul_rr(int w, int dst, int src);
void x_imul_rri(int w, int dst, int src, int32_t imm);
void x_f7(int ext, int w, int r);       // not=2 neg=3 mul=4 imul=5 div=6 idiv=7
void x_cqo(void);
void x_cdq(void);
void x_shift_cl(int ext, int w, int r); // shl=4 shr=5 sar=7
void x_shift_imm(int ext, int w, int r, uint8_t imm);
void x_movsx8(int dst, int src);
void x_movsx16(int dst, int src);
void x_movsxd(int dst, int src);
void x_movzx8(int dst, int src);
void x_movzx16(int dst, int src);
void x_setcc(int cc, int r);
void x_jmp(int label);
void x_jcc(int cc, int label);
void x_call(int label);
void x_call_reg(int reg);
void x_call_import(int imp);
void x_push(int r);
void x_pop(int r);
void x_ret(void);
void x_leave(void);
uint32_t x_sub_rsp_patchable(void);     // returns offset of imm32
void x_rep_movsb(void);
void x_rep_stosb(void);
// SSE2 scalar double/float helpers (XMM0, XMM1 <-> RAX, RCX)
void x_movq_xmm_gpr(int xmm, int gpr);
void x_movq_gpr_xmm(int gpr, int xmm);
void x_cvtss2sd(int xmm_dst, int xmm_src);
void x_cvtsd2ss(int xmm_dst, int xmm_src);
void x_cvtsi2sd(int xmm_dst, int gpr_src);
void x_cvttsd2si(int gpr_dst, int xmm_src);
void x_sse_sd(uint8_t op, int xmm_dst, int xmm_src); // addsd=0x58 mulsd=0x59 subsd=0x5C divsd=0x5E
void x_ucomisd(int xmm_a, int xmm_b);
void x_resolve_labels(void);

// ---------------------------------------------------------------------------
// Code generation (codegen.c) and PE writer (pe.c)
// ---------------------------------------------------------------------------

typedef struct {
    Buf text;
    Buf strings;          // read-only data (string literals, messages)
    Buf data;             // initialized .data contents
    uint32_t data_size;   // total .data size including zero-initialized part
    RipFixup *fixups;
    int nfixups;
    uint32_t entry;       // entry point offset in .text
    DllImportVec dll_imports;
    int total_iat_slots;
} Image;

Image codegen(Program *prog, bool release);
void write_pe(const char *path, Image *img);
bool emit_llvm_ir(Program *prog, NodeVec all_decls, const char *ll_path, bool release);
bool compile_opt_backend(Program *prog, NodeVec all_decls, const char *exe_path, bool release);

#endif
