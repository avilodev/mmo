#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

// Tells players the forecast at join and when a weather day begins

#include <stdint.h>

// Tell one connection the forecast now
void weather_service_send(int client_fd, uint32_t character_id);

// Tell every loaded player the forecast if a new weather day has begun
void weather_service_upkeep(void);

#endif /* WEATHER_SERVICE_H */
