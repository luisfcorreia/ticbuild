#ifndef TICBUILD_REMOTING_FPS_H
#define TICBUILD_REMOTING_FPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint64_t last_counter;
    uint64_t frame_count;
    uint64_t window_start;
    int fps;
} tb_fps_tracker;

void tb_fps_init(tb_fps_tracker* t);
/* Returns true when the FPS value has been updated (once per second). */
bool tb_fps_on_frame(tb_fps_tracker* t, uint64_t counter, uint64_t freq);
int tb_fps_get(const tb_fps_tracker* t);

/* Format fps as a string like "60.0"; returns required length excl null. */
size_t tb_format_fps(char* out, size_t outcap, const tb_fps_tracker* t);

#endif /* TICBUILD_REMOTING_FPS_H */
