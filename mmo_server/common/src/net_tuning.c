/**
 * @file
 * Read reactor pool overrides from the environment.
 */

#include "net_tuning.h"
#include "log.h"

#include <stdlib.h>

int net_tuning_workers(const char* variable) {
    const char* value = getenv(variable);
    if (!value || !*value) return 0;

    int workers = atoi(value);
    if (workers <= 0) {
        LOG_ERROR("[TUNING] %s='%s' is not a positive number; using the default",
                  variable, value);
        return 0;
    }

    LOG_WARN("[TUNING] %s set the worker pool to %d", variable, workers);
    return workers;
}
