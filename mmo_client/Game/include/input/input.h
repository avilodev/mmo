#ifndef INPUT_H
#define INPUT_H

#include <GLFW/glfw3.h>
#include "core/game_types.h"

// ============================================================================
// INPUT MODULE
// Handles all keyboard/mouse input processing
// ============================================================================

// Initialize input state
void input_init(InputState* input);

// Update input state (call once per frame, before processing)
void input_update(InputState* input, GLFWwindow* window, 
                  int viewport_width, int viewport_height,
                  int window_width, int window_height);

// Query functions
int input_key_pressed(const InputState* input, int key);
int input_key_just_pressed(const InputState* input, int key);
int input_mouse_in_rect(const InputState* input, float x, float y, float w, float h);

#endif // INPUT_H