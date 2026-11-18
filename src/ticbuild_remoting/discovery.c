#include "ticbuild_remoting/discovery.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ticbuild_remoting/remoting.h"

#if defined(_WIN32) || defined(__TIC_WINDOWS__)
# include <windows.h>
# include <direct.h>
# define tb_mkdir(p) _mkdir(p)
# define tb_unlink(p) _unlink(p)
# define TB_MKDIR_EXISTS() (GetLastError() == ERROR_ALREADY_EXISTS)
static int tb_getpid(void) { return (int)GetCurrentProcessId(); }
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
# include <unistd.h>
# include <sys/stat.h>
# include <sys/types.h>
# include <errno.h>
# define tb_mkdir(p) mkdir((p), 0755)
# define tb_unlink(p) unlink(p)
# define TB_MKDIR_EXISTS() (errno == EEXIST)
static int tb_getpid(void) { return (int)getpid(); }
#endif

#define TB_SESSION_FILE_PREFIX "tic80-remote."
#define TB_SESSION_FILE_SUFFIX ".json"

static char g_session_file_paths[4][1024];
static int g_session_file_count = 0;

/* Recursively create directories (mkdir -p). */
static bool tb_mkdir_p(const char* path)
{
    if(!path || !*path) return false;

    char tmp[1024];
    size_t len = strlen(path);
    if(len >= sizeof tmp) return false;
    memcpy(tmp, path, len + 1);

    /* strip trailing slash */
    while(len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for(size_t i = 1; i < len; i++)
    {
        if(tmp[i] == '/')
        {
            char c = tmp[i];
            tmp[i] = '\0';
            if(tb_mkdir(tmp) != 0 && !TB_MKDIR_EXISTS())
            {
                tmp[i] = c;
                return false;
            }
            tmp[i] = c;
        }
    }

    if(tb_mkdir(tmp) != 0 && !TB_MKDIR_EXISTS()) return false;
    return true;
}

static void tb_iso_timestamp(char* out, size_t outcap)
{
    time_t now = time(NULL);
    struct tm tmval;
#if defined(_WIN32) || defined(__TIC_WINDOWS__)
    gmtime_s(&tmval, &now);
#else
    gmtime_r(&now, &tmval);
#endif
    strftime(out, outcap, "%Y-%m-%dT%H:%M:%SZ", &tmval);
}

static bool tb_write_session_file(const char* dir, int port, bool* wrote)
{
    *wrote = false;
    if(!dir || !*dir) return true;

    if(!tb_mkdir_p(dir)) return false;

    int pid = tb_getpid();

    char path[1024];
    snprintf(path, sizeof path, "%s/%s%d%s", dir, TB_SESSION_FILE_PREFIX, pid, TB_SESSION_FILE_SUFFIX);

    char ts[64];
    tb_iso_timestamp(ts, sizeof ts);

    FILE* f = fopen(path, "w");
    if(!f) return false;

    fprintf(f, "{\n");
    fprintf(f, "  \"pid\": %d,\n", pid);
    fprintf(f, "  \"host\": \"127.0.0.1\",\n");
    fprintf(f, "  \"port\": %d,\n", port);
    fprintf(f, "  \"startedAt\": \"%s\",\n", ts);
    fprintf(f, "  \"remotingVersion\": \"%s\"\n", TB_REMOTING_PROTOCOL_VERSION_STRING);
    fprintf(f, "}\n");
    fclose(f);

    if(g_session_file_count < (int)(sizeof g_session_file_paths / sizeof g_session_file_paths[0]))
    {
        snprintf(g_session_file_paths[g_session_file_count++], sizeof g_session_file_paths[0], "%s", path);
    }

    *wrote = true;
    return true;
}

static void tb_global_session_dir(char* out, size_t outcap)
{
    out[0] = '\0';
#if defined(_WIN32) || defined(__TIC_WINDOWS__)
    const char* localappdata = getenv("LOCALAPPDATA");
    if(localappdata && localappdata[0])
    {
        snprintf(out, outcap, "%s\\TIC-80\\remoting\\sessions", localappdata);
    }
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    if(home && home[0])
    {
        snprintf(out, outcap, "%s/Library/Application Support/TIC-80/remoting/sessions", home);
    }
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    if(xdg && xdg[0])
    {
        snprintf(out, outcap, "%s/TIC-80/remoting/sessions", xdg);
    }
    else
    {
        const char* home = getenv("HOME");
        if(home && home[0])
        {
            snprintf(out, outcap, "%s/.local/share/TIC-80/remoting/sessions", home);
        }
    }
#endif
}

bool tb_discovery_start(int port, const char* session_dir, bool global_disco,
                        const char* instance_path, size_t instance_path_len)
{
    (void)instance_path;
    (void)instance_path_len;

    tb_discovery_stop();

    bool any = false;
    bool wrote = false;

    if(session_dir && session_dir[0])
    {
        if(tb_write_session_file(session_dir, port, &wrote) && wrote) any = true;
    }

    if(global_disco)
    {
        char global_dir[1024];
        tb_global_session_dir(global_dir, sizeof global_dir);
        if(global_dir[0])
        {
            if(tb_write_session_file(global_dir, port, &wrote) && wrote) any = true;
        }
    }

    return any;
}

void tb_discovery_stop(void)
{
    for(int i = 0; i < g_session_file_count; i++)
    {
        if(g_session_file_paths[i][0])
        {
            (void)tb_unlink(g_session_file_paths[i]);
            g_session_file_paths[i][0] = '\0';
        }
    }
    g_session_file_count = 0;
}
