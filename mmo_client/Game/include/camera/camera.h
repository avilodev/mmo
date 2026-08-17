#ifndef CAMERA_H
#define CAMERA_H

/** Track a smoothed world-space camera and its viewport. */
typedef struct {
    float x;
    float y;
    float target_x;
    float target_y;
    float smoothing;   /**< Interpolation factor from instant at 0.0 to stationary at 1.0. */
    int viewport_width;
    int viewport_height;
    float zoom;  
} Camera;

void camera_init(Camera* camera, int viewport_width, int viewport_height);

void camera_update(Camera* camera, float target_x, float target_y, double delta_time);

void camera_set_position(Camera* camera, float x, float y);

void camera_apply(const Camera* camera);

void camera_screen_to_world(const Camera* camera, float screen_x, float screen_y, 
                            float* world_x, float* world_y);

void camera_world_to_screen(const Camera* camera, float world_x, float world_y,
                            float* screen_x, float* screen_y);

#endif // CAMERA_H
