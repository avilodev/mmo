/**
 * @file
 * Capture GLFW keyboard and mouse input in logical viewport coordinates.
 */

#include "input.h"
#include "core/keybinds.h"
#include "render/letterbox.h"
#include <string.h>

/**
 * Clear all recorded input state.
 */
void input_init(InputState* input) {
    memset(input, 0, sizeof(InputState));
}

/**
 * Sample current input and derive per-frame button transitions.
 *
 * Previous button states are shared across calls and windows.
 *
 * @param window  GLFW window to sample.
 * @param viewport_width  Logical viewport width in pixels.
 * @param viewport_height  Logical viewport height in pixels.
 * @param window_width  Current window width in pixels; must be nonzero.
 * @param window_height  Current window height in pixels; must be nonzero.
 */
void input_update(InputState* input, GLFWwindow* window,
                  int viewport_width, int viewport_height,
                  int window_width, int window_height) {
    
    // Store previous key states for edge detection
    static int prev_keys[GLFW_KEY_LAST + 1] = {0};
    static int prev_mouse_left = 0;
    static int prev_mouse_right = 0;
    static int have_prev_mouse = 0;
    
    // Update mouse position with viewport scaling
    double raw_x, raw_y;
    glfwGetCursorPos(window, &raw_x, &raw_y);
    
    float prev_x = input->mouse_x;
    float prev_y = input->mouse_y;
	// Through the same letterbox the picture is drawn in, so bars shift the cursor
	letterbox_rect box = letterbox_fit(window_width, window_height,
	                                   viewport_width, viewport_height);
	letterbox_to_view(&box, raw_x, raw_y, viewport_width, viewport_height,
	                  &input->mouse_x, &input->mouse_y);

    /* The first sample has nothing to be relative to; a jump from (0,0) to
     * wherever the cursor starts would spin the camera on the first drag. */
    input->mouse_dx = have_prev_mouse ? input->mouse_x - prev_x : 0.0f;
    input->mouse_dy = have_prev_mouse ? input->mouse_y - prev_y : 0.0f;
    have_prev_mouse = 1;
    
    // Mouse buttons (edge triggered + held)
    int mouse_left = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    int mouse_right = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
    input->mouse_middle_down =
        (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS);
    
    input->mouse_left_clicked = (mouse_left && !prev_mouse_left);
    input->mouse_left_down = mouse_left;  // Held state for dragging
    input->mouse_right_clicked = (mouse_right && !prev_mouse_right);
    input->mouse_right_down = mouse_right;
    
    prev_mouse_left = mouse_left;
    prev_mouse_right = mouse_right;

	// Every button a binding can name (keybinds.h), edge and held
	static int prev_buttons[5] = {0};

	for(int b = 0; b < 5; b++) {
		int down = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_1 + b) == GLFW_PRESS;

		input->mouse_buttons_pressed[b] = down && !prev_buttons[b];
		input->mouse_buttons_down[b] = down;
		prev_buttons[b] = down;
	}
    
    // Update keyboard state
    for (int key = 0; key <= GLFW_KEY_LAST; key++) {
        int pressed = (glfwGetKey(window, key) == GLFW_PRESS);
        input->keys_just_pressed[key] = (pressed && !prev_keys[key]);
        input->keys_pressed[key] = pressed;
        prev_keys[key] = pressed;
    }
}

/**
 * Check whether a GLFW key is currently held.
 *
 * @param key  GLFW key index, which may be outside the supported range.
 * @return      Nonzero when the key is held; otherwise zero.
 */
int input_key_pressed(const InputState* input, int key) {
	int button = key - KEYBIND_MOUSE_BASE;

	if(button >= 0 && button < 5)
		return input->mouse_buttons_down[button];
    if (key < 0 || key > GLFW_KEY_LAST) return 0;
    return input->keys_pressed[key];
}

/**
 * Check whether a GLFW key transitioned to pressed this frame.
 *
 * @param key  GLFW key index, which may be outside the supported range.
 * @return      Nonzero on a new press; otherwise zero.
 */
int input_key_just_pressed(const InputState* input, int key) {
	int button = key - KEYBIND_MOUSE_BASE;

	if(button >= 0 && button < 5)
		return input->mouse_buttons_pressed[button];
    if (key < 0 || key > GLFW_KEY_LAST) return 0;
    return input->keys_just_pressed[key];
}

/**
 * Check whether the logical mouse position lies inside a rectangle.
 *
 * Rectangle edges are included in the hit area.
 *
 * @return      Nonzero when the mouse is inside; otherwise zero.
 */
int input_mouse_in_rect(const InputState* input, float x, float y, float w, float h) {
    return (input->mouse_x >= x && input->mouse_x <= x + w &&
            input->mouse_y >= y && input->mouse_y <= y + h);
}
