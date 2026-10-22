#ifndef SHOP_H
#define SHOP_H

#include <stdint.h>

#include "world_regions.h"

/** Bound loaded shops and items advertised by each shop. */
#define MAX_SHOPS 64
#define MAX_SHOP_ITEMS 32

/** Pair an item definition with its shop purchase price. */
typedef struct {
    uint32_t item_id;
    uint32_t buy_price;     // priced in the shop's currency
} ShopItemEntry;

/**
 * Define one named shop, the coin it trades in, and its available items.
 *
 * A shop belongs to a kingdom and deals only in that kingdom's currency: its
 * prices are quoted in it, purchases are charged in it, and sales pay it. When
 * cross-currency trade arrives, the discounted rate applies here, at the point
 * of sale, rather than changing what a shop's own coin is.
 */
typedef struct {
    uint32_t      shop_id;
    char          name[32];
    uint8_t       currency_id;   // CurrencyId this shop trades in
    uint8_t       item_count;
    ShopItemEntry items[MAX_SHOP_ITEMS];
} ShopDef;

// load shop definitions from JSON
int  shop_init(const char* json_path);

/** Look up a loaded shop by identifier.
 *
 * @return The shop, or NULL when nothing carries that identifier.
 */
ShopDef* shop_find(uint32_t shop_id);
// return unresolved or profitable-resale entry count
int shop_validate(void);

void shop_cleanup(void);

/** Send a shop's inventory and prices, and record it as the character's open shop.
 *
 * The merchant is passed in because buying and selling are gated on still
 * being at it -- see shop_session.h.
 */
void shop_open(uint32_t character_id, int client_fd, uint32_t shop_id,
               uint32_t npc_id, float npc_x, float npc_y);

void shop_handle_buy (uint32_t character_id, int client_fd, uint8_t* buffer, int bytes);
void shop_handle_sell(uint32_t character_id, int client_fd, uint8_t* buffer, int bytes);

#endif // SHOP_H
