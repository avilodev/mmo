#ifndef FPS_H
#define FPS_H

typedef struct {
    double last_time;
    double delta_time;
    int frame_count;
    double fps_timer;
    double current_fps;
    int target_fps;
} FPSCounter;

// Initialize FPS counter
void fps_init(FPSCounter* fps, int target_fps);

// Update FPS counter (call once per frame)
void fps_update(FPSCounter* fps);

// Get current FPS
double fps_get_current(const FPSCounter* fps);

// Get delta time (time since last frame in seconds)
double fps_get_delta_time(const FPSCounter* fps);

// Check if we should render this frame (for frame limiting)
int fps_should_render(FPSCounter* fps);

#endif // FPS_H