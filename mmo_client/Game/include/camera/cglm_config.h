#ifndef CGLM_CONFIG_H
#define CGLM_CONFIG_H

/**
 * @file
 * The one way the client includes cglm.
 *
 * Matrices live in plain float[16] fields inside structs the client allocates
 * however it likes, so cglm must not assume 16-byte alignment -- its SSE path
 * uses aligned loads otherwise, which fault on a misaligned struct member.
 * Every file that includes cglm goes through here so the setting is uniform.
 */

#define CGLM_ALL_UNALIGNED
#include <cglm/cglm.h>

#endif /* CGLM_CONFIG_H */
