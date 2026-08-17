/**
 * @file
 * Track client frame timing, frame rate, and optional frame limiting.
 */

#include "fps.h"
#include <GLFW/glfw3.h>

/**
 * Initialize a frame counter at the current GLFW time.
 *
 * @param fps  Counter state to initialize.
 * @param target_fps  Desired frame rate, or a non-positive value for no limit.
 */
void fps_init(FPSCounter* fps, int target_fps) {
    fps->last_time = glfwGetTime();
    fps->delta_time = 0.0;
    fps->frame_count = 0;
    fps->fps_timer = 0.0;
    fps->current_fps = 0.0;
    fps->target_fps = target_fps;
}

/**
 * Advance frame timing and update the measured frame rate.
 *
 * This call busy-waits until the target frame interval has elapsed when limiting is enabled.
 *
 * @param fps  Initialized counter state to update.
 */
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

/**
 * Return the most recently calculated frame rate.
 *
 * @return      Frames per second from the last completed measurement interval.
 */
double fps_get_current(const FPSCounter* fps) {
    return fps->current_fps;
}

/**
 * Return the duration of the most recent frame interval.
 *
 * @return      Frame interval in seconds.
 */
double fps_get_delta_time(const FPSCounter* fps) {
    return fps->delta_time;
}

/**
 * Check whether the recorded frame interval meets the configured target.
 *
 * @return      Nonzero when rendering is due or limiting is disabled; otherwise zero.
 */
int fps_should_render(FPSCounter* fps) {
    if (fps->target_fps <= 0) {
        return 1; // Unlimited FPS
    }
    
    double target_frame_time = 1.0 / fps->target_fps;
    return fps->delta_time >= target_frame_time;
}
