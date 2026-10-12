#define _POSIX_C_SOURCE 200809L
#include "bcxx.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum { LANG_AUTO, LANG_C, LANG_CXX, LANG_OBJECT } Language;
typedef struct { const char *path; Language language; } Input;
typedef struct {
    Arena arena;
    Input *inputs;
    size_t input_count, input_cap;
    Strings macros, includes, quote_includes, system_includes, after_includes;
    Strings forced, imacros, backend, link_order, targets, quoted_targets;
    const char *output, *depfile, *tcc, *query;
    int preprocess, compile, emit_c, save_temps, no_markers, dump_macros;
    int deps, deps_only, user_deps, phony_deps, nostdinc, undef, verbose, warnings;
    int strict_ansi, warnings_as_errors;
} Options;

static Strings temporaries;
static char temporary_directory[256];

static void cleanup(void)
{
    size_t i;
    for (i = 0; i < temporaries.len; i++) {
        unlink(temporaries.data[i]);
        free((void *)temporaries.data[i]);
    }
    strings_free(&temporaries);
    if (*temporary_directory) rmdir(temporary_directory);
}

static void interrupted(int signum)
{
    size_t i;
    /* Only async-signal-safe operations here. */
    for (i = 0; i < temporaries.len; i++) unlink(temporaries.data[i]);
    if (*temporary_directory) rmdir(temporary_directory);
    _exit(128 + signum);
}

static const char *temporary(Options *o, const char *suffix)
{
    char name[512];
    char *copy;
    if (!*temporary_directory) {
        const char *root = getenv("TMPDIR");
        if (!root || !*root) root = "/tmp";
        if (snprintf(temporary_directory, sizeof(temporary_directory), "%s/bcxx-XXXXXX", root) >= (int)sizeof(temporary_directory))
            fatal("TMPDIR path is too long");
        if (!mkdtemp(temporary_directory)) fatal("cannot create temporary directory: %s", strerror(errno));
    }
    snprintf(name, sizeof(name), "%s/%zu%s", temporary_directory, temporaries.len, suffix);
    copy = malloc(strlen(name) + 1);
    if (!copy) fatal("out of memory");
    strcpy(copy, name);
    strings_push(&temporaries, copy);
    return copy_string(&o->arena, name);
}

static void show_command(const Strings *argv)
{
    size_t i;
    fputs("bcxx: executing", stderr);
    for (i = 0; i < argv->len; i++) {
        const char *s = argv->data[i];
        fputs(" '", stderr);
        while (*s) { if (*s == '\'') fputs("'\\''", stderr); else fputc(*s, stderr); s++; }
        fputc('\'', stderr);
    }
    fputc('\n', stderr);
}

static int run(const Strings *command, int output_fd, int verbose)
{
    char **argv = calloc(command->len + 1, sizeof(*argv));
    pid_t child;
    int status;
    size_t i;
    if (!argv) fatal("out of memory");
    for (i = 0; i < command->len; i++) argv[i] = (char *)command->data[i];
    if (verbose) show_command(command);
    child = fork();
    if (child < 0) fatal("fork failed: %s", strerror(errno));
    if (!child) {
        if (output_fd >= 0 && dup2(output_fd, STDOUT_FILENO) < 0) _exit(126);
        if (output_fd > STDERR_FILENO) close(output_fd);
        execvp(argv[0], argv);
        fprintf(stderr, "bcxx: error: cannot execute '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    free(argv);
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) fatal("waitpid failed: %s", strerror(errno));
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return WEXITSTATUS(status);
}

static const char *capture(Options *o, const char *option)
{
    Strings command = {0};
    const char *path = temporary(o, ".query");
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600), result;
    char *text;
    if (fd < 0) fatal("cannot open temporary output: %s", strerror(errno));
    strings_push(&command, o->tcc);
    strings_push(&command, option);
    result = run(&command, fd, 0);
    close(fd);
    strings_free(&command);
    if (result) fatal("TCC query '%s' failed (exit %d)", option, result);
    text = read_file(&o->arena, path, NULL);
    if (!text) fatal("cannot read TCC query output");
    return text;
}

static void input_push(Options *o, const char *path, Language language)
{
    if (o->input_count == o->input_cap) {
        o->input_cap = o->input_cap ? o->input_cap * 2 : 16;
        o->inputs = realloc(o->inputs, o->input_cap * sizeof(*o->inputs));
        if (!o->inputs) fatal("out of memory");
    }
    o->inputs[o->input_count].path = path;
    o->inputs[o->input_count++].language = language;
    /* Object/link arguments stay in their original order around source files. */
    strings_push(&o->link_order, path);
}

static Language language_of(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!strcmp(path, "-")) return LANG_AUTO;
    if (!ext) return LANG_OBJECT;
    if (!strcmp(ext, ".c") || !strcmp(ext, ".i")) return LANG_C;
    if (!strcmp(ext, ".cc") || !strcmp(ext, ".cp") || !strcmp(ext, ".cpp") ||
        !strcmp(ext, ".cxx") || !strcmp(ext, ".C") || !strcmp(ext, ".ii")) return LANG_CXX;
    return LANG_OBJECT;
}

static const char *argument(int argc, char **argv, int *i, const char *name)
{
    if (*i + 1 >= argc) fatal("option '%s' requires an argument", name);
    return argv[++*i];
}

static void help(void)
{
    puts("bcxx " BCXX_VERSION " (driver/preprocessor milestone)\n"
         "Usage: bcxx [options] file...\n"
         "  -E [-P] [-dM]           Preprocess C or C++ internally\n"
         "  -c / -o FILE             Compile C objects / choose output\n"
         "  -x c|c++|none           Select input language\n"
         "  -I DIR -iquote DIR       Add include search paths\n"
         "  -isystem DIR -idirafter DIR -nostdinc\n"
         "  -D NAME[=VALUE] -U NAME  Define/undefine macros in order\n"
         "  -include FILE -imacros FILE -undef\n"
         "  -M -MM -MD -MMD -MF FILE -MT TARGET -MQ TARGET -MP\n"
         "  -L DIR -l LIB -Wl,ARGS   Pass native link arguments to TCC\n"
         "  --emit-c --save-temps    Save C / retain preprocessed inputs\n"
         "  -O0/-O1/-O2/-O3/-Os/-Og/-Ofast/-Oz (accepted, no effect)\n"
         "  --version -dumpmachine -dumpversion -dumpfullversion\n"
         "BCXX_TCC selects the TCC executable (one path, no shell expansion).\n"
         "C++ translation and bootstrap headers are not implemented yet.");
}

static int ignored_option(const char *arg)
{
    static const char *const options[] = {
        "-O", "-O0", "-O1", "-O2", "-O3", "-Os", "-Og", "-Ofast", "-Oz",
        "-fno-exceptions", "-fno-rtti", "-fno-strict-aliasing", "-nostdinc++",
        "-fno-omit-frame-pointer", "-pipe", NULL
    };
    size_t i;
    for (i = 0; options[i]; i++) if (!strcmp(arg, options[i])) return 1;
    return 0;
}

static void parse_options(Options *o, int argc, char **argv)
{
    int i, positional = 0;
    Language language = LANG_AUTO;
    memset(o, 0, sizeof(*o));
    o->tcc = getenv("BCXX_TCC");
    if (!o->tcc || !*o->tcc) o->tcc = "tcc";
    o->warnings = 1;
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i], *operand;
        if (!positional && !strcmp(arg, "--")) { positional = 1; continue; }
        if (positional || arg[0] != '-' || !strcmp(arg, "-")) {
            input_push(o, arg, language == LANG_AUTO ? language_of(arg) : language);
        } else if (!strcmp(arg, "--help")) { help(); exit(0); }
        else if (!strcmp(arg, "--version") || !strcmp(arg, "-dumpmachine") ||
                 !strcmp(arg, "-dumpversion") || !strcmp(arg, "-dumpfullversion") ||
                 !strcmp(arg, "-print-search-dirs") || !strncmp(arg, "-print-file-name=", 17) ||
                 !strncmp(arg, "-print-prog-name=", 17)) o->query = arg;
        else if (!strcmp(arg, "-v")) o->verbose = 1;
        else if (!strcmp(arg, "-E")) o->preprocess = 1;
        else if (!strcmp(arg, "-c")) o->compile = 1;
        else if (!strcmp(arg, "-P")) o->no_markers = 1;
        else if (!strcmp(arg, "-dM")) o->dump_macros = 1;
        else if (!strcmp(arg, "--emit-c")) o->emit_c = 1;
        else if (!strcmp(arg, "--save-temps") || !strcmp(arg, "-save-temps")) o->save_temps = 1;
        else if (!strcmp(arg, "-nostdinc")) o->nostdinc = 1;
        else if (!strcmp(arg, "-undef")) o->undef = 1;
        else if (!strcmp(arg, "-w")) { o->warnings = 0; strings_push(&o->backend, arg); }
        else if (!strcmp(arg, "-Werror")) { o->warnings_as_errors = 1; strings_push(&o->backend, arg); }
        else if (!strcmp(arg, "-o")) o->output = argument(argc, argv, &i, arg);
        else if (!strncmp(arg, "-o", 2)) o->output = arg + 2;
        else if (!strcmp(arg, "-x")) {
            operand = argument(argc, argv, &i, arg);
            if (!strcmp(operand, "c") || !strcmp(operand, "c-header")) language = LANG_C;
            else if (!strcmp(operand, "c++") || !strcmp(operand, "c++-header")) language = LANG_CXX;
            else if (!strcmp(operand, "none")) language = LANG_AUTO;
            else fatal("unsupported language '%s'", operand);
        } else if (!strncmp(arg, "-std=", 5)) {
            operand = arg + 5;
            if (!strcmp(operand, "c99") || !strcmp(operand, "c++14")) o->strict_ansi = 1;
            else if (!strcmp(operand, "gnu99") || !strcmp(operand, "gnu++14")) o->strict_ansi = 0;
            else fatal("unsupported language standard '%s' (supported: C99, C++14)", operand);
        } else if (!strncmp(arg, "-D", 2) || !strncmp(arg, "-U", 2)) {
            Buffer b = {0};
            operand = arg[2] ? arg + 2 : argument(argc, argv, &i, arg);
            buffer_char(&b, arg[1]); buffer_text(&b, operand);
            strings_push(&o->macros, copy_string(&o->arena, b.data));
            buffer_free(&b);
        } else if (!strncmp(arg, "-I", 2)) {
            operand = arg[2] ? arg + 2 : argument(argc, argv, &i, arg);
            if (!strcmp(operand, "-")) fatal("-I- is unsupported; use -iquote");
            strings_push(&o->includes, operand);
        } else if (!strcmp(arg, "-iquote")) strings_push(&o->quote_includes, argument(argc, argv, &i, arg));
        else if (!strcmp(arg, "-isystem")) strings_push(&o->system_includes, argument(argc, argv, &i, arg));
        else if (!strcmp(arg, "-idirafter")) strings_push(&o->after_includes, argument(argc, argv, &i, arg));
        else if (!strcmp(arg, "-include")) strings_push(&o->forced, argument(argc, argv, &i, arg));
        else if (!strcmp(arg, "-imacros")) strings_push(&o->imacros, argument(argc, argv, &i, arg));
        else if (!strcmp(arg, "-M") || !strcmp(arg, "-MM") || !strcmp(arg, "-MD") || !strcmp(arg, "-MMD")) {
            o->deps = 1;
            o->deps_only = !strcmp(arg, "-M") || !strcmp(arg, "-MM");
            o->user_deps = !strcmp(arg, "-MM") || !strcmp(arg, "-MMD");
        } else if (!strcmp(arg, "-MF")) o->depfile = argument(argc, argv, &i, arg);
        else if (!strcmp(arg, "-MT")) strings_push(&o->targets, argument(argc, argv, &i, arg));
        else if (!strcmp(arg, "-MQ")) strings_push(&o->quoted_targets, argument(argc, argv, &i, arg));
        else if (!strcmp(arg, "-MP")) o->phony_deps = 1;
        else if (!strncmp(arg, "-MF", 3)) o->depfile = arg + 3;
        else if (!strncmp(arg, "-MT", 3)) strings_push(&o->targets, arg + 3);
        else if (!strncmp(arg, "-MQ", 3)) strings_push(&o->quoted_targets, arg + 3);
        else if (!strcmp(arg, "-L") || !strcmp(arg, "-l")) {
            strings_push(&o->link_order, arg);
            strings_push(&o->link_order, argument(argc, argv, &i, arg));
        } else if (!strncmp(arg, "-L", 2) || !strncmp(arg, "-l", 2) || !strncmp(arg, "-Wl,", 4) ||
                   !strcmp(arg, "-static") || !strcmp(arg, "-shared") || !strcmp(arg, "-rdynamic") ||
                   !strcmp(arg, "-nostdlib") || !strcmp(arg, "-r")) strings_push(&o->link_order, arg);
        else if (!strcmp(arg, "-g") || !strcmp(arg, "-Wall") || !strcmp(arg, "-Wextra") ||
                 !strcmp(arg, "-fPIC") || !strcmp(arg, "-fpic")) strings_push(&o->backend, arg);
        else if (ignored_option(arg)) { }
        else fatal("unsupported option '%s'", arg);
    }
    if (o->dump_macros && !o->preprocess && !o->deps_only) fatal("-dM requires -E or -M/-MM");
    if (o->emit_c && (o->preprocess || o->compile || o->deps_only)) fatal("--emit-c cannot be combined with -E, -c, or -M/-MM");
    if (o->deps_only && o->compile) fatal("-M/-MM cannot be combined with -c");
}

static void copy_paths(Strings *to, const Strings *from)
{
    size_t i;
    for (i = 0; i < from->len; i++) strings_push(to, from->data[i]);
}

static void environment_paths(Preprocessor *pp, Strings *to, const char *name)
{
    const char *s = getenv(name), *start;
    if (!s) return;
    start = s;
    for (;;) {
        if (*s == ':' || !*s) {
            strings_push(to, s == start ? "." : copy_n(&pp->arena, start, (size_t)(s - start)));
            if (!*s) break;
            start = ++s;
        } else s++;
    }
}

static void standard_paths(Options *o, Preprocessor *pp)
{
    const char *search = capture(o, "-print-search-dirs"), *s = search;
    int includes = 0;
    while (*s) {
        const char *end = strchr(s, '\n');
        size_t width = end ? (size_t)(end - s) : strlen(s);
        if (width == 8 && !strncmp(s, "include:", width)) includes = 1;
        else if (includes && width && (s[0] == ' ' || s[0] == '\t')) {
            while (width && (*s == ' ' || *s == '\t')) { s++; width--; }
            if (width) strings_push(&pp->system_includes, copy_n(&pp->arena, s, width));
        } else if (includes) break;
        if (!end) break;
        s = end + 1;
    }
    if (!includes) fatal("cannot determine TCC system include paths from -print-search-dirs");
}

static FILE *open_output(const char *path)
{
    FILE *f;
    if (!path || !strcmp(path, "-")) return stdout;
    f = fopen(path, "w");
    if (!f) fatal("cannot write '%s': %s", path, strerror(errno));
    return f;
}

static int same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    if (!strcmp(a, b)) return 1;
    return !stat(a, &sa) && !stat(b, &sb) && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void protect_inputs(Options *o, const char *output)
{
    size_t i;
    if (!output || !strcmp(output, "-")) return;
    for (i = 0; i < o->input_count; i++) {
        if (same_file(output, o->inputs[i].path)) fatal("output '%s' would overwrite input '%s'", output, o->inputs[i].path);
    }
}

static void protect_dependencies(Preprocessor *pp, const char *output)
{
    size_t i;
    if (!output || !strcmp(output, "-")) return;
    for (i = 0; i < pp->dependency_count; i++) {
        if (same_file(output, pp->dependencies[i].path)) fatal("output '%s' would overwrite source '%s'", output, pp->dependencies[i].path);
    }
}

static void close_output(FILE *f)
{
    if (fflush(f) || ferror(f)) fatal("cannot write output: %s", strerror(errno));
    if (f != stdout && fclose(f)) fatal("cannot close output: %s", strerror(errno));
}

static int preprocess_input(Options *o, Input input, const char *output, const char *object)
{
    Preprocessor pp;
    Tokens tokens = {0};
    Strings targets = {0};
    FILE *f;
    size_t i;
    int ok = 1;
    pp_init(&pp, input.language == LANG_CXX, o->undef);
    pp.warnings = o->warnings;
    pp.warnings_as_errors = o->warnings_as_errors;
    if (o->strict_ansi) pp_command_define(&pp, "__STRICT_ANSI__=1");
    pp.base_file = !strcmp(input.path, "-") ? "<stdin>" : input.path;
    copy_paths(&pp.includes, &o->includes);
    copy_paths(&pp.quote_includes, &o->quote_includes);
    copy_paths(&pp.system_includes, &o->system_includes);
    copy_paths(&pp.after_includes, &o->after_includes);
    environment_paths(&pp, &pp.includes, "CPATH");
    environment_paths(&pp, &pp.system_includes, input.language == LANG_CXX ? "CPLUS_INCLUDE_PATH" : "C_INCLUDE_PATH");
    if (!o->nostdinc) standard_paths(o, &pp);
    for (i = 0; i < o->macros.len && ok; i++) {
        const char *s = o->macros.data[i];
        if (*s == 'D') ok = pp_command_define(&pp, s + 1);
        else {
            Tokens name = {0};
            ok = lex(&pp.arena, s + 1, "<command-line>", pp.cplusplus, &name);
            if (!ok || name.len != 1 || name.data[0].kind != TOK_IDENT) {
                error_at((Location){"<command-line>", 1, 1}, "-U requires a macro name"); ok = 0;
            } else pp_undef(&pp, s + 1);
            tokens_free(&name);
        }
    }
    for (i = 0; i < o->imacros.len && ok; i++) {
        Tokens discarded = {0};
        ok = pp_forced_include(&pp, o->imacros.data[i], &discarded);
        tokens_free(&discarded);
    }
    for (i = 0; i < o->forced.len && ok; i++) ok = pp_forced_include(&pp, o->forced.data[i], &tokens);
    if (ok) ok = pp_file(&pp, input.path, 0, -1, &tokens);
    if (ok) {
        protect_dependencies(&pp, o->output);
        if (!o->preprocess && !o->deps_only && !o->emit_c) protect_dependencies(&pp, object);
    }
    if (ok && !o->deps_only) {
        protect_dependencies(&pp, output);
        f = open_output(output);
        if (o->dump_macros) pp_dump_macros(&pp, f);
        else emit_tokens(f, &tokens, !o->no_markers);
        close_output(f);
    }
    if (ok && o->deps) {
        const char *depfile = o->depfile;
        if (!depfile && o->deps_only) depfile = o->output;
        if (!depfile && !o->deps_only) depfile = replace_suffix(&o->arena, object, ".d", 0);
        protect_inputs(o, depfile);
        protect_dependencies(&pp, depfile);
        if (!o->deps_only && depfile && output && same_file(depfile, output))
            fatal("dependency output and preprocessed output must use different files");
        f = open_output(depfile);
        copy_paths(&targets, &o->targets);
        if (!targets.len && !o->quoted_targets.len) {
            Strings quoted = {0};
            strings_push(&quoted, object);
            pp_dependencies(&pp, f, &targets, &quoted, o->user_deps, o->phony_deps);
            strings_free(&quoted);
        } else pp_dependencies(&pp, f, &targets, &o->quoted_targets, o->user_deps, o->phony_deps);
        close_output(f);
    }
    strings_free(&targets);
    tokens_free(&tokens);
    pp_destroy(&pp);
    return ok ? 0 : 1;
}

static int backend_compile(Options *o, const char *source, const char *object)
{
    Strings command = {0};
    int result;
    strings_push(&command, o->tcc);
    strings_push(&command, "-std=c99");
    copy_paths(&command, &o->backend);
    strings_push(&command, "-c");
    strings_push(&command, "-o");
    strings_push(&command, object);
    strings_push(&command, "-x");
    strings_push(&command, "c");
    strings_push(&command, source);
    result = run(&command, -1, o->verbose);
    strings_free(&command);
    return result;
}

static int query(Options *o, const char *program)
{
    if (!strcmp(o->query, "--version")) puts("bcxx " BCXX_VERSION " (C99 driver and internal preprocessor; C++ translation pending)");
    else if (!strcmp(o->query, "-dumpmachine")) puts(BCXX_TARGET);
    else if (!strcmp(o->query, "-dumpversion")) puts("0.1");
    else if (!strcmp(o->query, "-dumpfullversion")) puts(BCXX_VERSION);
    else if (!strncmp(o->query, "-print-prog-name=", 17)) {
        const char *name = o->query + 17;
        if (!strcmp(name, "cc1plus")) puts(program);
        else if (!strcmp(name, "cc1") || !strcmp(name, "tcc")) puts(o->tcc);
        else puts(name);
    } else if (!strcmp(o->query, "-print-search-dirs")) fputs(capture(o, o->query), stdout);
    else if (!strncmp(o->query, "-print-file-name=", 17)) {
        const char *name = o->query + 17;
        const char *s = capture(o, "-print-search-dirs");
        int search_paths = 0;
        while (*s) {
            const char *end = strchr(s, '\n'), *directory = s;
            size_t width = end ? (size_t)(end - s) : strlen(s);
            if (!strncmp(s, "libraries:", 10) || !strncmp(s, "libtcc1:", 8) || !strncmp(s, "crt:", 4)) search_paths = 1;
            else if (!strncmp(s, "install:", 8)) { directory += 8; width -= 8; }
            else if (*s != ' ' && *s != '\t') search_paths = 0;
            if (directory != s || (search_paths && (*s == ' ' || *s == '\t'))) {
                const char *path;
                while (width && (*directory == ' ' || *directory == '\t')) { directory++; width--; }
                directory = copy_n(&o->arena, directory, width);
                path = path_join(&o->arena, directory, name);
                if (access(path, R_OK) == 0) { puts(path); return 0; }
            }
            if (!end) break;
            s = end + 1;
        }
        puts(name);
    }
    return 0;
}

static void options_destroy(Options *o)
{
    strings_free(&o->macros); strings_free(&o->includes);
    strings_free(&o->quote_includes); strings_free(&o->system_includes);
    strings_free(&o->after_includes); strings_free(&o->forced);
    strings_free(&o->imacros); strings_free(&o->backend);
    strings_free(&o->link_order); strings_free(&o->targets);
    strings_free(&o->quoted_targets);
    free(o->inputs);
    arena_destroy(&o->arena);
}

int main(int argc, char **argv)
{
    Options options, *o = &options;
    Strings objects = {0}, link = {0};
    size_t i, source_count = 0, input_index = 0;
    int result = 0;
    diagnostic_cleanup(cleanup);
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);
    parse_options(o, argc, argv);
    if (o->query) { result = query(o, argv[0]); goto done; }
    if (o->verbose) fprintf(stderr, "bcxx %s; target %s; backend %s\n", BCXX_VERSION, BCXX_TARGET, o->tcc);
    if (!o->input_count) {
        if (!o->verbose) fatal("no input files");
        goto done;
    }
    protect_inputs(o, o->output);
    protect_inputs(o, o->depfile);
    for (i = 0; i < o->input_count; i++) {
        Input input = o->inputs[i];
        if (input.language == LANG_AUTO) fatal("stdin requires -x c or -x c++");
        if (input.language != LANG_OBJECT) source_count++;
    }
    if (source_count > 1 && o->output && (o->compile || o->preprocess || o->deps_only || o->emit_c))
        fatal("cannot specify -o with multiple inputs in this mode");
    if (source_count > 1 && o->depfile) fatal("-MF with multiple source inputs is ambiguous; compile each input separately");
    if (!source_count && (o->compile || o->preprocess || o->deps_only || o->emit_c)) fatal("this mode requires a source input");
    /* Reject C++ translation before creating outputs or invoking a backend. */
    for (i = 0; i < o->input_count; i++) {
        if (o->inputs[i].language == LANG_CXX && !o->preprocess && !o->deps_only) {
            error_at((Location){o->inputs[i].path, 1, 1},
                     "C++ translation is not implemented in the driver/preprocessor milestone; use -E");
            result = 1; goto done;
        }
    }
    for (i = 0; i < o->input_count; i++) {
        Input input = o->inputs[i];
        const char *object, *preprocessed;
        if (input.language == LANG_OBJECT) {
            if (o->compile || o->preprocess || o->deps_only || o->emit_c) fatal("non-source input '%s' in source-only mode", input.path);
            strings_push(&objects, input.path);
            continue;
        }
        object = o->compile && o->output ? o->output : replace_suffix(&o->arena, input.path, ".o", 1);
        if (o->preprocess || o->deps_only || o->emit_c) {
            const char *output = o->emit_c && !o->output ? replace_suffix(&o->arena, input.path, ".c", 1) : o->output;
            protect_inputs(o, output);
            result = preprocess_input(o, input, output, object);
        } else {
            preprocessed = o->save_temps ? replace_suffix(&o->arena, input.path, ".i", 1) : temporary(o, ".i");
            protect_inputs(o, preprocessed);
            result = preprocess_input(o, input, preprocessed, object);
            if (result) break;
            if (!o->compile) object = o->save_temps ? object : temporary(o, ".o");
            protect_inputs(o, object);
            result = backend_compile(o, preprocessed, object);
            strings_push(&objects, object);
        }
        if (result) break;
    }
    if (!result && !o->compile && !o->preprocess && !o->deps_only && !o->emit_c) {
        strings_push(&link, o->tcc);
        copy_paths(&link, &o->backend);
        strings_push(&link, "-o"); strings_push(&link, o->output ? o->output : "a.out");
        for (i = 0; i < o->link_order.len; i++) {
            if (input_index < o->input_count && o->link_order.data[i] == o->inputs[input_index].path)
                strings_push(&link, objects.data[input_index++]);
            else strings_push(&link, o->link_order.data[i]);
        }
        result = run(&link, -1, o->verbose);
    }
done:
    strings_free(&objects);
    strings_free(&link);
    options_destroy(o);
    cleanup();
    return result;
}
