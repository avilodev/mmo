#ifndef UI_ATLAS_H
#define UI_ATLAS_H

#define UI_ATLAS_MAX_SPRITES 256

// A single named sprite inside an atlas texture.
// Pixel coords (px,py,pw,ph) are kept for reference and natural-size drawing.
// UV coords (u0..v1) are the normalized [0..1] values ready for GL.
typedef struct {
    char  name[64];
    int   px, py, pw, ph;   // pixel position and size in the atlas image
    float u0, v0, u1, v1;   // normalized UV coords (top-left to bottom-right)
} AtlasSprite;

// An atlas is one texture + a list of named sprites inside it.
typedef struct {
    unsigned int texture_id;
    int          atlas_w, atlas_h;
    AtlasSprite  sprites[UI_ATLAS_MAX_SPRITES];
    int          count;
} UIAtlas;

// Load an atlas from a PNG texture and a matching .atlas definition file.
// Returns 1 on success, 0 on failure.
int ui_atlas_load(UIAtlas* atlas, const char* texture_path, const char* atlas_path);

// Free the atlas texture.
void ui_atlas_unload(UIAtlas* atlas);

// Look up a sprite by name. Returns NULL if not found.
const AtlasSprite* ui_atlas_get(const UIAtlas* atlas, const char* name);

// Draw a named sprite at screen position (x, y).
// Pass w=0, h=0 to use the sprite's natural pixel dimensions.
void ui_atlas_draw(const UIAtlas* atlas, const char* name,
                   float x, float y, float w, float h);

// Same but with a color tint/alpha multiplier. Pass (1,1,1,1) for no tint.
void ui_atlas_draw_tinted(const UIAtlas* atlas, const char* name,
                          float x, float y, float w, float h,
                          float r, float g, float b, float a);

#endif // UI_ATLAS_H
