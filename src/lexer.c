// lexer.c - tokenizer with Go-style automatic statement termination.
#include "sb.h"

typedef VEC(Token) TokVec;

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Spec §1.3: a newline terminates a statement if the last token on the line is
// an identifier, a literal, return/break/continue, or ) ] }.
static bool triggers_term(TokKind k) {
    switch (k) {
    case T_IDENT: case T_INT: case T_FLOAT: case T_STR:
    case T_TRUE: case T_FALSE: case T_NULL:
    case T_RETURN: case T_BREAK: case T_CONTINUE:
    case T_RPAREN: case T_RBRACKET: case T_RBRACE:
        return true;
    default:
        return false;
    }
}

static TokKind lookup_keyword(const char *s, int len) {
    static const struct { const char *kw; TokKind kind; } tab[] = {
        {"var", T_VAR}, {"const", T_CONST}, {"fn", T_FN}, {"struct", T_STRUCT},
        {"enum", T_ENUM}, {"match", T_MATCH}, {"defer", T_DEFER},
        {"import", T_IMPORT}, {"extern", T_EXTERN},
        {"return", T_RETURN}, {"if", T_IF}, {"else", T_ELSE}, {"loop", T_LOOP},
        {"in", T_IN}, {"by", T_BY}, {"break", T_BREAK}, {"continue", T_CONTINUE},
        {"true", T_TRUE}, {"false", T_FALSE}, {"null", T_NULL},
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if ((int)strlen(tab[i].kw) == len && memcmp(s, tab[i].kw, (size_t)len) == 0)
            return tab[i].kind;
    }
    return T_IDENT;
}

Token *lex(const char *src, int *count) {
    TokVec out = {0};
    int line = 1, col = 1;
    const char *p = src;
    if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) {
        p += 3; // skip UTF-8 BOM
    }

#define PUSH(k, pos_) do { Token t = {0}; t.kind = (k); t.pos = (pos_); vec_push(out, t); } while (0)
#define ADV()         do { if (*p == '\n') { line++; col = 1; } else { col++; } p++; } while (0)

    while (*p) {
        Pos pos = {line, col, g_file_id};
        char c = *p;

        if (c == ' ' || c == '\t' || c == '\r') {
            ADV();
            continue;
        }
        if (c == '\n') {
            if (out.len > 0 && triggers_term(out.items[out.len - 1].kind)) {
                PUSH(T_TERM, pos);
            }
            ADV();
            continue;
        }
        if (c == '/' && p[1] == '/') {
            while (*p && *p != '\n') ADV();
            continue;
        }

        // Identifiers and keywords.
        if (is_alpha(c)) {
            const char *start = p;
            while (is_alpha(*p) || is_digit(*p)) ADV();
            int len = (int)(p - start);
            TokKind k = lookup_keyword(start, len);
            Token t = {0};
            t.kind = k;
            t.pos = pos;
            if (k == T_IDENT) t.name = intern(start, len);
            vec_push(out, t);
            continue;
        }

        // Numbers (integer or float).
        if (is_digit(c)) {
            const char *start = p;
            int base = 10;
            if (c == '0' && (p[1] == 'x' || p[1] == 'X')) {
                base = 16; ADV(); ADV();
            } else if (c == '0' && (p[1] == 'b' || p[1] == 'B')) {
                base = 2; ADV(); ADV();
            } else if (c == '0' && (p[1] == 'o' || p[1] == 'O')) {
                base = 8; ADV(); ADV();
            }

            bool is_flt = false;
            if (base == 10) {
                const char *q = p;
                while (is_digit(*q) || *q == '_') q++;
                if (*q == '.' && q[1] != '.' && is_digit(q[1])) is_flt = true;
                else if (*q == 'e' || *q == 'E') is_flt = true;
            }

            if (is_flt) {
                char buf[128];
                int bi = 0;
                while (is_digit(*p) || *p == '_') {
                    if (*p != '_' && bi < 120) buf[bi++] = *p;
                    ADV();
                }
                if (*p == '.') {
                    buf[bi++] = *p; ADV();
                    while (is_digit(*p) || *p == '_') {
                        if (*p != '_' && bi < 120) buf[bi++] = *p;
                        ADV();
                    }
                }
                if (*p == 'e' || *p == 'E') {
                    buf[bi++] = *p; ADV();
                    if (*p == '+' || *p == '-') { buf[bi++] = *p; ADV(); }
                    if (!is_digit(*p)) fatal(pos, "malformed float exponent");
                    while (is_digit(*p)) {
                        if (bi < 120) buf[bi++] = *p;
                        ADV();
                    }
                }
                buf[bi] = '\0';
                Token t = {0};
                t.kind = T_FLOAT;
                t.pos = pos;
                t.fval = strtod(buf, NULL);
                vec_push(out, t);
                continue;
            }

            uint64_t val = 0;
            int digits = 0;
            bool prev_under = true;
            while (*p) {
                if (*p == '_') {
                    if (prev_under) fatal(pos, "invalid '_' in integer literal");
                    prev_under = true;
                    ADV();
                    continue;
                }
                int d = hex_val(*p);
                if (d < 0 || d >= base) break;
                prev_under = false;
                digits++;
                if (val > (UINT64_MAX - (uint64_t)d) / (uint64_t)base)
                    fatal(pos, "integer literal exceeds 64-bit range");
                val = val * (uint64_t)base + (uint64_t)d;
                ADV();
            }
            if (digits == 0 || prev_under)
                fatal(pos, "malformed integer literal '%.*s'", (int)(p - start), start);
            if (is_alpha(*p) || is_digit(*p))
                fatal(pos, "invalid character '%c' in integer literal (type suffixes are not supported)", *p);

            Token t = {0};
            t.kind = T_INT;
            t.pos = pos;
            t.ival = val;
            vec_push(out, t);
            continue;
        }

        // String literal.
        if (c == '"') {
            ADV();
            Buf b = {0};
            while (*p && *p != '"') {
                if (*p == '\n' || *p == '\r')
                    fatal(pos, "unterminated string literal");
                if (*p == '\\') {
                    ADV();
                    switch (*p) {
                    case 'n':  buf_u8(&b, '\n'); ADV(); break;
                    case 't':  buf_u8(&b, '\t'); ADV(); break;
                    case 'r':  buf_u8(&b, '\r'); ADV(); break;
                    case '\\': buf_u8(&b, '\\'); ADV(); break;
                    case '"':  buf_u8(&b, '"');  ADV(); break;
                    case '0':  buf_u8(&b, 0);    ADV(); break;
                    case 'x': {
                        ADV();
                        int hi = hex_val(p[0]), lo = hex_val(p[1]);
                        if (hi < 0 || lo < 0) fatal(pos, "invalid \\x escape");
                        buf_u8(&b, (uint8_t)((hi << 4) | lo));
                        ADV(); ADV();
                        break;
                    }
                    default:
                        fatal(pos, "unknown escape sequence '\\%c'", *p);
                    }
                } else {
                    buf_u8(&b, (uint8_t)*p);
                    ADV();
                }
            }
            if (*p != '"') fatal(pos, "unterminated string literal");
            ADV();
            buf_u8(&b, 0);
            Token t = {0};
            t.kind = T_STR;
            t.pos = pos;
            t.str = (const char *)b.data;
            t.str_len = (int)b.len - 1;
            vec_push(out, t);
            continue;
        }

        // Multi-character operators and punctuation.
        #define MATCH3(c1, c2, c3, k) \
            if (p[0] == (c1) && p[1] == (c2) && p[2] == (c3)) { ADV(); ADV(); ADV(); PUSH(k, pos); continue; }
        #define MATCH2(c1, c2, k) \
            if (p[0] == (c1) && p[1] == (c2)) { ADV(); ADV(); PUSH(k, pos); continue; }

        MATCH3('+', '%', '=', T_PLUSW_ASSIGN)
        MATCH3('-', '%', '=', T_MINUSW_ASSIGN)
        MATCH3('*', '%', '=', T_STARW_ASSIGN)
        MATCH3('<', '<', '=', T_SHL_ASSIGN)
        MATCH3('>', '>', '=', T_SHR_ASSIGN)

        MATCH2('+', '%', T_PLUSW)
        MATCH2('-', '%', T_MINUSW)
        MATCH2('*', '%', T_STARW)
        MATCH2('+', '=', T_PLUS_ASSIGN)
        MATCH2('-', '=', T_MINUS_ASSIGN)
        MATCH2('*', '=', T_STAR_ASSIGN)
        MATCH2('/', '=', T_SLASH_ASSIGN)
        MATCH2('%', '=', T_PERCENT_ASSIGN)
        MATCH2('&', '=', T_AMP_ASSIGN)
        MATCH2('|', '=', T_PIPE_ASSIGN)
        MATCH2('^', '=', T_CARET_ASSIGN)
        MATCH2('<', '<', T_SHL)
        MATCH2('>', '>', T_SHR)
        MATCH2('=', '=', T_EQ)
        MATCH2('!', '=', T_NE)
        MATCH2('<', '=', T_LE)
        MATCH2('>', '=', T_GE)
        MATCH2('&', '&', T_ANDAND)
        MATCH2('|', '|', T_OROR)
        MATCH2('.', '.', T_DOTDOT)

        // Single-character tokens.
        TokKind single = T_EOF;
        switch (c) {
        case '(': single = T_LPAREN; break;
        case ')': single = T_RPAREN; break;
        case '{': single = T_LBRACE; break;
        case '}': single = T_RBRACE; break;
        case '[': single = T_LBRACKET; break;
        case ']': single = T_RBRACKET; break;
        case ',': single = T_COMMA; break;
        case ':': single = T_COLON; break;
        case '.': single = T_DOT; break;
        case ';': single = T_SEMI; break;
        case '+': single = T_PLUS; break;
        case '-': single = T_MINUS; break;
        case '*': single = T_STAR; break;
        case '/': single = T_SLASH; break;
        case '%': single = T_PERCENT; break;
        case '&': single = T_AMP; break;
        case '|': single = T_PIPE; break;
        case '^': single = T_CARET; break;
        case '~': single = T_TILDE; break;
        case '!': single = T_BANG; break;
        case '<': single = T_LT; break;
        case '>': single = T_GT; break;
        case '=': single = T_ASSIGN; break;
        default:
            fatal(pos, "unexpected character '%c'", c);
        }
        ADV();
        PUSH(single, pos);
    }

    Pos end_pos = {line, col, g_file_id};
    if (out.len > 0 && triggers_term(out.items[out.len - 1].kind)) {
        PUSH(T_TERM, end_pos);
    }
    PUSH(T_EOF, end_pos);

    *count = out.len;
    return out.items;
}

bool is_assign_op(TokKind k) {
    return k >= T_ASSIGN && k <= T_STARW_ASSIGN;
}

TokKind assign_to_binop(TokKind k) {
    switch (k) {
    case T_PLUS_ASSIGN:    return T_PLUS;
    case T_MINUS_ASSIGN:   return T_MINUS;
    case T_STAR_ASSIGN:    return T_STAR;
    case T_SLASH_ASSIGN:   return T_SLASH;
    case T_PERCENT_ASSIGN: return T_PERCENT;
    case T_AMP_ASSIGN:     return T_AMP;
    case T_PIPE_ASSIGN:    return T_PIPE;
    case T_CARET_ASSIGN:   return T_CARET;
    case T_SHL_ASSIGN:     return T_SHL;
    case T_SHR_ASSIGN:     return T_SHR;
    case T_PLUSW_ASSIGN:   return T_PLUSW;
    case T_MINUSW_ASSIGN:  return T_MINUSW;
    case T_STARW_ASSIGN:   return T_STARW;
    default:               return T_EOF;
    }
}

const char *tok_name(TokKind k) {
    switch (k) {
    case T_EOF: return "end of file";
    case T_TERM: return "newline";
    case T_SEMI: return "';'";
    case T_IDENT: return "identifier";
    case T_INT: return "integer literal";
    case T_FLOAT: return "float literal";
    case T_STR: return "string literal";
    case T_VAR: return "'var'";
    case T_CONST: return "'const'";
    case T_FN: return "'fn'";
    case T_STRUCT: return "'struct'";
    case T_ENUM: return "'enum'";
    case T_MATCH: return "'match'";
    case T_DEFER: return "'defer'";
    case T_IMPORT: return "'import'";
    case T_EXTERN: return "'extern'";
    case T_RETURN: return "'return'";
    case T_IF: return "'if'";
    case T_ELSE: return "'else'";
    case T_LOOP: return "'loop'";
    case T_IN: return "'in'";
    case T_BY: return "'by'";
    case T_BREAK: return "'break'";
    case T_CONTINUE: return "'continue'";
    case T_TRUE: return "'true'";
    case T_FALSE: return "'false'";
    case T_NULL: return "'null'";
    case T_LPAREN: return "'('";
    case T_RPAREN: return "')'";
    case T_LBRACE: return "'{'";
    case T_RBRACE: return "'}'";
    case T_LBRACKET: return "'['";
    case T_RBRACKET: return "']'";
    case T_COMMA: return "','";
    case T_COLON: return "':'";
    case T_DOT: return "'.'";
    case T_DOTDOT: return "'..'";
    case T_PLUS: return "'+'";
    case T_MINUS: return "'-'";
    case T_STAR: return "'*'";
    case T_SLASH: return "'/'";
    case T_PERCENT: return "'%'";
    case T_PLUSW: return "'+%'";
    case T_MINUSW: return "'-%'";
    case T_STARW: return "'*%'";
    case T_AMP: return "'&'";
    case T_PIPE: return "'|'";
    case T_CARET: return "'^'";
    case T_TILDE: return "'~'";
    case T_BANG: return "'!'";
    case T_SHL: return "'<<'";
    case T_SHR: return "'>>'";
    case T_EQ: return "'=='";
    case T_NE: return "'!='";
    case T_LT: return "'<'";
    case T_LE: return "'<='";
    case T_GT: return "'>'";
    case T_GE: return "'>='";
    case T_ANDAND: return "'&&'";
    case T_OROR: return "'||'";
    case T_ASSIGN: return "'='";
    default: return "assignment operator";
    }
}
