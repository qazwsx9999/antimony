// main.c - CLI driver for the Antimony compiler (sbc).
//
// Usage:
//   sbc [build] <file.sb> [-o <out>] [--target windows-x64|linux-x64] [--release] [--llvm | -O3] [--emit-llvm] [--run]
//   sbc run <file.sb> [--release] [--llvm | -O3]
//   sbc test <file.sb> [--filter <pat> | -f <pat>] [--release] [--llvm | -O3]
//   sbc check [--lib] [--target windows-x64|linux-x64] <file.sb>
//   sbc fmt [-w | --write] [--check] [--stdin | <file.sb>...]
//   sbc --version
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "sb.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <limits.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

static TargetOs default_host_target(void) {
#ifdef _WIN32
    return TARGET_WINDOWS_X64;
#else
    return TARGET_LINUX_X64;
#endif
}

static TargetOs parse_target_os(const char *s) {
    if (sb_stricmp(s, "windows-x64") == 0 || sb_stricmp(s, "windows") == 0 || sb_stricmp(s, "win64") == 0) {
        return TARGET_WINDOWS_X64;
    }
    if (sb_stricmp(s, "linux-x64") == 0 || sb_stricmp(s, "linux") == 0) {
        return TARGET_LINUX_X64;
    }
    die("unknown target '%s' (supported: windows-x64, linux-x64)", s);
}

static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static int run_executable_path(const char *path) {
    char cmd[1152];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "\"%s\"", path);
    return system(cmd);
#else
    if (strchr(path, '/') == NULL) {
        snprintf(cmd, sizeof(cmd), "\"./%s\"", path);
    } else {
        snprintf(cmd, sizeof(cmd), "\"%s\"", path);
    }
    int status = system(cmd);
    if (status < 0) return 1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
#endif
}

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
static TargetOs g_load_target = TARGET_WINDOWS_X64;

static bool is_already_loaded(const char *canon) {
    for (int i = 0; i < g_loaded_files.len; i++) {
#ifdef _WIN32
        if (sb_stricmp(g_loaded_files.items[i], canon) == 0) return true;
#else
        if (strcmp(g_loaded_files.items[i], canon) == 0) return true;
#endif
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
    if (src) return src;

    size_t clen = strlen(out_path);
    bool has_sb_ext = (clen >= 3 && sb_stricmp(out_path + clen - 3, ".sb") == 0);
    if (!has_sb_ext) {
        char with_ext[1032];
        snprintf(with_ext, sizeof(with_ext), "%s.sb", out_path);
        src = read_entire_file_or_null(with_ext);
        if (src) {
            snprintf(out_path, out_cap, "%s", with_ext);
            return src;
        }
    }

    // Target-OS suffix fallback: e.g. `pal/mem.sb` -> `pal/mem_windows.sb` or `pal/mem_linux.sb`.
    size_t stem_len = has_sb_ext ? (clen - 3) : clen;
    if (stem_len > 0 && stem_len < 1000) {
        const char *os_suffix = (g_load_target == TARGET_LINUX_X64) ? "_linux" : "_windows";
        char os_path[1040];
        snprintf(os_path, sizeof(os_path), "%.*s%s.sb", (int)stem_len, out_path, os_suffix);
        src = read_entire_file_or_null(os_path);
        if (src) {
            snprintf(out_path, out_cap, "%s", os_path);
            return src;
        }
    }
    return NULL;
}

static void load_module_recursive(const char *raw_path, const char *base_dir,
                                  bool is_import, Pos import_pos, NodeVec *all_decls) {
    char candidate[1024];
    bool is_abs = (raw_path[0] == '/' || raw_path[0] == '\\' ||
                   (raw_path[0] && raw_path[1] == ':'));
    char *src = try_open_module(is_abs ? "" : base_dir, raw_path, candidate, sizeof(candidate));
    if (!src && is_import && !is_abs) {
        char exe_path[1024], exe_dir[1024];
#ifdef _WIN32
        if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) > 0) {
            extract_dir(exe_path, exe_dir, sizeof(exe_dir));
            src = try_open_module(exe_dir, raw_path, candidate, sizeof(candidate));
        }
#else
        ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (n > 0) {
            exe_path[n] = '\0';
            extract_dir(exe_path, exe_dir, sizeof(exe_dir));
            src = try_open_module(exe_dir, raw_path, candidate, sizeof(candidate));
        }
#endif
    }
    if (!src) {
        if (is_import) {
            fatal(import_pos, "cannot open imported module '%s'", raw_path);
        } else {
            die("cannot open '%s'", raw_path);
        }
    }

#ifdef _WIN32
    char full_buf[1024];
    const char *canon_str = _fullpath(full_buf, candidate, sizeof(full_buf)) ? full_buf : candidate;
#else
    char full_buf[PATH_MAX];
    const char *canon_str = realpath(candidate, full_buf) ? full_buf : candidate;
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

static char *default_output_path(const char *input, TargetOs target, bool emit_ir) {
    const char *slash1 = strrchr(input, '/');
    const char *slash2 = strrchr(input, '\\');
    const char *base = input;
    if (slash1 && slash1 + 1 > base) base = slash1 + 1;
    if (slash2 && slash2 + 1 > base) base = slash2 + 1;

    const char *dot = strrchr(base, '.');
    size_t stem_len = dot ? (size_t)(dot - input) : strlen(input);
    const char *ext = emit_ir ? ".ll" : (target == TARGET_WINDOWS_X64 ? ".exe" : "");
    size_t ext_len = strlen(ext);
    if (ext_len == 0 && !dot) {
        ext = ".out";
        ext_len = 4;
    }
    char *out = xmalloc(stem_len + ext_len + 1);
    memcpy(out, input, stem_len);
    memcpy(out + stem_len, ext, ext_len + 1);
    return out;
}

static void print_version(void) {
#ifdef _WIN32
    printf("sbc %s (Antimony, Windows x64)\n", SB_VERSION);
#elif defined(__linux__)
    printf("sbc %s (Antimony, Linux x64)\n", SB_VERSION);
#else
    printf("sbc %s (Antimony, POSIX x64)\n", SB_VERSION);
#endif
}

static void usage(int exit_code) {
    FILE *out = (exit_code == 0) ? stdout : stderr;
    fprintf(out,
        "Antimony compiler (sbc %s) — Dual-Backend (Native x64 & Optimizing/LLVM)\n"
        "\n"
        "Usage:\n"
        "  sbc [build] <file.sb> [-o <out>] [--target windows-x64|linux-x64] [--release] [--llvm | -O3] [--emit-llvm] [--run]\n"
        "  sbc run <file.sb> [--release] [--llvm | -O3]\n"
        "  sbc test <file.sb> [--filter <pat> | -f <pat>] [--release] [--llvm | -O3]\n"
        "  sbc check [--lib] [--target windows-x64|linux-x64] <file.sb>\n"
        "  sbc fmt [-w | --write] [--check] [--stdin | <file.sb>...]\n"
        "  sbc --version | --help\n",
        SB_VERSION);
    exit(exit_code);
}

static bool text_eq_normalized(const char *raw, const char *formatted) {
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
    TargetOs target = default_host_target();
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--lib") == 0) {
            allow_lib = true;
        } else if (strcmp(argv[i], "--target") == 0) {
            if (++i >= argc) usage(1);
            target = parse_target_os(argv[i]);
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

    g_load_target = target;
    NodeVec decls = {0};
    Pos root_pos = {1, 1, 0};
    load_module_recursive(input, "", false, root_pos, &decls);
    (void)check_ex(decls, !allow_lib);
    return 0;
}

static int run_test_cmd(int argc, char **argv) {
    const char *input = NULL;
    const char *filter = NULL;
    bool release = false;
    bool use_llvm = false;
    TargetOs target = default_host_target();

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--filter") == 0 || strcmp(argv[i], "-f") == 0) {
            if (++i >= argc) usage(1);
            filter = argv[i];
        } else if (strcmp(argv[i], "--release") == 0 || strcmp(argv[i], "-O") == 0 || strcmp(argv[i], "-O2") == 0) {
            release = true;
        } else if (strcmp(argv[i], "--llvm") == 0 || strcmp(argv[i], "-O3") == 0 || strcmp(argv[i], "--opt") == 0) {
            use_llvm = true;
            release = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(0);
        } else if (argv[i][0] == '-') {
            die("unknown option '%s' for 'sbc test'", argv[i]);
        } else if (!input) {
            input = argv[i];
        } else {
            usage(1);
        }
    }
    if (!input) usage(1);
    if (target == TARGET_LINUX_X64) use_llvm = true;

    double t0 = now_ms();

    g_load_target = target;
    NodeVec decls = {0};
    Pos root_pos = {1, 1, 0};
    load_module_recursive(input, "", false, root_pos, &decls);
    const int root_file_id = 0;

    StrVec tests = {0};
    for (int i = 0; i < decls.len; i++) {
        Node *d = decls.items[i];
        if (d->kind != N_FN || d->is_extern || d->name2 || d->pos.file_id != root_file_id) {
            continue;
        }
        if (strcmp(d->name, "main") == 0) {
            d->name = intern("__sb_test_user_main", 19);
            continue;
        }
        if (strncmp(d->name, "test_", 5) == 0) {
            if (d->tparams.len > 0 || d->list.len != 0 || d->type_name != NULL) {
                fatal(d->pos, "test function '%s' must have signature 'fn %s()' (no parameters or return type)",
                      d->name, d->name);
            }
            if (!filter || strstr(d->name, filter) != NULL) {
                vec_push(tests, d->name);
            }
        }
    }

    if (tests.len == 0) {
        (void)check_ex(decls, false);
        printf("running 0 tests\n\ntest result: ok. 0 passed; 0 failed\n");
        free(tests.items);
        return 0;
    }

    Buf harness = {0};
    char line[512];
    int n = snprintf(line, sizeof(line),
                     "fn main() {\n    print(\"running %d %s\")\n",
                     tests.len, tests.len == 1 ? "test" : "tests");
    buf_bytes(&harness, line, (size_t)n);
    for (int i = 0; i < tests.len; i++) {
        n = snprintf(line, sizeof(line),
                     "    %s()\n    print(\"test %s ... ok\")\n",
                     tests.items[i], tests.items[i]);
        buf_bytes(&harness, line, (size_t)n);
    }
    n = snprintf(line, sizeof(line),
                 "    print(\"\")\n    print(\"test result: ok. %d passed; 0 failed\")\n}\n",
                 tests.len);
    buf_bytes(&harness, line, (size_t)n);
    buf_u8(&harness, '\0');

    int saved_file_id = g_file_id;
    const char *saved_filename = g_filename;
    const char *saved_source = g_source;
    g_file_id = root_file_id;
    g_filename = input;
    g_source = (const char *)harness.data;

    int ntoks = 0;
    Token *toks = lex((const char *)harness.data, &ntoks);
    NodeVec hdecls = parse(toks, ntoks);
    free(toks);
    for (int i = 0; i < hdecls.len; i++) {
        vec_push(decls, hdecls.items[i]);
    }
    free(hdecls.items);

    g_file_id = saved_file_id;
    g_filename = saved_filename;
    g_source = saved_source;

    Program prog = check(decls);

    char tmp_exe[1024];
#ifdef _WIN32
    char tmp_dir[512];
    DWORD tlen = GetTempPathA((DWORD)sizeof(tmp_dir), tmp_dir);
    if (tlen > 0 && tlen < sizeof(tmp_dir)) {
        snprintf(tmp_exe, sizeof(tmp_exe), "%ssbc_test_%lu.exe", tmp_dir, (unsigned long)GetCurrentProcessId());
    } else {
        snprintf(tmp_exe, sizeof(tmp_exe), "sbc_test_%lu.exe", (unsigned long)GetCurrentProcessId());
    }
#else
    snprintf(tmp_exe, sizeof(tmp_exe), "/tmp/sbc_test_%ld", (long)getpid());
#endif

    if (use_llvm) {
        if (!compile_opt_backend(&prog, decls, tmp_exe, release, target)) {
            free(tests.items);
            free(harness.data);
            die("optimizing backend failed for test harness '%s'", input);
        }
    } else {
        Image img = codegen(&prog, release);
        write_pe(tmp_exe, &img);
    }

    double t1 = now_ms();
    int rc = run_executable_path(tmp_exe);
    remove(tmp_exe);
    double t2 = now_ms();

    double comp_ms = t1 - t0;
    double run_ms  = t2 - t1;
    if (rc != 0) {
        fprintf(stderr, "\ntest result: FAILED (exit code %d, %.2f ms)\n", rc, run_ms);
    } else {
        fprintf(stderr, "finished in %.2f ms (compile %.2f ms, run %.2f ms, %s%s)\n",
                comp_ms + run_ms, comp_ms, run_ms,
                release ? "release" : "debug",
                use_llvm ? ", llvm" : "");
    }

    free(tests.items);
    free(harness.data);
    return rc == 0 ? 0 : 1;
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
    if (strcmp(argv[1], "test") == 0) {
        return run_test_cmd(argc, argv);
    }

    const char *input = NULL;
    const char *output = NULL;
    bool release = false;
    bool use_llvm = false;
    bool emit_ir = false;
    bool run_after = false;
    TargetOs target = default_host_target();

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
        } else if (strcmp(argv[i], "--target") == 0) {
            if (++i >= argc) usage(1);
            target = parse_target_os(argv[i]);
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
    if (!output) output = default_output_path(input, target, emit_ir);
    if (target == TARGET_LINUX_X64 && !emit_ir) {
        use_llvm = true;
    }

    double t0 = now_ms();

    g_load_target = target;
    NodeVec decls = {0};
    Pos root_pos = {1, 1, 0};
    load_module_recursive(input, "", false, root_pos, &decls);
    Program prog = check(decls);

    if (emit_ir) {
        if (!emit_llvm_ir(&prog, decls, output, release, target)) {
            die("failed to write LLVM IR to '%s'", output);
        }
    } else if (use_llvm) {
        if (!compile_opt_backend(&prog, decls, output, release, target)) {
            die("optimizing backend failed for '%s'", output);
        }
    } else {
        Image img = codegen(&prog, release);
        write_pe(output, &img);
    }

    double ms = now_ms() - t0;
    fprintf(stderr, "compiled %s -> %s (%.2f ms, %s, %s%s)\n",
            input, output, ms,
            target == TARGET_LINUX_X64 ? "linux-x64" : "windows-x64",
            release ? "release" : "debug",
            use_llvm ? ", llvm" : (emit_ir ? ", llvm-ir" : ""));

    if (run_after) {
        return run_executable_path(output);
    }
    return 0;
}
