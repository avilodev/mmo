/**
 * @file
 * Select the online players within range of a world event.
 */

#include "interest.h"

#include "player_data.h"

#include <math.h>
#include <pthread.h>

extern ActivePlayer active_players[];

/**
 * Collect the descriptors of online players within a radius of a world point.
 *
 * Walks the online list rather than the slot table, so the cost tracks who is
 * actually connected instead of MAX_PLAYERS.
 *
 * @return The number of descriptors written, at most max_out.
 */
int interest_collect_fds(float x, float y, float radius, int* out_fds, int max_out) {
    if (!out_fds || max_out <= 0) return 0;
    if (!isfinite(x) || !isfinite(y) || !isfinite(radius) || radius < 0.0f) return 0;

    const float radius_sq = radius * radius;
    int count = 0;

    player_registry_rdlock();
    int online_count = 0;
    const int* online = player_active_list_locked(&online_count);

    for (int n = 0; n < online_count && count < max_out; n++) {
        int i = online[n];
        if (!active_players[i].is_loaded) continue;

        pthread_mutex_lock(&active_players[i].lock);
        float dx = active_players[i].pos_x - x;
        float dy = active_players[i].pos_y - y;
        int fd = active_players[i].client_fd;
        pthread_mutex_unlock(&active_players[i].lock);

        // Squared comparison: no sqrtf, and the radius is a constant per call.
        if (dx * dx + dy * dy <= radius_sq) out_fds[count++] = fd;
    }

    player_registry_unlock();
    return count;
}
