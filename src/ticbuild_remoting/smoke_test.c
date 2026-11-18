#include "ticbuild_remoting/remoting.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(__TIC_WINDOWS__)
# include <winsock2.h>
# include <ws2tcpip.h>
typedef SOCKET test_socket;
# define TEST_INVALID_SOCKET INVALID_SOCKET
# define test_close(s) closesocket(s)
static void test_platform_init(void) { WSADATA w; (void)WSAStartup(MAKEWORD(2, 2), &w); }
static void test_platform_shutdown(void) { WSACleanup(); }
#else
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <unistd.h>
# include <fcntl.h>
# include <signal.h>
typedef int test_socket;
# define TEST_INVALID_SOCKET (-1)
# define test_close(s) close(s)
static void test_platform_init(void) { signal(SIGPIPE, SIG_IGN); }
static void test_platform_shutdown(void) { }
#endif

static volatile int g_quit = 0;
static int g_hello_count = 0;
static int g_load_count = 0;

#if !defined(_WIN32) && !defined(__TIC_WINDOWS__)
/* Verify the discovery session file exists with the expected schema, then
 * that it is removed on close. Returns 0 on success. */
static int check_session_file(const char* dir, bool expect_exists)
{
    if(!dir || !*dir) return 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/tic80-remote.%d.json", dir, (int)getpid());
    FILE* f = fopen(path, "r");
    if(expect_exists)
    {
        if(!f) { fprintf(stderr, "FAIL: session file missing: %s\n", path); return 1; }
        char buf[1024];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        fclose(f);
        if(strstr(buf, "\"pid\"") == NULL ||
           strstr(buf, "\"host\": \"127.0.0.1\"") == NULL ||
           strstr(buf, "\"port\"") == NULL ||
           strstr(buf, "\"startedAt\"") == NULL ||
           strstr(buf, "\"remotingVersion\"") == NULL)
        {
            fprintf(stderr, "FAIL: session file has invalid schema: %s\n", buf);
            return 1;
        }
        printf("session file created: %s\n", path);
    }
    else
    {
        if(f) { fclose(f); fprintf(stderr, "FAIL: session file not removed: %s\n", path); return 1; }
        printf("session file removed: %s\n", path);
    }
    return 0;
}
#endif

static bool cb_hello(void* userdata, char* out, size_t outcap)
{
    (void)userdata;
    snprintf(out, outcap, "smoke-test TIC-80 remoting %s", TB_REMOTING_PROTOCOL_VERSION_STRING);
    g_hello_count++;
    return true;
}

static bool cb_load(void* userdata, const char* path, bool run, char* err, size_t errcap)
{
    (void)userdata; (void)run;
    snprintf(err, errcap, "load ok: %s", path);
    g_load_count++;
    return true;
}

static bool cb_quit(void* userdata, char* err, size_t errcap)
{
    (void)userdata; (void)err; (void)errcap;
    g_quit = 1;
    return true;
}

static bool cb_evalexpr(void* userdata, const char* expr, char* out, size_t outcap,
                        char* err, size_t errcap)
{
    (void)userdata; (void)err; (void)errcap;
    if(strcmp(expr, "1+2") == 0)
    {
        snprintf(out, outcap, "3");
        return true;
    }
    snprintf(err, errcap, "unknown expr");
    return false;
}

static bool send_line(test_socket s, const char* line)
{
    size_t n = strlen(line);
    const char* p = line;
    while(n > 0)
    {
        int r = (int)send(s, p, n, 0);
        if(r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static test_socket connect_to(int port)
{
    test_socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s == TEST_INVALID_SOCKET) return TEST_INVALID_SOCKET;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if(connect(s, (struct sockaddr*)&addr, sizeof addr) != 0)
    {
        test_close(s);
        return TEST_INVALID_SOCKET;
    }
    return s;
}

/* Pump the server until we've read `expected_lines` responses or time out. */
static int pump_until(TicbuildRemoting* ctx, test_socket client,
                      char* buf, size_t cap, int expected_lines, int max_ticks)
{
    size_t got = 0;
    for(int i = 0; i < max_ticks; i++)
    {
        ticbuild_remoting_tick(ctx);

        if(client != TEST_INVALID_SOCKET)
        {
            int r = (int)recv(client, buf + got, cap - got - 1, 0);
            if(r > 0)
            {
                got += (size_t)r;
                buf[got] = '\0';
                int lines = 0;
                for(size_t j = 0; j < got; j++)
                    if(buf[j] == '\n') lines++;
                if(lines >= expected_lines) return lines;
            }
        }
    }
    return -1; /* timed out */
}

int main(int argc, char** argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 19091;
    const char* session_dir = (argc > 2) ? argv[2] : NULL;

    test_platform_init();

    ticbuild_remoting_callbacks cbs;
    memset(&cbs, 0, sizeof cbs);
    cbs.hello = cb_hello;
    cbs.load = cb_load;
    cbs.quit = cb_quit;
    cbs.eval_expr = cb_evalexpr;

    TicbuildRemoting* ctx = ticbuild_remoting_create(port, session_dir, false, &cbs);
    if(!ctx)
    {
        fprintf(stderr, "FAIL: ticbuild_remoting_create(port=%d) returned NULL\n", port);
        test_platform_shutdown();
        return 1;
    }

#if !defined(_WIN32) && !defined(__TIC_WINDOWS__)
    if(check_session_file(session_dir, true) != 0)
    {
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }
#endif

    printf("READY port=%d\n", port);
    fflush(stdout);

    /* Give the listener a tick to come up. */
    ticbuild_remoting_tick(ctx);

    test_socket client = connect_to(port);
    if(client == TEST_INVALID_SOCKET)
    {
        fprintf(stderr, "FAIL: could not connect to 127.0.0.1:%d\n", port);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }

    char responses[8192];
    memset(responses, 0, sizeof responses);

    /* hello */
    send_line(client, "1 hello\n");
    if(pump_until(ctx, client, responses, sizeof responses, 1, 200) < 1)
    {
        fprintf(stderr, "FAIL: no response to hello\n");
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }
    printf("hello -> %s", responses);
    if(strstr(responses, "1 OK \"smoke-test") == NULL)
    {
        fprintf(stderr, "FAIL: unexpected hello response\n");
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }

    /* ping */
    memset(responses, 0, sizeof responses);
    send_line(client, "2 ping\n");
    if(pump_until(ctx, client, responses, sizeof responses, 1, 200) < 1 ||
       strstr(responses, "2 OK PONG") == NULL)
    {
        fprintf(stderr, "FAIL: unexpected ping response: %s\n", responses);
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }
    printf("ping -> %s", responses);

    /* evalexpr */
    memset(responses, 0, sizeof responses);
    send_line(client, "3 evalexpr \"1+2\"\n");
    if(pump_until(ctx, client, responses, sizeof responses, 1, 200) < 1 ||
       strstr(responses, "3 OK 3") == NULL)
    {
        fprintf(stderr, "FAIL: unexpected evalexpr response: %s\n", responses);
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }
    printf("evalexpr -> %s", responses);

    /* load */
    memset(responses, 0, sizeof responses);
    send_line(client, "4 load \"cart.lua\" 1\n");
    if(pump_until(ctx, client, responses, sizeof responses, 1, 200) < 1 ||
       strstr(responses, "4 OK load ok: cart.lua") == NULL)
    {
        fprintf(stderr, "FAIL: unexpected load response: %s\n", responses);
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }
    printf("load -> %s", responses);

    /* event_subscribe */
    memset(responses, 0, sizeof responses);
    send_line(client, "5 event_subscribe \"trace|script_error\" 1\n");
    if(pump_until(ctx, client, responses, sizeof responses, 1, 200) < 1 ||
       strstr(responses, "5 OK") == NULL)
    {
        fprintf(stderr, "FAIL: unexpected event_subscribe response: %s\n", responses);
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }
    printf("event_subscribe -> %s", responses);

    /* emit a trace event; expect a line with a negative id */
    memset(responses, 0, sizeof responses);
    if(ticbuild_remoting_emit_event(ctx, "trace", "\"hello from tic80\"") == 0)
    {
        fprintf(stderr, "FAIL: emit_event reported no subscribers\n");
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }
    if(pump_until(ctx, client, responses, sizeof responses, 1, 200) < 1 ||
       strstr(responses, " trace \"hello from tic80\"") == NULL ||
       responses[0] != '-')
    {
        fprintf(stderr, "FAIL: unexpected trace event: %s\n", responses);
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }
    printf("trace event -> %s", responses);

    /* script_error_last: store JSON and read it back as a binary literal */
    {
        const char* errjson = "{\"schemaVersion\":1,\"message\":\"boom\"}";
        if(!ticbuild_remoting_set_script_error_last(ctx, (const uint8_t*)errjson, strlen(errjson)))
        {
            fprintf(stderr, "FAIL: set_script_error_last returned false\n");
            test_close(client);
            ticbuild_remoting_close(ctx);
            test_platform_shutdown();
            return 1;
        }
    }
    memset(responses, 0, sizeof responses);
    send_line(client, "6 script_error_last\n");
    if(pump_until(ctx, client, responses, sizeof responses, 1, 200) < 1 ||
       strstr(responses, "6 OK <") == NULL)
    {
        fprintf(stderr, "FAIL: unexpected script_error_last response: %s\n", responses);
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }
    printf("script_error_last -> %s", responses);

    /* quit */
    memset(responses, 0, sizeof responses);
    send_line(client, "7 quit\n");
    if(pump_until(ctx, client, responses, sizeof responses, 1, 200) < 1 ||
       strstr(responses, "7 OK") == NULL)
    {
        fprintf(stderr, "FAIL: unexpected quit response: %s\n", responses);
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }
    printf("quit -> %s", responses);

    if(!g_quit || g_hello_count != 1 || g_load_count != 1)
    {
        fprintf(stderr, "FAIL: callbacks not invoked (quit=%d hello=%d load=%d)\n",
                g_quit, g_hello_count, g_load_count);
        test_close(client);
        ticbuild_remoting_close(ctx);
        test_platform_shutdown();
        return 1;
    }

    test_close(client);
    ticbuild_remoting_close(ctx);

#if !defined(_WIN32) && !defined(__TIC_WINDOWS__)
    if(check_session_file(session_dir, false) != 0)
    {
        test_platform_shutdown();
        return 1;
    }
#endif

    test_platform_shutdown();

    printf("PASS\n");
    return 0;
}
