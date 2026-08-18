/**
 * @file
 * Deterministic coordinate noise used by every world generation pass.
 */
#include "world/worldgen.h"

#include <stdint.h>

/**
 * Generate deterministic coordinate noise.
 *
 * @return A repeatable pseudo-random value in roughly [-1, 1].
 */
float worldgen_noise(int x, int y, int seed) {
    uint32_t n = (uint32_t)x + (uint32_t)y * 57u + (uint32_t)seed * 131u;
    n = (n << 13) ^ n;
    uint32_t h = (n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
    return 1.0f - (float)h / 1073741824.0f;
}

/**
 * Combine decreasing-amplitude noise octaves.
 *
 * @param octaves  Positive number of frequency layers.
 * @return         Normalized combined noise in roughly [-1, 1].
 */
float worldgen_noise_octaves(int x, int y, int seed, int octaves) {
    float result = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float max_value = 0.0f;

    for (int i = 0; i < octaves; i++) {
        result += worldgen_noise((int)(x * frequency), (int)(y * frequency),
                                 seed + i) * amplitude;
        max_value += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return (max_value > 0.0f) ? result / max_value : 0.0f;
}
