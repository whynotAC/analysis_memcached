#include "memcached.h"

/* default */
static void settings_init(void);

/** exported globals */
struct settings settings;

struct slab_rebalance slab_rebal;
volatile int slab_rebalance_signal;

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

/*
 * We keep the current time of day in a global variable that's updated by a
 * timer event. This saves us a bunch of time() system calls (we really only
 * need to get the time once a second, whereas these can be tens of thousans
 * of requests a second) and allows us to use server-start-relative timestamps
 * rather than absolute UNIX timestamps, a space savings on systems where
 * sizeof(time_t) > sizeof(unsigned int).
 */
volatile rel_time_t current_time;


int main (int argc, char **argv) {
    
    bool preallocate = false;
    int retval = EXIT_SUCCESS;

    bool start_assoc_maint = true;
    enum hashfunc_type hash_type = MURMUR3_HASH;

    uint32_t slab_sizes[MAX_NUMBER_OF_SLAB_CLASSES];
    bool use_slab_sizes = false;

    /* init settings */
    settings_init();

    /* Run regardless of initializing it later */
    init_lru_maintainer();

    if (hash_init(hash_type) != 0) {
        fprintf(stderr, "Failed to initialize hash_algorithm!\n");
        exit(EX_USAGE);
    }

    assoc_init(settings.hashpower_init);
    slabs_init(settings.maxbytes, settings.factor, preallocate,
                use_slabs_sizes ? slab_sizes : NULL);

#ifdef EXTSTORE
    memcached_thread_init(settings.num_threads, storage);
#else
    memcacehd_thread_init(settings.num_threads, NULL);
#endif

    if (start_assoc_maint && start_assoc_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    if (settings.slab_reassign &&
            start_slab_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    stop_assoc_maintenance_thread();
    
    return retval;
}
