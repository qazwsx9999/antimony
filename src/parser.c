// parser.c - recursive-descent parser for Antimony.
// Needs at most 2 tokens of lookahead and no symbol table.
#include "sb.h"

typedef struct {
    Token *toks;
    int count;
    int pos;
    bool no_struct_lit; // true inside `if`/`loop` headers unless inside `(...)` or `[...]`
} Parser;

static Node *new_node(NodeKind k, Pos pos) {
    Node *n = xcalloc(1, sizeof(Node));
    n->kind = k;
    n->pos = pos;
    return n;
}

static Token *cur(Parser *p) {
    return &p->toks[p->pos];
}

static Token *peek(Parser *p, int offset) {
    int i = p->pos + offset;
    if (i >= p->count) i = p->count - 1;
    return &p->toks[i];
}

static Token *next(Parser *p) {
    Token *t = &p->toks[p->pos];
    if (p->pos < p->count - 1) p->pos++;
    return t;
}

static bool match(Parser *p, TokKind k) {
    if (cur(p)->kind == k) {
        next(p);
        return true;
    }
    return false;
}

static Token *expect(Parser *p, TokKind k) {
    Token *t = cur(p);
    if (t->kind == T_SEMI && k != T_SEMI) {
        fatal(t->pos, "unexpected ';': statements end at a newline; ';' is only allowed inside a C-style loop header");
    }
    if (t->kind != k) {
        fatal(t->pos, "expected %s, got %s", tok_name(k), tok_name(t->kind));
    }
    return next(p);
}

static void skip_terms(Parser *p) {
    while (cur(p)->kind == T_TERM) next(p);
}

static void expect_term(Parser *p) {
    Token *t = cur(p);
    if (t->kind == T_SEMI) {
        fatal(t->pos, "unexpected ';': statements end at a newline; ';' is only allowed inside a C-style loop header");
    }
    if (t->kind == T_TERM) {
        while (cur(p)->kind == T_TERM) next(p);
        return;
    }
    if (t->kind == T_RBRACE || t->kind == T_EOF) return;
    fatal(t->pos, "expected newline after statement, got %s", tok_name(t->kind));
}

// Forward declarations.
static Node *parse_expr(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_stmt(Parser *p);

static const char *parse_type_spec(Parser *p, Pos *out_pos, DeclKind *dk, Node **len_expr, int *ptr_depth) {
    // Also accept prefix `[N]T` or `[]T` on return types / type annotations.
    if (dk && *dk == DECL_SCALAR && match(p, T_LBRACKET)) {
        if (match(p, T_RBRACKET)) {
            *dk = DECL_SLICE;
        } else {
            *dk = DECL_ARRAY;
            *len_expr = parse_expr(p);
            expect(p, T_RBRACKET);
        }
    }
    int stars = 0;
    while (match(p, T_STAR)) {
        stars++;
    }
    if (ptr_depth) *ptr_depth = stars;
    else if (stars > 0) fatal(cur(p)->pos, "pointer type is not allowed here");
    Token *t = expect(p, T_IDENT);
    if (out_pos) *out_pos = t->pos;
    return t->name;
}

static void parse_var_brackets(Parser *p, DeclKind *dk, Node **len_expr) {
    *dk = DECL_SCALAR;
    *len_expr = NULL;
    if (match(p, T_LBRACKET)) {
        if (match(p, T_RBRACKET)) {
            *dk = DECL_SLICE;
        } else {
            *dk = DECL_ARRAY;
            *len_expr = parse_expr(p);
            expect(p, T_RBRACKET);
        }
    }
}

// ---------------------------------------------------------------------------
// Expressions (6 precedence levels, spec §7.1)
// ---------------------------------------------------------------------------

static Node *parse_primary_base(Parser *p) {
    Token *t = cur(p);
    switch (t->kind) {
    case T_INT: {
        next(p);
        Node *n = new_node(N_INT, t->pos);
        n->cval = (i128)t->ival;
        return n;
    }
    case T_FLOAT: {
        next(p);
        Node *n = new_node(N_FLOAT, t->pos);
        n->fval = t->fval;
        return n;
    }
    case T_STR: {
        next(p);
        Node *n = new_node(N_STR, t->pos);
        n->str = t->str;
        n->str_len = t->str_len;
        return n;
    }
    case T_TRUE:
    case T_FALSE: {
        next(p);
        Node *n = new_node(N_BOOL, t->pos);
        n->cval = (t->kind == T_TRUE) ? 1 : 0;
        return n;
    }
    case T_NULL: {
        next(p);
        return new_node(N_NULL, t->pos);
    }
    case T_LBRACKET: {
        // Array literal: `[10, 20, 30]` or `[]`
        next(p);
        bool saved_nsl = p->no_struct_lit;
        p->no_struct_lit = false;
        Node *n = new_node(N_ARRAY_LIT, t->pos);
        skip_terms(p);
        if (cur(p)->kind != T_RBRACKET) {
            for (;;) {
                skip_terms(p);
                vec_push(n->list, parse_expr(p));
                skip_terms(p);
                if (!match(p, T_COMMA)) break;
                skip_terms(p);
                if (cur(p)->kind == T_RBRACKET) break;
            }
        }
        p->no_struct_lit = saved_nsl;
        expect(p, T_RBRACKET);
        return n;
    }
    case T_IDENT: {
        next(p);
        if (match(p, T_LPAREN)) {
            bool saved_nsl = p->no_struct_lit;
            p->no_struct_lit = false;
            Node *n = new_node(N_CALL, t->pos);
            n->name = t->name;
            skip_terms(p);
            if (cur(p)->kind != T_RPAREN) {
                for (;;) {
                    skip_terms(p);
                    vec_push(n->list, parse_expr(p));
                    skip_terms(p);
                    if (!match(p, T_COMMA)) break;
                    skip_terms(p);
                    if (cur(p)->kind == T_RPAREN) break;
                }
            }
            p->no_struct_lit = saved_nsl;
            expect(p, T_RPAREN);
            return n;
        }
        // Struct literal: `Point { x: 10, y: 20 }` or `Point {}`
        if (!p->no_struct_lit && cur(p)->kind == T_LBRACE) {
            next(p); // '{'
            Node *n = new_node(N_STRUCT_LIT, t->pos);
            n->name = t->name;
            skip_terms(p);
            while (cur(p)->kind != T_RBRACE && cur(p)->kind != T_EOF) {
                Token *fid = expect(p, T_IDENT);
                expect(p, T_COLON);
                skip_terms(p);
                Node *fi = new_node(N_FIELD_INIT, fid->pos);
                fi->name = fid->name;
                fi->rhs = parse_expr(p);
                vec_push(n->list, fi);
                if (match(p, T_COMMA)) {
                    skip_terms(p);
                } else if (cur(p)->kind == T_TERM) {
                    skip_terms(p);
                } else {
                    break;
                }
            }
            expect(p, T_RBRACE);
            return n;
        }
        Node *n = new_node(N_IDENT, t->pos);
        n->name = t->name;
        return n;
    }
    case T_LPAREN: {
        next(p);
        bool saved_nsl = p->no_struct_lit;
        p->no_struct_lit = false;
        skip_terms(p);
        Node *e = parse_expr(p);
        skip_terms(p);
        p->no_struct_lit = saved_nsl;
        expect(p, T_RPAREN);
        return e;
    }
    default:
        fatal(t->pos, "expected expression, got %s", tok_name(t->kind));
    }
}

// Parses postfix field access `p.x`, indexing `a[i]`, and slicing `a[lo..hi]`.
static Node *parse_primary(Parser *p) {
    Node *base = parse_primary_base(p);
    for (;;) {
        if (cur(p)->kind == T_DOT) {
            Token *dot = next(p);
            Token *fid = expect(p, T_IDENT);
            Node *n = new_node(N_FIELD, dot->pos);
            n->lhs = base;
            n->name = fid->name;
            base = n;
            continue;
        }
        if (cur(p)->kind == T_LBRACKET) {
            Token *lb = next(p);
            bool saved_nsl = p->no_struct_lit;
            p->no_struct_lit = false;
            skip_terms(p);
            if (match(p, T_DOTDOT)) {
                Node *s = new_node(N_SLICE, lb->pos);
                s->lhs = base;
                s->lo = NULL;
                if (cur(p)->kind != T_RBRACKET) {
                    s->hi = parse_expr(p);
                }
                p->no_struct_lit = saved_nsl;
                expect(p, T_RBRACKET);
                base = s;
            } else {
                Node *idx = parse_expr(p);
                if (match(p, T_DOTDOT)) {
                    Node *s = new_node(N_SLICE, lb->pos);
                    s->lhs = base;
                    s->lo = idx;
                    if (cur(p)->kind != T_RBRACKET) {
                        s->hi = parse_expr(p);
                    }
                    p->no_struct_lit = saved_nsl;
                    expect(p, T_RBRACKET);
                    base = s;
                } else {
                    p->no_struct_lit = saved_nsl;
                    expect(p, T_RBRACKET);
                    Node *n = new_node(N_INDEX, lb->pos);
                    n->lhs = base;
                    n->rhs = idx;
                    base = n;
                }
            }
            continue;
        }
        break;
    }
    return base;
}

static Node *parse_unary(Parser *p) {
    Token *t = cur(p);
    if (t->kind == T_AMP) {
        next(p);
        Node *n = new_node(N_ADDR, t->pos);
        n->lhs = parse_unary(p);
        return n;
    }
    if (t->kind == T_STAR) {
        next(p);
        Node *n = new_node(N_DEREF, t->pos);
        n->lhs = parse_unary(p);
        return n;
    }
    if (t->kind == T_MINUS || t->kind == T_BANG || t->kind == T_TILDE) {
        next(p);
        Node *n = new_node(N_UNARY, t->pos);
        n->op = t->kind;
        n->lhs = parse_unary(p);
        return n;
    }
    if (t->kind == T_PLUS) {
        fatal(t->pos, "unary '+' is not supported");
    }
    return parse_primary(p);
}

static bool is_mul_op(TokKind k) {
    return k == T_STAR || k == T_SLASH || k == T_PERCENT || k == T_STARW ||
           k == T_SHL || k == T_SHR || k == T_AMP;
}

static Node *parse_mul(Parser *p) {
    Node *lhs = parse_unary(p);
    while (is_mul_op(cur(p)->kind)) {
        Token *op = next(p);
        skip_terms(p);
        Node *n = new_node(N_BINARY, op->pos);
        n->op = op->kind;
        n->lhs = lhs;
        n->rhs = parse_unary(p);
        lhs = n;
    }
    return lhs;
}

static bool is_add_op(TokKind k) {
    return k == T_PLUS || k == T_MINUS || k == T_PLUSW || k == T_MINUSW ||
           k == T_PIPE || k == T_CARET;
}

static Node *parse_add(Parser *p) {
    Node *lhs = parse_mul(p);
    while (is_add_op(cur(p)->kind)) {
        Token *op = next(p);
        skip_terms(p);
        Node *n = new_node(N_BINARY, op->pos);
        n->op = op->kind;
        n->lhs = lhs;
        n->rhs = parse_mul(p);
        lhs = n;
    }
    return lhs;
}

static bool is_cmp_op(TokKind k) {
    return k == T_EQ || k == T_NE || k == T_LT || k == T_LE || k == T_GT || k == T_GE;
}

static Node *parse_cmp(Parser *p) {
    Node *lhs = parse_add(p);
    if (is_cmp_op(cur(p)->kind)) {
        Token *op = next(p);
        skip_terms(p);
        Node *n = new_node(N_BINARY, op->pos);
        n->op = op->kind;
        n->lhs = lhs;
        n->rhs = parse_add(p);
        if (is_cmp_op(cur(p)->kind)) {
            fatal(cur(p)->pos, "comparison operators cannot be chained; use '&&'");
        }
        return n;
    }
    return lhs;
}

static Node *parse_and(Parser *p) {
    Node *lhs = parse_cmp(p);
    while (cur(p)->kind == T_ANDAND) {
        Token *op = next(p);
        skip_terms(p);
        Node *n = new_node(N_BINARY, op->pos);
        n->op = op->kind;
        n->lhs = lhs;
        n->rhs = parse_cmp(p);
        lhs = n;
    }
    return lhs;
}

static Node *parse_expr(Parser *p) {
    Node *lhs = parse_and(p);
    while (cur(p)->kind == T_OROR) {
        Token *op = next(p);
        skip_terms(p);
        Node *n = new_node(N_BINARY, op->pos);
        n->op = op->kind;
        n->lhs = lhs;
        n->rhs = parse_and(p);
        lhs = n;
    }
    return lhs;
}

// ---------------------------------------------------------------------------
// Declarations and statements
// ---------------------------------------------------------------------------

// Parses `var x: T`, `var x[N]: T = ...`, `var x[]: T = ...`, or `var x = expr`.
static Node *parse_var_no_term(Parser *p) {
    Token *kw = expect(p, T_VAR);
    Token *id = expect(p, T_IDENT);
    Node *n = new_node(N_VAR, kw->pos);
    n->name = id->name;
    parse_var_brackets(p, &n->decl_kind, &n->len_expr);
    if (match(p, T_COLON)) {
        n->type_name = parse_type_spec(p, &n->type_pos, &n->decl_kind, &n->len_expr, &n->ptr_depth);
        if (match(p, T_ASSIGN)) {
            skip_terms(p);
            n->rhs = parse_expr(p);
        }
    } else if (match(p, T_ASSIGN)) {
        skip_terms(p);
        n->rhs = parse_expr(p);
    } else {
        fatal(cur(p)->pos, "expected ':' or '=' in variable declaration");
    }
    return n;
}

static Node *parse_const_no_term(Parser *p) {
    Token *kw = expect(p, T_CONST);
    Token *id = expect(p, T_IDENT);
    Node *n = new_node(N_CONST, kw->pos);
    n->name = id->name;
    if (match(p, T_COLON)) {
        n->type_name = parse_type_spec(p, &n->type_pos, NULL, NULL, NULL);
    }
    expect(p, T_ASSIGN);
    skip_terms(p);
    n->rhs = parse_expr(p);
    return n;
}

static Node *parse_if(Parser *p) {
    Token *kw = expect(p, T_IF);
    Node *n = new_node(N_IF, kw->pos);
    p->no_struct_lit = true;
    n->cond = parse_expr(p);
    p->no_struct_lit = false;
    n->body = parse_block(p);
    if (match(p, T_ELSE)) {
        if (cur(p)->kind == T_IF) {
            n->els = parse_if(p);
        } else {
            n->els = parse_block(p);
        }
    }
    return n;
}

static void parse_range_parts(Parser *p, Node *n) {
    const char *rev_ident = intern("rev", 3);
    if (cur(p)->kind == T_IDENT && cur(p)->name == rev_ident && peek(p, 1)->kind == T_LPAREN) {
        next(p); // rev
        next(p); // (
        n->rev = true;
        n->lo = parse_expr(p);
        expect(p, T_DOTDOT);
        n->hi = parse_expr(p);
        if (match(p, T_BY)) {
            n->by = parse_expr(p);
        }
        expect(p, T_RPAREN);
        if (cur(p)->kind == T_BY) {
            fatal(cur(p)->pos, "'by' must be inside rev(...): write rev(a..b by s)");
        }
    } else {
        n->lo = parse_expr(p);
        expect(p, T_DOTDOT);
        n->hi = parse_expr(p);
        if (match(p, T_BY)) {
            n->by = parse_expr(p);
        }
    }
}

// Spec §6.3: the four loop forms are selected by at most 2 tokens after `loop`.
static Node *parse_loop(Parser *p, const char *label) {
    Token *kw = expect(p, T_LOOP);

    // 1. Infinite loop: `loop { ... }`
    if (cur(p)->kind == T_LBRACE) {
        Node *n = new_node(N_LOOP_INF, kw->pos);
        n->label = label;
        n->body = parse_block(p);
        return n;
    }

    // 2. Range or collection loop:
    //    `loop i in a..b { ... }`
    //    `loop x in arr { ... }`
    //    `loop i, x in arr { ... }`
    if (cur(p)->kind == T_IDENT &&
        (peek(p, 1)->kind == T_IN ||
         (peek(p, 1)->kind == T_COMMA && peek(p, 2)->kind == T_IDENT && peek(p, 3)->kind == T_IN))) {
        Token *id1 = next(p);
        const char *id2_name = NULL;
        if (match(p, T_COMMA)) {
            id2_name = expect(p, T_IDENT)->name;
        }
        expect(p, T_IN);

        const char *rev_ident = intern("rev", 3);
        if (!id2_name && cur(p)->kind == T_IDENT && cur(p)->name == rev_ident &&
            peek(p, 1)->kind == T_LPAREN) {
            Node *n = new_node(N_LOOP_RANGE, kw->pos);
            n->label = label;
            n->name = id1->name;
            parse_range_parts(p, n);
            n->body = parse_block(p);
            return n;
        }

        p->no_struct_lit = true;
        Node *first = parse_expr(p);
        if (match(p, T_DOTDOT)) {
            if (id2_name) {
                fatal(kw->pos, "range loop takes 1 loop variable, not 2");
            }
            Node *n = new_node(N_LOOP_RANGE, kw->pos);
            n->label = label;
            n->name = id1->name;
            n->lo = first;
            n->hi = parse_expr(p);
            if (match(p, T_BY)) {
                n->by = parse_expr(p);
            }
            p->no_struct_lit = false;
            n->body = parse_block(p);
            return n;
        }
        p->no_struct_lit = false;

        Node *n = new_node(N_LOOP_ITER, kw->pos);
        n->label = label;
        n->name = id1->name;
        n->name2 = id2_name;
        n->lhs = first; // collection expression
        n->body = parse_block(p);
        return n;
    }

    // 3. C-style loop: `loop var i = 0; i < n; i += 1 { ... }`
    if (cur(p)->kind == T_VAR) {
        Node *n = new_node(N_LOOP_C, kw->pos);
        n->label = label;
        p->no_struct_lit = true;
        n->init = parse_var_no_term(p);
        if (cur(p)->kind != T_SEMI) {
            fatal(cur(p)->pos, "expected ';' after variable declaration in C-style loop header");
        }
        next(p); // ';'
        if (cur(p)->kind == T_SEMI) {
            fatal(cur(p)->pos, "missing condition in C-style loop header (all three parts are required)");
        }
        n->cond = parse_expr(p);
        if (cur(p)->kind != T_SEMI) {
            fatal(cur(p)->pos, "expected ';' after condition in C-style loop header");
        }
        next(p); // ';'
        if (cur(p)->kind == T_LBRACE) {
            fatal(cur(p)->pos, "missing step statement in C-style loop header (all three parts are required)");
        }
        if (cur(p)->kind != T_IDENT || !is_assign_op(peek(p, 1)->kind)) {
            fatal(cur(p)->pos, "C-style loop step must be an assignment statement (e.g. 'i += 1')");
        }
        Token *sid = next(p);
        Token *sop = next(p);
        Node *step = new_node(N_ASSIGN, sid->pos);
        step->name = sid->name;
        step->op = sop->kind;
        step->rhs = parse_expr(p);
        p->no_struct_lit = false;
        n->step = step;
        n->body = parse_block(p);
        return n;
    }

    if (cur(p)->kind == T_SEMI) {
        fatal(cur(p)->pos, "empty clauses are not allowed in 'loop'; use 'loop { ... }' for an infinite loop");
    }

    // 4. Conditional loop: `loop cond { ... }`
    Node *n = new_node(N_LOOP_COND, kw->pos);
    n->label = label;
    p->no_struct_lit = true;
    n->cond = parse_expr(p);
    p->no_struct_lit = false;
    if (cur(p)->kind == T_SEMI) {
        fatal(cur(p)->pos, "C-style loop must start with 'var' (e.g. 'loop var i = 0; i < n; i += 1')");
    }
    n->body = parse_block(p);
    return n;
}

static Node *parse_stmt(Parser *p) {
    Token *t = cur(p);
    switch (t->kind) {
    case T_VAR: {
        Node *n = parse_var_no_term(p);
        expect_term(p);
        return n;
    }
    case T_CONST: {
        Node *n = parse_const_no_term(p);
        expect_term(p);
        return n;
    }
    case T_IF: {
        Node *n = parse_if(p);
        expect_term(p);
        return n;
    }
    case T_LOOP: {
        Node *n = parse_loop(p, NULL);
        expect_term(p);
        return n;
    }
    case T_RETURN: {
        next(p);
        Node *n = new_node(N_RETURN, t->pos);
        if (cur(p)->kind != T_TERM && cur(p)->kind != T_RBRACE && cur(p)->kind != T_EOF) {
            n->lhs = parse_expr(p);
        }
        expect_term(p);
        return n;
    }
    case T_BREAK:
    case T_CONTINUE: {
        next(p);
        Node *n = new_node(t->kind == T_BREAK ? N_BREAK : N_CONTINUE, t->pos);
        if (cur(p)->kind == T_IDENT) {
            n->name = next(p)->name;
        }
        expect_term(p);
        return n;
    }
    case T_LBRACE: {
        Node *n = parse_block(p);
        expect_term(p);
        return n;
    }
    case T_STAR: {
        // Dereference assignment: `*p = expr`, `**pp = expr`, `*p += expr`
        Node *lhs = parse_unary(p);
        if (!is_assign_op(cur(p)->kind)) {
            fatal(cur(p)->pos, "expected assignment operator after pointer dereference");
        }
        Token *op = next(p);
        skip_terms(p);
        Node *n = new_node(N_ASSIGN, t->pos);
        n->op = op->kind;
        n->lhs = lhs;
        n->rhs = parse_expr(p);
        expect_term(p);
        return n;
    }
    case T_IDENT: {
        // Labeled loop: `outer: loop ...`
        if (peek(p, 1)->kind == T_COLON) {
            Token *lbl = next(p);
            next(p); // ':'
            skip_terms(p);
            if (cur(p)->kind != T_LOOP) {
                fatal(cur(p)->pos, "label '%s' must be followed by 'loop'", lbl->name);
            }
            Node *n = parse_loop(p, lbl->name);
            expect_term(p);
            return n;
        }
        // Parse primary lvalue or call expression (`x`, `arr[i]`, `p.x`, `foo(a, b)`).
        Node *lhs = parse_primary(p);
        if (is_assign_op(cur(p)->kind)) {
            if (lhs->kind != N_IDENT && lhs->kind != N_INDEX && lhs->kind != N_FIELD) {
                fatal(lhs->pos, "left side of assignment must be a variable, array element, or struct field");
            }
            Token *op = next(p);
            skip_terms(p);
            Node *n = new_node(N_ASSIGN, lhs->pos);
            n->op = op->kind;
            if (lhs->kind == N_IDENT) {
                n->name = lhs->name;
            } else {
                n->lhs = lhs; // N_INDEX or N_FIELD lvalue
            }
            n->rhs = parse_expr(p);
            expect_term(p);
            return n;
        }
        if (lhs->kind == N_CALL) {
            if (cur(p)->kind != T_TERM && cur(p)->kind != T_RBRACE && cur(p)->kind != T_EOF) {
                fatal(cur(p)->pos, "expression result is not used; only function calls and assignments may be statements");
            }
            Node *n = new_node(N_EXPR_STMT, t->pos);
            n->lhs = lhs;
            expect_term(p);
            return n;
        }
        fatal(t->pos, "expected assignment or function call");
    }
    default:
        if (t->kind == T_SEMI) {
            fatal(t->pos, "unexpected ';': statements end at a newline");
        }
        fatal(t->pos, "expected statement, got %s", tok_name(t->kind));
    }
}

static Node *parse_block(Parser *p) {
    Token *lb = expect(p, T_LBRACE);
    Node *b = new_node(N_BLOCK, lb->pos);
    skip_terms(p);
    while (cur(p)->kind != T_RBRACE && cur(p)->kind != T_EOF) {
        vec_push(b->list, parse_stmt(p));
        skip_terms(p);
    }
    Token *rb = expect(p, T_RBRACE);
    b->end_pos = rb->pos;
    return b;
}

static Node *parse_fn_header(Parser *p) {
    Token *kw = expect(p, T_FN);
    Token *id = expect(p, T_IDENT);
    Node *fn = new_node(N_FN, kw->pos);
    fn->name = id->name;

    expect(p, T_LPAREN);
    skip_terms(p);
    if (cur(p)->kind != T_RPAREN) {
        for (;;) {
            skip_terms(p);
            Token *pid = expect(p, T_IDENT);
            Node *param = new_node(N_PARAM, pid->pos);
            param->name = pid->name;
            parse_var_brackets(p, &param->decl_kind, &param->len_expr);
            expect(p, T_COLON);
            param->type_name = parse_type_spec(p, &param->type_pos, &param->decl_kind, &param->len_expr, &param->ptr_depth);
            vec_push(fn->list, param);
            skip_terms(p);
            if (!match(p, T_COMMA)) break;
            skip_terms(p);
            if (cur(p)->kind == T_RPAREN) break;
        }
    }
    expect(p, T_RPAREN);

    if (match(p, T_COLON)) {
        fn->type_name = parse_type_spec(p, &fn->type_pos, &fn->decl_kind, &fn->len_expr, &fn->ptr_depth);
    }
    return fn;
}

static Node *parse_fn(Parser *p) {
    Node *fn = parse_fn_header(p);
    fn->body = parse_block(p);
    expect_term(p);
    return fn;
}

static Node *parse_extern_fn(Parser *p, const char *dll_name) {
    Node *fn = parse_fn_header(p);
    fn->is_extern = true;
    fn->dll_name  = dll_name;
    if (cur(p)->kind == T_LBRACE) {
        fatal(cur(p)->pos, "extern function '%s' cannot have a body", fn->name);
    }
    expect_term(p);
    return fn;
}

static Node *parse_import(Parser *p) {
    Token *kw = expect(p, T_IMPORT);
    Token *s  = expect(p, T_STR);
    if (s->str_len == 0) {
        fatal(s->pos, "import path cannot be empty");
    }
    expect_term(p);
    Node *n = new_node(N_IMPORT, kw->pos);
    n->str     = intern(s->str, s->str_len);
    n->str_len = s->str_len;
    return n;
}

static Node *parse_struct(Parser *p) {
    Token *kw = expect(p, T_STRUCT);
    Token *id = expect(p, T_IDENT);
    Node *st = new_node(N_STRUCT, kw->pos);
    st->name = id->name;
    expect(p, T_LBRACE);
    skip_terms(p);
    while (cur(p)->kind != T_RBRACE && cur(p)->kind != T_EOF) {
        Token *fid = expect(p, T_IDENT);
        Node *fd = new_node(N_FIELD_DECL, fid->pos);
        fd->name = fid->name;
        parse_var_brackets(p, &fd->decl_kind, &fd->len_expr);
        expect(p, T_COLON);
        fd->type_name = parse_type_spec(p, &fd->type_pos, &fd->decl_kind, &fd->len_expr, &fd->ptr_depth);
        vec_push(st->list, fd);
        if (match(p, T_COMMA)) {
            skip_terms(p);
        } else {
            expect_term(p);
        }
    }
    expect(p, T_RBRACE);
    expect_term(p);
    return st;
}

NodeVec parse(Token *toks, int count) {
    Parser p = {toks, count, 0, false};
    NodeVec decls = {0};
    skip_terms(&p);
    while (cur(&p)->kind != T_EOF) {
        Token *t = cur(&p);
        if (t->kind == T_IMPORT) {
            vec_push(decls, parse_import(&p));
        } else if (t->kind == T_EXTERN) {
            next(&p); // consume 'extern'
            Token *dll_tok = expect(&p, T_STR);
            if (dll_tok->str_len == 0) {
                fatal(dll_tok->pos, "extern DLL name cannot be empty");
            }
            const char *dll_name = intern(dll_tok->str, dll_tok->str_len);
            if (match(&p, T_LBRACE)) {
                skip_terms(&p);
                while (cur(&p)->kind != T_RBRACE && cur(&p)->kind != T_EOF) {
                    vec_push(decls, parse_extern_fn(&p, dll_name));
                    skip_terms(&p);
                }
                expect(&p, T_RBRACE);
                expect_term(&p);
            } else {
                vec_push(decls, parse_extern_fn(&p, dll_name));
            }
        } else if (t->kind == T_FN) {
            vec_push(decls, parse_fn(&p));
        } else if (t->kind == T_STRUCT) {
            vec_push(decls, parse_struct(&p));
        } else if (t->kind == T_VAR) {
            Node *n = parse_var_no_term(&p);
            expect_term(&p);
            vec_push(decls, n);
        } else if (t->kind == T_CONST) {
            Node *n = parse_const_no_term(&p);
            expect_term(&p);
            vec_push(decls, n);
        } else {
            fatal(t->pos, "expected 'import', 'extern', 'fn', 'struct', 'var', or 'const' at top level, got %s", tok_name(t->kind));
        }
        skip_terms(&p);
    }
    return decls;
}
