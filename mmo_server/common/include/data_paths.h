#ifndef DATA_PATHS_H
#define DATA_PATHS_H

/** @file Resolve runtime data files relative to the running executable.
 *
 * Data files are packaged beside each binary by the Makefile, so a server finds
 * them no matter which directory it was launched from. The race registry and the
 * progression tunables are needed by more than one server — the world server to run
 * characters, the realm server to validate creating them — so both get a copy.
 */

#include <stddef.h>

/** Write the directory holding the running executable.
 *
 * @param out       Receives the directory with no trailing slash.
 * @param out_size  Capacity of `out`.
 * @return          1 on success, or 0 when the path does not fit, in which case
 *                  `out` receives "." so callers still have a usable relative base.
 */
int data_path_exe_dir(char* out, size_t out_size);

/** Build the path to one runtime data file beside the executable.
 *
 * @param out       Receives the full path.
 * @param out_size  Capacity of `out`.
 * @param relative  Path relative to the executable, beginning with a slash,
 *                  e.g. "/data/races.json".
 * @return          1 on success, or 0 when the result would not fit.
 */
int data_path_resolve(char* out, size_t out_size, const char* relative);

#endif // DATA_PATHS_H
