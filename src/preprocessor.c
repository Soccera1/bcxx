#define _XOPEN_SOURCE 700
#include "bcxx.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct Node Node;
struct Node { Token token; Node *next; };
typedef struct { int parent, active, taken, saw_else; Location loc; } Conditional;

static unsigned macro_hash(const char *s)
{
    unsigned hash = 2166136261u;
    while (*s) { hash ^= (unsigned char)*s++; hash *= 16777619u; }
    return hash % 4096;
}

Macro *pp_macro(Preprocessor *pp, const char *name)
{
    Macro *m;
    for (m = pp->macro_table[macro_hash(name)]; m; m = m->hash_next)
        if (!strcmp(m->name, name)) return m;
    return NULL;
}

void pp_undef(Preprocessor *pp, const char *name)
{
    Macro **slot = &pp->macro_table[macro_hash(name)], **list;
    while (*slot && strcmp((*slot)->name, name)) slot = &(*slot)->hash_next;
    if (*slot) {
        Macro *m = *slot;
        *slot = m->hash_next;
        for (list = &pp->macros; *list != m; list = &(*list)->next) { }
        *list = m->next;
        strings_free(&m->parameters);
        tokens_free(&m->body);
    }
}

static int macro_equal(const Macro *a, const Macro *b)
{
    size_t i;
    if (a->function != b->function || a->variadic != b->variadic ||
        a->parameters.len != b->parameters.len || a->body.len != b->body.len) return 0;
    for (i = 0; i < a->parameters.len; i++)
        if (strcmp(a->parameters.data[i], b->parameters.data[i])) return 0;
    for (i = 0; i < a->body.len; i++)
        if (strcmp(a->body.data[i].text, b->body.data[i].text) ||
            (i && a->body.data[i].space != b->body.data[i].space)) return 0;
    return 1;
}

int pp_define(Preprocessor *pp, const Tokens *tokens, Location loc)
{
    Macro *m, *previous;
    size_t i = 1;
    unsigned h;
    if (!tokens->len || tokens->data[0].kind != TOK_IDENT) {
        error_at(loc, "expected identifier in #define"); return 0;
    }
    m = allocate(&pp->arena, sizeof(*m));
    m->name = tokens->data[0].text;
    if (!strcmp(m->name, "defined")) { error_at(loc, "cannot define 'defined'"); return 0; }
    if (i < tokens->len && token_is(tokens->data[i], "(") && !tokens->data[i].space) {
        m->function = 1;
        i++;
        while (i < tokens->len && !token_is(tokens->data[i], ")")) {
            Token t = tokens->data[i++];
            const char *parameter;
            if (token_is(t, "...")) { parameter = "__VA_ARGS__"; m->variadic = 1; }
            else if (t.kind == TOK_IDENT) {
                parameter = t.text;
                if (i < tokens->len && token_is(tokens->data[i], "...")) { m->variadic = 1; i++; }
            } else { error_at(t.loc, "expected macro parameter"); goto fail; }
            if (strings_contains(&m->parameters, parameter)) {
                error_at(t.loc, "duplicate macro parameter '%s'", parameter); goto fail;
            }
            strings_push(&m->parameters, parameter);
            if (i < tokens->len && token_is(tokens->data[i], ")")) break;
            if (m->variadic || i >= tokens->len || !token_is(tokens->data[i++], ",")) {
                error_at(t.loc, "expected ')' or ',' in macro parameters"); goto fail;
            }
            if (i < tokens->len && token_is(tokens->data[i], ")")) {
                error_at(t.loc, "trailing comma in macro parameters"); goto fail;
            }
        }
        if (i >= tokens->len || !token_is(tokens->data[i], ")")) {
            error_at(loc, "unterminated macro parameter list"); goto fail;
        }
        i++;
    }
    for (; i < tokens->len; i++) tokens_push(&m->body, tokens->data[i]);
    for (i = 0; i < m->body.len; i++) {
        Token t = m->body.data[i];
        if (token_is(t, "##") && (!i || i + 1 == m->body.len || token_is(m->body.data[i + 1], "##"))) {
            error_at(t.loc, "'##' requires tokens on both sides"); goto fail;
        }
        if (m->function && token_is(t, "#") &&
            (i + 1 == m->body.len || !strings_contains(&m->parameters, m->body.data[i + 1].text))) {
            error_at(t.loc, "'#' must be followed by a macro parameter"); goto fail;
        }
    }
    previous = pp_macro(pp, m->name);
    if (previous && !macro_equal(m, previous) && pp->warnings) {
        if (pp->warnings_as_errors) { error_at(loc, "macro '%s' redefined", m->name); goto fail; }
        warning_at(loc, "macro '%s' redefined", m->name);
    }
    pp_undef(pp, m->name);
    h = macro_hash(m->name);
    m->hash_next = pp->macro_table[h];
    pp->macro_table[h] = m;
    m->next = pp->macros;
    pp->macros = m;
    return 1;
fail:
    strings_free(&m->parameters);
    tokens_free(&m->body);
    return 0;
}

int pp_command_define(Preprocessor *pp, const char *definition)
{
    Buffer b = {0};
    Tokens tokens = {0};
    const char *eq = strchr(definition, '=');
    Location loc = {"<command-line>", 1, 1};
    int ok;
    if (eq) {
        buffer_n(&b, definition, (size_t)(eq - definition));
        buffer_char(&b, ' ');
        buffer_n(&b, eq + 1, strcspn(eq + 1, "\r\n"));
    } else { buffer_text(&b, definition); buffer_text(&b, " 1"); }
    ok = lex(&pp->arena, b.data, loc.file, pp->cplusplus, &tokens) && pp_define(pp, &tokens, loc);
    tokens_free(&tokens);
    buffer_free(&b);
    return ok;
}

static void builtin(Preprocessor *pp, const char *name)
{
    if (!pp_command_define(pp, name)) fatal("invalid internal builtin definition");
    pp->macros->builtin = 1;
}

void pp_init(Preprocessor *pp, int cplusplus, int undef_builtins)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char date[32], clock[32];
    memset(pp, 0, sizeof(*pp));
    pp->cplusplus = cplusplus;
    pp->undef_builtins = undef_builtins;
    pp->warnings = 1;
    strftime(date, sizeof(date), "\"%b %d %Y\"", tm);
    if (date[5] == '0') date[5] = ' ';
    strftime(clock, sizeof(clock), "\"%H:%M:%S\"", tm);
    pp->date = copy_string(&pp->arena, date);
    pp->time = copy_string(&pp->arena, clock);
    pp_command_define(pp, "__STDC__=1");
    pp_command_define(pp, "__STDC_HOSTED__=1");
    if (cplusplus) pp_command_define(pp, "__cplusplus=201402L");
    else pp_command_define(pp, "__STDC_VERSION__=199901L");
    if (!undef_builtins) {
        static const char *const definitions[] = {
            "__BCXX__=1", "__BCXX_MINOR__=0", "__BCXX_PATCHLEVEL__=0",
            "__BCXX_DRIVER_STAGE__=1", "__linux__=1", "__linux=1", "linux=1",
            "__unix__=1", "__unix=1", "unix=1", "__x86_64__=1", "__amd64__=1",
            "__LP64__=1", "_LP64=1", "__ELF__=1", "__CHAR_BIT__=8",
            "__SIZEOF_SHORT__=2", "__SIZEOF_INT__=4", "__SIZEOF_LONG__=8",
            "__SIZEOF_LONG_LONG__=8", "__SIZEOF_POINTER__=8", "__SIZEOF_SIZE_T__=8",
            "__SIZEOF_PTRDIFF_T__=8", "__SIZEOF_FLOAT__=4", "__SIZEOF_DOUBLE__=8",
            "__SIZE_TYPE__=unsigned long", "__PTRDIFF_TYPE__=long", "__WCHAR_TYPE__=int",
            "__WINT_TYPE__=unsigned int", "__INTMAX_TYPE__=long", "__UINTMAX_TYPE__=unsigned long",
            "__INT64_TYPE__=long", "__UINT64_TYPE__=unsigned long",
            "__ORDER_LITTLE_ENDIAN__=1234", "__ORDER_BIG_ENDIAN__=4321",
            "__BYTE_ORDER__=1234", "__FLOAT_WORD_ORDER__=1234", NULL
        };
        size_t i;
        for (i = 0; definitions[i]; i++) pp_command_define(pp, definitions[i]);
    }
    builtin(pp, "__LINE__");
    builtin(pp, "__FILE__");
    builtin(pp, "__BASE_FILE__");
    builtin(pp, "__DATE__");
    builtin(pp, "__TIME__");
    builtin(pp, "__COUNTER__");
    builtin(pp, "__INCLUDE_LEVEL__");
    builtin(pp, "__has_include(x)");
    builtin(pp, "__has_attribute(x)");
    builtin(pp, "__has_cpp_attribute(x)");
    builtin(pp, "__has_builtin(x)");
    builtin(pp, "_Pragma(x)");
}

void pp_destroy(Preprocessor *pp)
{
    Macro *m;
    for (m = pp->macros; m; m = m->next) {
        tokens_free(&m->body);
        strings_free(&m->parameters);
    }
    strings_free(&pp->includes);
    strings_free(&pp->quote_includes);
    strings_free(&pp->system_includes);
    strings_free(&pp->after_includes);
    strings_free(&pp->once_files);
    free(pp->dependencies);
    arena_destroy(&pp->arena);
}

static int hidden(Hide *h, const char *name)
{
    for (; h; h = h->next) if (!strcmp(h->name, name)) return 1;
    return 0;
}

static Hide *hide_add(Preprocessor *pp, Hide *h, const char *name)
{
    Hide *next;
    if (hidden(h, name)) return h;
    next = allocate(&pp->arena, sizeof(*next));
    next->name = name;
    next->next = h;
    return next;
}

static Hide *hide_union(Preprocessor *pp, Hide *a, Hide *b)
{
    for (; b; b = b->next) a = hide_add(pp, a, b->name);
    return a;
}

static Hide *hide_intersection(Preprocessor *pp, Hide *a, Hide *b)
{
    Hide *h = NULL;
    for (; a; a = a->next) if (hidden(b, a->name)) h = hide_add(pp, h, a->name);
    return h;
}

static Node *prepend(Preprocessor *pp, const Tokens *tokens, Node *tail)
{
    size_t i = tokens->len;
    while (i) {
        Node *n = allocate(&pp->arena, sizeof(*n));
        n->token = tokens->data[--i];
        n->next = tail;
        tail = n;
    }
    return tail;
}

static int parameter_index(const Macro *m, Token t)
{
    size_t i;
    if (t.kind != TOK_IDENT) return -1;
    for (i = 0; i < m->parameters.len; i++)
        if (!strcmp(m->parameters.data[i], t.text)) return (int)i;
    return -1;
}

static Token stringize(Preprocessor *pp, const Tokens *arg, Token origin)
{
    Buffer b = {0};
    size_t i;
    buffer_char(&b, '"');
    for (i = 0; i < arg->len; i++) {
        const char *s = arg->data[i].text;
        int escape = arg->data[i].kind == TOK_STRING || arg->data[i].kind == TOK_CHAR;
        if (i && arg->data[i].space) buffer_char(&b, ' ');
        while (*s) {
            if (escape && *s == '\n') buffer_text(&b, "\\n");
            else if (escape && *s == '\r') buffer_text(&b, "\\r");
            else {
                if (escape && (*s == '\\' || *s == '"')) buffer_char(&b, '\\');
                buffer_char(&b, *s);
            }
            s++;
        }
    }
    buffer_char(&b, '"');
    origin.kind = TOK_STRING;
    origin.text = copy_string(&pp->arena, b.data);
    buffer_free(&b);
    return origin;
}

static int paste(Preprocessor *pp, Tokens *input, Tokens *result)
{
    size_t i;
    for (i = 0; i < input->len; i++) {
        Token t = input->data[i];
        if (t.kind == TOK_PASTE) {
            Token left = result->data[--result->len], right = input->data[++i];
            if (left.kind == TOK_EMPTY) { right.space = left.space; tokens_push(result, right); }
            else if (right.kind == TOK_EMPTY) tokens_push(result, left);
            else {
                Tokens token = {0};
                Buffer b = {0};
                int ok;
                buffer_text(&b, left.text);
                buffer_text(&b, right.text);
                ok = lex(&pp->arena, b.data, t.loc.file, pp->cplusplus, &token);
                buffer_free(&b);
                if (!ok || token.len != 1) {
                    error_at(t.loc, "pasting '%s' and '%s' does not form a preprocessing token", left.text, right.text);
                    tokens_free(&token);
                    return 0;
                }
                t = token.data[0];
                t.loc = left.loc;
                t.space = left.space;
                t.hide = hide_intersection(pp, left.hide, right.hide);
                tokens_push(result, t);
                tokens_free(&token);
            }
        } else tokens_push(result, t);
    }
    return 1;
}

static int substitute(Preprocessor *pp, const Macro *m, Tokens *args,
                      Token call, Hide *hide, int omitted, Tokens *result, unsigned depth)
{
    Tokens intermediate = {0};
    Tokens *expanded = calloc(m->parameters.len ? m->parameters.len : 1, sizeof(*expanded));
    unsigned char *ready = calloc(m->parameters.len ? m->parameters.len : 1, 1);
    size_t i;
    int ok = 1;
    if (!expanded || !ready) fatal("out of memory");
    for (i = 0; i < m->body.len && ok; i++) {
        Token t = m->body.data[i];
        int index = parameter_index(m, t);
        t.loc = call.loc;
        if (m->function && token_is(t, "#")) {
            int p = parameter_index(m, m->body.data[++i]);
            tokens_push(&intermediate, stringize(pp, &args[p], t));
        } else if (m->variadic && token_is(t, ",") && i + 2 < m->body.len &&
                   token_is(m->body.data[i + 1], "##") &&
                   parameter_index(m, m->body.data[i + 2]) == (int)m->parameters.len - 1) {
            /* GNU comma elision only applies to an omitted variadic argument. */
            int p = (int)m->parameters.len - 1;
            if (!omitted) tokens_push(&intermediate, t);
            if (!ready[p]) { ok = pp_expand(pp, &args[p], &expanded[p], depth + 1); ready[p] = 1; }
            tokens_append(&intermediate, &expanded[p]);
            i += 2;
        } else if (index >= 0) {
            int raw = (i && token_is(m->body.data[i - 1], "##")) ||
                      (i + 1 < m->body.len && token_is(m->body.data[i + 1], "##"));
            const Tokens *arg;
            size_t j;
            if (!raw && !ready[index]) {
                ok = pp_expand(pp, &args[index], &expanded[index], depth + 1);
                ready[index] = 1;
            }
            arg = raw ? &args[index] : &expanded[index];
            for (j = 0; j < arg->len; j++) {
                Token at = arg->data[j];
                at.loc = call.loc;
                if (!j) at.space = t.space;
                tokens_push(&intermediate, at);
            }
            if (!arg->len && raw) { t.kind = TOK_EMPTY; t.text = ""; tokens_push(&intermediate, t); }
        } else {
            if (token_is(t, "##")) t.kind = TOK_PASTE;
            tokens_push(&intermediate, t);
        }
    }
    if (ok) ok = paste(pp, &intermediate, result);
    if (ok) {
        size_t j = 0;
        for (i = 0; i < result->len; i++) {
            Token t = result->data[i];
            if (t.kind == TOK_EMPTY) continue;
            t.hide = hide_union(pp, t.hide, hide);
            if (!j) t.space = call.space;
            result->data[j++] = t;
        }
        result->len = j;
    }
    for (i = 0; i < m->parameters.len; i++) tokens_free(&expanded[i]);
    free(expanded);
    free(ready);
    tokens_free(&intermediate);
    return ok;
}

static const char *find_include(Preprocessor *, const char *, int, const char *, int, int *, int *);
static const char *canonical(Preprocessor *, const char *);

static char *header_name(Preprocessor *pp, const Tokens *tokens, int *angled)
{
    Buffer b = {0};
    char *result;
    size_t i;
    *angled = 0;
    if (tokens->len == 1 && tokens->data[0].kind == TOK_STRING) {
        const char *s = tokens->data[0].text;
        size_t n = strlen(s);
        if (n >= 2 && s[0] == '"' && s[n - 1] == '"') return copy_n(&pp->arena, s + 1, n - 2);
    }
    if (tokens->len < 3 || !token_is(tokens->data[0], "<") ||
        !token_is(tokens->data[tokens->len - 1], ">")) return NULL;
    *angled = 1;
    for (i = 1; i + 1 < tokens->len; i++) {
        if (i > 1 && tokens->data[i].space) buffer_char(&b, ' ');
        buffer_text(&b, tokens->data[i].text);
    }
    result = copy_string(&pp->arena, b.data);
    buffer_free(&b);
    return result;
}

static int expand_builtin(Preprocessor *pp, Macro *m, Tokens *args, Token *t, unsigned depth)
{
    char number[64];
    if (!strcmp(m->name, "__LINE__")) snprintf(number, sizeof(number), "%u", t->loc.line);
    else if (!strcmp(m->name, "__COUNTER__")) snprintf(number, sizeof(number), "%u", pp->counter++);
    else if (!strcmp(m->name, "__INCLUDE_LEVEL__")) snprintf(number, sizeof(number), "%u", pp->include_depth - 1);
    else if (!strcmp(m->name, "__DATE__")) { t->text = pp->date; t->kind = TOK_STRING; return 1; }
    else if (!strcmp(m->name, "__TIME__")) { t->text = pp->time; t->kind = TOK_STRING; return 1; }
    else if (!strcmp(m->name, "__FILE__") || !strcmp(m->name, "__BASE_FILE__")) {
        Token name = *t;
        Tokens one = {0};
        name.text = !strcmp(m->name, "__FILE__") ? t->loc.file : pp->base_file;
        name.kind = TOK_STRING;
        tokens_push(&one, name);
        *t = stringize(pp, &one, *t);
        tokens_free(&one);
        return 1;
    } else if (!strcmp(m->name, "__has_include")) {
        Tokens expanded = {0};
        const char *header;
        int angled, system = 0, slot = -1, found;
        if (!pp_expand(pp, &args[0], &expanded, depth + 1)) { tokens_free(&expanded); return 0; }
        header = header_name(pp, &expanded, &angled);
        if (!header) { error_at(t->loc, "expected header name in __has_include"); tokens_free(&expanded); return 0; }
        found = find_include(pp, header, angled, pp->physical_file ? pp->physical_file : t->loc.file, -2, &system, &slot) != NULL;
        snprintf(number, sizeof(number), "%d", found);
        tokens_free(&expanded);
    } else {
        /* No C++ or backend feature is promised by a preprocessor query. */
        snprintf(number, sizeof(number), "0");
    }
    t->kind = TOK_NUMBER;
    t->text = copy_string(&pp->arena, number);
    return 1;
}

static int expand_pragma(Preprocessor *pp, Tokens *args, Token call, Tokens *out, unsigned depth)
{
    Tokens expanded = {0}, pragma = {0};
    Buffer text = {0};
    const char *s;
    size_t n, i;
    int ok = pp_expand(pp, &args[0], &expanded, depth + 1);
    if (!ok) goto done;
    if (expanded.len != 1 || expanded.data[0].kind != TOK_STRING) {
        error_at(call.loc, "_Pragma requires one string literal"); ok = 0; goto done;
    }
    s = expanded.data[0].text;
    if (*s == 'L') s++;
    n = strlen(s);
    if (n < 2 || *s != '"' || s[n - 1] != '"') {
        error_at(call.loc, "_Pragma requires an ordinary or wide string literal"); ok = 0; goto done;
    }
    for (i = 1; i + 1 < n; i++) {
        if (s[i] == '\\' && (s[i + 1] == '\\' || s[i + 1] == '"')) i++;
        buffer_char(&text, s[i]);
    }
    ok = lex(&pp->arena, text.data ? text.data : "", call.loc.file, pp->cplusplus, &pragma);
    if (!ok) goto done;
    if (pragma.len == 1 && token_is(pragma.data[0], "once")) {
        strings_push(&pp->once_files, canonical(pp, pp->physical_file ? pp->physical_file : call.loc.file));
    } else {
        Token t = call;
        t.kind = TOK_NEWLINE; t.text = "\n"; tokens_push(out, t);
        t.kind = TOK_PUNCT; t.text = "#"; tokens_push(out, t);
        t.kind = TOK_IDENT; t.text = "pragma"; tokens_push(out, t);
        for (i = 0; i < pragma.len; i++) {
            t = pragma.data[i]; t.loc = call.loc; tokens_push(out, t);
        }
        t = call; t.kind = TOK_NEWLINE; t.text = "\n"; tokens_push(out, t);
    }
done:
    buffer_free(&text);
    tokens_free(&expanded);
    tokens_free(&pragma);
    return ok;
}

int pp_expand(Preprocessor *pp, const Tokens *input, Tokens *out, unsigned depth)
{
    Node *work;
    size_t steps = 0;
    if (depth > 256) {
        Location loc = input->len ? input->data[0].loc : (Location){0};
        error_at(loc, "macro argument nesting exceeds 256"); return 0;
    }
    work = prepend(pp, input, NULL);
    while (work) {
        Token call = work->token;
        if (pp->cplusplus && (call.kind == TOK_STRING || call.kind == TOK_CHAR)) {
            const char *quote = strrchr(call.text, call.kind == TOK_STRING ? '"' : '\'');
            if (quote && quote[1] && quote[1] != '_' && pp_macro(pp, quote + 1)) {
                Node *suffix = allocate(&pp->arena, sizeof(*suffix));
                suffix->token = call;
                suffix->token.kind = TOK_IDENT;
                suffix->token.text = quote + 1;
                suffix->token.space = 0;
                suffix->next = work->next;
                call.text = copy_n(&pp->arena, call.text, (size_t)(quote - call.text) + 1);
                tokens_push(out, call);
                work = suffix;
                continue;
            }
        }
        Macro *m = call.kind == TOK_IDENT ? pp_macro(pp, call.text) : NULL;
        Tokens *args = NULL, replacement = {0};
        Hide *hide = call.hide;
        size_t argc = 0, argcap = 0, i;
        int omitted = 0, ok = 1;
        Node *after = work->next;
        if (!m || hidden(call.hide, m->name)) {
            tokens_push(out, call);
            work = after;
            continue;
        }
        if (++steps > 1000000) { error_at(call.loc, "macro expansion exceeds 1000000 replacements"); return 0; }
        if (m->function) {
            Node *open = after, *n;
            unsigned nesting = 0, space = 0;
            while (open && open->token.kind == TOK_NEWLINE) open = open->next;
            if (!open || !token_is(open->token, "(")) {
                tokens_push(out, call);
                work = after;
                continue;
            }
            argcap = 8;
            args = calloc(argcap, sizeof(*args));
            if (!args) fatal("out of memory");
            argc = 1;
            for (n = open->next; n; n = n->next) {
                Token t = n->token;
                if (t.kind == TOK_NEWLINE) { space = 1; continue; }
                if (token_is(t, ")") && !nesting) break;
                if (token_is(t, "(") ) nesting++;
                else if (token_is(t, ")")) nesting--;
                if (token_is(t, ",") && !nesting) {
                    if (argc == argcap) {
                        size_t oldcap = argcap;
                        argcap *= 2;
                        args = realloc(args, argcap * sizeof(*args));
                        if (!args) fatal("out of memory");
                        memset(args + oldcap, 0, (argcap - oldcap) * sizeof(*args));
                    }
                    argc++;
                } else {
                    t.space |= space;
                    tokens_push(&args[argc - 1], t);
                }
                space = 0;
            }
            if (!n) { error_at(call.loc, "unterminated invocation of macro '%s'", m->name); ok = 0; }
            else {
                size_t required = m->parameters.len - (m->variadic ? 1 : 0);
                if (!m->parameters.len && argc == 1 && !args[0].len) argc = 0;
                if ((!m->variadic && argc != required) || (m->variadic && argc < required)) {
                    error_at(call.loc, "macro '%s' expects %zu%s arguments, got %zu", m->name,
                             required, m->variadic ? " or more" : "", argc);
                    ok = 0;
                } else if (m->variadic) {
                    size_t j;
                    omitted = argc == required || (!required && argc == 1 && !args[0].len);
                    for (j = required + 1; j < argc; j++) {
                        Token comma = call;
                        comma.kind = TOK_PUNCT;
                        comma.text = ",";
                        comma.space = 0;
                        tokens_push(&args[required], comma);
                        tokens_append(&args[required], &args[j]);
                    }
                }
                after = n->next;
                hide = hide_intersection(pp, call.hide, n->token.hide);
            }
        }
        hide = hide_add(pp, hide, m->name);
        if (ok && m->builtin && !strcmp(m->name, "_Pragma")) {
            ok = expand_pragma(pp, args, call, out, depth);
        } else if (ok && m->builtin) {
            Token t = call;
            ok = expand_builtin(pp, m, args, &t, depth);
            t.hide = hide;
            tokens_push(&replacement, t);
        } else if (ok) ok = substitute(pp, m, args, call, hide, omitted, &replacement, depth);
        for (i = 0; i < argcap; i++) tokens_free(&args[i]);
        free(args);
        if (!ok) { tokens_free(&replacement); return 0; }
        work = prepend(pp, &replacement, after);
        tokens_free(&replacement);
    }
    return 1;
}

static int readable(const char *path) { return access(path, R_OK) == 0; }

/* Search slots are stable across recursive #include_next processing. */
static const char *find_include(Preprocessor *pp, const char *name, int angled,
                                const char *from, int next, int *system, int *slot)
{
    const Strings *groups[4] = {&pp->quote_includes, &pp->includes, &pp->system_includes, &pp->after_includes};
    size_t g, i;
    int index = 0;
    if (*name == '/' && next < -1) {
        if (!readable(name)) return NULL;
        *slot = -1;
        return copy_string(&pp->arena, name);
    }
    if (!angled && next < -1) {
        char *path = path_join(&pp->arena, path_directory(&pp->arena, from), name);
        if (readable(path)) { *slot = -1; return path; }
    }
    for (g = 0; g < 4; g++) {
        for (i = 0; i < groups[g]->len; i++, index++) {
            char *path;
            if (index <= next || (angled && !g)) continue;
            path = path_join(&pp->arena, groups[g]->data[i], name);
            if (readable(path)) { *system |= g >= 2; *slot = index; return path; }
        }
    }
    return NULL;
}

static const char *canonical(Preprocessor *pp, const char *path)
{
    char *real = realpath(path, NULL);
    const char *result;
    if (!real) return path;
    result = copy_string(&pp->arena, real);
    free(real);
    return result;
}

static void dependency(Preprocessor *pp, const char *path, int system)
{
    size_t i;
    const char *key = canonical(pp, path);
    for (i = 0; i < pp->dependency_count; i++) {
        if (!strcmp(pp->dependencies[i].key, key)) {
            pp->dependencies[i].system &= (unsigned)system;
            return;
        }
    }
    if (pp->dependency_count == pp->dependency_cap) {
        pp->dependency_cap = pp->dependency_cap ? pp->dependency_cap * 2 : 32;
        pp->dependencies = realloc(pp->dependencies, pp->dependency_cap * sizeof(*pp->dependencies));
        if (!pp->dependencies) fatal("out of memory");
    }
    pp->dependencies[pp->dependency_count].path = copy_string(&pp->arena, path);
    pp->dependencies[pp->dependency_count].key = key;
    pp->dependencies[pp->dependency_count++].system = (unsigned)system;
    if (pp->base_file && !strcmp(key, canonical(pp, pp->base_file)) && pp->dependency_count > 1) {
        Dependency main = pp->dependencies[pp->dependency_count - 1];
        memmove(pp->dependencies + 1, pp->dependencies, (pp->dependency_count - 1) * sizeof(*pp->dependencies));
        pp->dependencies[0] = main;
    }
}

static int pp_file_internal(Preprocessor *, const char *, int, int, Tokens *);

static int include_directive(Preprocessor *pp, const Tokens *args, Location loc,
                             const char *path, int system, int next_slot, Tokens *out)
{
    Tokens expanded = {0};
    const char *name, *found;
    int angled, slot = -1, ok;
    /* Header-name tokens are recognized before ordinary macro expansion. */
    name = header_name(pp, args, &angled);
    if (!name) {
        if (!pp_expand(pp, args, &expanded, 0)) { tokens_free(&expanded); return 0; }
        name = header_name(pp, &expanded, &angled);
    }
    if (!name || !*name) { error_at(loc, "expected header name in #include"); tokens_free(&expanded); return 0; }
    found = find_include(pp, name, angled, path, next_slot, &system, &slot);
    if (!found) { error_at(loc, "header '%s' not found", name); tokens_free(&expanded); return 0; }
    ok = pp_file_internal(pp, found, system, slot, out);
    tokens_free(&expanded);
    return ok;
}

static int single_identifier(const Tokens *tokens, Location loc)
{
    if (tokens->len == 1 && tokens->data[0].kind == TOK_IDENT) return 1;
    error_at(loc, "expected one macro name");
    return 0;
}

static int flush(Preprocessor *pp, Tokens *pending, Tokens *out)
{
    int ok = pp_expand(pp, pending, out, 0);
    pending->len = 0;
    return ok;
}

static void directive_message(Location loc, const Tokens *args, int warning)
{
    Buffer b = {0};
    size_t i;
    for (i = 0; i < args->len; i++) {
        if (i && args->data[i].space) buffer_char(&b, ' ');
        buffer_text(&b, args->data[i].text);
    }
    if (warning) warning_at(loc, "%s", b.data ? b.data : "#warning");
    else error_at(loc, "%s", b.data ? b.data : "#error");
    buffer_free(&b);
}

static int pp_file_internal(Preprocessor *pp, const char *path, int system, int slot, Tokens *out)
{
    Tokens input = {0}, pending = {0};
    Conditional stack[256];
    size_t start = 0, size;
    unsigned level = 0;
    int ok = 1, active = 1;
    int64_t line_delta = 0;
    const char *logical_file = !strcmp(path, "-") ? "<stdin>" : path, *key = canonical(pp, path);
    const char *previous_physical_file = pp->physical_file;
    char *source;
    dependency(pp, path, system);
    if (strings_contains(&pp->once_files, key)) return 1;
    if (++pp->include_depth > 256) {
        error_at((Location){path, 1, 1}, "include nesting exceeds 256");
        pp->include_depth--;
        return 0;
    }
    source = read_file(&pp->arena, path, &size);
    pp->physical_file = path;
    if (!source) { error_at((Location){path, 1, 1}, "cannot read file: %s", strerror(errno)); ok = 0; goto done; }
    if (memchr(source, 0, size)) { error_at((Location){path, 1, 1}, "NUL byte in source"); ok = 0; goto done; }
    if (!lex(&pp->arena, source, logical_file, pp->cplusplus, &input)) { ok = 0; goto done; }
    while (start < input.len && ok) {
        size_t end = start, i;
        Tokens line, args;
        Token directive;
        Location loc;
        while (end < input.len && input.data[end].kind != TOK_NEWLINE) end++;
        for (i = start; i < end; i++) {
            input.data[i].loc.line = (unsigned)((int64_t)input.data[i].loc.line + line_delta);
            input.data[i].loc.file = logical_file;
        }
        line.data = input.data + start; line.len = end - start; line.cap = line.len;
        if (!line.len || !token_is(line.data[0], "#")) {
            if (active) {
                tokens_append(&pending, &line);
                if (end < input.len) {
                    Token newline = input.data[end];
                    newline.loc.file = logical_file;
                    newline.loc.line = (unsigned)((int64_t)newline.loc.line + line_delta);
                    tokens_push(&pending, newline);
                }
            }
            start = end + (end < input.len);
            continue;
        }
        if (!flush(pp, &pending, out)) { ok = 0; break; }
        if (line.len == 1) { start = end + (end < input.len); continue; }
        directive = line.data[1];
        loc = directive.loc;
        args.data = line.data + 2; args.len = line.len - 2; args.cap = args.len;
        if (token_is(directive, "if") || token_is(directive, "ifdef") || token_is(directive, "ifndef")) {
            int condition = 0;
            if (level == 256) { error_at(loc, "conditional nesting exceeds 256"); ok = 0; break; }
            if (token_is(directive, "if")) {
                if (active) ok = pp_expression(pp, &args, &condition);
            } else {
                ok = single_identifier(&args, loc);
                if (ok) condition = pp_macro(pp, args.data[0].text) != NULL;
                if (token_is(directive, "ifndef")) condition = !condition;
            }
            stack[level].parent = active;
            stack[level].active = active && condition;
            stack[level].taken = stack[level].active;
            stack[level].saw_else = 0;
            stack[level].loc = loc;
            active = stack[level++].active;
        } else if (token_is(directive, "elif")) {
            int condition = 0;
            if (!level || stack[level - 1].saw_else) { error_at(loc, "unexpected #elif"); ok = 0; }
            else {
                Conditional *c = &stack[level - 1];
                if (c->parent && !c->taken) ok = pp_expression(pp, &args, &condition);
                active = c->active = c->parent && !c->taken && condition;
                c->taken |= active;
            }
        } else if (token_is(directive, "else")) {
            if (!level || stack[level - 1].saw_else || args.len) { error_at(loc, "unexpected #else or extra tokens"); ok = 0; }
            else {
                Conditional *c = &stack[level - 1];
                active = c->active = c->parent && !c->taken;
                c->taken = 1;
                c->saw_else = 1;
            }
        } else if (token_is(directive, "endif")) {
            if (!level || args.len) { error_at(loc, "unexpected #endif or extra tokens"); ok = 0; }
            else active = stack[--level].parent;
        } else if (active) {
            if (token_is(directive, "define")) ok = pp_define(pp, &args, loc);
            else if (token_is(directive, "undef")) {
                ok = single_identifier(&args, loc);
                if (ok) pp_undef(pp, args.data[0].text);
            } else if (token_is(directive, "include") || token_is(directive, "include_next")) {
                ok = include_directive(pp, &args, loc, path, system,
                                       token_is(directive, "include_next") ? slot : -2, out);
            } else if (token_is(directive, "error")) { directive_message(loc, &args, 0); ok = 0; }
            else if (token_is(directive, "warning")) {
                if (pp->warnings) {
                    directive_message(loc, &args, !pp->warnings_as_errors);
                    if (pp->warnings_as_errors) ok = 0;
                }
            }
            else if (token_is(directive, "pragma")) {
                if (args.len == 1 && token_is(args.data[0], "once")) strings_push(&pp->once_files, key);
                else {
                    /* Preserve backend pragmas and C++ pragmas for later phases. */
                    tokens_append(out, &line);
                    if (end < input.len) tokens_push(out, input.data[end]);
                }
            } else if (token_is(directive, "line") || directive.kind == TOK_NUMBER) {
                Tokens expanded = {0}, linemarker = args;
                char *number_end;
                unsigned long number;
                size_t allowed;
                if (directive.kind == TOK_NUMBER) {
                    linemarker.data = line.data + 1;
                    linemarker.len = line.len - 1;
                }
                ok = pp_expand(pp, &linemarker, &expanded, 0);
                number = expanded.len ? strtoul(expanded.data[0].text, &number_end, 10) : 0;
                allowed = directive.kind == TOK_NUMBER ? expanded.len : 2;
                if (!expanded.len || expanded.data[0].kind != TOK_NUMBER || !number ||
                    number > 2147483647UL || *number_end || expanded.len > allowed ||
                    (expanded.len > 1 && expanded.data[1].kind != TOK_STRING)) {
                    error_at(loc, "invalid #line directive"); ok = 0;
                } else {
                    unsigned physical_next = end < input.len ? input.data[end].loc.line + 1 : loc.line + 1;
                    line_delta = (int64_t)number - physical_next;
                    if (expanded.len > 1) {
                        const char *s = expanded.data[1].text;
                        Buffer file = {0};
                        size_t j;
                        for (j = 1; j + 1 < strlen(s); j++) {
                            if (s[j] == '\\' && (s[j + 1] == '\\' || s[j + 1] == '"')) j++;
                            buffer_char(&file, s[j]);
                        }
                        logical_file = copy_string(&pp->arena, file.data ? file.data : "");
                        buffer_free(&file);
                    }
                }
                tokens_free(&expanded);
            } else { error_at(loc, "unsupported preprocessing directive '#%s'", directive.text); ok = 0; }
        }
        start = end + (end < input.len);
    }
    if (ok && level) { error_at(stack[level - 1].loc, "unterminated conditional directive"); ok = 0; }
    if (ok) ok = flush(pp, &pending, out);
done:
    pp->physical_file = previous_physical_file;
    tokens_free(&input);
    tokens_free(&pending);
    pp->include_depth--;
    return ok;
}

int pp_file(Preprocessor *pp, const char *path, int system, int slot, Tokens *out)
{
    if (!pp->base_file) pp->base_file = path;
    return pp_file_internal(pp, path, system, slot, out);
}

int pp_forced_include(Preprocessor *pp, const char *name, Tokens *out)
{
    int system = 0, slot = -1, ok;
    const char *path = find_include(pp, name, 0, "./<command-line>", -2, &system, &slot);
    if (!path) { error_at((Location){"<command-line>", 1, 1}, "header '%s' not found", name); return 0; }
    pp->include_depth++;
    ok = pp_file_internal(pp, path, system, slot, out);
    pp->include_depth--;
    return ok;
}

static int compare_macros(const void *a, const void *b)
{
    return strcmp((*(Macro *const *)a)->name, (*(Macro *const *)b)->name);
}

void pp_dump_macros(Preprocessor *pp, FILE *f)
{
    Macro *m, **list;
    size_t count = 0, i, j;
    for (m = pp->macros; m; m = m->next) if (!m->builtin) count++;
    list = malloc((count ? count : 1) * sizeof(*list));
    if (!list) fatal("out of memory");
    for (m = pp->macros, i = 0; m; m = m->next) if (!m->builtin) list[i++] = m;
    qsort(list, count, sizeof(*list), compare_macros);
    for (i = 0; i < count; i++) {
        m = list[i];
        fprintf(f, "#define %s", m->name);
        if (m->function) {
            fputc('(', f);
            for (j = 0; j < m->parameters.len; j++) {
                if (j) fputc(',', f);
                if (m->variadic && j + 1 == m->parameters.len && !strcmp(m->parameters.data[j], "__VA_ARGS__")) fputs("...", f);
                else {
                    fputs(m->parameters.data[j], f);
                    if (m->variadic && j + 1 == m->parameters.len) fputs("...", f);
                }
            }
            fputc(')', f);
        }
        for (j = 0; j < m->body.len; j++) {
            if (!j || m->body.data[j].space) fputc(' ', f);
            fputs(m->body.data[j].text, f);
        }
        fputc('\n', f);
    }
    free(list);
}

static void make_quote(FILE *f, const char *s)
{
    size_t slashes = 0;
    while (*s) {
        if (*s == '\\') { slashes++; fputc(*s++, f); continue; }
        if (*s == ' ' || *s == '\t' || *s == '#') {
            while (slashes) { fputc('\\', f); slashes--; }
            fputc('\\', f);
        } else if (*s == '$') fputc('$', f);
        fputc(*s++, f);
        slashes = 0;
    }
    /* A trailing backslash precedes whitespace in the dependency rule. */
    while (slashes) { fputc('\\', f); slashes--; }
}

void pp_dependencies(Preprocessor *pp, FILE *f, const Strings *targets,
                     const Strings *quoted_targets, int user_only, int phony)
{
    size_t i;
    int first = 1;
    for (i = 0; i < targets->len; i++) {
        if (!first) fputc(' ', f);
        fputs(targets->data[i], f); first = 0;
    }
    for (i = 0; i < quoted_targets->len; i++) {
        if (!first) fputc(' ', f);
        make_quote(f, quoted_targets->data[i]); first = 0;
    }
    fputc(':', f);
    for (i = 0; i < pp->dependency_count; i++) {
        if (user_only && pp->dependencies[i].system) continue;
        fputs(" \\\n  ", f);
        make_quote(f, pp->dependencies[i].path);
    }
    fputc('\n', f);
    if (phony) for (i = 1; i < pp->dependency_count; i++) {
        if (user_only && pp->dependencies[i].system) continue;
        fputc('\n', f);
        make_quote(f, pp->dependencies[i].path);
        fputs(":\n", f);
    }
}
