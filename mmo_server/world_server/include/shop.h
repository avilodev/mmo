#ifndef SHOP_H
#define SHOP_H

#include <stdint.h>

/** Bound loaded shops and items advertised by each shop. */
#define MAX_SHOPS 64
#define MAX_SHOP_ITEMS 32

/** Pair an item definition with its shop purchase price. */
typedef struct {
    uint32_t item_id;
    uint32_t buy_price;     // gold to buy
} ShopItemEntry;

/** Define one named shop and its available items. */
typedef struct {
    uint32_t      shop_id;
    char          name[32];
    uint8_t       item_count;
    ShopItemEntry items[MAX_SHOP_ITEMS];
} ShopDef;

// load shop definitions from JSON
int  shop_init(const char* json_path);
// return unresolved or profitable-resale entry count
int shop_validate(void);

void shop_cleanup(void);

void shop_open(uint32_t character_id, int client_fd, uint32_t shop_id);

void shop_handle_buy (uint32_t character_id, int client_fd, uint8_t* buffer, int bytes);
void shop_handle_sell(uint32_t character_id, int client_fd, uint8_t* buffer, int bytes);

#endif // SHOP_H
