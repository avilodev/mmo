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

#endif // UI_ATLAS_H
