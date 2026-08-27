/**
 * @file
 * Resolve character layer art to textures and draw the stack.
 */
#include "player/paperdoll.h"
#include "texture/texture.h"
#include "render/renderer.h"
#include "core/client_log.h"

#include <stddef.h>

/** Map each layer to its file, in the enum's draw order.
 *
 * Race parts and gear live in different trees on purpose. Ears and tails
 * belong to the wolf; a shirt does not belong to anybody, and putting it under
 * Races/Wolf/ would mean drawing the same shirt again for the next race.
 */
static const char* const kLayerPath[PAPERDOLL_LAYERS] = {
    [PAPERDOLL_TAIL]  = "Game/Sprites/Player/Races/Wolf/tail/default.png",
    [PAPERDOLL_BODY]  = "Game/Sprites/Player/Races/Wolf/body/default.png",
    [PAPERDOLL_EYES]  = "Game/Sprites/Player/Races/Wolf/eyes/default.png",
    [PAPERDOLL_LEGS]  = "Game/Sprites/Player/Gear/legs/trousers.png",
    [PAPERDOLL_BOOTS] = "Game/Sprites/Player/Gear/boots/shoes.png",
    [PAPERDOLL_CHEST] = "Game/Sprites/Player/Gear/chest/tee.png",
    [PAPERDOLL_EARS]  = "Game/Sprites/Player/Races/Wolf/ears/default.png",
    [PAPERDOLL_HAIR]  = "Game/Sprites/Player/Races/Wolf/hair/default.png",
};

static const char* const kLayerName[PAPERDOLL_LAYERS] = {
    [PAPERDOLL_TAIL]  = "tail",
    [PAPERDOLL_BODY]  = "body",
    [PAPERDOLL_EYES]  = "eyes",
    [PAPERDOLL_LEGS]  = "legs",
    [PAPERDOLL_BOOTS] = "boots",
    [PAPERDOLL_CHEST] = "chest",
    [PAPERDOLL_EARS]  = "ears",
    [PAPERDOLL_HAIR]  = "hair",
};

static Paperdoll g_shared;

const char* paperdoll_layer_name(PaperdollLayer layer) {
    if (layer < 0 || layer >= PAPERDOLL_LAYERS) return "?";
    return kLayerName[layer];
}

const char* paperdoll_layer_path(PaperdollLayer layer) {
    if (layer < 0 || layer >= PAPERDOLL_LAYERS) return NULL;
    return kLayerPath[layer];
}

int paperdoll_load(Paperdoll* doll) {
    if (!doll) return 0;

    paperdoll_unload(doll);

    for (int i = 0; i < PAPERDOLL_LAYERS; i++) {
        doll->texture[i] = texture_load(kLayerPath[i]);
        if (doll->texture[i])
            doll->loaded++;
        else
            CLOG_ERROR("[PAPERDOLL] Missing layer '%s': %s",
                       kLayerName[i], kLayerPath[i]);
    }

    CLOG_INFO("[PAPERDOLL] Loaded %d/%d layers", doll->loaded, PAPERDOLL_LAYERS);
    return doll->loaded;
}

void paperdoll_unload(Paperdoll* doll) {
    if (!doll) return;

    for (int i = 0; i < PAPERDOLL_LAYERS; i++) {
        if (doll->texture[i]) {
            texture_unload(doll->texture[i]);
            doll->texture[i] = 0;
        }
    }
    doll->loaded = 0;
}

void paperdoll_render(const Paperdoll* doll, float cx, float cy, float size) {
    if (!doll || doll->loaded == 0) return;

    const float x = cx - size / 2.0f;
    const float y = cy - size / 2.0f;

    /* Back to front, which is enum order. Every layer is the same square drawn
     * at the same place, so alignment is a property of the art rather than of
     * anything computed here. */
    for (int i = 0; i < PAPERDOLL_LAYERS; i++) {
        if (doll->texture[i])
            renderer_draw_sprite(x, y, size, size, doll->texture[i]);
    }
}

Paperdoll* paperdoll_shared(void) {
    return &g_shared;
}

int paperdoll_load_shared(void) {
    return paperdoll_load(&g_shared);
}

void paperdoll_unload_shared(void) {
    paperdoll_unload(&g_shared);
}
