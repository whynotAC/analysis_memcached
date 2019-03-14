#pragma once

/**
 * When adding a setting, be sure to update process_stat_settings
 * Globally accessible settings as derived from commandline. 
 */
struct settings {
    size_t maxbytes;
    int verbose;
    rel_time_t oldest_live; // ignore existing items older than this
    uint64_t oldest_cas; // ignore existing items with CAS values lower than this
    double factor; // chunk size growth factor
    int chunk_size;
    bool use_cas;

    int item_size_max; // Maximum item size
    int slab_chunk_size_max; // Upper end for chunks within slab pages
    int slab_page_size; // Slab's page units

    bool slab_reassign; // Whether or not slab reassignment is allowed
    int slab_automove; // Whether or not to automatically move slabs
    double slab_automove_ratio;
    unsigned int slab_automove_window;
    int hashpower_init;
    int tail_repair_time;
    bool flush_enabled;
    char *hash_algorithm;
}
