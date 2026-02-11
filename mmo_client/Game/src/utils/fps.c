#include "fps.h"
#include <GLFW/glfw3.h>

void fps_init(FPSCounter* fps, int target_fps) {
    fps->last_time = glfwGetTime();
    fps->delta_time = 0.0;
    fps->frame_count = 0;
    fps->fps_timer = 0.0;
    fps->current_fps = 0.0;
    fps->target_fps = target_fps;
}

void fps_update(FPSCounter* fps) {
    double current_time = glfwGetTime();
    fps->delta_time = current_time - fps->last_time;
    
    // Frame rate limiting
    if (fps->target_fps > 0) {
        double target_frame_time = 1.0 / fps->target_fps;
        
        // Sleep if we're rendering too fast
        while (fps->delta_time < target_frame_time) {
            current_time = glfwGetTime();
            fps->delta_time = current_time - fps->last_time;
        }
    }
    
    fps->last_time = current_time;
    
    // Update FPS counter
    fps->frame_count++;
    fps->fps_timer += fps->delta_time;
    
    // Update FPS every second
    if (fps->fps_timer >= 1.0) {
        fps->current_fps = fps->frame_count / fps->fps_timer;
        fps->frame_count = 0;
        fps->fps_timer = 0.0;
    }
}

double fps_get_current(const FPSCounter* fps) {
    return fps->current_fps;
}

double fps_get_delta_time(const FPSCounter* fps) {
    return fps->delta_time;
}

int fps_should_render(FPSCounter* fps) {
    if (fps->target_fps <= 0) {
        return 1; // Unlimited FPS
    }
    
    double target_frame_time = 1.0 / fps->target_fps;
    return fps->delta_time >= target_frame_time;
}