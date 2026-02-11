#ifndef CAMERA_H
#define CAMERA_H

typedef struct {
    float x;           // Camera position X
    float y;           // Camera position Y
    float target_x;    // Target position X (usually player position)
    float target_y;    // Target position Y
    float smoothing;   // Camera smoothing factor (0.0 = instant, 1.0 = no movement)
    int viewport_width;
    int viewport_height;
    float zoom;  
} Camera;

// Initialize the camera
void camera_init(Camera* camera, int viewport_width, int viewport_height);

// Update camera position (call this every frame)
void camera_update(Camera* camera, float target_x, float target_y, double delta_time);

// Set camera to immediately snap to position (no smoothing)
void camera_set_position(Camera* camera, float x, float y);

// Apply camera transformation to OpenGL
void camera_apply(const Camera* camera);

// Convert screen coordinates to world coordinates
void camera_screen_to_world(const Camera* camera, float screen_x, float screen_y, 
                            float* world_x, float* world_y);

// Convert world coordinates to screen coordinates
void camera_world_to_screen(const Camera* camera, float world_x, float world_y,
                            float* screen_x, float* screen_y);

#endif // CAMERA_H