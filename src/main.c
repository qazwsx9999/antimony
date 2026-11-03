// main.c - CLI driver for the Antimony compiler (sbc).
//
// Usage:
//   sbc <input.sb> [-o <output.exe>] [--release] [--run]
#include "sb.h"

#ifdef _WIN32
#include <windows.h>
#endif

static char *read_entire_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) die("cannot open '%s'", path);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) die("cannot read '%s'", path);
    char *buf = xmalloc((size_t)sz + 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        die("failed to read '%s'", path);
    }
    fclose(fp);
    buf[sz] = '\0';
    return buf;
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

    g_filename = input;
    g_source = read_entire_file(input);

    int ntoks = 0;
    Token *toks = lex(g_source, &ntoks);
    NodeVec decls = parse(toks, ntoks);
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
