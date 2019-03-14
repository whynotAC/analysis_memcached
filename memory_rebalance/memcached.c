#include "memcached.h"

/* default */
static void settings_init(void);

/** exported globals */
struct settings settings;


static void settings_init(void) {
    settings.use_cas = true;
    settings.maxbytes = 64 * 1024 * 1024; // default is 64MB
    settings.verbose = 1;
    settings.oldest_live = 0;
    settings.oldest_cas = 0;

    settings.factor = 1.25;
    settings.chunk_size = 48;

    settings.item_size_max = 1024 * 1024;
    settings.slab_page_size = 1024 * 1024;
    settings.slab_chunk_size_max = settings.slab_page_size / 2;
    
    settings.hashpower_init = 0;
    
    settings.slab_reassign = true;
    settings.slab_automove = 1;
    settings.slab_automove_ratio = 0.8;
    settings.slab_automove_window = 30;

    settings.tail_repair_time = TAIL_REPAIR_TIME_DEFAULT;
    settings.flush_enabled = true;
}

int main (int argc, char **argv) {
    
    /* init settings */
    settings_init();
}
