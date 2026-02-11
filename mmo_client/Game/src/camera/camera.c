#include "camera.h"
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>

void camera_init(Camera* camera, int viewport_width, int viewport_height) {
    camera->x = 0.0f;
    camera->y = 0.0f;
    camera->target_x = 0.0f;
    camera->target_y = 0.0f;
    camera->smoothing = 0.05f;  // Adjust this for smoother/snappier camera (0.05 = very smooth, 0.2 = snappy)
    camera->viewport_width = viewport_width;
    camera->viewport_height = viewport_height;

    camera->zoom = 1.0f;
    
    printf("Camera initialized: viewport %dx%d\n", viewport_width, viewport_height);
}

void camera_update(Camera* camera, float target_x, float target_y, double delta_time) {
    // Store target position
    camera->target_x = target_x;
    camera->target_y = target_y; 
    
    // Smooth camera interpolation (lerp with delta time)
    float lerp_factor = 1.0f - powf(camera->smoothing, delta_time * 60.0f);
    
    // Calculate the offset to center the camera on the target
    // This should remain constant regardless of zoom
    float target_camera_x = target_x;
    float target_camera_y = target_y;
    
    // Smoothly move camera towards target
    camera->x += (target_camera_x - camera->x) * lerp_factor;
    camera->y += (target_camera_y - camera->y) * lerp_factor;
}

void camera_set_position(Camera* camera, float x, float y) {
    camera->x = x;
    camera->y = y;
    camera->target_x = x;
    camera->target_y = y;
}

void camera_apply(const Camera* camera) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    
    // Apply zoom by scaling the viewport
    float half_width = (camera->viewport_width / camera->zoom) / 2.0f;
    float half_height = (camera->viewport_height / camera->zoom) / 2.0f;
    
    // camera->x and camera->y are already the center position
    float center_x = camera->x;
    float center_y = camera->y;
    
    glOrtho(center_x - half_width,
            center_x + half_width,
            center_y + half_height,
            center_y - half_height,
            -1.0, 1.0);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void camera_screen_to_world(const Camera* camera, float screen_x, float screen_y, 
                            float* world_x, float* world_y) {
    *world_x = screen_x + camera->x;
    *world_y = screen_y + camera->y;
}

void camera_world_to_screen(const Camera* camera, float world_x, float world_y,
                            float* screen_x, float* screen_y) {
    *screen_x = world_x - camera->x;
    *screen_y = world_y - camera->y;
}