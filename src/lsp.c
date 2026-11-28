// lsp.c - Built-in Language Server Protocol (LSP 3.17, JSON-RPC 2.0 over stdio)
// for the Antimony compiler (`sbc lsp`).
//
// Provides real compiler-backed:
//   - Live diagnostics (textDocument/publishDiagnostics)
//   - Go to Definition across files & std/ modules (textDocument/definition)
//   - Hover signatures & docs (textDocument/hover)
//   - Document outline symbols (textDocument/documentSymbol)
//   - Context-aware completion (textDocument/completion)
//   - Canonical formatting via fmt_source (textDocument/formatting)
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "sb.h"
#include <ctype.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

static TargetOs lsp_host_target(void) {
#ifdef _WIN32
    return TARGET_WINDOWS_X64;
#else
    return TARGET_LINUX_X64;
#endif
}

// ---------------------------------------------------------------------------
// Growable string builder helper (backed by Buf)
// ---------------------------------------------------------------------------

static void sb_append(Buf *b, const char *s) {
    if (s) buf_bytes(b, s, strlen(s));
}

static void sb_printf(Buf *b, const char *fmt, ...) {
    char stack[512];
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n >= 0 && (size_t)n < sizeof(stack)) {
        buf_bytes(b, stack, (size_t)n);
    } else if (n > 0) {
        char *heap = xmalloc((size_t)n + 1);
        vsnprintf(heap, (size_t)n + 1, fmt, ap2);
        buf_bytes(b, heap, (size_t)n);
        free(heap);
    }
    va_end(ap2);
}

static void sb_json_str(Buf *b, const char *s) {
    buf_u8(b, '"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            unsigned char c = *p;
            if (c == '"') sb_append(b, "\\\"");
            else if (c == '\\') sb_append(b, "\\\\");
            else if (c == '\n') sb_append(b, "\\n");
            else if (c == '\r') sb_append(b, "\\r");
            else if (c == '\t') sb_append(b, "\\t");
            else if (c < 0x20) sb_printf(b, "\\u%04x", (unsigned)c);
            else buf_u8(b, c);
        }
    }
    buf_u8(b, '"');
}

// ---------------------------------------------------------------------------
// Minimal RFC 8259 JSON parser
// ---------------------------------------------------------------------------

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JKind;

typedef struct JVal JVal;

typedef struct {
    char *key;
    JVal *val;
} JMember;

struct JVal {
    JKind kind;
    bool bval;
    double nval;
    int64_t ival;
    char *sval;
    VEC(JVal *) items;
    VEC(JMember) members;
};

typedef struct {
    const char *s;
    size_t pos;
    size_t len;
} JParser;

static void j_free(JVal *v) {
    if (!v) return;
    if (v->kind == J_STR) free(v->sval);
    else if (v->kind == J_ARR) {
        for (int i = 0; i < v->items.len; i++) j_free(v->items.items[i]);
        free(v->items.items);
    } else if (v->kind == J_OBJ) {
        for (int i = 0; i < v->members.len; i++) {
            free(v->members.items[i].key);
            j_free(v->members.items[i].val);
        }
        free(v->members.items);
    }
    free(v);
}

static void j_skip_ws(JParser *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p->pos++;
        else break;
    }
}

static int j_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void j_append_utf8(Buf *b, unsigned cp) {
    if (cp < 0x80) {
        buf_u8(b, (uint8_t)cp);
    } else if (cp < 0x800) {
        buf_u8(b, (uint8_t)(0xC0 | (cp >> 6)));
        buf_u8(b, (uint8_t)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_u8(b, (uint8_t)(0xE0 | (cp >> 12)));
        buf_u8(b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        buf_u8(b, (uint8_t)(0x80 | (cp & 0x3F)));
    } else {
        buf_u8(b, (uint8_t)(0xF0 | (cp >> 18)));
        buf_u8(b, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
        buf_u8(b, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        buf_u8(b, (uint8_t)(0x80 | (cp & 0x3F)));
    }
}

static char *j_parse_string_raw(JParser *p) {
    if (p->pos >= p->len || p->s[p->pos] != '"') return NULL;
    p->pos++;
    Buf b = {0};
    while (p->pos < p->len) {
        char c = p->s[p->pos++];
        if (c == '"') {
            buf_u8(&b, '\0');
            return (char *)b.data;
        }
        if (c == '\\') {
            if (p->pos >= p->len) { free(b.data); return NULL; }
            char e = p->s[p->pos++];
            switch (e) {
                case '"':  buf_u8(&b, '"'); break;
                case '\\': buf_u8(&b, '\\'); break;
                case '/':  buf_u8(&b, '/'); break;
                case 'b':  buf_u8(&b, '\b'); break;
                case 'f':  buf_u8(&b, '\f'); break;
                case 'n':  buf_u8(&b, '\n'); break;
                case 'r':  buf_u8(&b, '\r'); break;
                case 't':  buf_u8(&b, '\t'); break;
                case 'u': {
                    if (p->pos + 4 > p->len) { free(b.data); return NULL; }
                    unsigned cp = 0;
                    for (int i = 0; i < 4; i++) {
                        int h = j_hex(p->s[p->pos++]);
                        if (h < 0) { free(b.data); return NULL; }
                        cp = (cp << 4) | (unsigned)h;
                    }
                    j_append_utf8(&b, cp);
                    break;
                }
                default: free(b.data); return NULL;
            }
        } else {
            buf_u8(&b, (uint8_t)c);
        }
    }
    free(b.data);
    return NULL;
}

static JVal *j_parse_val(JParser *p, int depth);

static JVal *j_parse_val(JParser *p, int depth) {
    if (depth > 64) return NULL;
    j_skip_ws(p);
    if (p->pos >= p->len) return NULL;
    char c = p->s[p->pos];
    if (c == '"') {
        char *str = j_parse_string_raw(p);
        if (!str) return NULL;
        JVal *v = xcalloc(1, sizeof(JVal));
        v->kind = J_STR;
        v->sval = str;
        return v;
    }
    if (c == '{') {
        p->pos++;
        JVal *v = xcalloc(1, sizeof(JVal));
        v->kind = J_OBJ;
        j_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return v; }
        while (p->pos < p->len) {
            j_skip_ws(p);
            char *key = j_parse_string_raw(p);
            if (!key) { j_free(v); return NULL; }
            j_skip_ws(p);
            if (p->pos >= p->len || p->s[p->pos] != ':') { free(key); j_free(v); return NULL; }
            p->pos++;
            JVal *mv = j_parse_val(p, depth + 1);
            if (!mv) { free(key); j_free(v); return NULL; }
            JMember m = {key, mv};
            vec_push(v->members, m);
            j_skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return v; }
            j_free(v);
            return NULL;
        }
        j_free(v);
        return NULL;
    }
    if (c == '[') {
        p->pos++;
        JVal *v = xcalloc(1, sizeof(JVal));
        v->kind = J_ARR;
        j_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return v; }
        while (p->pos < p->len) {
            JVal *iv = j_parse_val(p, depth + 1);
            if (!iv) { j_free(v); return NULL; }
            vec_push(v->items, iv);
            j_skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return v; }
            j_free(v);
            return NULL;
        }
        j_free(v);
        return NULL;
    }
    if (p->pos + 4 <= p->len && memcmp(p->s + p->pos, "true", 4) == 0) {
        p->pos += 4;
        JVal *v = xcalloc(1, sizeof(JVal));
        v->kind = J_BOOL;
        v->bval = true;
        return v;
    }
    if (p->pos + 5 <= p->len && memcmp(p->s + p->pos, "false", 5) == 0) {
        p->pos += 5;
        JVal *v = xcalloc(1, sizeof(JVal));
        v->kind = J_BOOL;
        v->bval = false;
        return v;
    }
    if (p->pos + 4 <= p->len && memcmp(p->s + p->pos, "null", 4) == 0) {
        p->pos += 4;
        JVal *v = xcalloc(1, sizeof(JVal));
        v->kind = J_NULL;
        return v;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *start = p->s + p->pos;
        char *end = NULL;
        double d = strtod(start, &end);
        if (end == start) return NULL;
        p->pos = (size_t)(end - p->s);
        JVal *v = xcalloc(1, sizeof(JVal));
        v->kind = J_NUM;
        v->nval = d;
        v->ival = (int64_t)d;
        return v;
    }
    return NULL;
}

static JVal *j_parse(const char *s, size_t len) {
    JParser p = {s, 0, len};
    return j_parse_val(&p, 0);
}

static JVal *j_get(JVal *obj, const char *key) {
    if (!obj || obj->kind != J_OBJ) return NULL;
    for (int i = 0; i < obj->members.len; i++) {
        if (strcmp(obj->members.items[i].key, key) == 0) return obj->members.items[i].val;
    }
    return NULL;
}

static const char *j_str(JVal *v) {
    return (v && v->kind == J_STR) ? v->sval : NULL;
}

static int j_int(JVal *v, int def) {
    return (v && v->kind == J_NUM) ? (int)v->ival : def;
}

// ---------------------------------------------------------------------------
// JSON-RPC 2.0 Framing over Binary Stdio
// ---------------------------------------------------------------------------

static char *lsp_read_message(size_t *out_len) {
    size_t content_len = 0;
    bool have_len = false;
    char line[512];

    for (;;) {
        size_t i = 0;
        int ch = EOF;
        while ((ch = fgetc(stdin)) != EOF) {
            if (ch == '\n') break;
            if (ch != '\r' && i + 1 < sizeof(line)) line[i++] = (char)ch;
        }
        if (ch == EOF && i == 0) return NULL;
        line[i] = '\0';
        if (i == 0) {
            if (have_len) break;
            if (ch == EOF) return NULL;
            continue;
        }
        if (i > 15 && sb_stricmp(line, "Content-Length:") != 0) {
            char prefix[16];
            memcpy(prefix, line, 15);
            prefix[15] = '\0';
            if (sb_stricmp(prefix, "Content-Length:") == 0) {
                const char *p = line + 15;
                while (*p == ' ' || *p == '\t') p++;
                content_len = (size_t)strtoull(p, NULL, 10);
                have_len = true;
            }
        }
    }

    if (!have_len || content_len > 64 * 1024 * 1024) return NULL;
    char *buf = xmalloc(content_len + 1);
    size_t got = 0;
    while (got < content_len) {
        size_t n = fread(buf + got, 1, content_len - got, stdin);
        if (n == 0) {
            free(buf);
            return NULL;
        }
        got += n;
    }
    buf[content_len] = '\0';
    *out_len = content_len;
    return buf;
}

static void lsp_write_message(const char *json, size_t len) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", len);
    if (len > 0) fwrite(json, 1, len, stdout);
    fflush(stdout);
}

static void lsp_send_response(JVal *id, const char *result_json) {
    Buf b = {0};
    sb_append(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (!id || id->kind == J_NULL) {
        sb_append(&b, "null");
    } else if (id->kind == J_NUM) {
        sb_printf(&b, "%lld", (long long)id->ival);
    } else if (id->kind == J_STR) {
        sb_json_str(&b, id->sval);
    } else {
        sb_append(&b, "null");
    }
    sb_append(&b, ",\"result\":");
    sb_append(&b, result_json ? result_json : "null");
    sb_append(&b, "}");
    lsp_write_message((const char *)b.data, b.len);
    free(b.data);
}

static void lsp_send_notification(const char *method, const char *params_json) {
    Buf b = {0};
    sb_append(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
    sb_json_str(&b, method);
    sb_append(&b, ",\"params\":");
    sb_append(&b, params_json ? params_json : "{}");
    sb_append(&b, "}");
    lsp_write_message((const char *)b.data, b.len);
    free(b.data);
}

// ---------------------------------------------------------------------------
// File URI <-> Native Path Conversion
// ---------------------------------------------------------------------------

static void uri_to_path(const char *uri, char *out, size_t cap) {
    if (!uri || cap == 0) { if (cap) out[0] = '\0'; return; }
    const char *p = uri;
    if (strncmp(p, "file://", 7) == 0) p += 7;
    size_t j = 0;
    while (*p && j + 1 < cap) {
        if (*p == '%' && p[1] && p[2]) {
            int h1 = j_hex(p[1]), h2 = j_hex(p[2]);
            if (h1 >= 0 && h2 >= 0) {
                out[j++] = (char)((h1 << 4) | h2);
                p += 3;
                continue;
            }
        }
        out[j++] = *p++;
    }
    out[j] = '\0';
#ifdef _WIN32
    // `/C:/foo` -> `C:/foo`
    if (out[0] == '/' && isalpha((unsigned char)out[1]) && out[2] == ':') {
        memmove(out, out + 1, strlen(out));
    }
    for (size_t k = 0; out[k]; k++) {
        if (out[k] == '/') out[k] = '\\';
    }
#endif
}

static void path_to_uri(const char *path, char *out, size_t cap) {
    char full[1024];
    const char *src = path;
#ifdef _WIN32
    if (_fullpath(full, path, sizeof(full))) src = full;
#else
    char rbuf[PATH_MAX];
    if (realpath(path, rbuf)) src = rbuf;
    (void)full;
#endif
    Buf b = {0};
    sb_append(&b, "file://");
#ifdef _WIN32
    if (isalpha((unsigned char)src[0]) && src[1] == ':') {
        buf_u8(&b, '/');
    }
#endif
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char c = *p;
        if (c == '\\') c = '/';
        if (c == ' ' || c == '%' || c == '#' || c == '?') {
            sb_printf(&b, "%%%02X", (unsigned)c);
        } else {
            buf_u8(&b, c);
        }
    }
    buf_u8(&b, '\0');
    snprintf(out, cap, "%s", (const char *)b.data);
    free(b.data);
}

static const char *path_basename_c(const char *p) {
    if (!p) return "";
    const char *s1 = strrchr(p, '/');
    const char *s2 = strrchr(p, '\\');
    const char *s = s1;
    if (s2 && (!s || s2 > s)) s = s2;
    return s ? s + 1 : p;
}

static void dir_of_path(const char *path, char *out, size_t cap) {
    const char *s1 = strrchr(path, '/');
    const char *s2 = strrchr(path, '\\');
    const char *sep = s1;
    if (s2 && (!sep || s2 > sep)) sep = s2;
    if (!sep) { out[0] = '\0'; return; }
    size_t len = (size_t)(sep - path + 1);
    if (len >= cap) len = cap - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

// ---------------------------------------------------------------------------
// Compiler-Backed Symbol Index for an Open Document
// ---------------------------------------------------------------------------

typedef struct {
    char *name;
    char *recv_type;        // non-NULL for methods (`fn (self: *Vec) push(...)`)
    char *detail;           // short detail for outline/completion
    char *signature;        // full markdown code-block signature for hover
    char *file_path;        // declaring file path
    int file_id;
    int line;               // 1-based
    int col;                // 1-based
    int end_line;           // 1-based
    int lsp_kind;           // LSP SymbolKind
    int comp_kind;          // LSP CompletionItemKind
    bool is_in_root;
    bool is_priv;
    bool is_local;
    int scope_start;        // 1-based enclosing fn start line (for locals)
    int scope_end;          // 1-based enclosing fn end line (for locals)
} LspSym;

typedef struct {
    int line;               // 1-based line of `import "..."`
    char *raw_path;
    char *resolved_path;
} LspImport;

typedef struct {
    char *uri;
    char *path;
    char *text;
    VEC(LspSym) syms;
    VEC(LspImport) imports;
} LspDoc;

static VEC(LspDoc *) g_docs;

static void clear_doc_index(LspDoc *doc) {
    for (int i = 0; i < doc->syms.len; i++) {
        free(doc->syms.items[i].name);
        free(doc->syms.items[i].recv_type);
        free(doc->syms.items[i].detail);
        free(doc->syms.items[i].signature);
        free(doc->syms.items[i].file_path);
    }
    doc->syms.len = 0;
    for (int i = 0; i < doc->imports.len; i++) {
        free(doc->imports.items[i].raw_path);
        free(doc->imports.items[i].resolved_path);
    }
    doc->imports.len = 0;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *c = xmalloc(n + 1);
    memcpy(c, s, n + 1);
    return c;
}

static LspDoc *find_doc(const char *uri) {
    for (int i = 0; i < g_docs.len; i++) {
#ifdef _WIN32
        if (sb_stricmp(g_docs.items[i]->uri, uri) == 0) return g_docs.items[i];
#else
        if (strcmp(g_docs.items[i]->uri, uri) == 0) return g_docs.items[i];
#endif
    }
    return NULL;
}

static LspDoc *upsert_doc(const char *uri, const char *text) {
    LspDoc *d = find_doc(uri);
    if (!d) {
        d = xcalloc(1, sizeof(LspDoc));
        d->uri = xstrdup(uri);
        char pbuf[1024];
        uri_to_path(uri, pbuf, sizeof(pbuf));
        d->path = xstrdup(pbuf);
        vec_push(g_docs, d);
    }
    free(d->text);
    d->text = xstrdup(text ? text : "");
    return d;
}

static void remove_doc(const char *uri) {
    for (int i = 0; i < g_docs.len; i++) {
        LspDoc *d = g_docs.items[i];
        bool match =
#ifdef _WIN32
            (sb_stricmp(d->uri, uri) == 0);
#else
            (strcmp(d->uri, uri) == 0);
#endif
        if (match) {
            clear_doc_index(d);
            free(d->syms.items);
            free(d->imports.items);
            free(d->uri);
            free(d->path);
            free(d->text);
            free(d);
            for (int j = i + 1; j < g_docs.len; j++) g_docs.items[j - 1] = g_docs.items[j];
            g_docs.len--;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Formatting AST Declarations into Clean Antimony Signatures
// ---------------------------------------------------------------------------

static void format_type_annot(Buf *b, Node *n) {
    if (n->type && n->type->name) {
        sb_append(b, n->type->name);
        return;
    }
    if (n->decl_kind == DECL_SLICE) {
        sb_append(b, n->is_const_slice ? "[]const " : "[]");
    } else if (n->decl_kind == DECL_ARRAY) {
        sb_append(b, "[..]");
    }
    for (int i = n->ptr_depth - 1; i >= 0; i--) {
        if ((n->ptr_const_mask >> i) & 1u) sb_append(b, "*const ");
        else sb_append(b, "*");
    }
    if (n->type_name) sb_append(b, n->type_name);
    else sb_append(b, "?");
}

static void format_fn_sig(Buf *b, Node *d, bool include_pub) {
    if (include_pub) {
        if (d->is_pub) sb_append(b, "pub ");
        else if (d->is_priv) sb_append(b, "priv ");
    }
    if (d->is_extern) {
        sb_append(b, "extern ");
        if (d->dll_name) {
            sb_printf(b, "\"%s\" ", d->dll_name);
        }
    }
    sb_append(b, "fn ");
    int start_param = 0;
    if (d->name2 && d->list.len > 0) {
        // Method: d->list[0] is receiver, d->name2 is method name
        Node *recv = d->list.items[0];
        sb_printf(b, "(%s: ", recv->name ? recv->name : "self");
        format_type_annot(b, recv);
        sb_printf(b, ") %s", d->name2);
        start_param = 1;
    } else {
        sb_append(b, d->name ? d->name : "<fn>");
    }
    if (d->tparams.len > 0) {
        sb_append(b, "[");
        for (int i = 0; i < d->tparams.len; i++) {
            if (i > 0) sb_append(b, ", ");
            sb_append(b, d->tparams.items[i]);
        }
        sb_append(b, "]");
    }
    sb_append(b, "(");
    for (int i = start_param; i < d->list.len; i++) {
        if (i > start_param) sb_append(b, ", ");
        Node *p = d->list.items[i];
        sb_append(b, p->name ? p->name : "_");
        if (p->decl_kind == DECL_SLICE) sb_append(b, "[]");
        sb_append(b, ": ");
        if (p->decl_kind == DECL_SLICE) {
            if (p->is_const_slice) sb_append(b, "const ");
            for (int k = p->ptr_depth - 1; k >= 0; k--) {
                sb_append(b, ((p->ptr_const_mask >> k) & 1u) ? "*const " : "*");
            }
            sb_append(b, p->type_name ? p->type_name : "?");
        } else {
            format_type_annot(b, p);
        }
    }
    sb_append(b, ")");
    if (d->type && d->type != &ty_void && d->type->name) {
        sb_printf(b, ": %s", d->type->name);
    } else if (d->type_name) {
        sb_append(b, ": ");
        format_type_annot(b, d);
    }
}

static void strip_base_type_name(const char *tname, char *out, size_t cap) {
    if (!tname) { out[0] = '\0'; return; }
    size_t i = 0;
    while (tname[i] && tname[i] != '[' && i + 1 < cap) {
        out[i] = tname[i];
        i++;
    }
    out[i] = '\0';
}

static void collect_stmt_locals(LspDoc *doc, Node *stmt, const char *root_path,
                                int scope_start, int scope_end) {
    if (!stmt) return;
    if (stmt->kind == N_VAR || stmt->kind == N_CONST) {
        if (stmt->name && stmt->pos.file_id == 0) {
            Buf sig = {0};
            sb_append(&sig, stmt->kind == N_CONST ? "const " : "var ");
            sb_append(&sig, stmt->name);
            if (stmt->type && stmt->type->name) {
                sb_printf(&sig, ": %s", stmt->type->name);
            } else if (stmt->type_name) {
                sb_append(&sig, ": ");
                format_type_annot(&sig, stmt);
            }
            buf_u8(&sig, '\0');

            LspSym sym = {0};
            sym.name = xstrdup(stmt->name);
            sym.detail = xstrdup((stmt->type && stmt->type->name) ? stmt->type->name
                                 : (stmt->type_name ? stmt->type_name : "local"));
            sym.signature = (char *)sig.data;
            sym.file_path = xstrdup(root_path);
            sym.file_id = 0;
            sym.line = stmt->pos.line;
            sym.col = stmt->pos.col;
            sym.end_line = stmt->pos.line;
            sym.lsp_kind = (stmt->kind == N_CONST) ? 14 : 13;
            sym.comp_kind = (stmt->kind == N_CONST) ? 21 : 6;
            sym.is_in_root = true;
            sym.is_local = true;
            sym.scope_start = scope_start;
            sym.scope_end = scope_end;
            vec_push(doc->syms, sym);
        }
        return;
    }
    if (stmt->kind == N_LOOP_RANGE || stmt->kind == N_LOOP_ITER) {
        if (stmt->name && stmt->pos.file_id == 0) {
            Buf sig = {0};
            sb_printf(&sig, "loop var %s", stmt->name);
            if (stmt->sym && stmt->sym->type && stmt->sym->type->name) {
                sb_printf(&sig, ": %s", stmt->sym->type->name);
            }
            buf_u8(&sig, '\0');
            LspSym sym = {0};
            sym.name = xstrdup(stmt->name);
            sym.detail = xstrdup("loop var");
            sym.signature = (char *)sig.data;
            sym.file_path = xstrdup(root_path);
            sym.file_id = 0;
            sym.line = stmt->pos.line;
            sym.col = stmt->pos.col;
            sym.end_line = stmt->pos.line;
            sym.lsp_kind = 13;
            sym.comp_kind = 6;
            sym.is_in_root = true;
            sym.is_local = true;
            sym.scope_start = scope_start;
            sym.scope_end = scope_end;
            vec_push(doc->syms, sym);
        }
        if (stmt->name2 && stmt->pos.file_id == 0) {
            Buf sig = {0};
            sb_printf(&sig, "loop var %s", stmt->name2);
            if (stmt->sym2 && stmt->sym2->type && stmt->sym2->type->name) {
                sb_printf(&sig, ": %s", stmt->sym2->type->name);
            }
            buf_u8(&sig, '\0');
            LspSym sym = {0};
            sym.name = xstrdup(stmt->name2);
            sym.detail = xstrdup("loop var");
            sym.signature = (char *)sig.data;
            sym.file_path = xstrdup(root_path);
            sym.file_id = 0;
            sym.line = stmt->pos.line;
            sym.col = stmt->pos.col;
            sym.end_line = stmt->pos.line;
            sym.lsp_kind = 13;
            sym.comp_kind = 6;
            sym.is_in_root = true;
            sym.is_local = true;
            sym.scope_start = scope_start;
            sym.scope_end = scope_end;
            vec_push(doc->syms, sym);
        }
    }
    if (stmt->kind == N_BLOCK) {
        for (int i = 0; i < stmt->list.len; i++) {
            collect_stmt_locals(doc, stmt->list.items[i], root_path, scope_start, scope_end);
        }
    } else if (stmt->kind == N_IF) {
        collect_stmt_locals(doc, stmt->body, root_path, scope_start, scope_end);
        collect_stmt_locals(doc, stmt->els, root_path, scope_start, scope_end);
    } else if (stmt->kind == N_LOOP_INF || stmt->kind == N_LOOP_COND ||
               stmt->kind == N_LOOP_RANGE || stmt->kind == N_LOOP_ITER || stmt->kind == N_LOOP_C) {
        collect_stmt_locals(doc, stmt->init, root_path, scope_start, scope_end);
        collect_stmt_locals(doc, stmt->body, root_path, scope_start, scope_end);
    } else if (stmt->kind == N_MATCH) {
        for (int i = 0; i < stmt->list.len; i++) {
            Node *arm = stmt->list.items[i];
            if (arm && arm->body) collect_stmt_locals(doc, arm->body, root_path, scope_start, scope_end);
        }
    }
}

static void index_decls(LspDoc *doc, NodeVec decls) {
    clear_doc_index(doc);

    // Compute per-file opt-in `pub` visibility so private symbols in imported modules are marked.
    int max_fid = 0;
    for (int i = 0; i < decls.len; i++) {
        if (decls.items[i]->pos.file_id > max_fid) max_fid = decls.items[i]->pos.file_id;
    }
    bool *file_has_pub = xcalloc((size_t)(max_fid + 1), sizeof(bool));
    for (int i = 0; i < decls.len; i++) {
        if (decls.items[i]->is_pub) file_has_pub[decls.items[i]->pos.file_id] = true;
    }

    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        int fid = d->pos.file_id;
        const char *fpath = (fid == 0) ? doc->path : source_file_name(fid);
        bool is_priv = d->is_priv || (file_has_pub[fid] && !d->is_pub);

        if (d->kind == N_FN) {
            Buf sig = {0};
            format_fn_sig(&sig, d, true);
            buf_u8(&sig, '\0');

            Buf det = {0};
            format_fn_sig(&det, d, false);
            buf_u8(&det, '\0');

            LspSym sym = {0};
            sym.name = xstrdup(d->name2 ? d->name2 : (d->name ? d->name : "<fn>"));
            if (d->name2 && d->list.len > 0 && d->list.items[0]->type_name) {
                char base[128];
                strip_base_type_name(d->list.items[0]->type_name, base, sizeof(base));
                sym.recv_type = xstrdup(base);
            }
            sym.detail = (char *)det.data;
            sym.signature = (char *)sig.data;
            sym.file_path = xstrdup(fpath);
            sym.file_id = fid;
            sym.line = d->pos.line;
            sym.col = d->pos.col;
            sym.end_line = (d->body && d->body->end_pos.line >= d->pos.line) ? d->body->end_pos.line : d->pos.line;
            sym.lsp_kind = d->name2 ? 6 : 12;   // Method=6, Function=12
            sym.comp_kind = d->name2 ? 2 : 3;   // Method=2, Function=3
            sym.is_in_root = (fid == 0);
            sym.is_priv = is_priv;
            vec_push(doc->syms, sym);

            // Index parameters and local variables for functions in the root document
            if (fid == 0 && d->body) {
                int s_start = d->pos.line;
                int s_end = sym.end_line;
                for (int pi = 0; pi < d->list.len; pi++) {
                    Node *p = d->list.items[pi];
                    if (!p->name) continue;
                    Buf psig = {0};
                    sb_printf(&psig, "param %s: ", p->name);
                    format_type_annot(&psig, p);
                    buf_u8(&psig, '\0');
                    LspSym psym = {0};
                    psym.name = xstrdup(p->name);
                    psym.detail = xstrdup(p->type_name ? p->type_name : "param");
                    psym.signature = (char *)psig.data;
                    psym.file_path = xstrdup(doc->path);
                    psym.file_id = 0;
                    psym.line = p->pos.line;
                    psym.col = p->pos.col;
                    psym.end_line = p->pos.line;
                    psym.lsp_kind = 13;
                    psym.comp_kind = 6;
                    psym.is_in_root = true;
                    psym.is_local = true;
                    psym.scope_start = s_start;
                    psym.scope_end = s_end;
                    vec_push(doc->syms, psym);
                }
                collect_stmt_locals(doc, d->body, doc->path, s_start, s_end);
            }
        } else if (d->kind == N_STRUCT) {
            Buf sig = {0};
            if (d->is_pub) sb_append(&sig, "pub ");
            else if (is_priv) sb_append(&sig, "priv ");
            sb_printf(&sig, "struct %s", d->name);
            if (d->tparams.len > 0) {
                sb_append(&sig, "[");
                for (int k = 0; k < d->tparams.len; k++) {
                    if (k > 0) sb_append(&sig, ", ");
                    sb_append(&sig, d->tparams.items[k]);
                }
                sb_append(&sig, "]");
            }
            sb_append(&sig, " {");
            int show = d->list.len < 12 ? d->list.len : 12;
            for (int k = 0; k < show; k++) {
                Node *f = d->list.items[k];
                sb_printf(&sig, "\n    %s: ", f->name);
                format_type_annot(&sig, f);
            }
            if (d->list.len > show) sb_printf(&sig, "\n    // ... %d more fields", d->list.len - show);
            sb_append(&sig, "\n}");
            buf_u8(&sig, '\0');

            int eline = d->pos.line;
            if (d->list.len > 0 && d->list.items[d->list.len - 1]->pos.line > eline) {
                eline = d->list.items[d->list.len - 1]->pos.line + 1;
            }
            LspSym sym = {0};
            sym.name = xstrdup(d->name);
            sym.detail = xstrdup("struct");
            sym.signature = (char *)sig.data;
            sym.file_path = xstrdup(fpath);
            sym.file_id = fid;
            sym.line = d->pos.line;
            sym.col = d->pos.col;
            sym.end_line = eline;
            sym.lsp_kind = 23;  // Struct
            sym.comp_kind = 22; // Struct
            sym.is_in_root = (fid == 0);
            sym.is_priv = is_priv;
            vec_push(doc->syms, sym);
        } else if (d->kind == N_ENUM) {
            Buf sig = {0};
            if (d->is_pub) sb_append(&sig, "pub ");
            else if (is_priv) sb_append(&sig, "priv ");
            sb_printf(&sig, "enum %s", d->name);
            if (d->tparams.len > 0) {
                sb_append(&sig, "[");
                for (int k = 0; k < d->tparams.len; k++) {
                    if (k > 0) sb_append(&sig, ", ");
                    sb_append(&sig, d->tparams.items[k]);
                }
                sb_append(&sig, "]");
            }
            sb_append(&sig, " {");
            int show = d->list.len < 12 ? d->list.len : 12;
            for (int k = 0; k < show; k++) {
                Node *v = d->list.items[k];
                sb_printf(&sig, "\n    %s", v->name);
                if (v->type_name) {
                    sb_append(&sig, "(");
                    format_type_annot(&sig, v);
                    sb_append(&sig, ")");
                }
            }
            if (d->list.len > show) sb_printf(&sig, "\n    // ... %d more variants", d->list.len - show);
            sb_append(&sig, "\n}");
            buf_u8(&sig, '\0');

            int eline = d->pos.line;
            if (d->list.len > 0 && d->list.items[d->list.len - 1]->pos.line > eline) {
                eline = d->list.items[d->list.len - 1]->pos.line + 1;
            }
            LspSym sym = {0};
            sym.name = xstrdup(d->name);
            sym.detail = xstrdup("enum");
            sym.signature = (char *)sig.data;
            sym.file_path = xstrdup(fpath);
            sym.file_id = fid;
            sym.line = d->pos.line;
            sym.col = d->pos.col;
            sym.end_line = eline;
            sym.lsp_kind = 10;  // Enum
            sym.comp_kind = 13; // Enum
            sym.is_in_root = (fid == 0);
            sym.is_priv = is_priv;
            vec_push(doc->syms, sym);
        } else if (d->kind == N_TYPE_ALIAS) {
            Buf sig = {0};
            if (d->is_pub) sb_append(&sig, "pub ");
            else if (is_priv) sb_append(&sig, "priv ");
            sb_printf(&sig, "type %s = ", d->name);
            format_type_annot(&sig, d);
            buf_u8(&sig, '\0');

            LspSym sym = {0};
            sym.name = xstrdup(d->name);
            sym.detail = xstrdup("type alias");
            sym.signature = (char *)sig.data;
            sym.file_path = xstrdup(fpath);
            sym.file_id = fid;
            sym.line = d->pos.line;
            sym.col = d->pos.col;
            sym.end_line = d->pos.line;
            sym.lsp_kind = 26;  // TypeParameter
            sym.comp_kind = 7;  // Class/Type
            sym.is_in_root = (fid == 0);
            sym.is_priv = is_priv;
            vec_push(doc->syms, sym);
        } else if (d->kind == N_CONST || d->kind == N_VAR) {
            Buf sig = {0};
            if (d->is_pub) sb_append(&sig, "pub ");
            else if (is_priv) sb_append(&sig, "priv ");
            sb_append(&sig, d->kind == N_CONST ? "const " : "var ");
            sb_append(&sig, d->name);
            if (d->type && d->type->name) {
                sb_printf(&sig, ": %s", d->type->name);
            } else if (d->type_name) {
                sb_append(&sig, ": ");
                format_type_annot(&sig, d);
            }
            buf_u8(&sig, '\0');

            LspSym sym = {0};
            sym.name = xstrdup(d->name);
            sym.detail = xstrdup(d->kind == N_CONST ? "const" : "var");
            sym.signature = (char *)sig.data;
            sym.file_path = xstrdup(fpath);
            sym.file_id = fid;
            sym.line = d->pos.line;
            sym.col = d->pos.col;
            sym.end_line = d->pos.line;
            sym.lsp_kind = (d->kind == N_CONST) ? 14 : 13;
            sym.comp_kind = (d->kind == N_CONST) ? 21 : 6;
            sym.is_in_root = (fid == 0);
            sym.is_priv = is_priv;
            vec_push(doc->syms, sym);
        }
    }

    free(file_has_pub);
}

static void collect_root_imports(LspDoc *doc, TargetOs target) {
    char base_dir[1024];
    dir_of_path(doc->path, base_dir, sizeof(base_dir));

    const char *p = doc->text;
    int line = 1;
    while (*p) {
        const char *ls = p;
        while (*p && *p != '\n') p++;
        const char *le = p;
        if (*p == '\n') p++;

        const char *q = ls;
        while (q < le && (*q == ' ' || *q == '\t')) q++;
        if ((size_t)(le - q) > 7 && memcmp(q, "import", 6) == 0 && (q[6] == ' ' || q[6] == '\t')) {
            q += 6;
            while (q < le && (*q == ' ' || *q == '\t')) q++;
            if (q < le && *q == '"') {
                q++;
                const char *s0 = q;
                while (q < le && *q != '"') q++;
                if (q < le && *q == '"') {
                    size_t rlen = (size_t)(q - s0);
                    char *raw = xmalloc(rlen + 1);
                    memcpy(raw, s0, rlen);
                    raw[rlen] = '\0';
                    char resolved[1024];
                    if (resolve_import_path_for_lsp(raw, base_dir, target, resolved, sizeof(resolved))) {
                        LspImport imp = {line, raw, xstrdup(resolved)};
                        vec_push(doc->imports, imp);
                    } else {
                        free(raw);
                    }
                }
            }
        }
        line++;
    }
}

static void publish_diagnostics(const char *uri, const SbErrorCapture *err) {
    Buf b = {0};
    sb_append(&b, "{\"uri\":");
    sb_json_str(&b, uri);
    sb_append(&b, ",\"diagnostics\":[");
    if (err && err->has_error) {
        int ln = (err->pos.line > 0) ? (err->pos.line - 1) : 0;
        int col = (err->pos.col > 0) ? (err->pos.col - 1) : 0;
        char msg_buf[1536];
        if (err->pos.file_id != 0 && err->filename[0]) {
            ln = 0;
            col = 0;
            snprintf(msg_buf, sizeof(msg_buf), "%s:%d:%d: %s",
                     path_basename_c(err->filename), err->pos.line, err->pos.col, err->message);
        } else {
            snprintf(msg_buf, sizeof(msg_buf), "%s", err->message);
        }
        sb_printf(&b,
            "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
            "\"end\":{\"line\":%d,\"character\":%d}},"
            "\"severity\":1,\"source\":\"sbc\",\"message\":",
            ln, col, ln, col + 1);
        sb_json_str(&b, msg_buf);
        sb_append(&b, "}");
    }
    sb_append(&b, "]}");
    buf_u8(&b, '\0');
    lsp_send_notification("textDocument/publishDiagnostics", (const char *)b.data);
    free(b.data);
}

static void analyze_and_publish(LspDoc *doc) {
    TargetOs target = lsp_host_target();
    reset_source_files();

    NodeVec decls = {0};
    SbErrorCapture cap = {0};
    jmp_buf env;

    sb_error_begin(&env, &cap);
    if (setjmp(env) == 0) {
        load_module_for_lsp(doc->path, doc->text, &decls, target);
        // Index parsed declarations before type-checking so navigation/hover still work
        // even if a type error occurs inside a function body.
        index_decls(doc, decls);
        collect_root_imports(doc, target);

        (void)check_ex(decls, false);
        // Re-index after successful check so inferred types on `var` / `fn` are populated!
        index_decls(doc, decls);
        collect_root_imports(doc, target);
    }
    sb_error_end();

    free(decls.items);
    publish_diagnostics(doc->uri, cap.has_error ? &cap : NULL);
}

// ---------------------------------------------------------------------------
// Text / Cursor Utilities
// ---------------------------------------------------------------------------

static bool is_ident_char_c(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// Extracts the identifier under 0-based (line, character) and checks whether it is preceded by `.`.
static bool word_at_pos(const char *text, int target_line, int target_col,
                        char *word_out, size_t word_cap, bool *after_dot_out) {
    if (after_dot_out) *after_dot_out = false;
    if (!text || target_line < 0 || target_col < 0) return false;
    const char *p = text;
    int line = 0;
    while (*p && line < target_line) {
        if (*p++ == '\n') line++;
    }
    if (line != target_line) return false;
    const char *ls = p;
    const char *le = ls;
    while (*le && *le != '\n' && *le != '\r') le++;
    int llen = (int)(le - ls);
    int col = target_col;
    if (col > llen) col = llen;
    if (col > 0 && (col == llen || !is_ident_char_c(ls[col])) && is_ident_char_c(ls[col - 1])) {
        col--;
    }
    if (col >= llen || !is_ident_char_c(ls[col])) return false;

    int s = col, e = col;
    while (s > 0 && is_ident_char_c(ls[s - 1])) s--;
    while (e < llen && is_ident_char_c(ls[e])) e++;
    size_t wlen = (size_t)(e - s);
    if (wlen == 0 || wlen >= word_cap) return false;
    memcpy(word_out, ls + s, wlen);
    word_out[wlen] = '\0';

    if (after_dot_out && s > 0 && ls[s - 1] == '.') {
        *after_dot_out = true;
    }
    return true;
}

static void prefix_at_pos(const char *text, int target_line, int target_col,
                          char *prefix_out, size_t prefix_cap, bool *after_dot_out) {
    prefix_out[0] = '\0';
    if (after_dot_out) *after_dot_out = false;
    if (!text || target_line < 0 || target_col < 0) return;
    const char *p = text;
    int line = 0;
    while (*p && line < target_line) {
        if (*p++ == '\n') line++;
    }
    if (line != target_line) return;
    const char *ls = p;
    const char *le = ls;
    while (*le && *le != '\n' && *le != '\r') le++;
    int llen = (int)(le - ls);
    int col = target_col > llen ? llen : target_col;
    int s = col;
    while (s > 0 && is_ident_char_c(ls[s - 1])) s--;
    size_t plen = (size_t)(col - s);
    if (plen >= prefix_cap) plen = prefix_cap - 1;
    memcpy(prefix_out, ls + s, plen);
    prefix_out[plen] = '\0';
    if (after_dot_out && s > 0 && ls[s - 1] == '.') {
        *after_dot_out = true;
    }
}

typedef struct {
    const char *name;
    const char *doc;
    int comp_kind;
} BuiltinEntry;

static const BuiltinEntry k_builtins[] = {
    {"i8",          "i8 — 8-bit signed integer (-128..127)", 14},
    {"i16",         "i16 — 16-bit signed integer", 14},
    {"i32",         "i32 — 32-bit signed integer (default integer type)", 14},
    {"i64",         "i64 — 64-bit signed integer", 14},
    {"isize",       "isize — pointer-sized signed integer (64-bit)", 14},
    {"u8",          "u8 — 8-bit unsigned integer (0..255)", 14},
    {"u16",         "u16 — 16-bit unsigned integer", 14},
    {"u32",         "u32 — 32-bit unsigned integer", 14},
    {"u64",         "u64 — 64-bit unsigned integer", 14},
    {"usize",       "usize — pointer-sized unsigned integer (64-bit)", 14},
    {"f32",         "f32 — 32-bit IEEE-754 floating-point number", 14},
    {"f64",         "f64 — 64-bit IEEE-754 floating-point number", 14},
    {"bool",        "bool — boolean value (`true` or `false`)", 14},
    {"str",         "str — immutable UTF-8 string slice `{ptr, len}`", 14},
    {"print",       "fn print(fmt: str, args...) — built-in formatted stdout output", 3},
    {"exit",        "fn exit(code: i32) — terminates the process immediately", 3},
    {"len",         "fn len(x): i64 — element/byte count of array, slice, or `str`", 3},
    {"sizeof",      "fn sizeof(T): usize — compile-time size of type `T` in bytes", 3},
    {"alignof",     "fn alignof(T): usize — compile-time alignment of type `T` in bytes", 3},
    {"ptr_cast",    "fn ptr_cast(*T, ptr): *T — explicit pointer cast", 3},
    {"rev",         "rev(lo..hi) — reverses a half-open range in a `loop` header", 14},
    {"atomic_load", "fn atomic_load(p: *i64): i64 — atomic 64-bit load", 3},
    {"atomic_store","fn atomic_store(p: *i64, v: i64) — atomic 64-bit store", 3},
    {"atomic_add",  "fn atomic_add(p: *i64, d: i64): i64 — atomic fetch-and-add", 3},
    {"atomic_cas",  "fn atomic_cas(p: *i64, expected: i64, desired: i64): i64 — atomic CAS", 3},
    {"fn",          "keyword `fn` — function or method declaration", 14},
    {"var",         "keyword `var` — mutable variable declaration", 14},
    {"const",       "keyword `const` — compile-time constant or read-only qualifier", 14},
    {"struct",      "keyword `struct` — composite value type", 14},
    {"enum",        "keyword `enum` — integer enum or tagged union", 14},
    {"type",        "keyword `type` — transparent type alias", 14},
    {"match",       "keyword `match` — exhaustive pattern matching", 14},
    {"defer",       "keyword `defer` — LIFO scope-exit cleanup statement", 14},
    {"loop",        "keyword `loop` — unified loop construct", 14},
    {"import",      "keyword `import` — module import declaration", 14},
    {"extern",      "keyword `extern` — foreign shared-library FFI declaration", 14},
    {"pub",         "keyword `pub` — export declaration from module", 14},
    {"priv",        "keyword `priv` — file-private declaration", 14},
    {"return",      "keyword `return` — return from function", 14},
    {"null",        "keyword `null` — null pointer literal", 14},
    {NULL, NULL, 0}
};

static const LspSym *lookup_symbol(LspDoc *doc, const char *word, int line1, bool after_dot) {
    // 1. If preceded by `.`, prefer methods matching `word`
    if (after_dot) {
        const LspSym *best = NULL;
        for (int i = 0; i < doc->syms.len; i++) {
            const LspSym *s = &doc->syms.items[i];
            if (!s->recv_type) continue;
            if (!s->is_in_root && s->is_priv) continue;
            if (strcmp(s->name, word) == 0) {
                if (s->is_in_root) return s;
                if (!best) best = s;
            }
        }
        if (best) return best;
    }

    // 2. Local variables / parameters in enclosing function
    if (!after_dot) {
        const LspSym *best_local = NULL;
        for (int i = 0; i < doc->syms.len; i++) {
            const LspSym *s = &doc->syms.items[i];
            if (!s->is_local) continue;
            if (line1 >= s->scope_start && line1 <= s->scope_end && strcmp(s->name, word) == 0) {
                if (!best_local || s->line <= line1) best_local = s;
            }
        }
        if (best_local) return best_local;
    }

    // 3. Top-level declarations (prefer root file, then imported public declarations)
    const LspSym *imported_match = NULL;
    for (int i = 0; i < doc->syms.len; i++) {
        const LspSym *s = &doc->syms.items[i];
        if (s->is_local || s->recv_type) continue;
        if (!s->is_in_root && s->is_priv) continue;
        if (strcmp(s->name, word) == 0) {
            if (s->is_in_root) return s;
            if (!imported_match) imported_match = s;
        }
    }
    return imported_match;
}

// ---------------------------------------------------------------------------
// LSP Request Handlers
// ---------------------------------------------------------------------------

static void handle_hover(JVal *id, JVal *params) {
    const char *uri = j_str(j_get(j_get(params, "textDocument"), "uri"));
    JVal *pos = j_get(params, "position");
    LspDoc *doc = uri ? find_doc(uri) : NULL;
    if (!doc || !pos) { lsp_send_response(id, "null"); return; }

    int line0 = j_int(j_get(pos, "line"), 0);
    int col0  = j_int(j_get(pos, "character"), 0);
    char word[128];
    bool after_dot = false;
    if (!word_at_pos(doc->text, line0, col0, word, sizeof(word), &after_dot)) {
        lsp_send_response(id, "null");
        return;
    }

    Buf md = {0};
    const LspSym *sym = lookup_symbol(doc, word, line0 + 1, after_dot);
    if (sym) {
        sb_append(&md, "```antimony\n");
        sb_append(&md, sym->signature);
        sb_append(&md, "\n```");
        if (sym->file_path) {
            sb_printf(&md, "\n\n*Defined in `%s` (line %d)*", path_basename_c(sym->file_path), sym->line);
        }
    } else {
        for (int i = 0; k_builtins[i].name; i++) {
            if (strcmp(k_builtins[i].name, word) == 0) {
                sb_append(&md, "```antimony\n");
                sb_append(&md, k_builtins[i].doc);
                sb_append(&md, "\n```");
                break;
            }
        }
    }
    if (md.len == 0) {
        lsp_send_response(id, "null");
        return;
    }
    buf_u8(&md, '\0');

    Buf resp = {0};
    sb_append(&resp, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    sb_json_str(&resp, (const char *)md.data);
    sb_append(&resp, "}}");
    buf_u8(&resp, '\0');
    lsp_send_response(id, (const char *)resp.data);
    free(md.data);
    free(resp.data);
}

static void handle_definition(JVal *id, JVal *params) {
    const char *uri = j_str(j_get(j_get(params, "textDocument"), "uri"));
    JVal *pos = j_get(params, "position");
    LspDoc *doc = uri ? find_doc(uri) : NULL;
    if (!doc || !pos) { lsp_send_response(id, "null"); return; }

    int line0 = j_int(j_get(pos, "line"), 0);
    int col0  = j_int(j_get(pos, "character"), 0);

    // 1. Check if cursor is on an `import "..."` line -> jump directly to imported module!
    for (int i = 0; i < doc->imports.len; i++) {
        if (doc->imports.items[i].line == line0 + 1) {
            char target_uri[1200];
            path_to_uri(doc->imports.items[i].resolved_path, target_uri, sizeof(target_uri));
            Buf resp = {0};
            sb_append(&resp, "{\"uri\":");
            sb_json_str(&resp, target_uri);
            sb_append(&resp, ",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}}}");
            buf_u8(&resp, '\0');
            lsp_send_response(id, (const char *)resp.data);
            free(resp.data);
            return;
        }
    }

    // 2. Resolve identifier under cursor to its declaration location (same file or imported!)
    char word[128];
    bool after_dot = false;
    if (!word_at_pos(doc->text, line0, col0, word, sizeof(word), &after_dot)) {
        lsp_send_response(id, "null");
        return;
    }

    const LspSym *sym = lookup_symbol(doc, word, line0 + 1, after_dot);
    if (!sym || !sym->file_path) {
        lsp_send_response(id, "null");
        return;
    }

    char target_uri[1200];
    if (sym->file_id == 0) {
        snprintf(target_uri, sizeof(target_uri), "%s", doc->uri);
    } else {
        path_to_uri(sym->file_path, target_uri, sizeof(target_uri));
    }
    int dline = sym->line > 0 ? sym->line - 1 : 0;
    int dcol  = sym->col  > 0 ? sym->col  - 1 : 0;
    int dlen  = (int)strlen(sym->name);

    Buf resp = {0};
    sb_append(&resp, "{\"uri\":");
    sb_json_str(&resp, target_uri);
    sb_printf(&resp,
        ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
        "\"end\":{\"line\":%d,\"character\":%d}}}",
        dline, dcol, dline, dcol + dlen);
    buf_u8(&resp, '\0');
    lsp_send_response(id, (const char *)resp.data);
    free(resp.data);
}

static void handle_document_symbol(JVal *id, JVal *params) {
    const char *uri = j_str(j_get(j_get(params, "textDocument"), "uri"));
    LspDoc *doc = uri ? find_doc(uri) : NULL;
    if (!doc) { lsp_send_response(id, "[]"); return; }

    Buf resp = {0};
    sb_append(&resp, "[");
    int count = 0;
    for (int i = 0; i < doc->syms.len; i++) {
        const LspSym *s = &doc->syms.items[i];
        if (!s->is_in_root || s->is_local) continue;
        if (count++ > 0) sb_append(&resp, ",");
        int sl = s->line > 0 ? s->line - 1 : 0;
        int sc = s->col > 0 ? s->col - 1 : 0;
        int el = s->end_line >= s->line ? s->end_line - 1 : sl;
        int nlen = (int)strlen(s->name);

        sb_append(&resp, "{\"name\":");
        if (s->recv_type) {
            Buf full = {0};
            sb_printf(&full, "%s.%s", s->recv_type, s->name);
            buf_u8(&full, '\0');
            sb_json_str(&resp, (const char *)full.data);
            free(full.data);
        } else {
            sb_json_str(&resp, s->name);
        }
        sb_append(&resp, ",\"detail\":");
        sb_json_str(&resp, s->detail ? s->detail : "");
        sb_printf(&resp,
            ",\"kind\":%d,"
            "\"range\":{\"start\":{\"line\":%d,\"character\":0},\"end\":{\"line\":%d,\"character\":0}},"
            "\"selectionRange\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
            s->lsp_kind, sl, el, sl, sc, sl, sc + nlen);
    }
    sb_append(&resp, "]");
    buf_u8(&resp, '\0');
    lsp_send_response(id, (const char *)resp.data);
    free(resp.data);
}

static bool starts_with_ci(const char *str, const char *prefix) {
    while (*prefix) {
        if (!*str) return false;
        unsigned char a = (unsigned char)*str++;
        unsigned char b = (unsigned char)*prefix++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static void handle_completion(JVal *id, JVal *params) {
    const char *uri = j_str(j_get(j_get(params, "textDocument"), "uri"));
    JVal *pos = j_get(params, "position");
    LspDoc *doc = uri ? find_doc(uri) : NULL;
    if (!doc || !pos) { lsp_send_response(id, "[]"); return; }

    int line0 = j_int(j_get(pos, "line"), 0);
    int col0  = j_int(j_get(pos, "character"), 0);
    char prefix[128];
    bool after_dot = false;
    prefix_at_pos(doc->text, line0, col0, prefix, sizeof(prefix), &after_dot);

    Buf resp = {0};
    sb_append(&resp, "[");
    StrVec seen = {0};
    int emitted = 0;

    for (int i = 0; i < doc->syms.len && emitted < 120; i++) {
        const LspSym *s = &doc->syms.items[i];
        if (!s->is_in_root && s->is_priv) continue;
        if (after_dot) {
            if (!s->recv_type) continue;
        } else {
            if (s->recv_type) continue;
            if (s->is_local && (line0 + 1 < s->scope_start || line0 + 1 > s->scope_end)) continue;
        }
        if (prefix[0] && !starts_with_ci(s->name, prefix)) continue;

        bool dup = false;
        for (int k = 0; k < seen.len; k++) {
            if (strcmp(seen.items[k], s->name) == 0) { dup = true; break; }
        }
        if (dup) continue;
        vec_push(seen, s->name);

        if (emitted++ > 0) sb_append(&resp, ",");
        sb_append(&resp, "{\"label\":");
        sb_json_str(&resp, s->name);
        sb_printf(&resp, ",\"kind\":%d,\"detail\":", s->comp_kind);
        sb_json_str(&resp, s->detail ? s->detail : "");
        sb_append(&resp, "}");
    }

    if (!after_dot) {
        for (int i = 0; k_builtins[i].name && emitted < 150; i++) {
            if (prefix[0] && !starts_with_ci(k_builtins[i].name, prefix)) continue;
            bool dup = false;
            for (int k = 0; k < seen.len; k++) {
                if (strcmp(seen.items[k], k_builtins[i].name) == 0) { dup = true; break; }
            }
            if (dup) continue;
            if (emitted++ > 0) sb_append(&resp, ",");
            sb_append(&resp, "{\"label\":");
            sb_json_str(&resp, k_builtins[i].name);
            sb_printf(&resp, ",\"kind\":%d,\"detail\":", k_builtins[i].comp_kind);
            sb_json_str(&resp, k_builtins[i].doc);
            sb_append(&resp, "}");
        }
    }

    sb_append(&resp, "]");
    buf_u8(&resp, '\0');
    free(seen.items);
    lsp_send_response(id, (const char *)resp.data);
    free(resp.data);
}

static void handle_formatting(JVal *id, JVal *params) {
    const char *uri = j_str(j_get(j_get(params, "textDocument"), "uri"));
    LspDoc *doc = uri ? find_doc(uri) : NULL;
    if (!doc) { lsp_send_response(id, "[]"); return; }

    char *formatted = NULL;
    SbErrorCapture cap = {0};
    jmp_buf env;
    sb_error_begin(&env, &cap);
    if (setjmp(env) == 0) {
        formatted = fmt_source(doc->path, doc->text);
    }
    sb_error_end();

    if (!formatted || cap.has_error) {
        free(formatted);
        lsp_send_response(id, "[]");
        return;
    }

    // Count end line & character of the current document text
    int end_line = 0, end_col = 0;
    for (const char *p = doc->text; *p; p++) {
        if (*p == '\n') { end_line++; end_col = 0; }
        else if (*p != '\r') { end_col++; }
    }

    Buf resp = {0};
    sb_printf(&resp,
        "[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":%d,\"character\":%d}},\"newText\":",
        end_line, end_col);
    sb_json_str(&resp, formatted);
    sb_append(&resp, "}]");
    buf_u8(&resp, '\0');
    free(formatted);
    lsp_send_response(id, (const char *)resp.data);
    free(resp.data);
}

// ---------------------------------------------------------------------------
// Main LSP Server Loop
// ---------------------------------------------------------------------------

int run_lsp_server(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    bool shutdown_requested = false;

    for (;;) {
        size_t msg_len = 0;
        char *raw = lsp_read_message(&msg_len);
        if (!raw) break;

        JVal *msg = j_parse(raw, msg_len);
        free(raw);
        if (!msg || msg->kind != J_OBJ) {
            j_free(msg);
            continue;
        }

        const char *method = j_str(j_get(msg, "method"));
        JVal *id = j_get(msg, "id");
        JVal *params = j_get(msg, "params");

        if (!method) {
            j_free(msg);
            continue;
        }

        if (strcmp(method, "initialize") == 0) {
            Buf resp = {0};
            sb_printf(&resp,
                "{\"capabilities\":{"
                "\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":true}},"
                "\"hoverProvider\":true,"
                "\"definitionProvider\":true,"
                "\"documentSymbolProvider\":true,"
                "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
                "\"documentFormattingProvider\":true"
                "},\"serverInfo\":{\"name\":\"sbc-lsp\",\"version\":\"%s\"}}",
                SB_VERSION);
            buf_u8(&resp, '\0');
            lsp_send_response(id, (const char *)resp.data);
            free(resp.data);
        } else if (strcmp(method, "initialized") == 0) {
            // Notification: no response required.
        } else if (strcmp(method, "shutdown") == 0) {
            shutdown_requested = true;
            lsp_send_response(id, "null");
        } else if (strcmp(method, "exit") == 0) {
            j_free(msg);
            break;
        } else if (strcmp(method, "textDocument/didOpen") == 0) {
            JVal *td = j_get(params, "textDocument");
            const char *uri = j_str(j_get(td, "uri"));
            const char *text = j_str(j_get(td, "text"));
            if (uri && text) {
                LspDoc *doc = upsert_doc(uri, text);
                analyze_and_publish(doc);
            }
        } else if (strcmp(method, "textDocument/didChange") == 0) {
            const char *uri = j_str(j_get(j_get(params, "textDocument"), "uri"));
            JVal *changes = j_get(params, "contentChanges");
            if (uri && changes && changes->kind == J_ARR && changes->items.len > 0) {
                JVal *last = changes->items.items[changes->items.len - 1];
                const char *text = j_str(j_get(last, "text"));
                if (text) {
                    LspDoc *doc = upsert_doc(uri, text);
                    analyze_and_publish(doc);
                }
            }
        } else if (strcmp(method, "textDocument/didSave") == 0) {
            const char *uri = j_str(j_get(j_get(params, "textDocument"), "uri"));
            const char *text = j_str(j_get(params, "text"));
            if (uri) {
                LspDoc *doc = text ? upsert_doc(uri, text) : find_doc(uri);
                if (doc) analyze_and_publish(doc);
            }
        } else if (strcmp(method, "textDocument/didClose") == 0) {
            const char *uri = j_str(j_get(j_get(params, "textDocument"), "uri"));
            if (uri) {
                publish_diagnostics(uri, NULL);
                remove_doc(uri);
            }
        } else if (strcmp(method, "textDocument/hover") == 0) {
            handle_hover(id, params);
        } else if (strcmp(method, "textDocument/definition") == 0) {
            handle_definition(id, params);
        } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
            handle_document_symbol(id, params);
        } else if (strcmp(method, "textDocument/completion") == 0) {
            handle_completion(id, params);
        } else if (strcmp(method, "textDocument/formatting") == 0) {
            handle_formatting(id, params);
        } else if (id) {
            // Unknown request with an id -> reply cleanly with null so clients never hang.
            lsp_send_response(id, "null");
        }

        j_free(msg);
    }

    while (g_docs.len > 0) {
        remove_doc(g_docs.items[0]->uri);
    }
    free(g_docs.items);
    return shutdown_requested ? 0 : 1;
}
