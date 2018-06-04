#include <cstdint>

#include "memcached.h"
#include "assoc.h"

extern enum hashfunc_type {
    JENKINS_HASH = 0,
    MURMUR3_HASH
};
extern int hash_init(enum hashfunc_type type);

static void settings_init(void) {
    settings.use_cas = true;

    settings.maxbytes = 64 * 1024 * 1024; // default is 64MB
    settings.verbose = 0;
    settings.factor = 1.25;
    settings.chunk_size = 48; // space for a modest key and value
    settings.slab_page_size = 1024 * 1024; // chunks are split from 1MB pages
    settings.slab_chunk_size_max = settings.slab_page_size / 2;
    settings.slab_reassign = true;
    settings.hashpower_init = 0;
    settings.num_threads = 4; // N workers
}

int main(int argc, char **argv) {
    // memory
    printf("start program\n");
    int retval = EXIT_SUCCESS;
    
    bool start_assoc_maint = true;

#ifdef EXTSTORE
    void *storage = NULL;
#endif

    printf("start settings_init function\n");
    settings_init();
    printf("end settings_init function\n");

    // hash
    enum hashfunc_type hash_type = MURMUR3_HASH;

    if (hash_init(hash_type) != 0) {
        fprintf(stderr, "Failed to initialize has_algorithm");
        exit(0);
    }

    assoc_init(settings.hashpower_init);

#ifdef EXTSTORE
    memcached_thread_init(settings.num_threads, storage);
#else
    memcached_thread_init(settings.num_threads, NULL);
#endif
    if (start_assoc_maint && start_assoc_maintenance_thread() == -1) {
        exit(EXIT_FAILURE);
    }


    stop_assoc_maintenance_thread();

    return retval;
}
