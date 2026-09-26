/**
 * @file
 * Parse public-key pin files and compare fingerprints.
 */
#include "cert_pin.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/** Clear the pin set. */
void cert_pin_reset(CertPinSet* set) {
    if (!set) return;
    memset(set, 0, sizeof(*set));
}

/** Report an empty pin set. */
int cert_pin_is_empty(const CertPinSet* set) {
    return !set || set->count == 0;
}

/** Map one base64 character to its 6-bit value, or -1 when it is not one. */
static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/**
 * Decode exactly CERT_PIN_DIGEST_LEN bytes of standard base64.
 *
 * A 32-byte digest is 44 base64 characters ending in a single '='. Anything
 * else is refused rather than decoded as far as it goes.
 *
 * @return 1 on success, otherwise 0.
 */
static int base64_decode_digest(const char* text, uint8_t* out) {
    if (strlen(text) != 44) return 0;
    if (text[43] != '=') return 0;

    uint32_t accumulator = 0;
    int      bits        = 0;
    size_t   produced    = 0;

    for (int i = 0; i < 43; i++) {
        int value = base64_value(text[i]);
        if (value < 0) return 0;

        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            if (produced >= CERT_PIN_DIGEST_LEN) return 0;
            out[produced++] = (uint8_t)((accumulator >> bits) & 0xFF);
        }
    }

    return produced == CERT_PIN_DIGEST_LEN;
}

/** Map one hex digit to its value, or -1 when it is not one. */
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * Decode 64 hex digits, tolerating ':' separators as `openssl` prints them.
 *
 * @return 1 on success, otherwise 0.
 */
static int hex_decode_digest(const char* text, uint8_t* out) {
    size_t produced = 0;
    int    high     = -1;

    for (const char* p = text; *p; p++) {
        if (*p == ':') continue;

        int value = hex_value(*p);
        if (value < 0) return 0;

        if (high < 0) {
            high = value;
            continue;
        }

        if (produced >= CERT_PIN_DIGEST_LEN) return 0;
        out[produced++] = (uint8_t)((high << 4) | value);
        high = -1;
    }

    return high < 0 && produced == CERT_PIN_DIGEST_LEN;
}

/** Copy a line with surrounding whitespace removed. */
static void trim_into(const char* line, char* out, size_t out_size) {
    out[0] = '\0';
    if (out_size == 0) return;

    while (*line && isspace((unsigned char)*line)) line++;

    size_t length = strlen(line);
    while (length > 0 && isspace((unsigned char)line[length - 1])) length--;
    if (length >= out_size) length = out_size - 1;

    memcpy(out, line, length);
    out[length] = '\0';
}

/** Parse and store one pin line. */
int cert_pin_add_line(CertPinSet* set, const char* line) {
    if (!set || !line) return 0;

    char trimmed[128];
    trim_into(line, trimmed, sizeof(trimmed));

    if (trimmed[0] == '\0' || trimmed[0] == '#') return 1;   // nothing to store
    if (set->count >= CERT_PIN_MAX) return 0;

    const char* body = trimmed;
    if (strncmp(body, "sha256/", 7) == 0) body += 7;
    if (strncmp(body, "sha256:", 7) == 0) body += 7;

    uint8_t digest[CERT_PIN_DIGEST_LEN];
    if (!base64_decode_digest(body, digest) && !hex_decode_digest(body, digest))
        return 0;

    memcpy(set->digests[set->count], digest, sizeof(digest));
    set->count++;
    return 1;
}

/** Read a pin file into the set. */
int cert_pin_load_file(CertPinSet* set, const char* path,
                       char* error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!set || !path) {
        if (error && error_size) snprintf(error, error_size, "invalid arguments");
        return 0;
    }

    cert_pin_reset(set);

    FILE* file = fopen(path, "r");
    if (!file) {
        if (error && error_size)
            snprintf(error, error_size, "cannot open '%s'", path);
        return 0;
    }

    char line[256];
    int  line_number = 0;
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        if (!cert_pin_add_line(set, line)) {
            fclose(file);
            cert_pin_reset(set);
            if (error && error_size)
                snprintf(error, error_size, "%s:%d is not a usable pin",
                         path, line_number);
            return 0;
        }
    }

    fclose(file);

    if (cert_pin_is_empty(set)) {
        if (error && error_size)
            snprintf(error, error_size, "%s contains no pins", path);
        return 0;
    }
    return 1;
}

/** Compare a digest against every pin. */
int cert_pin_matches(const CertPinSet* set, const uint8_t* digest) {
    if (cert_pin_is_empty(set) || !digest) return 0;

    int matched = 0;
    for (int i = 0; i < set->count; i++) {
        unsigned char diff = 0;
        for (int b = 0; b < CERT_PIN_DIGEST_LEN; b++)
            diff |= (unsigned char)(set->digests[i][b] ^ digest[b]);
        matched |= (diff == 0);
    }
    return matched;
}

/** Render a digest as lowercase hex. */
void cert_pin_format(const uint8_t* digest, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!digest || out_size < CERT_PIN_DIGEST_LEN * 2 + 1) return;

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < CERT_PIN_DIGEST_LEN; i++) {
        out[i * 2]     = hex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[CERT_PIN_DIGEST_LEN * 2] = '\0';
}
