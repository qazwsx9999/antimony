// main.c - CLI driver for the Antimony compiler (sbc).
//
// Usage:
//   sbc [build] <file.sb> [-o <out.exe>] [--release] [--llvm | -O3] [--emit-llvm] [--run]
//   sbc run <file.sb> [--release] [--llvm | -O3]
//   sbc check [--lib] <file.sb>
//   sbc fmt [-w | --write] [--check] [--stdin | <file.sb>...]
//   sbc --version
#include "sb.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
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

static char *read_all_stdin(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    Buf b = {0};
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
        buf_bytes(&b, chunk, n);
    }
    buf_u8(&b, '\0');
    return (char *)b.data;
}

static void write_stdout_binary(const char *data, size_t len) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (len > 0) {
        fwrite(data, 1, len, stdout);
    }
    fflush(stdout);
}

static bool write_entire_file(const char *path, const char *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    bool ok = (len == 0) || (fwrite(data, 1, len, fp) == len);
    fclose(fp);
    return ok;
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

static void print_version(void) {
    printf("sbc %s (Antimony, Windows x64)\n", SB_VERSION);
}

static void usage(int exit_code) {
    FILE *out = (exit_code == 0) ? stdout : stderr;
    fprintf(out,
        "Antimony compiler (sbc %s) — Dual-Backend (Native x64 & Optimizing/LLVM)\n"
        "\n"
        "Usage:\n"
        "  sbc [build] <file.sb> [-o <out.exe>] [--release] [--llvm | -O3] [--emit-llvm] [--run]\n"
        "  sbc run <file.sb> [--release] [--llvm | -O3]\n"
        "  sbc check [--lib] <file.sb>\n"
        "  sbc fmt [-w | --write] [--check] [--stdin | <file.sb>...]\n"
        "  sbc --version | --help\n",
        SB_VERSION);
    exit(exit_code);
}

static bool text_eq_normalized(const char *raw, const char *formatted) {
    // Compare ignoring CRLF vs LF differences so Windows checkouts match `--check` cleanly.
    const char *a = raw;
    const char *b = formatted;
    if ((uint8_t)a[0] == 0xEF && (uint8_t)a[1] == 0xBB && (uint8_t)a[2] == 0xBF) {
        a += 3;
    }
    while (*a || *b) {
        if (*a == '\r' && a[1] == '\n') a++;
        if (*a != *b) return false;
        if (*a) a++;
        if (*b) b++;
    }
    return true;
}

static int run_fmt_cmd(int argc, char **argv) {
    bool write_in_place = false;
    bool check_only = false;
    bool use_stdin = false;
    VEC(const char *) files = {0};

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--write") == 0) {
            write_in_place = true;
        } else if (strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else if (strcmp(argv[i], "--stdin") == 0 || strcmp(argv[i], "-") == 0) {
            use_stdin = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(0);
        } else if (argv[i][0] == '-') {
            die("unknown option '%s' for 'sbc fmt'", argv[i]);
        } else {
            vec_push(files, argv[i]);
        }
    }

    if (use_stdin) {
        char *src = read_all_stdin();
        char *formatted = fmt_source("<stdin>", src);
        int rc = 0;
        if (check_only) {
            if (!text_eq_normalized(src, formatted)) rc = 1;
        } else {
            write_stdout_binary(formatted, strlen(formatted));
        }
        free(src);
        free(formatted);
        free(files.items);
        return rc;
    }

    if (files.len == 0) {
        usage(1);
    }

    int unformatted = 0;
    for (int i = 0; i < files.len; i++) {
        const char *path = files.items[i];
        char *src = read_entire_file_or_null(path);
        if (!src) die("cannot open '%s'", path);
        char *formatted = fmt_source(path, src);
        bool same = text_eq_normalized(src, formatted);

        if (check_only) {
            if (!same) {
                fprintf(stderr, "%s\n", path);
                unformatted++;
            }
        } else if (write_in_place) {
            if (!same) {
                if (!write_entire_file(path, formatted, strlen(formatted))) {
                    die("cannot write '%s'", path);
                }
            }
        } else {
            write_stdout_binary(formatted, strlen(formatted));
        }
        free(src);
        free(formatted);
    }
    free(files.items);
    return (check_only && unformatted > 0) ? 1 : 0;
}

static int run_check_cmd(int argc, char **argv) {
    const char *input = NULL;
    bool allow_lib = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--lib") == 0) {
            allow_lib = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(0);
        } else if (argv[i][0] == '-') {
            die("unknown option '%s' for 'sbc check'", argv[i]);
        } else if (!input) {
            input = argv[i];
        } else {
            usage(1);
        }
    }
    if (!input) usage(1);

    NodeVec decls = {0};
    Pos root_pos = {1, 1, 0};
    load_module_recursive(input, "", false, root_pos, &decls);
    (void)check_ex(decls, !allow_lib);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) usage(1);

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "version") == 0) {
        print_version();
        return 0;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        usage(0);
    }
    if (strcmp(argv[1], "fmt") == 0) {
        return run_fmt_cmd(argc, argv);
    }
    if (strcmp(argv[1], "check") == 0) {
        return run_check_cmd(argc, argv);
    }

    const char *input = NULL;
    const char *output = NULL;
    bool release = false;
    bool use_llvm = false;
    bool emit_ir = false;
    bool run_after = false;

    int start_idx = 1;
    if (strcmp(argv[1], "build") == 0) {
        start_idx = 2;
    } else if (strcmp(argv[1], "run") == 0) {
        run_after = true;
        start_idx = 2;
    }

    for (int i = start_idx; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (++i >= argc) usage(1);
            output = argv[i];
        } else if (strcmp(argv[i], "--release") == 0 || strcmp(argv[i], "-O") == 0 || strcmp(argv[i], "-O2") == 0) {
            release = true;
        } else if (strcmp(argv[i], "--llvm") == 0 || strcmp(argv[i], "-O3") == 0 || strcmp(argv[i], "--opt") == 0) {
            use_llvm = true;
            release = true;
        } else if (strcmp(argv[i], "--emit-llvm") == 0) {
            emit_ir = true;
        } else if (strcmp(argv[i], "--run") == 0 || strcmp(argv[i], "-r") == 0) {
            run_after = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(0);
        } else if (argv[i][0] == '-') {
            die("unknown option '%s'", argv[i]);
        } else if (!input) {
            input = argv[i];
        } else {
            usage(1);
        }
    }
    if (!input) usage(1);
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

    if (emit_ir) {
        if (!emit_llvm_ir(&prog, decls, output, release)) {
            die("failed to write LLVM IR to '%s'", output);
        }
    } else if (use_llvm) {
        if (!compile_opt_backend(&prog, decls, output, release)) {
            die("optimizing backend failed for '%s'", output);
        }
    } else {
        Image img = codegen(&prog, release);
        write_pe(output, &img);
    }

#ifdef _WIN32
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    fprintf(stderr, "compiled %s -> %s (%.2f ms, %s%s)\n",
            input, output, ms,
            release ? "release" : "debug",
            use_llvm ? ", llvm-O3" : (emit_ir ? ", llvm-ir" : ""));
#endif

    if (run_after) {
        return system(output);
    }
    return 0;
}
