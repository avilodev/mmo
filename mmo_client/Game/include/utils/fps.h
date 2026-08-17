#ifndef FPS_H
#define FPS_H

/** Track frame timing, measured FPS, and a target frame rate. */
typedef struct {
    double last_time;
    double delta_time;      /**< Seconds elapsed since the prior frame. */
    int frame_count;
    double fps_timer;
    double current_fps;
    int target_fps;
} FPSCounter;

void fps_init(FPSCounter* fps, int target_fps);

void fps_update(FPSCounter* fps);

double fps_get_current(const FPSCounter* fps);

double fps_get_delta_time(const FPSCounter* fps);

int fps_should_render(FPSCounter* fps);

#endif // FPS_H
