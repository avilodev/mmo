/**
 * @file
 * Generate a standalone binary world map with terrain, collision, and decorations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define WORLD_WIDTH  2000
#define WORLD_HEIGHT 2000
#define TILE_SIZE 16
#define CHUNK_SIZE 32

// Tile types
#define TILE_EMPTY  0
#define TILE_GRASS  1
#define TILE_WATER  2
#define TILE_ROCK   3

// Decoration types
#define DECO_NONE   0
#define DECO_TREE1  1
#define DECO_SHRUB1 2

/** Describe one serialized decoration and its chunk-local offset. */
typedef struct {
    uint16_t decoration_id;
    float offset_x;
    float offset_y;
} Decoration;

/**
 * Generate deterministic coordinate noise.
 *
 * @return      A deterministic pseudo-random value derived from the coordinates and seed.
 */
float noise(int x, int y, int seed) {
    int n = x + y * 57 + seed * 131;
    n = (n << 13) ^ n;
    return (1.0f - ((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f);
}

/**
 * Combine decreasing-amplitude noise octaves.
 *
 * @param octaves  Positive number of frequency layers.
 * @return      Normalized combined noise value.
 */
float noise_octaves(int x, int y, int seed, int octaves) {
    float result = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float max_value = 0.0f;
    
    for (int i = 0; i < octaves; i++) {
        result += noise((int)(x * frequency), (int)(y * frequency), seed + i) * amplitude;
        max_value += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    
    return result / max_value;
}

/**
 * Generate and write the configured world data file.
 *
 * This function performs blocking random-access file I/O and leaves errors reported on stderr.
 *
 * @param output_path  Destination file path.
 */
void generate_large_world(const char* output_path) {
    FILE* f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "Failed to create %s\n", output_path);
        return;
    }
    
    int width = WORLD_WIDTH;
    int height = WORLD_HEIGHT;
    int tile_size = TILE_SIZE;
    
    printf("[WORLDGEN] Generating LARGE %dx%d world...\n", width, height);
    printf("[WORLDGEN] This will take a few minutes...\n");
    
    // Write header
    fwrite(&width, sizeof(int), 1, f);
    fwrite(&height, sizeof(int), 1, f);
    fwrite(&tile_size, sizeof(int), 1, f);
    
    // Generate and write tiles
    printf("[WORLDGEN] Writing tiles...\n");
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint16_t tile;
            
            // Normalized coordinates
            float nx = (float)x / width;
            float ny = (float)y / height;
            
            // Create biomes using multi-octave noise
            float elevation = noise_octaves(x, y, 1000, 4) * 0.5f + 0.5f;
            float moisture = noise_octaves(x, y, 2000, 3) * 0.5f + 0.5f;
            float temperature = noise_octaves(x, y, 3000, 3) * 0.5f + 0.5f;
            
            // Add some randomness
            float n = noise(x / 4, y / 4, 12345) * 0.5f + 0.5f;
            
            // Biome determination
            if (elevation < 0.3f) {
                // Low areas = water
                tile = TILE_WATER;
            } else if (elevation < 0.45f) {
                // Shore areas = mix of grass and water
                tile = (moisture > 0.5f) ? TILE_WATER : TILE_GRASS;
            } else if (elevation > 0.75f) {
                // High elevation = mountains (rocks)
                tile = TILE_ROCK;
            } else {
                // Mid elevation = plains
                if (moisture < 0.3f && temperature > 0.6f) {
                    // Hot and dry = scattered rocks (desert-like)
                    tile = (n > 0.7f) ? TILE_ROCK : TILE_GRASS;
                } else if (moisture > 0.7f) {
                    // Very wet = swamp (water patches)
                    tile = (n > 0.6f) ? TILE_WATER : TILE_GRASS;
                } else {
                    // Normal grasslands
                    tile = (n > 0.85f) ? TILE_ROCK : TILE_GRASS;
                }
            }
            
            // Add rivers (vertical and horizontal)
            float river_noise = noise(x / 8, y / 8, 4000) * 0.5f + 0.5f;
            if (river_noise > 0.48f && river_noise < 0.52f) {
                tile = TILE_WATER;
            }
            
            // Add some paths/roads through the world
            if ((x % 200 < 5 && y % 200 < 100) || (y % 200 < 5 && x % 200 < 100)) {
                tile = TILE_GRASS;
            }
            
            fwrite(&tile, sizeof(uint16_t), 1, f);
        }
        
        if (y % 200 == 0) {
            printf("[WORLDGEN] Tile progress: %d%%\n", (y * 100) / height);
        }
    }
    
    // Generate and write collision
    printf("[WORLDGEN] Writing collision...\n");
    long collision_start = ftell(f);
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // Read tile
            fseek(f, sizeof(int) * 3 + (y * width + x) * sizeof(uint16_t), SEEK_SET);
            uint16_t tile;
            fread(&tile, sizeof(uint16_t), 1, f);
            
            uint8_t collision = (tile == TILE_WATER || tile == TILE_ROCK) ? 1 : 0;
            
            // Write collision
            fseek(f, collision_start + (y * width + x) * sizeof(uint8_t), SEEK_SET);
            fwrite(&collision, sizeof(uint8_t), 1, f);
        }
        
        if (y % 200 == 0) {
            printf("[WORLDGEN] Collision progress: %d%%\n", (y * 100) / height);
        }
    }
    
    // Move to end of collision data
    fseek(f, collision_start + width * height * sizeof(uint8_t), SEEK_SET);
    
    // Generate and write decorations
    printf("[WORLDGEN] Writing decorations...\n");
    int chunks_x = (width + CHUNK_SIZE - 1) / CHUNK_SIZE;
    int chunks_y = (height + CHUNK_SIZE - 1) / CHUNK_SIZE;
    int total_decos = 0;
    
    for (int cy = 0; cy < chunks_y; cy++) {
        for (int cx = 0; cx < chunks_x; cx++) {
            Decoration decos[16];
            uint8_t deco_count = 0;
            
            // Vary decoration count based on chunk position (biome-like)
            int base_decos = 3;
            float chunk_nx = (float)cx / chunks_x;
            float chunk_ny = (float)cy / chunks_y;
            float chunk_noise = noise(cx * 4, cy * 4, 5000) * 0.5f + 0.5f;
            
            // Dense forest areas
            if (chunk_noise > 0.6f) {
                base_decos = 8;
            }
            // Sparse areas
            else if (chunk_noise < 0.3f) {
                base_decos = 1;
            }
            
            int num_decos = base_decos + (rand() % 3);
            if (num_decos > 16) num_decos = 16;
            
            for (int i = 0; i < num_decos && deco_count < 16; i++) {
                int local_x = rand() % CHUNK_SIZE;
                int local_y = rand() % CHUNK_SIZE;
                int world_x = cx * CHUNK_SIZE + local_x;
                int world_y = cy * CHUNK_SIZE + local_y;
                
                if (world_x >= width || world_y >= height) continue;
                
                // Check tile type
                fseek(f, sizeof(int) * 3 + (world_y * width + world_x) * sizeof(uint16_t), SEEK_SET);
                uint16_t tile;
                fread(&tile, sizeof(uint16_t), 1, f);
                
                // Only place decorations on grass
                if (tile == TILE_GRASS) {
                    // 70% trees, 30% shrubs
                    decos[deco_count].decoration_id = (rand() % 10 < 7) ? DECO_TREE1 : DECO_SHRUB1;
                    decos[deco_count].offset_x = local_x + ((rand() % 100) / 100.0f - 0.5f);
                    decos[deco_count].offset_y = local_y + ((rand() % 100) / 100.0f - 0.5f);
                    deco_count++;
                    total_decos++;
                }
            }
            
            // Move to end for next chunk
            fseek(f, 0, SEEK_END);
            
            // Write decoration count and data
            fwrite(&deco_count, sizeof(uint8_t), 1, f);
            if (deco_count > 0) {
                fwrite(decos, sizeof(Decoration), deco_count, f);
            }
        }
        
        if (cy % 10 == 0) {
            printf("[WORLDGEN] Decoration progress: %d%%\n", (cy * 100) / chunks_y);
        }
    }
    
    fclose(f);
    
    size_t file_size = sizeof(int) * 3 + 
                      (size_t)width * height * sizeof(uint16_t) + 
                      (size_t)width * height * sizeof(uint8_t) +
                      (size_t)chunks_x * chunks_y * (sizeof(uint8_t) + 16 * sizeof(Decoration));
    
    printf("[WORLDGEN] Complete!\n");
    printf("[WORLDGEN] World: %dx%d tiles (%dx%d chunks)\n", width, height, chunks_x, chunks_y);
    printf("[WORLDGEN] File: %s\n", output_path);
    printf("[WORLDGEN] Size: %zu bytes (%.2f MB)\n", file_size, file_size / (1024.0f * 1024.0f));
    printf("[WORLDGEN] Total decorations: %d\n", total_decos);
    printf("[WORLDGEN] Features:\n");
    printf("  - Multi-octave noise terrain generation\n");
    printf("  - Elevation-based biomes (water/plains/mountains)\n");
    printf("  - Moisture and temperature variation\n");
    printf("  - Rivers and paths\n");
    printf("  - Dense and sparse forest areas\n");
    printf("  - 70%% trees, 30%% shrubs\n");
}

/**
 * Generate a world file at the requested or default output path.
 *
 * @param argc  Command-line argument count.
 * @param argv  Command-line arguments; argv[1] optionally selects the output path.
 * @return      Zero after the generation attempt.
 */
int main(int argc, char* argv[]) {
    printf("=== Large World Generator ===\n\n");
    
    srand(time(NULL));
    
    const char* output = "world_large.dat";
    if (argc > 1) {
        output = argv[1];
    }
    
    printf("Generating large world: %dx%d tiles\n", WORLD_WIDTH, WORLD_HEIGHT);
    printf("Expected file size: ~18 MB\n");
    printf("This will take 2-5 minutes...\n");
    printf("Output: %s\n\n", output);
    
    generate_large_world(output);
    
    printf("\n=== Done ===\n");
    printf("Copy %s to Game/bin/world.dat\n", output);
    printf("\nWorld details:\n");
    printf("- Total tiles: %d\n", WORLD_WIDTH * WORLD_HEIGHT);
    printf("- World size: %dx%d pixels\n", WORLD_WIDTH * TILE_SIZE, WORLD_HEIGHT * TILE_SIZE);
    printf("- Recommended spawn: (16000, 16000) - center of map\n");
    
    return 0;
}
