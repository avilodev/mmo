/**
 * @file
 * Check legacy world configuration parsing and named packet-limit overrides.
 */

#include "config.h"
#include "packet_limiter.h"
#include "limit_profiles.h"
#include "log.h"

#include <assert.h>
#include <stdio.h>

/**
 * Write a configuration fixture beneath /tmp.
 *
 * @return      A static buffer containing the fixture path.
 */
static const char* write_conf(const char* name, const char* body) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/%s", name);
    FILE* f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
    return path;
}

/**
 * Run configuration compatibility and limiter override checks.
 *
 * @return      Zero after all assertions pass.
 */
int main(void) {
    log_init();
    log_set_level(LOG_LEVEL_ERROR);

    printf("TEST 1: a legacy positional config still loads unchanged\n");
    const char* legacy = write_conf("limits_legacy.conf",
        "# Server Name\n"
        "Legacyworld\n"
        "# Region\n"
        "North America\n"
        "# IP:Port\n"
        "127.0.0.1:7778\n"
        "# Max Players\n"
        "800\n"
        "# Hardcore\n"
        "0\n");

    assert(set_config(legacy) == 1);
    printf("  name=%s port=%u max_players=%u\n",
           g_server.server_name, g_server.port, g_server.max_players);
    assert(g_server.port == 7778);
    assert(g_server.max_players == 800);

    // preserve compiled defaults without named settings
    PacketLimitProfile profile;
    packet_limiter_apply_overrides(&profile, limit_profile_world(), &g_server.limits);
    printf("  overall rate=%.0f (expect compiled default 200)\n", profile.overall.rate);
    printf("  social  rate=%.0f (expect compiled default 5)\n",
           profile.classes[LIMIT_CLASS_SOCIAL].rate);
    assert(profile.overall.rate == 200.0);
    assert(profile.classes[LIMIT_CLASS_SOCIAL].rate == 5.0);
    assert(profile.violation_limit == 200);

    printf("\nTEST 2: named settings after the positional block are applied\n");
    const char* tuned = write_conf("limits_tuned.conf",
        "# Server Name\n"
        "Tunedworld\n"
        "# Region\n"
        "Europe\n"
        "# IP:Port\n"
        "127.0.0.1:7779\n"
        "# Max Players\n"
        "400\n"
        "# Hardcore\n"
        "1\n"
        "\n"
        "limit_overall_rate     = 90\n"
        "limit_overall_burst    = 180\n"
        "limit_social_rate      = 2\n"
        "limit_social_burst     = 6\n"
        "limit_violation_limit  = 50\n"
        "limit_violation_window = 5\n");

    assert(set_config(tuned) == 1);
    printf("  name=%s port=%u hardcore=%d\n",
           g_server.server_name, g_server.port, (int)g_server.hardcore);
    assert(g_server.port == 7779);
    assert(g_server.hardcore == true);

    packet_limiter_apply_overrides(&profile, limit_profile_world(), &g_server.limits);
    printf("  overall %.0f/s burst %.0f | social %.0f/s burst %.0f | kick %u in %.0fs\n",
           profile.overall.rate, profile.overall.capacity,
           profile.classes[LIMIT_CLASS_SOCIAL].rate,
           profile.classes[LIMIT_CLASS_SOCIAL].capacity,
           profile.violation_limit, profile.violation_window);
    assert(profile.overall.rate     == 90.0);
    assert(profile.overall.capacity == 180.0);
    assert(profile.classes[LIMIT_CLASS_SOCIAL].rate     == 2.0);
    assert(profile.classes[LIMIT_CLASS_SOCIAL].capacity == 6.0);
    assert(profile.violation_limit  == 50);
    assert(profile.violation_window == 5.0);

    // Untouched classes must keep their compiled values.
    printf("  movement left alone: %.0f/s (expect 150)\n",
           profile.classes[LIMIT_CLASS_MOVEMENT].rate);
    assert(profile.classes[LIMIT_CLASS_MOVEMENT].rate == 150.0);

    printf("\nTEST 3: overridden budgets actually bind at runtime\n");
    packet_limiter_init(&profile);
    packet_limiter_reset(30);
    int allowed = 0;
    for (int i = 0; i < 100; i++)
        if (packet_limiter_check(30, PACKET_CHAT_SEND) == PACKET_LIMIT_ALLOW) allowed++;
    // tuned social burst permits two chat costs
    printf("  chats allowed under the tuned budget: %d (expect 2)\n", allowed);
    assert(allowed == 2);

    printf("\nTEST 4: an unknown key is reported but does not fail the load\n");
    const char* unknown = write_conf("limits_unknown.conf",
        "# Server Name\n"
        "Unknownworld\n"
        "# Region\n"
        "Asia\n"
        "# IP:Port\n"
        "127.0.0.1:7780\n"
        "# Max Players\n"
        "100\n"
        "# Hardcore\n"
        "0\n"
        "limit_nonsense_rate = 5\n"
        "totally_made_up     = 1\n");
    printf("  (two 'Unknown config key' warnings are expected here)\n");
    assert(set_config(unknown) == 1);
    assert(g_server.port == 7780);

    printf("\nTEST 5: a burst below its own rate is corrected, not obeyed\n");
    PacketLimitOverrides bad = {0};
    bad.class_rate[LIMIT_CLASS_QUERY]     = 40.0;
    bad.class_capacity[LIMIT_CLASS_QUERY] = 10.0;   // nonsensical
    packet_limiter_apply_overrides(&profile, limit_profile_world(), &bad);
    printf("  query rate=%.0f burst=%.0f (burst raised to meet rate)\n",
           profile.classes[LIMIT_CLASS_QUERY].rate,
           profile.classes[LIMIT_CLASS_QUERY].capacity);
    assert(profile.classes[LIMIT_CLASS_QUERY].capacity >= profile.classes[LIMIT_CLASS_QUERY].rate);

    printf("\nTEST 6: max_players above the compiled ceiling refuses to start\n");
    {
        /* The whole point of the check. A configured capacity larger than
         * MAX_PLAYERS used to load, print itself in the startup banner, and then
         * make every login past slot 1000 fail with a message naming neither
         * number -- while the realm server, which is told this value, went on
         * advertising the world as having room. */
        char body[256];
        snprintf(body, sizeof(body),
                 "# Server Name\nTooBig\n# Region\nNorth America\n"
                 "# IP:Port\n127.0.0.1:7781\n# Max Players\n%d\n# Hardcore\n0\n",
                 MAX_PLAYERS + 1);
        const char* too_big = write_conf("limits_toobig.conf", body);
        printf("  (a max_players ceiling error is expected here)\n");
        assert(set_config(too_big) == 0);

        // Exactly at the ceiling is a valid deployment, not an off-by-one refusal.
        snprintf(body, sizeof(body),
                 "# Server Name\nAtCap\n# Region\nNorth America\n"
                 "# IP:Port\n127.0.0.1:7782\n# Max Players\n%d\n# Hardcore\n0\n",
                 MAX_PLAYERS);
        const char* at_cap = write_conf("limits_atcap.conf", body);
        assert(set_config(at_cap) == 1);
        assert(g_server.max_players == MAX_PLAYERS);
        printf("  max_players=%u at the ceiling loads\n", g_server.max_players);
    }

    printf("\nTEST 7: max_players is rejected rather than truncated to its field\n");
    {
        /* The field is uint16_t and the parse used to be atoi() into an int, so
         * 70000 became 4464 with no diagnostic -- a silent ceiling inside the
         * very setting that exists to declare one. */
        const char* wrapped = write_conf("limits_wrapped.conf",
            "# Server Name\nWrapped\n# Region\nNorth America\n"
            "# IP:Port\n127.0.0.1:7783\n# Max Players\n70000\n# Hardcore\n0\n");
        printf("  (a max_players ceiling error is expected here)\n");
        assert(set_config(wrapped) == 0);
        printf("  70000 is refused instead of becoming 4464\n");
    }

    printf("\nALL ASSERTIONS PASSED\n");
    return 0;
}
