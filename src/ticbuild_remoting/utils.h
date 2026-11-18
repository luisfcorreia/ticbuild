#ifndef TICBUILD_REMOTING_UTILS_H
#define TICBUILD_REMOTING_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    const char* ptr;
    size_t len;
} tb_slice;

typedef enum
{
    TB_ARG_INT,
    TB_ARG_STR,
    TB_ARG_BYTES
} tb_arg_type;

typedef struct
{
    tb_arg_type type;
    union
    {
        int64_t i;
        struct { const char* ptr; size_t len; } s;
        struct { const uint8_t* ptr; size_t len; } b;
    } v;
} tb_arg;

/* Set a human-readable error message (truncated to errcap-1, null-terminated). */
void tb_set_err(char* err, size_t errcap, const char* msg);

/* Trim leading/trailing whitespace from *line / *len in place. */
void tb_trim_whitespace(const char** line, size_t* len);

/* Check that all n bytes are ASCII (0..127). */
bool tb_is_ascii_only(const char* s, size_t n);

/* Parse the next whitespace-delimited token from *p / *len.
 * Advances p/len past the token. Returns false if none remain. */
bool tb_next_token(const char** p, size_t* len, tb_slice* tok,
                   char* err, size_t errcap);

/* Parse a decimal integer from a slice. */
bool tb_parse_int(tb_slice tok, int64_t* out);

/* Parse a quoted string arg (leading '"'). Returns allocated decoded string. */
bool tb_parse_quoted(const char* ptr, size_t len, char** decoded,
                     size_t* dlen, char* err, size_t errcap);

/* Free all decoded string arguments in args[0..argc). */
void tb_free_args(tb_arg* args, size_t argc);

/* Escape a string for the wire protocol (quote/backslash/control chars).
 * Returns required length (excluding null). out may be NULL to query size. */
size_t tb_escape_string_len(const char* s, size_t len);
size_t tb_escape_string(const char* s, size_t len, char* out, size_t outcap);

/* Format an unsigned 64-bit as "<num><suffix>" k/M/G with one decimal.
 * Returns required length (excluding null). */
size_t tb_format_kc1(char* out, size_t outcap, uint64_t val);
size_t tb_format_kb1(char* out, size_t outcap, uint64_t val);

/* Convert a single hex nibble char to its 0-15 value. */
bool tb_hex_nibble(char c, uint8_t* out);

#endif /* TICBUILD_REMOTING_UTILS_H */
