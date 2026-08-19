/**
 * @file
 * Resolve runtime data files relative to the running executable.
 */

#include "data_paths.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * Write the directory holding the running executable.
 *
 * @return 1 on success, or 0 when the path does not fit; `out` then receives ".".
 */
int data_path_exe_dir(char* out, size_t out_size) {
    if (!out || out_size < 2) return 0;

    ssize_t len = readlink("/proc/self/exe", out, out_size - 1);
    if (len <= 0) {
        out[0] = '.';
        out[1] = '\0';
        return 0;
    }

    out[len] = '\0';
    char* slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    return 1;
}

/**
 * Build the path to one runtime data file beside the executable.
 *
 * @return 1 on success, or 0 when the result would not fit.
 */
int data_path_resolve(char* out, size_t out_size, const char* relative) {
    if (!out || !relative || out_size == 0) return 0;

    char dir[512];
    data_path_exe_dir(dir, sizeof(dir));

    size_t dir_len = strlen(dir);
    size_t rel_len = strlen(relative);
    if (dir_len + rel_len + 1 > out_size) {
        fprintf(stderr, "Runtime data path is too long: %s%s\n", dir, relative);
        out[0] = '\0';
        return 0;
    }

    memcpy(out, dir, dir_len);
    memcpy(out + dir_len, relative, rel_len + 1);
    return 1;
}
