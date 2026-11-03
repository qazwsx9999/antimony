// main.c - CLI driver for the Antimony compiler (sbc).
//
// Usage:
//   sbc <input.sb> [-o <output.exe>] [--release] [--run]
#include "sb.h"

#ifdef _WIN32
#include <windows.h>
#endif

static char *read_entire_file_or_null(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = xmalloc((size_t)sz + 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        free(buf);
        return NULL;
    }
    fclose(fp);
    buf[sz] = '\0';
    return buf;
}

static VEC(const char *) g_loaded_files;

static bool is_already_loaded(const char *canon) {
    for (int i = 0; i < g_loaded_files.len; i++) {
        if (_stricmp(g_loaded_files.items[i], canon) == 0) return true;
    }
    return false;
}

static void extract_dir(const char *path, char *dir_out, size_t cap) {
    const char *s1 = strrchr(path, '/');
    const char *s2 = strrchr(path, '\\');
    const char *sep = s1;
    if (s2 && (!sep || s2 > sep)) sep = s2;
    if (!sep) {
        dir_out[0] = '\0';
        return;
    }
    size_t len = (size_t)(sep - path + 1);
    if (len >= cap) len = cap - 1;
    memcpy(dir_out, path, len);
    dir_out[len] = '\0';
}

static char *try_open_module(const char *prefix, const char *raw_path, char *out_path, size_t out_cap) {
    if (prefix && prefix[0]) {
        snprintf(out_path, out_cap, "%s%s", prefix, raw_path);
    } else {
        snprintf(out_path, out_cap, "%s", raw_path);
    }
    char *src = read_entire_file_or_null(out_path);
    if (!src) {
        size_t clen = strlen(out_path);
        if (clen < 3 || _stricmp(out_path + clen - 3, ".sb") != 0) {
            char with_ext[1032];
            snprintf(with_ext, sizeof(with_ext), "%s.sb", out_path);
            src = read_entire_file_or_null(with_ext);
            if (src) snprintf(out_path, out_cap, "%s", with_ext);
        }
    }
    return src;
}

static void load_module_recursive(const char *raw_path, const char *base_dir,
                                  bool is_import, Pos import_pos, NodeVec *all_decls) {
    char candidate[1024];
    bool is_abs = (raw_path[0] == '/' || raw_path[0] == '\\' ||
                   (raw_path[0] && raw_path[1] == ':'));
    char *src = try_open_module(is_abs ? "" : base_dir, raw_path, candidate, sizeof(candidate));
#ifdef _WIN32
    if (!src && is_import && !is_abs) {
        char exe_path[1024], exe_dir[1024];
        if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) > 0) {
            extract_dir(exe_path, exe_dir, sizeof(exe_dir));
            src = try_open_module(exe_dir, raw_path, candidate, sizeof(candidate));
        }
    }
#endif
    if (!src) {
        if (is_import) {
            fatal(import_pos, "cannot open imported module '%s'", raw_path);
        } else {
            die("cannot open '%s'", raw_path);
        }
    }

    char full_buf[1024];
#ifdef _WIN32
    const char *canon_str = _fullpath(full_buf, candidate, sizeof(full_buf)) ? full_buf : candidate;
#else
    const char *canon_str = candidate;
#endif
    const char *canon = intern(canon_str, (int)strlen(canon_str));
    if (is_already_loaded(canon)) {
        free(src);
        return;
    }
    vec_push(g_loaded_files, canon);

    const char *disp_name = intern(candidate, (int)strlen(candidate));
    int saved_file_id = g_file_id;
    const char *saved_filename = g_filename;
    const char *saved_source = g_source;

    register_source_file(disp_name, src);

    int ntoks = 0;
    Token *toks = lex(src, &ntoks);
    NodeVec file_decls = parse(toks, ntoks);
    free(toks);

    char cur_dir[1024];
    extract_dir(candidate, cur_dir, sizeof(cur_dir));

    for (int i = 0; i < file_decls.len; i++) {
        Node *d = file_decls.items[i];
        if (d->kind == N_IMPORT) {
            load_module_recursive(d->str, cur_dir, true, d->pos, all_decls);
        } else {
            vec_push(*all_decls, d);
        }
    }
    free(file_decls.items);

    g_file_id = saved_file_id;
    g_filename = saved_filename;
    g_source = saved_source;
}

static char *default_output_path(const char *input) {
    const char *slash1 = strrchr(input, '/');
    const char *slash2 = strrchr(input, '\\');
    const char *base = input;
    if (slash1 && slash1 + 1 > base) base = slash1 + 1;
    if (slash2 && slash2 + 1 > base) base = slash2 + 1;

    const char *dot = strrchr(base, '.');
    size_t stem_len = dot ? (size_t)(dot - input) : strlen(input);
    char *out = xmalloc(stem_len + 5);
    memcpy(out, input, stem_len);
    memcpy(out + stem_len, ".exe", 5);
    return out;
}

static void usage(void) {
    fprintf(stderr,
        "Antimony compiler (sbc) — Windows x64 native backend\n"
        "Usage: sbc <file.sb> [-o <out.exe>] [--release] [--run]\n");
    exit(1);
}

int main(int argc, char **argv) {
    const char *input = NULL;
    const char *output = NULL;
    bool release = false;
    bool run_after = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) usage();
            output = argv[i];
        } else if (strcmp(argv[i], "--release") == 0 || strcmp(argv[i], "-O") == 0) {
            release = true;
        } else if (strcmp(argv[i], "--run") == 0 || strcmp(argv[i], "-r") == 0) {
            run_after = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
        } else if (argv[i][0] == '-') {
            die("unknown option '%s'", argv[i]);
        } else if (!input) {
            input = argv[i];
        } else {
            usage();
        }
    }
    if (!input) usage();
    if (!output) output = default_output_path(input);

#ifdef _WIN32
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
#endif

    NodeVec decls = {0};
    Pos root_pos = {1, 1, 0};
    load_module_recursive(input, "", false, root_pos, &decls);
    Program prog = check(decls);
    Image img = codegen(&prog, release);
    write_pe(output, &img);

#ifdef _WIN32
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    fprintf(stderr, "compiled %s -> %s (%.2f ms, %s)\n",
            input, output, ms, release ? "release" : "debug");
#endif

    if (run_after) {
        return system(output);
    }
    return 0;
}
