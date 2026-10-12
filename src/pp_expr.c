#include "bcxx.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t bits; int uns; } Value;
typedef struct {
    Preprocessor *pp;
    const Tokens *tokens;
    size_t index;
    unsigned depth;
    int failed;
} Expression;

static Value expression(Expression *, int, int);
static Value value(uint64_t bits, int uns) { Value v = {bits, uns}; return v; }

static Token current(Expression *e)
{
    Token end;
    memset(&end, 0, sizeof(end));
    end.text = "<end>";
    if (e->index < e->tokens->len) return e->tokens->data[e->index];
    if (e->tokens->len) end.loc = e->tokens->data[e->tokens->len - 1].loc;
    return end;
}

static void fail(Expression *e, const char *message)
{
    if (!e->failed) error_at(current(e).loc, "%s", message);
    e->failed = 1;
}

static int take(Expression *e, const char *s)
{
    if (!token_is(current(e), s)) return 0;
    e->index++;
    return 1;
}

static Value number(Expression *e, Token t)
{
    Buffer b = {0};
    const char *s = t.text;
    char *end;
    uint64_t n;
    int base = 10, uns = 0;
    unsigned longs = 0, us = 0;
    while (*s) {
        if (*s != '\'') buffer_char(&b, (char)tolower((unsigned char)*s));
        s++;
    }
    s = b.data;
    if (s[0] == '0') {
        base = 8;
        if (s[1] == 'x') { base = 16; s += 2; }
        else if (s[1] == 'b') { base = 2; s += 2; }
    }
    errno = 0;
    n = strtoull(s, &end, base);
    if (end == s || errno == ERANGE) fail(e, "invalid or out-of-range integer in #if");
    while (*end) {
        if (*end == 'u' && !us) { us++; uns = 1; end++; }
        else if (*end == 'l' && !longs) {
            longs = 1;
            end++;
            if (*end == 'l') { longs++; end++; }
        } else { fail(e, "invalid integer suffix in #if"); break; }
    }
    if (n > INT64_MAX) {
        if (base == 10 && !uns) fail(e, "decimal integer does not fit intmax_t in #if");
        uns = 1;
    }
    buffer_free(&b);
    return value(n, uns);
}

static Value character(Expression *e, Token t)
{
    const char *s = strchr(t.text, '\'');
    uint64_t n = 0;
    unsigned count = 0;
    s++;
    while (*s && *s != '\'') {
        unsigned c = (unsigned char)*s++;
        if (c == '\\') {
            c = (unsigned char)*s++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'a': c = '\a'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'v': c = '\v'; break;
            case 'x': {
                unsigned digits = 0;
                c = 0;
                while (isxdigit((unsigned char)*s)) {
                    unsigned d = (unsigned)tolower((unsigned char)*s++);
                    c = c * 16 + (d <= '9' ? d - '0' : d - 'a' + 10);
                    digits++;
                }
                if (!digits) fail(e, "empty hexadecimal character escape");
                break;
            }
            default:
                if (c >= '0' && c <= '7') {
                    unsigned i;
                    c -= '0';
                    for (i = 1; i < 3 && *s >= '0' && *s <= '7'; i++) c = c * 8 + (unsigned)(*s++ - '0');
                } else if (c != '\\' && c != '\'' && c != '"' && c != '?') {
                    fail(e, "unsupported character escape in #if");
                }
            }
        }
        n = (n << 8) | c;
        count++;
    }
    if (!count || count > 8 || s[1]) fail(e, "invalid character constant in #if");
    if (t.text[0] == '\'' && count == 1 && n >= 128 && n <= 255) n |= UINT64_MAX << 8;
    else if (t.text[0] == '\'' && count == 4 && (n & (UINT64_C(1) << 31))) n |= UINT64_MAX << 32;
    return value(n, 0);
}

static Value unary(Expression *e, int evaluate)
{
    Token t = current(e);
    Value v;
    if (++e->depth > 256) {
        fail(e, "#if expression nesting exceeds 256");
        e->depth--;
        return value(0, 0);
    }
    if (take(e, "(")) {
        v = expression(e, 1, evaluate);
        if (!take(e, ")")) fail(e, "expected ')' in #if");
    } else if (take(e, "+")) v = unary(e, evaluate);
    else if (take(e, "-")) { v = unary(e, evaluate); v.bits = 0 - v.bits; }
    else if (take(e, "!")) { v = unary(e, evaluate); v = value(!v.bits, 0); }
    else if (take(e, "~")) { v = unary(e, evaluate); v.bits = ~v.bits; }
    else if (t.kind == TOK_NUMBER) { e->index++; v = number(e, t); }
    else if (t.kind == TOK_CHAR) { e->index++; v = character(e, t); }
    else if (t.kind == TOK_IDENT && e->index < e->tokens->len) {
        e->index++;
        v = value(e->pp->cplusplus && token_is(t, "true"), 0);
    } else { fail(e, "expected integer expression in #if"); v = value(0, 0); }
    e->depth--;
    return v;
}

static int precedence(Token t)
{
    if (token_is(t, "?")) return 1;
    if (token_is(t, "||")) return 2;
    if (token_is(t, "&&")) return 3;
    if (token_is(t, "|")) return 4;
    if (token_is(t, "^")) return 5;
    if (token_is(t, "&")) return 6;
    if (token_is(t, "==") || token_is(t, "!=")) return 7;
    if (token_is(t, "<") || token_is(t, ">") || token_is(t, "<=") || token_is(t, ">=")) return 8;
    if (token_is(t, "<<") || token_is(t, ">>")) return 9;
    if (token_is(t, "+") || token_is(t, "-")) return 10;
    if (token_is(t, "*") || token_is(t, "/") || token_is(t, "%")) return 11;
    return 0;
}

static int less(Value a, Value b)
{
    if (a.uns || b.uns) return a.bits < b.bits;
    return (a.bits ^ (UINT64_C(1) << 63)) < (b.bits ^ (UINT64_C(1) << 63));
}

static Value binary(Expression *e, Token op, Value a, Value b, int evaluate)
{
    Value r = value(0, a.uns || b.uns);
    if (token_is(op, "+")) r.bits = a.bits + b.bits;
    else if (token_is(op, "-")) r.bits = a.bits - b.bits;
    else if (token_is(op, "*")) r.bits = a.bits * b.bits;
    else if (token_is(op, "/") || token_is(op, "%")) {
        uint64_t x = a.bits, y = b.bits;
        int nx = !r.uns && (x >> 63), ny = !r.uns && (y >> 63);
        if (!y) { if (evaluate) fail(e, "division by zero in #if"); return r; }
        if (nx) x = 0 - x;
        if (ny) y = 0 - y;
        if (token_is(op, "/")) { r.bits = x / y; if (nx != ny) r.bits = 0 - r.bits; }
        else { r.bits = x % y; if (nx) r.bits = 0 - r.bits; }
    } else if (token_is(op, "<<") || token_is(op, ">>")) {
        r.uns = a.uns;
        if (b.bits >= 64) { if (evaluate) fail(e, "invalid shift count in #if"); return r; }
        if (token_is(op, "<<")) r.bits = a.bits << b.bits;
        else {
            r.bits = a.bits >> b.bits;
            if (!a.uns && (a.bits >> 63) && b.bits) r.bits |= UINT64_MAX << (64 - b.bits);
        }
    } else if (token_is(op, "&")) r.bits = a.bits & b.bits;
    else if (token_is(op, "|")) r.bits = a.bits | b.bits;
    else if (token_is(op, "^")) r.bits = a.bits ^ b.bits;
    else {
        r.uns = 0;
        if (token_is(op, "==")) r.bits = a.bits == b.bits;
        else if (token_is(op, "!=")) r.bits = a.bits != b.bits;
        else if (token_is(op, "<")) r.bits = less(a, b);
        else if (token_is(op, ">")) r.bits = less(b, a);
        else if (token_is(op, "<=")) r.bits = !less(b, a);
        else if (token_is(op, ">=")) r.bits = !less(a, b);
        else if (token_is(op, "&&")) r.bits = a.bits && b.bits;
        else if (token_is(op, "||")) r.bits = a.bits || b.bits;
    }
    return r;
}

static Value expression(Expression *e, int minimum, int evaluate)
{
    Value left;
    if (++e->depth > 256) {
        fail(e, "#if expression nesting exceeds 256");
        e->depth--;
        return value(0, 0);
    }
    left = unary(e, evaluate);
    while (!e->failed) {
        Token op = current(e);
        int p = precedence(op), right_evaluate = evaluate;
        Value right;
        if (p < minimum) break;
        e->index++;
        if (token_is(op, "?")) {
            Value yes = expression(e, 1, evaluate && left.bits != 0), no;
            if (!take(e, ":")) { fail(e, "expected ':' in #if conditional expression"); break; }
            no = expression(e, 1, evaluate && left.bits == 0);
            left = value(left.bits ? yes.bits : no.bits, yes.uns || no.uns);
            continue;
        }
        if (token_is(op, "&&") && !left.bits) right_evaluate = 0;
        if (token_is(op, "||") && left.bits) right_evaluate = 0;
        right = expression(e, p + 1, right_evaluate);
        left = binary(e, op, left, right, evaluate);
    }
    e->depth--;
    return left;
}

int pp_expression(Preprocessor *pp, const Tokens *input, int *result)
{
    Tokens defined = {0}, expanded = {0};
    Expression e;
    Value v;
    size_t i;
    int ok = 1;
    for (i = 0; i < input->len; i++) {
        Token t = input->data[i];
        if (token_is(t, "defined")) {
            int parentheses = 0;
            const char *name;
            if (++i < input->len && token_is(input->data[i], "(")) { parentheses = 1; i++; }
            if (i >= input->len || input->data[i].kind != TOK_IDENT) {
                error_at(t.loc, "expected macro name after defined"); ok = 0; break;
            }
            name = input->data[i].text;
            if (parentheses && (++i >= input->len || !token_is(input->data[i], ")"))) {
                error_at(t.loc, "expected ')' after defined"); ok = 0; break;
            }
            t.kind = TOK_NUMBER;
            t.text = pp_macro(pp, name) ? "1" : "0";
        }
        tokens_push(&defined, t);
    }
    if (ok) ok = pp_expand(pp, &defined, &expanded, 0);
    memset(&e, 0, sizeof(e));
    e.pp = pp;
    e.tokens = &expanded;
    v = value(0, 0);
    if (ok) {
        v = expression(&e, 1, 1);
        if (!e.failed && e.index != expanded.len) fail(&e, "unexpected token in #if expression");
        ok = !e.failed;
    }
    *result = v.bits != 0;
    tokens_free(&defined);
    tokens_free(&expanded);
    return ok;
}
