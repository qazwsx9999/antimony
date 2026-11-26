// fmt.c - canonical, lossless, comment-preserving source formatter for Antimony (`sbc fmt`).
//
// Guarantees:
//   1. Syntax-validates input with the real lexer + parser before formatting.
//   2. Preserves all comments, raw strings, char literals, and numeric literal spellings verbatim.
//   3. Enforces canonical 4-space indentation, consistent operator/delimiter spacing,
//      single-blank-line normalization, no trailing whitespace, LF line endings,
//      and a single trailing newline.
//   4. Strictly idempotent: fmt(fmt(src)) == fmt(src).
#include "sb.h"

typedef enum {
    FT_IDENT,
    FT_KEYWORD,
    FT_LITERAL,     // int, float, string, raw string, char
    FT_COMMENT,     // // ... (to end of line, trailing whitespace stripped)
    FT_LBRACE,      // {
    FT_RBRACE,      // }
    FT_LPAREN,      // (
    FT_RPAREN,      // )
    FT_LBRACKET,    // [
    FT_RBRACKET,    // ]
    FT_COMMA,       // ,
    FT_SEMI,        // ;
    FT_COLON,       // :
    FT_DOT,         // .
    FT_DOTDOT,      // ..
    FT_QUESTION,    // ?
    FT_UNARY,       // ! ~ (always unary)
    FT_AMBIG_UNARY, // - * & (unary or binary depending on previous token)
    FT_BINOP,       // + / % +% -% *% == != < <= > >= && || << >> | ^
    FT_ASSIGN,      // = += -= *= /= %= &= |= ^= <<= >>= +%= -%= *%=
    FT_NEWLINE,
    FT_EOF
} FmtTokKind;

typedef struct {
    FmtTokKind kind;
    const char *text;
    int len;
    bool resolved_unary; // for FT_AMBIG_UNARY: true when acting as unary/prefix
} FmtTok;

typedef VEC(FmtTok) FmtTokVec;

static bool fmt_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool fmt_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool fmt_is_kw(const char *s, int len) {
    static const char *kws[] = {
        "var", "const", "fn", "struct", "enum", "type", "match", "defer",
        "import", "extern", "pub", "priv", "return", "if", "else", "loop",
        "in", "by", "break", "continue", "true", "false", "null"
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        if ((int)strlen(kws[i]) == len && memcmp(s, kws[i], (size_t)len) == 0) {
            return true;
        }
    }
    return false;
}

static bool tok_eq(const FmtTok *t, const char *lit) {
    int n = (int)strlen(lit);
    return t->len == n && memcmp(t->text, lit, (size_t)n) == 0;
}

static FmtTokVec fmt_scan(const char *src) {
    FmtTokVec out = {0};
    const char *p = src;
    if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) {
        p += 3; // strip UTF-8 BOM
    }

    while (*p) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r') {
            p++;
            continue;
        }
        if (c == '\n') {
            FmtTok t = { FT_NEWLINE, p, 1, false };
            vec_push(out, t);
            p++;
            continue;
        }
        // Line comment
        if (c == '/' && p[1] == '/') {
            const char *start = p;
            while (*p && *p != '\n' && *p != '\r') p++;
            int len = (int)(p - start);
            while (len > 2 && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
            FmtTok t = { FT_COMMENT, start, len, false };
            vec_push(out, t);
            continue;
        }
        // Identifier / keyword
        if (fmt_is_alpha(c)) {
            const char *start = p;
            while (fmt_is_alpha(*p) || fmt_is_digit(*p)) p++;
            int len = (int)(p - start);
            FmtTok t = { fmt_is_kw(start, len) ? FT_KEYWORD : FT_IDENT, start, len, false };
            vec_push(out, t);
            continue;
        }
        // Numeric literal (int / float, preserving hex/bin/oct/underscores)
        if (fmt_is_digit(c)) {
            const char *start = p;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'b' || p[1] == 'B' || p[1] == 'o' || p[1] == 'O')) {
                p += 2;
                while (fmt_is_digit(*p) || fmt_is_alpha(*p)) p++;
            } else {
                while (fmt_is_digit(*p) || *p == '_') p++;
                if (*p == '.' && p[1] != '.' && fmt_is_digit(p[1])) {
                    p++;
                    while (fmt_is_digit(*p) || *p == '_') p++;
                }
                if (*p == 'e' || *p == 'E') {
                    p++;
                    if (*p == '+' || *p == '-') p++;
                    while (fmt_is_digit(*p) || *p == '_') p++;
                }
            }
            FmtTok t = { FT_LITERAL, start, (int)(p - start), false };
            vec_push(out, t);
            continue;
        }
        // Escaped string literal
        if (c == '"') {
            const char *start = p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
            }
            if (*p == '"') p++;
            FmtTok t = { FT_LITERAL, start, (int)(p - start), false };
            vec_push(out, t);
            continue;
        }
        // Raw string literal (`...`)
        if (c == '`') {
            const char *start = p++;
            while (*p && *p != '`') p++;
            if (*p == '`') p++;
            FmtTok t = { FT_LITERAL, start, (int)(p - start), false };
            vec_push(out, t);
            continue;
        }
        // Character literal ('...')
        if (c == '\'') {
            const char *start = p++;
            while (*p && *p != '\'') {
                if (*p == '\\' && p[1]) p += 2;
                else p++;
            }
            if (*p == '\'') p++;
            FmtTok t = { FT_LITERAL, start, (int)(p - start), false };
            vec_push(out, t);
            continue;
        }

        // 3-char operators
        if ((p[0] == '<' && p[1] == '<' && p[2] == '=') ||
            (p[0] == '>' && p[1] == '>' && p[2] == '=') ||
            (p[0] == '+' && p[1] == '%' && p[2] == '=') ||
            (p[0] == '-' && p[1] == '%' && p[2] == '=') ||
            (p[0] == '*' && p[1] == '%' && p[2] == '=')) {
            FmtTok t = { FT_ASSIGN, p, 3, false };
            vec_push(out, t);
            p += 3;
            continue;
        }

        // 2-char operators
        if (p[0] == '.' && p[1] == '.') {
            FmtTok t = { FT_DOTDOT, p, 2, false };
            vec_push(out, t);
            p += 2;
            continue;
        }
        if ((p[0] == '+' && p[1] == '=') || (p[0] == '-' && p[1] == '=') ||
            (p[0] == '*' && p[1] == '=') || (p[0] == '/' && p[1] == '=') ||
            (p[0] == '%' && p[1] == '=') || (p[0] == '&' && p[1] == '=') ||
            (p[0] == '|' && p[1] == '=') || (p[0] == '^' && p[1] == '=')) {
            FmtTok t = { FT_ASSIGN, p, 2, false };
            vec_push(out, t);
            p += 2;
            continue;
        }
        if ((p[0] == '=' && p[1] == '=') || (p[0] == '!' && p[1] == '=') ||
            (p[0] == '<' && p[1] == '=') || (p[0] == '>' && p[1] == '=') ||
            (p[0] == '&' && p[1] == '&') || (p[0] == '|' && p[1] == '|') ||
            (p[0] == '<' && p[1] == '<') || (p[0] == '>' && p[1] == '>') ||
            (p[0] == '+' && p[1] == '%') || (p[0] == '-' && p[1] == '%') ||
            (p[0] == '*' && p[1] == '%')) {
            FmtTok t = { FT_BINOP, p, 2, false };
            vec_push(out, t);
            p += 2;
            continue;
        }

        // 1-char tokens
        FmtTokKind k = FT_BINOP;
        switch (c) {
        case '{': k = FT_LBRACE; break;
        case '}': k = FT_RBRACE; break;
        case '(': k = FT_LPAREN; break;
        case ')': k = FT_RPAREN; break;
        case '[': k = FT_LBRACKET; break;
        case ']': k = FT_RBRACKET; break;
        case ',': k = FT_COMMA; break;
        case ';': k = FT_SEMI; break;
        case ':': k = FT_COLON; break;
        case '.': k = FT_DOT; break;
        case '?': k = FT_QUESTION; break;
        case '!': case '~': k = FT_UNARY; break;
        case '-': case '*': case '&': k = FT_AMBIG_UNARY; break;
        case '=': k = FT_ASSIGN; break;
        default:  k = FT_BINOP; break;
        }
        FmtTok t = { k, p, 1, false };
        vec_push(out, t);
        p++;
    }

    FmtTok eof = { FT_EOF, p, 0, false };
    vec_push(out, eof);
    return out;
}

static bool is_value_ending(const FmtTok *t) {
    if (!t) return false;
    switch (t->kind) {
    case FT_IDENT:
    case FT_LITERAL:
    case FT_RPAREN:
    case FT_RBRACKET:
    case FT_QUESTION:
        return true;
    case FT_KEYWORD:
        return tok_eq(t, "true") || tok_eq(t, "false") || tok_eq(t, "null");
    default:
        return false;
    }
}

// Determines if `fn` at `toks[idx]` is a method declaration `fn (recv: T) name(...)`
// rather than a function type `fn(i32): bool` or anonymous function `fn(x: i32) { ... }`.
static bool is_method_fn_receiver(const FmtTokVec *toks, int fn_idx) {
    int i = fn_idx + 1;
    if (i >= toks->len || toks->items[i].kind != FT_LPAREN) return false;
    int depth = 0;
    while (i < toks->len) {
        FmtTokKind k = toks->items[i].kind;
        if (k == FT_LPAREN) depth++;
        else if (k == FT_RPAREN) {
            depth--;
            if (depth == 0) {
                i++;
                break;
            }
        }
        i++;
    }
    while (i < toks->len && toks->items[i].kind == FT_NEWLINE) i++;
    return i < toks->len && toks->items[i].kind == FT_IDENT;
}

// Resolves unary vs binary classification for `-`, `*`, `&` across the token stream.
static void classify_unary(FmtTokVec *toks) {
    const FmtTok *prev_sig = NULL;
    for (int i = 0; i < toks->len; i++) {
        FmtTok *t = &toks->items[i];
        if (t->kind == FT_COMMENT) continue;
        if (t->kind == FT_NEWLINE) {
            // A newline ends a statement if the previous token was value-ending,
            // so a leading `*p = ...` on the next line is unary!
            if (prev_sig && (is_value_ending(prev_sig) || prev_sig->kind == FT_RBRACE ||
                             (prev_sig->kind == FT_KEYWORD &&
                              (tok_eq(prev_sig, "return") || tok_eq(prev_sig, "break") ||
                               tok_eq(prev_sig, "continue"))))) {
                prev_sig = NULL;
            }
            continue;
        }
        if (t->kind == FT_AMBIG_UNARY) {
            // Special case: `[]*u8` or `[N]*u8` — `*` right after `]` in a type annotation.
            if (t->text[0] == '*' && prev_sig && prev_sig->kind == FT_RBRACKET) {
                // Look back to check if the matching `[` was preceded by `:`, `=`, `,`, `(`, `[`, or `type`.
                // Simpler: if next token is `const` or `*` or if prev token was `]` preceded by `[` (`[]*T`), it's unary!
                int j = i - 1;
                int bdepth = 0;
                while (j >= 0) {
                    if (toks->items[j].kind == FT_RBRACKET) bdepth++;
                    else if (toks->items[j].kind == FT_LBRACKET) {
                        bdepth--;
                        if (bdepth == 0) { j--; break; }
                    }
                    j--;
                }
                while (j >= 0 && toks->items[j].kind == FT_NEWLINE) j--;
                if (j >= 0) {
                    FmtTokKind bk = toks->items[j].kind;
                    if (bk == FT_COLON || bk == FT_ASSIGN || bk == FT_COMMA ||
                        bk == FT_LPAREN || bk == FT_LBRACKET) {
                        t->resolved_unary = true;
                        prev_sig = t;
                        continue;
                    }
                }
            }
            t->resolved_unary = !is_value_ending(prev_sig);
        }
        prev_sig = t;
    }
}

static bool needs_space_between(const FmtTokVec *toks, int prev_idx, int cur_idx) {
    const FmtTok *a = &toks->items[prev_idx];
    const FmtTok *b = &toks->items[cur_idx];

    // Comments always get 1 space before them when trailing on a code line.
    if (b->kind == FT_COMMENT) return true;

    // Never put spaces around `.` unless `.` starts an inferred enum variant (`.Some`, `.Ok`).
    if (a->kind == FT_DOT) return false;
    if (b->kind == FT_DOT) {
        if (is_value_ending(a) || a->kind == FT_RBRACE) return false;
        if (a->kind == FT_LPAREN || a->kind == FT_LBRACKET ||
            a->kind == FT_UNARY || (a->kind == FT_AMBIG_UNARY && a->resolved_unary)) {
            return false;
        }
        return true;
    }

    // Never put spaces around `..` (ranges & slicing).
    if (a->kind == FT_DOTDOT || b->kind == FT_DOTDOT) return false;

    // Postfix `?`, `,`, `;`, `:` never have a leading space.
    if (b->kind == FT_QUESTION || b->kind == FT_COMMA || b->kind == FT_SEMI || b->kind == FT_COLON) {
        return false;
    }
    // `,`, `;`, `:` always have a trailing space (unless followed by closer).
    if (a->kind == FT_COMMA || a->kind == FT_SEMI || a->kind == FT_COLON) {
        if (b->kind == FT_RPAREN || b->kind == FT_RBRACKET || b->kind == FT_RBRACE) return false;
        return true;
    }

    // No space immediately inside `(` `)` or `[` `]`.
    if (a->kind == FT_LPAREN || a->kind == FT_LBRACKET) return false;
    if (b->kind == FT_RPAREN || b->kind == FT_RBRACKET) return false;

    // Unary prefix operators (`!`, `~`, unary `-`, `*`, `&`): no space after them.
    if (a->kind == FT_UNARY || (a->kind == FT_AMBIG_UNARY && a->resolved_unary)) {
        return false;
    }

    // Braces `{` and `}`
    if (b->kind == FT_LBRACE) {
        if (a->kind == FT_LPAREN || a->kind == FT_LBRACKET || a->kind == FT_LBRACE) return false;
        return true;
    }
    if (a->kind == FT_LBRACE) {
        // Empty `{}` has no space inside; single-line `{ return x }` gets a space.
        return b->kind != FT_RBRACE;
    }
    if (b->kind == FT_RBRACE) {
        return a->kind != FT_LBRACE;
    }
    if (a->kind == FT_RBRACE) {
        // `} else`, etc.
        return true;
    }

    // Opening `(`
    if (b->kind == FT_LPAREN) {
        if (a->kind == FT_IDENT || a->kind == FT_RPAREN || a->kind == FT_RBRACKET) return false;
        if (a->kind == FT_KEYWORD) {
            if (tok_eq(a, "fn")) {
                return is_method_fn_receiver(toks, prev_idx);
            }
            // `if (cond)`, `loop (cond)`, `return (x)`, etc.
            return true;
        }
        if (a->kind == FT_BINOP || a->kind == FT_ASSIGN ||
            (a->kind == FT_AMBIG_UNARY && !a->resolved_unary)) {
            return true;
        }
        return false;
    }

    // Opening `[`
    if (b->kind == FT_LBRACKET) {
        if (a->kind == FT_IDENT || a->kind == FT_RPAREN || a->kind == FT_RBRACKET) return false;
        if (a->kind == FT_KEYWORD) return true; // `fn [T]`, `return [1, 2]`
        if (a->kind == FT_BINOP || a->kind == FT_ASSIGN ||
            (a->kind == FT_AMBIG_UNARY && !a->resolved_unary)) {
            return true;
        }
        return false;
    }

    // Closing `]` followed by type name / `const` / unary `*` in prefix types (`[]T`, `[64]i64`, `[]const u8`, `[]*u8`)
    if (a->kind == FT_RBRACKET) {
        if (b->kind == FT_IDENT) return false;
        if (b->kind == FT_KEYWORD && tok_eq(b, "const")) return false;
        if (b->kind == FT_AMBIG_UNARY && b->resolved_unary && b->text[0] == '*') return false;
        return true;
    }

    // Binary and assignment operators always have spaces on both sides.
    if (a->kind == FT_BINOP || a->kind == FT_ASSIGN ||
        (a->kind == FT_AMBIG_UNARY && !a->resolved_unary) ||
        b->kind == FT_BINOP || b->kind == FT_ASSIGN ||
        (b->kind == FT_AMBIG_UNARY && !b->resolved_unary)) {
        return true;
    }

    // Default: separate adjacent words, literals, closers, unary ops after keywords, etc.
    return true;
}

static bool is_continuation_end(const FmtTok *t) {
    if (!t) return false;
    return t->kind == FT_BINOP || t->kind == FT_ASSIGN ||
           (t->kind == FT_AMBIG_UNARY && !t->resolved_unary);
}

static void emit_indent(Buf *out, int level) {
    for (int i = 0; i < level * 4; i++) {
        buf_u8(out, ' ');
    }
}

char *fmt_source(const char *filename, const char *src) {
    // 1. Syntax-validate with the real lexer + parser first so invalid files are never mangled.
    int saved_file_id = g_file_id;
    const char *saved_filename = g_filename;
    const char *saved_source = g_source;
    register_source_file(filename, src);

    int ntoks = 0;
    Token *ptoks = lex(src, &ntoks);
    NodeVec decls = parse(ptoks, ntoks);
    free(ptoks);
    free(decls.items);

    g_file_id = saved_file_id;
    g_filename = saved_filename;
    g_source = saved_source;

    // 2. Lossless scan + unary classification.
    FmtTokVec toks = fmt_scan(src);
    classify_unary(&toks);

    // 3. Emit formatted lines.
    Buf out = {0};
    int brace_depth = 0;
    int paren_depth = 0;    // combined open `(` and `[` spanning lines
    bool prev_was_cont = false;
    int blank_run = 0;
    bool emitted_any = false;

    int i = 0;
    while (i < toks.len && toks.items[i].kind != FT_EOF) {
        // Find end of current line: [line_start, line_end)
        int line_start = i;
        while (i < toks.len && toks.items[i].kind != FT_NEWLINE && toks.items[i].kind != FT_EOF) {
            i++;
        }
        int line_end = i;
        if (i < toks.len && toks.items[i].kind == FT_NEWLINE) {
            i++;
        }

        // Empty line
        if (line_start == line_end) {
            if (emitted_any) blank_run++;
            continue;
        }

        // Emit at most 1 blank line between non-empty lines (and none immediately after `{` or before `}`).
        bool starts_with_rbrace = (toks.items[line_start].kind == FT_RBRACE);
        if (blank_run > 0 && emitted_any && !starts_with_rbrace) {
            // Avoid blank line directly after an opening `{` on the previous line.
            if (out.len < 2 || out.data[out.len - 2] != '{') {
                buf_u8(&out, '\n');
            }
        }
        blank_run = 0;

        // Compute leading closer dedents on this line (`}`, `)`, `]`).
        int lead_rbrace = 0;
        int lead_rparen = 0;
        for (int k = line_start; k < line_end; k++) {
            FmtTokKind tk = toks.items[k].kind;
            if (tk == FT_RBRACE) lead_rbrace++;
            else if (tk == FT_RPAREN || tk == FT_RBRACKET) lead_rparen++;
            else break;
        }

        int line_brace = brace_depth - lead_rbrace;
        if (line_brace < 0) line_brace = 0;
        int line_paren = paren_depth - lead_rparen;
        if (line_paren < 0) line_paren = 0;

        int indent = line_brace + (line_paren > 0 ? 1 : 0);
        if (prev_was_cont && line_paren == 0 && lead_rbrace == 0) {
            indent += 1;
        }

        emit_indent(&out, indent);

        // Emit tokens on this line with canonical spacing.
        const FmtTok *last_code = NULL;
        for (int k = line_start; k < line_end; k++) {
            if (k > line_start && needs_space_between(&toks, k - 1, k)) {
                buf_u8(&out, ' ');
            }
            const FmtTok *t = &toks.items[k];
            // Normalize CRLF inside multi-line raw strings to LF.
            if (t->kind == FT_LITERAL && t->len > 0 && t->text[0] == '`') {
                for (int c = 0; c < t->len; c++) {
                    if (t->text[c] == '\r' && c + 1 < t->len && t->text[c + 1] == '\n') continue;
                    buf_u8(&out, (uint8_t)t->text[c]);
                }
            } else {
                buf_bytes(&out, t->text, (size_t)t->len);
            }

            if (t->kind == FT_LBRACE) brace_depth++;
            else if (t->kind == FT_RBRACE) { if (brace_depth > 0) brace_depth--; }
            else if (t->kind == FT_LPAREN || t->kind == FT_LBRACKET) paren_depth++;
            else if (t->kind == FT_RPAREN || t->kind == FT_RBRACKET) { if (paren_depth > 0) paren_depth--; }

            if (t->kind != FT_COMMENT) {
                last_code = t;
            }
        }

        buf_u8(&out, '\n');
        emitted_any = true;
        prev_was_cont = is_continuation_end(last_code);
    }

    free(toks.items);
    buf_u8(&out, '\0');
    return (char *)out.data;
}
