#include "ticbuild_remoting/utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tb_set_err(char* err, size_t errcap, const char* msg)
{
    if(!err || errcap == 0) return;
    size_t mlen = strlen(msg);
    if(mlen >= errcap) mlen = errcap - 1;
    memcpy(err, msg, mlen);
    err[mlen] = '\0';
}

void tb_trim_whitespace(const char** line, size_t* len)
{
    const char* p = *line;
    size_t n = *len;
    while(n > 0 && (p[0] == ' ' || p[0] == '\t' || p[0] == '\r' || p[0] == '\n'))
    { p++; n--; }
    while(n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' || p[n - 1] == '\n'))
    { n--; }
    *line = p;
    *len = n;
}

bool tb_is_ascii_only(const char* s, size_t n)
{
    for(size_t i = 0; i < n; i++)
    {
        if((unsigned char)s[i] > 127) return false;
    }
    return true;
}

bool tb_next_token(const char** p, size_t* len, tb_slice* tok,
                   char* err, size_t errcap)
{
    (void)err; (void)errcap;
    const char* s = *p;
    size_t n = *len;

    /* skip leading whitespace */
    while(n > 0 && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r' || s[0] == '\n'))
    { s++; n--; }
    if(n == 0) { *p = s; *len = 0; return false; }

    /* handle quoted string: starts with " */
    if(s[0] == '"')
    {
        size_t i = 1;
        bool escaped = false;
        while(i < n)
        {
            char c = s[i];
            if(escaped)
            {
                escaped = false;
                i++;
                continue;
            }
            if(c == '\\') { escaped = true; i++; continue; }
            if(c == '"') break;
            i++;
        }
        size_t toklen = i + 1; /* include closing quote */
        if(i >= n) toklen = i; /* unterminated: take rest */
        tok->ptr = s;
        tok->len = toklen;
        *p = s + toklen;
        *len = n - toklen;
        return true;
    }

    /* handle binary literal: starts with < */
    if(s[0] == '<')
    {
        size_t i = 1;
        while(i < n && s[i] != '>') i++;
        size_t toklen = (i < n) ? (i + 1) : i; /* include closing > if found */
        tok->ptr = s;
        tok->len = toklen;
        *p = s + toklen;
        *len = n - toklen;
        return true;
    }

    /* plain token: run of non-whitespace */
    size_t i = 0;
    while(i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
    { i++; }
    tok->ptr = s;
    tok->len = i;
    *p = s + i;
    *len = n - i;
    return true;
}

bool tb_parse_int(tb_slice tok, int64_t* out)
{
    if(tok.len == 0) return false;
    char buf[32];
    if(tok.len >= sizeof buf) return false;
    memcpy(buf, tok.ptr, tok.len);
    buf[tok.len] = '\0';
    char* end = NULL;
    long long v = strtoll(buf, &end, 10);
    if(end == buf || *end != '\0') return false;
    *out = (int64_t)v;
    return true;
}

bool tb_parse_quoted(const char* ptr, size_t len, char** decoded,
                     size_t* dlen, char* err, size_t errcap)
{
    /* ptr points at opening quote; len includes both quotes */
    if(len < 2 || ptr[0] != '"' || ptr[len - 1] != '"')
    {
        tb_set_err(err, errcap, "invalid quoted string");
        return false;
    }
    size_t dcap = len; /* decoded is never longer than source */
    char* d = (char*)malloc(dcap + 1);
    if(!d) { tb_set_err(err, errcap, "out of memory"); return false; }
    size_t di = 0;
    bool esc = false;
    for(size_t i = 1; i + 1 < len; i++)
    {
        char c = ptr[i];
        if(esc)
        {
            switch(c)
            {
                case '"': d[di++] = '"'; break;
                case '\\': d[di++] = '\\'; break;
                case 'n': d[di++] = '\n'; break;
                case 't': d[di++] = '\t'; break;
                case 'r': d[di++] = '\r'; break;
                default: d[di++] = c; break;
            }
            esc = false;
            continue;
        }
        if(c == '\\') { esc = true; continue; }
        d[di++] = c;
    }
    d[di] = '\0';
    *decoded = d;
    *dlen = di;
    return true;
}

void tb_free_args(tb_arg* args, size_t argc)
{
    if(!args) return;
    for(size_t i = 0; i < argc; i++)
    {
        if(args[i].type == TB_ARG_STR && args[i].v.s.ptr)
        { free((void*)args[i].v.s.ptr); }
        /* TB_ARG_BYTES may point into ctx tmp buffer (not owned) or malloc'd;
         * callers manage separately where needed. */
    }
}

size_t tb_escape_string_len(const char* s, size_t len)
{
    size_t need = 0;
    for(size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if(c == '"' || c == '\\' || c < 0x20) need += 2;
        else need += 1;
    }
    return need;
}

size_t tb_escape_string(const char* s, size_t len, char* out, size_t outcap)
{
    size_t pos = 0;
    for(size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if(c == '"' || c == '\\' || c < 0x20)
        {
            if(pos + 2 <= outcap)
            {
                out[pos++] = '\\';
                if(c == '"') out[pos++] = '"';
                else if(c == '\\') out[pos++] = '\\';
                else if(c == '\n') out[pos++] = 'n';
                else if(c == '\t') out[pos++] = 't';
                else if(c == '\r') out[pos++] = 'r';
                else
                {
                    /* \xHH */
                    if(pos + 3 > outcap) break;
                    out[pos++] = 'x';
                    out[pos++] = "0123456789abcdef"[c >> 4];
                    out[pos++] = "0123456789abcdef"[c & 0x0f];
                }
            }
            else
            {
                break;
            }
        }
        else
        {
            if(pos + 1 <= outcap) out[pos++] = (char)c;
            else break;
        }
    }
    if(pos < outcap) out[pos] = '\0';
    return pos;
}

size_t tb_format_kc1(char* out, size_t outcap, uint64_t val)
{
    if(val >= 1000000000ULL)
    {
        char buf[32];
        size_t n = snprintf(buf, sizeof buf, "%lluG", (unsigned long long)(val / 1000000000ULL));
        if(n >= outcap) return n + 1;
        memcpy(out, buf, n + 1);
        return n;
    }
    if(val >= 1000000ULL)
    {
        char buf[32];
        size_t n = snprintf(buf, sizeof buf, "%lluM", (unsigned long long)(val / 1000000ULL));
        if(n >= outcap) return n + 1;
        memcpy(out, buf, n + 1);
        return n;
    }
    if(val >= 1000ULL)
    {
        char buf[32];
        size_t n = snprintf(buf, sizeof buf, "%lluk", (unsigned long long)(val / 1000ULL));
        if(n >= outcap) return n + 1;
        memcpy(out, buf, n + 1);
        return n;
    }
    char buf[32];
    size_t n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)val);
    if(n >= outcap) return n + 1;
    memcpy(out, buf, n + 1);
    return n;
}

size_t tb_format_kb1(char* out, size_t outcap, uint64_t val)
{
    return tb_format_kc1(out, outcap, val);
}

bool tb_hex_nibble(char c, uint8_t* out)
{
    if(c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return true; }
    if(c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return true; }
    if(c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return true; }
    return false;
}
