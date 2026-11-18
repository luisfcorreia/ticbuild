#include "ticbuild_remoting/fps.h"

#include <stdio.h>

void tb_fps_init(tb_fps_tracker* t)
{
    t->last_counter = 0;
    t->frame_count = 0;
    t->window_start = 0;
    t->fps = 0;
}

bool tb_fps_on_frame(tb_fps_tracker* t, uint64_t counter, uint64_t freq)
{
    if(freq == 0) return false;
    if(t->window_start == 0) t->window_start = counter;
    t->frame_count++;

    uint64_t elapsed = counter - t->window_start;
    if(elapsed >= freq)
    {
        t->fps = (int)(t->frame_count * freq / (elapsed ? elapsed : 1));
        t->frame_count = 0;
        t->window_start = counter;
        return true;
    }
    return false;
}

int tb_fps_get(const tb_fps_tracker* t)
{
    return t->fps;
}

size_t tb_format_fps(char* out, size_t outcap, const tb_fps_tracker* t)
{
    int n = snprintf(out, outcap, "%d", t->fps);
    if(n < 0) n = 0;
    return (size_t)n;
}
