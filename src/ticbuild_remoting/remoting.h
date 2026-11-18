#ifndef TICBUILD_REMOTING_H
#define TICBUILD_REMOTING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TB_REMOTING_PROTOCOL_VERSION_STRING "1.0"
#define TB_REMOTING_PROTOCOL_VERSION 1

typedef struct TicbuildRemoting TicbuildRemoting;

typedef struct
{
    void* userdata;

    /* hello: fills out with a human-readable greeting (returns true on success) */
    bool (*hello)(void* userdata, char* out, size_t outcap);

    /* sync: apply flags; returns true on success, err filled on failure */
    bool (*sync)(void* userdata, uint32_t flags, char* err, size_t errcap);

    /* poke: write bytes at address; returns true on success */
    bool (*poke)(void* userdata, uint32_t addr, const uint8_t* data, size_t len,
                 char* err, size_t errcap);

    /* peek: read size bytes at address into tmp; returns true on success */
    bool (*peek)(void* userdata, uint32_t addr, uint32_t size, uint8_t* tmp,
                 char* err, size_t errcap);

    /* load: load cartridge path, optionally run; returns true on success */
    bool (*load)(void* userdata, const char* path, bool run, char* err, size_t errcap);

    /* restart: restart the cartridge; returns true on success */
    bool (*restart)(void* userdata, char* err, size_t errcap);

    /* quit: exit TIC-80; returns true on success */
    bool (*quit)(void* userdata, char* err, size_t errcap);

    /* eval: run Lua code; returns true on success */
    bool (*eval)(void* userdata, const char* code, char* err, size_t errcap);

    /* eval_expr: evaluate expression, fill out buffer; returns true on success */
    bool (*eval_expr)(void* userdata, const char* expr, char* out, size_t outcap,
                      char* err, size_t errcap);

    /* list_globals: fill out buffer with global names; returns true on success */
    bool (*list_globals)(void* userdata, char* out, size_t outcap,
                         char* err, size_t errcap);

    /* cart_path: fill out buffer with current cart path; returns true on success */
    bool (*cart_path)(void* userdata, char* out, size_t outcap,
                      char* err, size_t errcap);

    /* fs_path: fill out buffer with save directory path; returns true on success */
    bool (*fs_path)(void* userdata, char* out, size_t outcap,
                    char* err, size_t errcap);

    /* metadata: look up metadata key, fill out buffer; returns true on success */
    bool (*metadata)(void* userdata, const char* key, char* out, size_t outcap,
                     char* err, size_t errcap);
} ticbuild_remoting_callbacks;

/* Create a remoting server listening on `port` (127.0.0.1 only).
 * session_dir: directory to write discovery JSON session files (may be NULL).
 * global_disco: whether to also write to the global/OS session dir.
 * Returns NULL on failure. */
TicbuildRemoting* ticbuild_remoting_create(int port, const char* session_dir,
                                           bool global_disco,
                                           const ticbuild_remoting_callbacks* callbacks);

void ticbuild_remoting_close(TicbuildRemoting* ctx);

/* Call once per frame / main loop iteration. Accepts clients, reads input,
 * dispatches commands, flushes output. */
void ticbuild_remoting_tick(TicbuildRemoting* ctx);

/* Called each frame with the frame counter and frequency for FPS tracking. */
void ticbuild_remoting_on_frame(TicbuildRemoting* ctx, uint64_t counter, uint64_t freq);

int ticbuild_remoting_get_fps(const TicbuildRemoting* ctx);

void ticbuild_remoting_set_user_time_ms10(TicbuildRemoting* ctx,
                                          uint32_t tic_ms10, uint32_t scn_ms10,
                                          uint32_t bdr_ms10, uint32_t total_ms10);

void ticbuild_remoting_set_lua_perf(TicbuildRemoting* ctx,
                                    uint64_t tic_cycles, uint64_t scn_cycles,
                                    uint64_t bdr_cycles, uint64_t lua_gc_mem_bytes);

/* Fill `out` (capacity `outcap`) with a status string; returns after writing. */
void ticbuild_remoting_get_title_info(const TicbuildRemoting* ctx, char* out, size_t outcap);

/* Consume and return the "title dirty" flag. */
bool ticbuild_remoting_take_title_dirty(TicbuildRemoting* ctx);

/* Emit an event to all subscribed clients (wire format: `-<id> <type> <data>`).
 * `data` must already be wire-encoded (e.g. "\"text\"" or a "<hex>" literal).
 * Returns the negative event id used, or 0 if nobody was subscribed. */
int ticbuild_remoting_emit_event(TicbuildRemoting* ctx, const char* event_type, const char* data);

/* Store the most recent script-error JSON payload, returned by the
 * `script_error_last` command as a binary literal. */
bool ticbuild_remoting_set_script_error_last(TicbuildRemoting* ctx,
                                             const uint8_t* json, size_t len);

#endif /* TICBUILD_REMOTING_H */
