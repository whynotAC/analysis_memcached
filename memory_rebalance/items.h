#pragma once

/* See items. */
uint64_t get_cas_id();

item *do_item_alloc(char *key, const size_t nkey, const unsigned int flags, const rel_time_t exptime, const int nbytes);
item_chunk *do_item_alloc_chunk(item_chunk *ch, const size_t bytes_remain);
item *do_item_alloc_pull(const size_t ntotal, const unsigned int id);
void item_free(item *it);
bool item_size_ok(const size_t nkey, const int flags, const int nbytes);

int do_item_link(item *it, const uint32_t hv);
void do_item_unlink(item *it, const uint32_t hv);
void do_item_unlink_nolock(item *it, const uint32_t hv);
void do_item_remove(item *it);
void do_item_update(item *it);
void do_item_update_nolock(item *it);
int do_item_replace(item *it, item *new_it, const uint32_t hv);

int item_is_flushed(item *it);

/**
 * stats getter for slab automover
 */
typedef struct {
    int64_t evicted;
    int64_t outofmemory;
    uint32_t age;
} item_stats_automove;
void fill_item_stats_automove(item_stats_automove *am);

int init_lru_maintainer(void);


