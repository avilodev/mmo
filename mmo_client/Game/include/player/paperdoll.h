#ifndef PAPERDOLL_H
#define PAPERDOLL_H

/**
 * @file
 * Compose a character out of stacked sprite layers rather than one flat image.
 *
 * A character is not a picture, it is a stack of pictures drawn at the same
 * place: tail, then body, then whatever is worn over it. Swapping a shirt is
 * then binding a different file to one layer, and nothing else in the stack
 * needs to know it happened -- which is the whole reason the art is exported
 * per layer instead of flattened.
 *
 * Every layer file is PAPERDOLL_FRAME_PX square and untrimmed, so a layer's
 * position within its own image *is* its position on the character. That is
 * the invariant the whole scheme rests on: trim one file and the hat comes
 * off the head. paperdoll_test.c checks it for every shipped layer.
 */

/** Size of one layer image, in pixels, on both axes.
 *
 * Not a rendering size -- sprites are scaled to the world's tile size. This is
 * the authoring contract every layer file must satisfy.
 */
#define PAPERDOLL_FRAME_PX 100

/** Name each layer of the stack, in the order they are drawn.
 *
 * The order is the enum order, back to front, and that is deliberate: there is
 * no separate order table to keep in sync, so a layer cannot be added to one
 * and forgotten in the other. Inserting a layer here moves it in the drawing.
 *
 * Occlusion falls out of this for free. Leggings come before the chest piece,
 * so a long coat drawn into PAPERDOLL_CHEST covers the trousers without any
 * masking logic -- it is simply painted later.
 */
typedef enum {
    PAPERDOLL_TAIL = 0,  /* behind everything: the body hides its root */
    PAPERDOLL_BODY,      /* the bare character, drawn whole beneath the gear */
    PAPERDOLL_EYES,      /* on the face, under anything covering the head */
    PAPERDOLL_LEGS,
    PAPERDOLL_BOOTS,
    PAPERDOLL_CHEST,
    PAPERDOLL_EARS,
    PAPERDOLL_HAIR,      /* front-most: falls over the ears and the face */
    PAPERDOLL_LAYERS
} PaperdollLayer;

/** Hold one resolved character appearance as GPU textures.
 *
 * A zero entry is a layer that is simply not worn, not an error -- a character
 * with no boots has no boots texture, and the stack draws correctly without
 * it. That is what makes an empty equipment slot free rather than special.
 */
typedef struct {
    unsigned int texture[PAPERDOLL_LAYERS];
    int          loaded;  /* how many layers resolved to a real texture */
} Paperdoll;

/** Return the short name of a layer, for logging. Never NULL. */
const char* paperdoll_layer_name(PaperdollLayer layer);

/** Return the asset path a layer loads from, or NULL when out of range.
 *
 * Separated from loading so the paths are checkable without an OpenGL context,
 * which is what lets a headless test catch a renamed or resized asset.
 */
const char* paperdoll_layer_path(PaperdollLayer layer);

/** Load every layer into textures. Requires a current OpenGL context.
 *
 * Missing files are logged and left as zero rather than failing the load: one
 * absent boot sprite should cost you the boots, not the character.
 *
 * @return How many layers loaded. Zero means nothing drew.
 */
int paperdoll_load(Paperdoll* doll);

/** Release every texture and zero the stack. Safe on an already-unloaded doll. */
void paperdoll_unload(Paperdoll* doll);

/** Draw the stack centred on a world position.
 *
 * @param cx, cy  Centre of the character in world pixels.
 * @param size    Edge length to scale the square frame to.
 */
void paperdoll_render(const Paperdoll* doll, float cx, float cy, float size);

/** Return the one appearance every character currently wears.
 *
 * There is exactly one set of art so far, so every race and class shares it.
 * The type above is per-character on purpose -- when appearances start
 * differing this becomes a lookup instead of a singleton, and the render path
 * does not change.
 */
Paperdoll* paperdoll_shared(void);

/** Load paperdoll_shared(). Call once on entering gameplay. */
int paperdoll_load_shared(void);

/** Unload paperdoll_shared(). Call on leaving gameplay. */
void paperdoll_unload_shared(void);

#endif // PAPERDOLL_H
