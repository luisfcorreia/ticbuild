#include "ticbuild_remoting/remoting.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ticbuild_remoting/discovery.h"
#include "ticbuild_remoting/fps.h"
#include "ticbuild_remoting/utils.h"

/* -------------------------------------------------------------------------- */
/* Platform socket abstraction                                                */
/* -------------------------------------------------------------------------- */

#if defined(_WIN32) || defined(__TIC_WINDOWS__)

# include <winsock2.h>
# include <ws2tcpip.h>
# pragma comment(lib, "Ws2_32.lib")

typedef SOCKET tb_socket;
typedef int tb_socklen;
# define TB_INVALID_SOCKET INVALID_SOCKET
# define tb_close_socket(s) closesocket(s)
# define tb_send(s, b, l, f) send((s), (b), (l), (f))
# define TB_SEND_FLAGS 0
static int tb_last_socket_error(void) { return WSAGetLastError(); }
static bool tb_would_block(int err) { return err == WSAEWOULDBLOCK; }
static bool tb_sock_init_platform(TicbuildRemoting* ctx)
{
    (void)ctx;
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
static void tb_sock_shutdown_platform(void) { WSACleanup(); }
static bool tb_set_nonblocking(tb_socket s)
{
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <unistd.h>
# include <fcntl.h>
# include <errno.h>
# include <signal.h>

typedef int tb_socket;
typedef socklen_t tb_socklen;
# define TB_INVALID_SOCKET (-1)
# define tb_close_socket(s) close(s)
# define tb_recv_len_type size_t
# if defined(__APPLE__)
/* macOS/BSD have no MSG_NOSIGNAL; SO_NOSIGPIPE is set on the socket instead. */
#  define TB_SEND_FLAGS 0
# else
#  define TB_SEND_FLAGS MSG_NOSIGNAL
# endif
static int tb_last_socket_error(void) { return errno; }
static bool tb_would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK || err == EINTR; }
static bool tb_sock_init_platform(TicbuildRemoting* ctx)
{
    (void)ctx;
    /* Prevent SIGPIPE from terminating the process on macOS/BSD where
     * MSG_NOSIGNAL is unavailable. Harmless on Linux. */
    signal(SIGPIPE, SIG_IGN);
    return true;
}
static void tb_sock_shutdown_platform(void) { /* no-op */ }
static bool tb_set_nonblocking(tb_socket s)
{
    int flags = fcntl(s, F_GETFL, 0);
    if(flags == -1) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
static int tb_send(tb_socket s, const void* b, size_t l, int f)
{
    return (int)send(s, b, l, f);
}
# if defined(__APPLE__)
static void tb_set_nosigpipe(int s)
{
    int set = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (void*)&set, sizeof(set));
}
# else
#  define tb_set_nosigpipe(s) ((void)0)
# endif

#else
# error "Unsupported platform for ticbuild remoting"
#endif

/* -------------------------------------------------------------------------- */
/* Constants & data structures                                                */
/* -------------------------------------------------------------------------- */

enum { TB_INBUF_LIMIT = 1024 * 1024 };
enum { TB_OUTBUF_LIMIT = 1024 * 1024 };
enum { TB_LINE_LIMIT = 1024 * 1024 };
enum { TB_PEEK_LIMIT = 1024 * 1024 };
enum { TB_MAX_CLIENTS = 10 };

#define TB_EVENT_TRACE               (1u << 0)
#define TB_EVENT_CART_RUN            (1u << 1)
#define TB_EVENT_LUA_PROFILER_STOPPED (1u << 2)
#define TB_EVENT_SCRIPT_ERROR        (1u << 3)

typedef struct
{
    tb_socket sock;
    char* inbuf;
    size_t inlen;
    char* outbuf;
    size_t outlen;
    size_t outpos;
    uint32_t event_mask;   /* which event types this client subscribed to */
} tb_client;

struct TicbuildRemoting
{
    int port;
    ticbuild_remoting_callbacks cb;

    char* discovery_dir;
    bool global_disco;

    tb_fps_tracker fps;

    bool title_dirty;
    int last_client_count;
    char last_listen_err[128];

    uint32_t user_tic_ms10;
    uint32_t user_scn_ms10;
    uint32_t user_bdr_ms10;
    uint32_t user_total_ms10;

    uint64_t user_tic_cycles;
    uint64_t user_scn_cycles;
    uint64_t user_bdr_cycles;
    uint64_t lua_gc_mem_bytes;

    bool sock_platform_started;
    tb_socket listen_sock;

    tb_client clients[TB_MAX_CLIENTS];
    int client_count;

    uint8_t* tmpbytes;
    size_t tmpbytes_cap;

    uint8_t* script_error_last;
    size_t script_error_last_len;
    int next_event_id;
};

static void tb_queue_output(tb_client* client, const char* s, size_t n);

static void tb_mark_title_dirty(TicbuildRemoting* ctx)
{
    if(ctx) ctx->title_dirty = true;
}

/* Map an event-type name to its bitmask; returns 0 for unknown types. */
static uint32_t tb_event_mask_for_type(const char* s, size_t n)
{
    if(n == 5 && memcmp(s, "trace", 5) == 0) return TB_EVENT_TRACE;
    if(n == 8 && memcmp(s, "cart_run", 8) == 0) return TB_EVENT_CART_RUN;
    if(n == 20 && memcmp(s, "lua_profiler_stopped", 20) == 0) return TB_EVENT_LUA_PROFILER_STOPPED;
    if(n == 12 && memcmp(s, "script_error", 12) == 0) return TB_EVENT_SCRIPT_ERROR;
    return 0;
}

/* Parse a "|"-separated list of event-type names into a bitmask. */
static uint32_t tb_parse_event_mask(const char* s, size_t n, char* err, size_t errcap)
{
    uint32_t mask = 0;
    size_t i = 0;
    while(i < n)
    {
        size_t start = i;
        while(i < n && s[i] != '|') i++;
        uint32_t bit = tb_event_mask_for_type(s + start, i - start);
        if(!bit)
        {
            tb_set_err(err, errcap, "unknown event type");
            return 0;
        }
        mask |= bit;
        if(i < n && s[i] == '|') i++;
    }
    return mask;
}

/* Queue an event line to one client. `data` is already wire-encoded. */
static void tb_queue_event(tb_client* client, int id, const char* event_type, const char* data)
{
    char line[2048];
    int n;
    if(data && data[0])
        n = snprintf(line, sizeof line, "%d %s %s\n", id, event_type, data);
    else
        n = snprintf(line, sizeof line, "%d %s\n", id, event_type);

    if(n < 0 || (size_t)n >= sizeof line) return;
    tb_queue_output(client, line, (size_t)n);
}

/* -------------------------------------------------------------------------- */
/* Public API: FPS / stats / title                                            */
/* -------------------------------------------------------------------------- */

void ticbuild_remoting_on_frame(TicbuildRemoting* ctx, uint64_t counter, uint64_t freq)
{
    if(!ctx) return;
    if(tb_fps_on_frame(&ctx->fps, counter, freq))
        tb_mark_title_dirty(ctx);
}

int ticbuild_remoting_get_fps(const TicbuildRemoting* ctx)
{
    return ctx ? tb_fps_get(&ctx->fps) : 0;
}

void ticbuild_remoting_set_user_time_ms10(TicbuildRemoting* ctx,
                                          uint32_t tic_ms10, uint32_t scn_ms10,
                                          uint32_t bdr_ms10, uint32_t total_ms10)
{
    if(!ctx) return;
    if(ctx->user_tic_ms10 != tic_ms10 || ctx->user_scn_ms10 != scn_ms10 ||
       ctx->user_bdr_ms10 != bdr_ms10 || ctx->user_total_ms10 != total_ms10)
    {
        ctx->user_tic_ms10 = tic_ms10;
        ctx->user_scn_ms10 = scn_ms10;
        ctx->user_bdr_ms10 = bdr_ms10;
        ctx->user_total_ms10 = total_ms10;
        tb_mark_title_dirty(ctx);
    }
}

void ticbuild_remoting_set_lua_perf(TicbuildRemoting* ctx,
                                    uint64_t tic_cycles, uint64_t scn_cycles,
                                    uint64_t bdr_cycles, uint64_t lua_gc_mem_bytes)
{
    if(!ctx) return;
    if(ctx->user_tic_cycles != tic_cycles || ctx->user_scn_cycles != scn_cycles ||
       ctx->user_bdr_cycles != bdr_cycles || ctx->lua_gc_mem_bytes != lua_gc_mem_bytes)
    {
        ctx->user_tic_cycles = tic_cycles;
        ctx->user_scn_cycles = scn_cycles;
        ctx->user_bdr_cycles = bdr_cycles;
        ctx->lua_gc_mem_bytes = lua_gc_mem_bytes;
        tb_mark_title_dirty(ctx);
    }
}

void ticbuild_remoting_get_title_info(const TicbuildRemoting* ctx, char* out, size_t outcap)
{
    if(!out || outcap == 0) return;
    out[0] = '\0';
    if(!ctx) return;

    const char* listen_state = NULL;
    char listen_buf[256];

    if(ctx->listen_sock != TB_INVALID_SOCKET)
    {
        snprintf(listen_buf, sizeof listen_buf, "listening on 127.0.0.1:%d (%d clients)",
                 ctx->port, ctx->client_count);
        listen_state = listen_buf;
    }
    else if(ctx->last_listen_err[0])
    {
        snprintf(listen_buf, sizeof listen_buf, "remoting error: %s", ctx->last_listen_err);
        listen_state = listen_buf;
    }
    else
    {
        listen_state = "remoting not listening";
    }

    char ticbuf[32], scnbuf[32], totbuf[32], luabuf[32];
    uint64_t scn_bdr_cycles = ctx->user_scn_cycles + ctx->user_bdr_cycles;

    tb_format_kc1(ticbuf, sizeof ticbuf, ctx->user_tic_cycles);
    tb_format_kc1(scnbuf, sizeof scnbuf, scn_bdr_cycles);
    tb_format_kc1(totbuf, sizeof totbuf, ctx->user_tic_cycles + scn_bdr_cycles);
    tb_format_kb1(luabuf, sizeof luabuf, ctx->lua_gc_mem_bytes);

    snprintf(out, outcap,
             "FPS: %d | TIC %sk SCN+BDR %sk TOT %sk | LUA %sKB | %s",
             tb_fps_get(&ctx->fps), ticbuf, scnbuf, totbuf, luabuf, listen_state);
}

bool ticbuild_remoting_take_title_dirty(TicbuildRemoting* ctx)
{
    if(!ctx) return false;
    bool v = ctx->title_dirty;
    ctx->title_dirty = false;
    return v;
}

bool ticbuild_remoting_set_script_error_last(TicbuildRemoting* ctx,
                                             const uint8_t* json, size_t len)
{
    if(!ctx || !json || len == 0) return false;

    uint8_t* copy = (uint8_t*)malloc(len);
    if(!copy) return false;
    memcpy(copy, json, len);

    free(ctx->script_error_last);
    ctx->script_error_last = copy;
    ctx->script_error_last_len = len;
    return true;
}

int ticbuild_remoting_emit_event(TicbuildRemoting* ctx, const char* event_type, const char* data)
{
    if(!ctx || !event_type) return 0;

    uint32_t bit = tb_event_mask_for_type(event_type, strlen(event_type));
    if(!bit) return 0;

    int delivered = 0;
    for(int i = 0; i < TB_MAX_CLIENTS; i++)
    {
        tb_client* client = &ctx->clients[i];
        if(client->sock == TB_INVALID_SOCKET) continue;
        if(!(client->event_mask & bit)) continue;

        if(ctx->next_event_id >= -1) ctx->next_event_id = -1;
        int id = ctx->next_event_id--;

        tb_queue_event(client, id, event_type, data ? data : "");
        delivered++;
    }
    return delivered ? -1 : 0;
}

/* -------------------------------------------------------------------------- */
/* Binary literal parsing                                                     */
/* -------------------------------------------------------------------------- */

static bool tb_parse_bytes(const char* s, size_t n, uint8_t** out, size_t* outlen,
                           TicbuildRemoting* ctx, char* err, size_t errcap)
{
    if(n < 2 || s[0] != '<' || s[n - 1] != '>')
    {
        tb_set_err(err, errcap, "invalid binary literal");
        return false;
    }

    char* hex = (char*)malloc(n);
    if(!hex) { tb_set_err(err, errcap, "out of memory"); return false; }

    size_t hn = 0;
    for(size_t i = 1; i + 1 < n; i++)
    {
        char c = s[i];
        if(c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if(!isxdigit((unsigned char)c))
        {
            free(hex);
            tb_set_err(err, errcap, "binary contains non-hex");
            return false;
        }
        hex[hn++] = c;
    }

    if(hn % 2 != 0)
    {
        free(hex);
        tb_set_err(err, errcap, "binary hex digit count must be even");
        return false;
    }

    size_t bytes = hn / 2;
    if(bytes > TB_PEEK_LIMIT)
    {
        free(hex);
        tb_set_err(err, errcap, "binary too large");
        return false;
    }

    if(ctx->tmpbytes_cap < bytes)
    {
        uint8_t* nb = (uint8_t*)realloc(ctx->tmpbytes, bytes);
        if(!nb)
        {
            free(hex);
            tb_set_err(err, errcap, "out of memory");
            return false;
        }
        ctx->tmpbytes = nb;
        ctx->tmpbytes_cap = bytes;
    }

    for(size_t i = 0; i < bytes; i++)
    {
        uint8_t hi, lo;
        if(!tb_hex_nibble(hex[i * 2], &hi) || !tb_hex_nibble(hex[i * 2 + 1], &lo))
        {
            free(hex);
            tb_set_err(err, errcap, "invalid hex");
            return false;
        }
        ctx->tmpbytes[i] = (uint8_t)((hi << 4) | lo);
    }

    free(hex);
    *out = ctx->tmpbytes;
    *outlen = bytes;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Output buffering / response helpers                                        */
/* -------------------------------------------------------------------------- */

static void tb_queue_output(tb_client* client, const char* s, size_t n)
{
    if(n == 0) return;
    if(client->outlen + n > TB_OUTBUF_LIMIT) return;

    char* nb = (char*)realloc(client->outbuf, client->outlen + n);
    if(!nb) return;
    memcpy(nb + client->outlen, s, n);
    client->outbuf = nb;
    client->outlen += n;
}

static void tb_send_response_str(tb_client* client, int64_t id, bool ok, const char* data)
{
    char line[2048];
    if(ok)
    {
        if(data && data[0])
            snprintf(line, sizeof line, "%lld OK %s\n", (long long)id, data);
        else
            snprintf(line, sizeof line, "%lld OK\n", (long long)id);
    }
    else
    {
        char esc[1500];
        const char* msg = data ? data : "error";
        (void)tb_escape_string(msg, strlen(msg), esc, sizeof esc);
        snprintf(line, sizeof line, "%lld ERR \"%s\"\n", (long long)id, esc);
    }
    tb_queue_output(client, line, strlen(line));
}

static void tb_send_response_bytes(tb_client* client, int64_t id, const uint8_t* bytes, size_t n)
{
    size_t cap = 64 + n * 3 + 4;
    char* line = (char*)malloc(cap);
    if(!line) { tb_send_response_str(client, id, false, "out of memory"); return; }

    size_t pos = 0;
    pos += (size_t)snprintf(line + pos, cap - pos, "%lld OK <", (long long)id);
    for(size_t i = 0; i < n; i++)
    {
        if(i) line[pos++] = ' ';
        pos += (size_t)snprintf(line + pos, cap - pos, "%02x", (unsigned)bytes[i]);
    }
    pos += (size_t)snprintf(line + pos, cap - pos, ">\n");

    tb_queue_output(client, line, pos);
    free(line);
}

/* -------------------------------------------------------------------------- */
/* Socket init / accept / read / flush                                        */
/* -------------------------------------------------------------------------- */

static bool tb_socket_init(TicbuildRemoting* ctx, char* err, size_t errcap)
{
    if(!ctx->sock_platform_started)
    {
        if(!tb_sock_init_platform(ctx))
        {
            tb_set_err(err, errcap, "platform socket init failed");
            return false;
        }
        ctx->sock_platform_started = true;
    }

    if(ctx->listen_sock != TB_INVALID_SOCKET) return true;

    tb_socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s == TB_INVALID_SOCKET)
    {
        tb_set_err(err, errcap, "socket() failed");
        return false;
    }

#if defined(__APPLE__)
    tb_set_nosigpipe((int)s);
#endif

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    if(!tb_set_nonblocking(s))
    {
        tb_close_socket(s);
        tb_set_err(err, errcap, "failed to set nonblocking");
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)ctx->port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if(bind(s, (struct sockaddr*)&addr, sizeof addr) != 0)
    {
        tb_close_socket(s);
        tb_set_err(err, errcap, "bind() failed (port in use?)");
        return false;
    }

    if(listen(s, TB_MAX_CLIENTS) != 0)
    {
        tb_close_socket(s);
        tb_set_err(err, errcap, "listen() failed");
        return false;
    }

    ctx->listen_sock = s;

    /* Best-effort discovery file creation; don't block remoting on failure. */
    tb_discovery_start(ctx->port, ctx->discovery_dir, ctx->global_disco, NULL, 0);
    return true;
}

static void tb_disconnect_client(TicbuildRemoting* ctx, int index)
{
    if(index < 0 || index >= TB_MAX_CLIENTS) return;

    tb_client* client = &ctx->clients[index];
    if(client->sock != TB_INVALID_SOCKET)
    {
        tb_close_socket(client->sock);
        client->sock = TB_INVALID_SOCKET;
        if(ctx->client_count > 0) ctx->client_count--;
    }

    tb_mark_title_dirty(ctx);

    client->inlen = 0;
    client->outlen = 0;
    client->outpos = 0;
    client->event_mask = 0;
    free(client->inbuf);
    free(client->outbuf);
    client->inbuf = NULL;
    client->outbuf = NULL;
}

static void tb_accept_client(TicbuildRemoting* ctx)
{
    struct sockaddr_in addr;
    tb_socklen alen = (tb_socklen)sizeof addr;

    for(;;)
    {
        tb_socket c = accept(ctx->listen_sock, (struct sockaddr*)&addr, &alen);
        if(c == TB_INVALID_SOCKET)
        {
            int err = tb_last_socket_error();
            if(tb_would_block(err)) return;
            return;
        }

        int slot = -1;
        for(int i = 0; i < TB_MAX_CLIENTS; i++)
        {
            if(ctx->clients[i].sock == TB_INVALID_SOCKET)
            { slot = i; break; }
        }

        if(slot < 0)
        {
            tb_close_socket(c);
            continue;
        }

#if defined(__APPLE__)
        tb_set_nosigpipe((int)c);
#endif
        (void)tb_set_nonblocking(c);
        ctx->clients[slot].sock = c;
        ctx->clients[slot].inlen = 0;
        ctx->clients[slot].outlen = 0;
        ctx->clients[slot].outpos = 0;
        ctx->clients[slot].event_mask = 0;
        ctx->client_count++;
        tb_mark_title_dirty(ctx);
    }
}

static bool tb_read_client(TicbuildRemoting* ctx, int index)
{
    if(index < 0 || index >= TB_MAX_CLIENTS) return false;

    tb_client* client = &ctx->clients[index];
    if(client->sock == TB_INVALID_SOCKET) return false;

    if(client->inbuf == NULL)
    {
        client->inbuf = (char*)malloc(TB_INBUF_LIMIT);
        if(!client->inbuf) return false;
    }

    for(;;)
    {
        if(client->inlen >= TB_INBUF_LIMIT)
        {
            tb_disconnect_client(ctx, index);
            return false;
        }

        int r = (int)recv(client->sock, client->inbuf + client->inlen,
                          (int)(TB_INBUF_LIMIT - client->inlen), 0);
        if(r == 0)
        {
            tb_disconnect_client(ctx, index);
            return false;
        }
        if(r < 0)
        {
            int err = tb_last_socket_error();
            if(tb_would_block(err)) break;
            tb_disconnect_client(ctx, index);
            return false;
        }

        client->inlen += (size_t)r;
    }

    return true;
}

static void tb_flush_output(TicbuildRemoting* ctx, int index)
{
    if(index < 0 || index >= TB_MAX_CLIENTS) return;

    tb_client* client = &ctx->clients[index];
    if(client->sock == TB_INVALID_SOCKET) return;
    if(client->outpos >= client->outlen) return;

    for(;;)
    {
        size_t remain = client->outlen - client->outpos;
        if(remain == 0) break;

        int r = tb_send(client->sock, client->outbuf + client->outpos,
                        remain, TB_SEND_FLAGS);
        if(r < 0)
        {
            int err = tb_last_socket_error();
            if(tb_would_block(err)) break;
            tb_disconnect_client(ctx, index);
            break;
        }

        client->outpos += (size_t)r;

        if(client->outpos == client->outlen)
        {
            client->outpos = 0;
            client->outlen = 0;
            free(client->outbuf);
            client->outbuf = NULL;
            break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Command dispatch                                                           */
/* -------------------------------------------------------------------------- */

static void tb_handle_line(TicbuildRemoting* ctx, tb_client* client,
                           const char* line, size_t n)
{
    char err[1000] = {0};

    if(n > TB_LINE_LIMIT)
    {
        tb_send_response_str(client, 0, false, "line too long");
        return;
    }

    if(!tb_is_ascii_only(line, n))
    {
        tb_send_response_str(client, 0, false, "non-ascii input");
        return;
    }

    tb_trim_whitespace(&line, &n);
    if(n == 0) return;

    const char* p = line;
    size_t len = n;

    tb_slice tok;
    if(!tb_next_token(&p, &len, &tok, err, sizeof err))
    {
        tb_send_response_str(client, 0, false, err[0] ? err : "missing id");
        return;
    }

    int64_t id;
    if(!tb_parse_int(tok, &id))
    {
        tb_send_response_str(client, 0, false, "invalid id");
        return;
    }

    if(!tb_next_token(&p, &len, &tok, err, sizeof err))
    {
        tb_send_response_str(client, id, false, "missing command");
        return;
    }

    char cmd[64];
    if(tok.len >= sizeof cmd)
    {
        tb_send_response_str(client, id, false, "command too long");
        return;
    }
    for(size_t i = 0; i < tok.len; i++)
        cmd[i] = (char)tolower((unsigned char)tok.ptr[i]);
    cmd[tok.len] = '\0';

    tb_arg args[8];
    size_t argc = 0;

    while(1)
    {
        tb_slice atok;
        if(!tb_next_token(&p, &len, &atok, err, sizeof err)) break;

        if(argc >= (sizeof args / sizeof args[0]))
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "too many args");
            return;
        }

        if(atok.len && atok.ptr[0] == '"')
        {
            char* decoded;
            size_t dlen;
            if(!tb_parse_quoted(atok.ptr, atok.len, &decoded, &dlen, err, sizeof err))
            {
                tb_free_args(args, argc);
                tb_send_response_str(client, id, false, err);
                return;
            }
            args[argc++] = (tb_arg){TB_ARG_STR, {.s = {decoded, dlen}}};
        }
        else if(atok.len && atok.ptr[0] == '<')
        {
            uint8_t* b;
            size_t blen;
            if(!tb_parse_bytes(atok.ptr, atok.len, &b, &blen, ctx, err, sizeof err))
            {
                tb_free_args(args, argc);
                tb_send_response_str(client, id, false, err);
                return;
            }
            args[argc++] = (tb_arg){TB_ARG_BYTES, {.b = {b, blen}}};
        }
        else
        {
            int64_t v;
            if(!tb_parse_int(atok, &v))
            {
                tb_free_args(args, argc);
                tb_send_response_str(client, id, false, "invalid number");
                return;
            }
            args[argc++] = (tb_arg){TB_ARG_INT, {.i = v}};
        }
    }

    /* --- Dispatch --- */

    if(strcmp(cmd, "ping") == 0)
    {
        tb_free_args(args, argc);
        tb_send_response_str(client, id, true, "PONG");
        return;
    }

    if(strcmp(cmd, "hello") == 0)
    {
        char buf[256] = {0};
        if(ctx->cb.hello) ctx->cb.hello(ctx->cb.userdata, buf, sizeof buf);
        else snprintf(buf, sizeof buf, "TIC-80 remoting %s", TB_REMOTING_PROTOCOL_VERSION_STRING);

        char esc[300];
        (void)tb_escape_string(buf, strlen(buf), esc, sizeof esc);
        char data[340];
        snprintf(data, sizeof data, "\"%s\"", esc);

        tb_free_args(args, argc);
        tb_send_response_str(client, id, true, data);
        return;
    }

    if(strcmp(cmd, "sync") == 0)
    {
        if(argc != 1 || args[0].type != TB_ARG_INT)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> sync <flags>");
            return;
        }
        if(!ctx->cb.sync)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "sync not supported");
            return;
        }
        bool ok = ctx->cb.sync(ctx->cb.userdata, (uint32_t)args[0].v.i, err, sizeof err);
        tb_free_args(args, argc);
        tb_send_response_str(client, id, ok, ok ? NULL : err);
        return;
    }

    if(strcmp(cmd, "poke") == 0)
    {
        if(argc != 2 || args[0].type != TB_ARG_INT || args[1].type != TB_ARG_BYTES)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> poke <addr> <data>");
            return;
        }
        if(!ctx->cb.poke)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "poke not supported");
            return;
        }
        bool ok = ctx->cb.poke(ctx->cb.userdata, (uint32_t)args[0].v.i,
                               args[1].v.b.ptr, args[1].v.b.len, err, sizeof err);
        tb_free_args(args, argc);
        tb_send_response_str(client, id, ok, ok ? NULL : err);
        return;
    }

    if(strcmp(cmd, "peek") == 0)
    {
        if(argc != 2 || args[0].type != TB_ARG_INT || args[1].type != TB_ARG_INT)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> peek <addr> <size>");
            return;
        }
        if(!ctx->cb.peek)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "peek not supported");
            return;
        }
        uint32_t size = (uint32_t)args[1].v.i;
        if(size == 0 || size > TB_PEEK_LIMIT)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "invalid size");
            return;
        }
        uint8_t* tmp = (uint8_t*)malloc(size);
        if(!tmp)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "out of memory");
            return;
        }
        bool ok = ctx->cb.peek(ctx->cb.userdata, (uint32_t)args[0].v.i, size, tmp,
                               err, sizeof err);
        tb_free_args(args, argc);
        if(ok) tb_send_response_bytes(client, id, tmp, size);
        else tb_send_response_str(client, id, false, err);
        free(tmp);
        return;
    }

    if(strcmp(cmd, "load") == 0)
    {
        if(argc != 2 || args[0].type != TB_ARG_STR || args[1].type != TB_ARG_INT)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> load \"path\" <run:1|0>");
            return;
        }
        if(!ctx->cb.load)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "load not supported");
            return;
        }
        bool run = args[1].v.i ? true : false;
        bool ok = ctx->cb.load(ctx->cb.userdata, args[0].v.s.ptr, run, err, sizeof err);
        tb_free_args(args, argc);
        tb_send_response_str(client, id, ok, ok ? NULL : err);
        return;
    }

    if(strcmp(cmd, "restart") == 0)
    {
        if(argc != 0)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> restart");
            return;
        }
        if(!ctx->cb.restart)
        {
            tb_send_response_str(client, id, false, "restart not supported");
            return;
        }
        bool ok = ctx->cb.restart(ctx->cb.userdata, err, sizeof err);
        tb_send_response_str(client, id, ok, ok ? NULL : err);
        return;
    }

    if(strcmp(cmd, "quit") == 0)
    {
        if(argc != 0)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> quit");
            return;
        }
        if(!ctx->cb.quit)
        {
            tb_send_response_str(client, id, false, "quit not supported");
            return;
        }
        bool ok = ctx->cb.quit(ctx->cb.userdata, err, sizeof err);
        tb_send_response_str(client, id, ok, ok ? NULL : err);
        return;
    }

    if(strcmp(cmd, "eval") == 0)
    {
        if(argc != 1 || args[0].type != TB_ARG_STR)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> eval \"code\"");
            return;
        }
        if(!ctx->cb.eval)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "eval not supported");
            return;
        }
        bool ok = ctx->cb.eval(ctx->cb.userdata, args[0].v.s.ptr, err, sizeof err);
        tb_free_args(args, argc);
        tb_send_response_str(client, id, ok, ok ? NULL : err);
        return;
    }

    if(strcmp(cmd, "evalexpr") == 0)
    {
        if(argc != 1 || args[0].type != TB_ARG_STR)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> evalexpr \"expr\"");
            return;
        }
        if(!ctx->cb.eval_expr)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "evalexpr not supported");
            return;
        }
        char out[1024];
        out[0] = '\0';
        bool ok = ctx->cb.eval_expr(ctx->cb.userdata, args[0].v.s.ptr, out, sizeof out,
                                    err, sizeof err);
        tb_free_args(args, argc);
        tb_send_response_str(client, id, ok, ok ? out : err);
        return;
    }

    if(strcmp(cmd, "listglobals") == 0)
    {
        if(argc != 0)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> listglobals");
            return;
        }
        if(!ctx->cb.list_globals)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "listglobals not supported");
            return;
        }
        size_t outcap = TB_OUTBUF_LIMIT > 128 ? (TB_OUTBUF_LIMIT - 128) : TB_OUTBUF_LIMIT;
        char* out = (char*)malloc(outcap);
        if(!out)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "out of memory");
            return;
        }
        out[0] = '\0';
        bool ok = ctx->cb.list_globals(ctx->cb.userdata, out, outcap, err, sizeof err);
        tb_free_args(args, argc);
        tb_send_response_str(client, id, ok, ok ? out : err);
        free(out);
        return;
    }

    if(strcmp(cmd, "getfps") == 0)
    {
        if(argc != 0)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> getfps");
            return;
        }
        char data[32];
        snprintf(data, sizeof data, "%d", ticbuild_remoting_get_fps(ctx));
        tb_free_args(args, argc);
        tb_send_response_str(client, id, true, data);
        return;
    }

    if(strcmp(cmd, "perf") == 0)
    {
        if(argc != 0)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> perf");
            return;
        }
        char fpsbuf[32];
        char data[512];
        tb_format_fps(fpsbuf, sizeof fpsbuf, &ctx->fps);
        snprintf(data, sizeof data,
                 "client_count=%d,fps=%s,tic_cycles=%llu,scn_cycles=%llu,bdr_cycles=%llu,lua_gc_mem=%llu",
                 ctx->client_count, fpsbuf,
                 (unsigned long long)ctx->user_tic_cycles,
                 (unsigned long long)ctx->user_scn_cycles,
                 (unsigned long long)ctx->user_bdr_cycles,
                 (unsigned long long)ctx->lua_gc_mem_bytes);
        tb_free_args(args, argc);
        tb_send_response_str(client, id, true, data);
        return;
    }

    if(strcmp(cmd, "cartpath") == 0)
    {
        if(argc != 0)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> cartpath");
            return;
        }
        if(!ctx->cb.cart_path)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "cartpath not supported");
            return;
        }
        char raw[1024] = {0};
        bool ok = ctx->cb.cart_path(ctx->cb.userdata, raw, sizeof raw, err, sizeof err);
        tb_free_args(args, argc);
        if(!ok) { tb_send_response_str(client, id, false, err); return; }
        size_t rawlen = strlen(raw);
        size_t ecap = tb_escape_string_len(raw, rawlen) + 1;
        char* esc = (char*)malloc(ecap);
        if(!esc) { tb_send_response_str(client, id, false, "out of memory"); return; }
        (void)tb_escape_string(raw, rawlen, esc, ecap);
        size_t dlen = ecap + 1;
        char* data = (char*)malloc(dlen);
        if(!data) { free(esc); tb_send_response_str(client, id, false, "out of memory"); return; }
        snprintf(data, dlen, "\"%s\"", esc);
        tb_send_response_str(client, id, true, data);
        free(esc);
        free(data);
        return;
    }

    if(strcmp(cmd, "fs") == 0)
    {
        if(argc != 0)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> fs");
            return;
        }
        if(!ctx->cb.fs_path)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "fs not supported");
            return;
        }
        char raw[1024] = {0};
        bool ok = ctx->cb.fs_path(ctx->cb.userdata, raw, sizeof raw, err, sizeof err);
        tb_free_args(args, argc);
        if(!ok) { tb_send_response_str(client, id, false, err); return; }
        size_t rawlen = strlen(raw);
        size_t ecap = tb_escape_string_len(raw, rawlen) + 1;
        char* esc = (char*)malloc(ecap);
        if(!esc) { tb_send_response_str(client, id, false, "out of memory"); return; }
        (void)tb_escape_string(raw, rawlen, esc, ecap);
        size_t dlen = ecap + 1;
        char* data = (char*)malloc(dlen);
        if(!data) { free(esc); tb_send_response_str(client, id, false, "out of memory"); return; }
        snprintf(data, dlen, "\"%s\"", esc);
        tb_send_response_str(client, id, true, data);
        free(esc);
        free(data);
        return;
    }

    if(strcmp(cmd, "metadata") == 0)
    {
        if(argc != 1 || args[0].type != TB_ARG_STR)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> metadata \"key\"");
            return;
        }
        if(!ctx->cb.metadata)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "metadata not supported");
            return;
        }
        char raw[1024] = {0};
        bool ok = ctx->cb.metadata(ctx->cb.userdata, args[0].v.s.ptr, raw, sizeof raw,
                                   err, sizeof err);
        tb_free_args(args, argc);
        if(!ok) { tb_send_response_str(client, id, false, err); return; }
        size_t rawlen = strlen(raw);
        size_t ecap = tb_escape_string_len(raw, rawlen) + 1;
        char* esc = (char*)malloc(ecap);
        if(!esc) { tb_send_response_str(client, id, false, "out of memory"); return; }
        (void)tb_escape_string(raw, rawlen, esc, ecap);
        size_t dlen = ecap + 1;
        char* data = (char*)malloc(dlen);
        if(!data) { free(esc); tb_send_response_str(client, id, false, "out of memory"); return; }
        snprintf(data, dlen, "\"%s\"", esc);
        tb_send_response_str(client, id, true, data);
        free(esc);
        free(data);
        return;
    }

    if(strcmp(cmd, "event_subscribe") == 0)
    {
        if(argc != 2 || args[0].type != TB_ARG_STR || args[1].type != TB_ARG_INT)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> event_subscribe \"types\" <enabled:1|0>");
            return;
        }

        uint32_t mask = tb_parse_event_mask(args[0].v.s.ptr, args[0].v.s.len, err, sizeof err);
        if(!mask)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, err[0] ? err : "invalid event types");
            return;
        }

        if(args[1].v.i)
            client->event_mask |= mask;
        else
            client->event_mask &= ~mask;

        tb_free_args(args, argc);
        tb_send_response_str(client, id, true, NULL);
        return;
    }

    if(strcmp(cmd, "script_error_last") == 0)
    {
        if(argc != 0)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "usage: <id> script_error_last");
            return;
        }

        if(!ctx->script_error_last)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "no script error recorded");
            return;
        }

        size_t n = ctx->script_error_last_len;
        size_t cap = 64 + n * 3 + 4;
        char* line = (char*)malloc(cap);
        if(!line)
        {
            tb_free_args(args, argc);
            tb_send_response_str(client, id, false, "out of memory");
            return;
        }

        size_t pos = 0;
        pos += (size_t)snprintf(line + pos, cap - pos, "%lld OK <", (long long)id);
        for(size_t i = 0; i < n; i++)
        {
            if(i) line[pos++] = ' ';
            pos += (size_t)snprintf(line + pos, cap - pos, "%02x",
                                    (unsigned)ctx->script_error_last[i]);
        }
        pos += (size_t)snprintf(line + pos, cap - pos, ">\n");

        tb_free_args(args, argc);
        tb_queue_output(client, line, pos);
        free(line);
        return;
    }

    tb_free_args(args, argc);
    tb_send_response_str(client, id, false, "unknown command");
}

/* -------------------------------------------------------------------------- */
/* Input processing                                                           */
/* -------------------------------------------------------------------------- */

static void tb_process_input(TicbuildRemoting* ctx, int index)
{
    if(index < 0 || index >= TB_MAX_CLIENTS) return;

    tb_client* client = &ctx->clients[index];
    if(client->inlen == 0) return;

    size_t start = 0;
    for(size_t i = 0; i < client->inlen; i++)
    {
        if(client->inbuf[i] == '\n')
        {
            size_t linelen = i - start;
            if(linelen && client->inbuf[start + linelen - 1] == '\r')
                linelen--;

            tb_handle_line(ctx, client, client->inbuf + start, linelen);
            start = i + 1;
        }
    }

    if(start > 0)
    {
        memmove(client->inbuf, client->inbuf + start, client->inlen - start);
        client->inlen -= start;
    }
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

TicbuildRemoting* ticbuild_remoting_create(int port, const char* session_dir,
                                           bool global_disco,
                                           const ticbuild_remoting_callbacks* callbacks)
{
    if(port <= 0) return NULL;

    TicbuildRemoting* ctx = (TicbuildRemoting*)calloc(1, sizeof(TicbuildRemoting));
    if(!ctx) return NULL;

    ctx->port = port;
    if(callbacks) ctx->cb = *callbacks;
    if(session_dir && session_dir[0]) ctx->discovery_dir = strdup(session_dir);
    ctx->global_disco = global_disco;

    tb_fps_init(&ctx->fps);

    ctx->listen_sock = TB_INVALID_SOCKET;
    ctx->client_count = 0;
    ctx->last_client_count = 0;
    ctx->next_event_id = -1;
    for(int i = 0; i < TB_MAX_CLIENTS; i++)
    {
        ctx->clients[i].sock = TB_INVALID_SOCKET;
        ctx->clients[i].inbuf = NULL;
        ctx->clients[i].inlen = 0;
        ctx->clients[i].outbuf = NULL;
        ctx->clients[i].outlen = 0;
        ctx->clients[i].outpos = 0;
        ctx->clients[i].event_mask = 0;
    }

    char err[128];
    if(!tb_socket_init(ctx, err, sizeof err))
    {
        ticbuild_remoting_close(ctx);
        return NULL;
    }

    return ctx;
}

void ticbuild_remoting_close(TicbuildRemoting* ctx)
{
    if(!ctx) return;

    for(int i = 0; i < TB_MAX_CLIENTS; i++)
        tb_disconnect_client(ctx, i);

    tb_discovery_stop();

    if(ctx->listen_sock != TB_INVALID_SOCKET)
    {
        tb_close_socket(ctx->listen_sock);
        ctx->listen_sock = TB_INVALID_SOCKET;
    }

    if(ctx->sock_platform_started)
        tb_sock_shutdown_platform();

    free(ctx->tmpbytes);
    free(ctx->script_error_last);
    free(ctx->discovery_dir);
    free(ctx);
}

void ticbuild_remoting_tick(TicbuildRemoting* ctx)
{
    if(!ctx) return;

    char err[128];
    if(!tb_socket_init(ctx, err, sizeof err))
    {
        tb_discovery_stop();
        tb_set_err(ctx->last_listen_err, sizeof ctx->last_listen_err,
                   err[0] ? err : "socket init failed");
        tb_mark_title_dirty(ctx);
        return;
    }

    if(ctx->last_listen_err[0])
    {
        ctx->last_listen_err[0] = '\0';
        tb_mark_title_dirty(ctx);
    }

    tb_accept_client(ctx);

    for(int i = 0; i < TB_MAX_CLIENTS; i++)
    {
        if(ctx->clients[i].sock == TB_INVALID_SOCKET) continue;
        tb_read_client(ctx, i);
        tb_process_input(ctx, i);
        tb_flush_output(ctx, i);
    }

    if(ctx->client_count != ctx->last_client_count)
    {
        ctx->last_client_count = ctx->client_count;
        tb_mark_title_dirty(ctx);
    }
}
