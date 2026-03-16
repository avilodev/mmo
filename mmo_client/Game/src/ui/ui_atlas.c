#include "ui_atlas.h"
#include "renderer.h"
#include "texture.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int ui_atlas_load(UIAtlas* atlas, const char* texture_path, const char* atlas_path) {
    memset(atlas, 0, sizeof(*atlas));

    atlas->texture_id = texture_load(texture_path);
    if (!atlas->texture_id) {
        fprintf(stderr, "[ATLAS] Failed to load texture: %s\n", texture_path);
        return 0;
    }

    FILE* f = fopen(atlas_path, "r");
    if (!f) {
        fprintf(stderr, "[ATLAS] Failed to open atlas definition: %s\n", atlas_path);
        texture_unload(atlas->texture_id);
        atlas->texture_id = 0;
        return 0;
    }

    char line[256];
    int got_dims = 0;

    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        // Skip comments and blank lines
        if (line[0] == '#' || line[0] == '\0') continue;

        // First data line is the atlas dimensions
        if (!got_dims) {
            if (sscanf(line, "%d %d", &atlas->atlas_w, &atlas->atlas_h) == 2)
                got_dims = 1;
            continue;
        }

        if (atlas->count >= UI_ATLAS_MAX_SPRITES) {
            fprintf(stderr, "[ATLAS] Too many sprites (max %d), ignoring rest\n",
                    UI_ATLAS_MAX_SPRITES);
            break;
        }

        AtlasSprite* s = &atlas->sprites[atlas->count];
        int x, y, w, h;
        if (sscanf(line, "%63s %d %d %d %d", s->name, &x, &y, &w, &h) == 5) {
            s->px = x;
            s->py = y;
            s->pw = w;
            s->ph = h;
            s->u0 = (float)x             / (float)atlas->atlas_w;
            s->v0 = (float)y             / (float)atlas->atlas_h;
            s->u1 = (float)(x + w)       / (float)atlas->atlas_w;
            s->v1 = (float)(y + h)       / (float)atlas->atlas_h;
            atlas->count++;
        }
    }

    fclose(f);
    printf("[ATLAS] Loaded %d sprites from '%s'\n", atlas->count, atlas_path);
    return 1;
}

void ui_atlas_unload(UIAtlas* atlas) {
    if (atlas->texture_id) {
        texture_unload(atlas->texture_id);
        atlas->texture_id = 0;
    }
    atlas->count = 0;
}

const AtlasSprite* ui_atlas_get(const UIAtlas* atlas, const char* name) {
    for (int i = 0; i < atlas->count; i++) {
        if (strcmp(atlas->sprites[i].name, name) == 0)
            return &atlas->sprites[i];
    }
    fprintf(stderr, "[ATLAS] Sprite not found: '%s'\n", name);
    return NULL;
}

void ui_atlas_draw(const UIAtlas* atlas, const char* name,
                   float x, float y, float w, float h) {
    const AtlasSprite* s = ui_atlas_get(atlas, name);
    if (!s) return;
    float dw = (w > 0.0f) ? w : (float)s->pw;
    float dh = (h > 0.0f) ? h : (float)s->ph;
    renderer_draw_sprite_uv(x, y, dw, dh, atlas->texture_id,
                            s->u0, s->v0, s->u1, s->v1);
}

void ui_atlas_draw_tinted(const UIAtlas* atlas, const char* name,
                          float x, float y, float w, float h,
                          float r, float g, float b, float a) {
    const AtlasSprite* s = ui_atlas_get(atlas, name);
    if (!s) return;
    float dw = (w > 0.0f) ? w : (float)s->pw;
    float dh = (h > 0.0f) ? h : (float)s->ph;
    renderer_draw_sprite_uv_tinted(x, y, dw, dh, atlas->texture_id,
                                   s->u0, s->v0, s->u1, s->v1,
                                   r, g, b, a);
}
