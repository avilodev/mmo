#include "input.h"
#include <string.h>

void input_init(InputState* input) {
    memset(input, 0, sizeof(InputState));
}

void input_update(InputState* input, GLFWwindow* window,
                  int viewport_width, int viewport_height,
                  int window_width, int window_height) {
    
    // Store previous key states for edge detection
    static int prev_keys[GLFW_KEY_LAST + 1] = {0};
    static int prev_mouse_left = 0;
    static int prev_mouse_right = 0;
    
    // Update mouse position with viewport scaling
    double raw_x, raw_y;
    glfwGetCursorPos(window, &raw_x, &raw_y);
    
    input->mouse_x = (float)(raw_x * ((double)viewport_width / (double)window_width));
    input->mouse_y = (float)(raw_y * ((double)viewport_height / (double)window_height));
    
    // Mouse buttons (edge triggered + held)
    int mouse_left = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    int mouse_right = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
    
    input->mouse_left_clicked = (mouse_left && !prev_mouse_left);
    input->mouse_left_down = mouse_left;  // Held state for dragging
    input->mouse_right_clicked = (mouse_right && !prev_mouse_right);
    
    prev_mouse_left = mouse_left;
    prev_mouse_right = mouse_right;
    
    // Update keyboard state
    for (int key = 0; key <= GLFW_KEY_LAST; key++) {
        int pressed = (glfwGetKey(window, key) == GLFW_PRESS);
        input->keys_just_pressed[key] = (pressed && !prev_keys[key]);
        input->keys_pressed[key] = pressed;
        prev_keys[key] = pressed;
    }
}

int input_key_pressed(const InputState* input, int key) {
    if (key < 0 || key > GLFW_KEY_LAST) return 0;
    return input->keys_pressed[key];
}

int input_key_just_pressed(const InputState* input, int key) {
    if (key < 0 || key > GLFW_KEY_LAST) return 0;
    return input->keys_just_pressed[key];
}

int input_mouse_in_rect(const InputState* input, float x, float y, float w, float h) {
    return (input->mouse_x >= x && input->mouse_x <= x + w &&
            input->mouse_y >= y && input->mouse_y <= y + h);
}