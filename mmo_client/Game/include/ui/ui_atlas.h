#ifndef UI_ATLAS_H
#define UI_ATLAS_H

#define UI_ATLAS_MAX_SPRITES 256

/** Describe one named atlas region in pixel and normalized UV coordinates. */
typedef struct {
    char  name[64];
    int   px, py, pw, ph;   /**< Pixel position and size in the atlas image. */
    float u0, v0, u1, v1;   /**< Normalized top-left through bottom-right UV coordinates. */
} AtlasSprite;

/** Own one atlas texture and its fixed-capacity named regions. */
typedef struct {
    unsigned int texture_id;
    int          atlas_w, atlas_h;
    AtlasSprite  sprites[UI_ATLAS_MAX_SPRITES];
    int          count;
} UIAtlas;

int ui_atlas_load(UIAtlas* atlas, const char* texture_path, const char* atlas_path);

void ui_atlas_unload(UIAtlas* atlas);

const AtlasSprite* ui_atlas_get(const UIAtlas* atlas, const char* name);

/** Draw a named sprite, using natural dimensions when width or height is zero. */
void ui_atlas_draw(const UIAtlas* atlas, const char* name,
                   float x, float y, float w, float h);

void ui_atlas_draw_tinted(const UIAtlas* atlas, const char* name,
                          float x, float y, float w, float h,
                          float r, float g, float b, float a);

#endif // UI_ATLAS_H
