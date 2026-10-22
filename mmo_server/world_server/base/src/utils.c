#define _POSIX_C_SOURCE 200809L

/**
 * @file
 * Provide the world-server clock.
 */

#include "utils.h"
#include "log.h"

/**
 * Read the monotonic clock as fractional seconds.
 *
 * @return      Monotonic time in seconds.
 */
double get_time_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}
