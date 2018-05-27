#include "memcached.h"

#include <cstdint>

static void settings_init(void) {
    settings.use_cas = true;

    settings.maxbytes = 64 * 1024 * 1024; // default is 64MB
    settings.verbose = 0;
    settings.factor = 1.25;
    settings.chunk_size = 48; // space for a modest key and value
    settings.slab_page_size = 1024 * 1024; // chunks are split from 1MB pages
    settings.slab_chunk_size_max = settings.slab_page_size / 2;
    settings.slab_reassign = true;
}

int main(int argc, char **argv) {
    
    printf("start program\n");
    printf("item size: %lu\n", sizeof(item));
    bool preallocate = true;
    int retval = EXIT_SUCCESS;
   
    uint32_t slab_sizes[MAX_NUMBER_OF_SLAB_CLASSES]; 
    bool use_slab_sizes = false;
    
    printf("start settings_init function\n");
    settings_init();
    printf("end settings_init function\n");

    slabs_init(settings.maxbytes, settings.factor, preallocate,
                use_slab_sizes ? slab_sizes : NULL);

    slabs_init_show();
    return retval;
}
