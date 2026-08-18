/**
 * @file
 * Deterministic coordinate noise used by every world generation pass.
 */
#include "world/worldgen.h"

/**
 * Generate deterministic coordinate noise.
 *
 * @return A repeatable pseudo-random value in roughly [-1, 1].
 */
float worldgen_noise(int x, int y, int seed) {
    int n = x + y * 57 + seed * 131;
    n = (n << 13) ^ n;
    return (1.0f - ((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff)
                    / 1073741824.0f);
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
