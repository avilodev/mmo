#ifndef STR_FIXED_H
#define STR_FIXED_H

/** @file Copy strings into the fixed-width character fields the wire format uses.
 *
 * Every protocol packet carries names, regions, and messages as `char name[32]`
 * rather than as pointers, so filling one is always "copy up to N-1 bytes and
 * terminate". Written with strncpy that reads as
 * `strncpy(dst, src, sizeof(dst) - 1)`, which is correct only because the
 * surrounding packet was memset to zero first — a precondition that lives
 * several lines away from the copy and is invisible at the call site.
 *
 * These helpers terminate unconditionally instead of relying on that, which is
 * also what lets the tree build clean under -O2: GCC's -Wstringop-truncation
 * cannot see the earlier memset and flags the strncpy form on every such field.
 */

#include <stddef.h>
#include <string.h>

/**
 * Copy a string into a fixed-width field, truncating to fit and always terminating.
 *
 * @param destination  Field to fill; left untouched when NULL or zero-sized.
 * @param destination_size  Full byte size of the field, including its terminator.
 * @param source  String to copy, or NULL to produce an empty field.
 */
static inline void str_copy_fixed(char* destination, size_t destination_size,
                                  const char* source) {
    if (!destination || destination_size == 0) return;

    if (!source) {
        destination[0] = '\0';
        return;
    }

    // strnlen, not strlen: source may itself be an unterminated fixed-width field.
    size_t length = strnlen(source, destination_size - 1);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

/** Fill a fixed-width array field from a string, sizing the field automatically.
 *
 * Only valid where the destination is a real array — on a `char*` this would
 * measure the pointer. Call str_copy_fixed() directly in that case.
 */
#define STR_COPY_FIELD(field, source) \
    str_copy_fixed((field), sizeof(field), (source))

#endif // STR_FIXED_H
