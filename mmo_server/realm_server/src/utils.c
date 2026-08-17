/**
 * @file
 * Provide monotonic timing for realm-server support code.
 */
#define _POSIX_C_SOURCE 200809L

#include "utils.h"

/**
 * Return the current monotonic time in fractional seconds.
 */
double get_time_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}
