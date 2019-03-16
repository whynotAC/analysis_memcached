#include "items.h"

static int lru_maintainer_initialized = 0;
static pthread_mutex_t lru_maintainer_lock = PTHREAD_MUTEX_INITIALIZER;

int init_lru_maintainer(void) {
    if (lru_maintainer_initialized == 0) {
        pthread_mutex_init(&lru_maintainer_lock, NULL);
        lru_maintainer_initialized = 1;
    }
    return 0;
}
