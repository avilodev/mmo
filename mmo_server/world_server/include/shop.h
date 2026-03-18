#ifndef SHOP_H
#define SHOP_H

#include <stdint.h>

#define MAX_SHOPS 64
#define MAX_SHOP_ITEMS 32

typedef struct {
    uint32_t item_id;
    uint32_t buy_price;     // gold to buy
} ShopItemEntry;

typedef struct {
    uint32_t      shop_id;
    char          name[32];
    uint8_t       item_count;
    ShopItemEntry items[MAX_SHOP_ITEMS];
} ShopDef;

// Init: load shops.json
int  shop_init(const char* json_path);
void shop_cleanup(void);

// Open shop for player — sends PACKET_SHOP_OPEN
void shop_open(uint32_t character_id, int client_fd, uint32_t shop_id);

// Handle buy/sell packets
void shop_handle_buy (uint32_t character_id, int client_fd, uint8_t* buffer, int bytes);
void shop_handle_sell(uint32_t character_id, int client_fd, uint8_t* buffer, int bytes);

#endif // SHOP_H
