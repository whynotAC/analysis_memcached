#pragma once

#define HOT_LRU 0
#define WARM_LRU 64
#define COLD_LRU 128
#define TEMP_LRU 192

#define CLEAR_LRU(id) (id & ~(3<<6))
#define GET_LRU(id) (id & (3<<6))

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

#define LRU_PULL_EVICT 1
#define LRU_PULL_CRAWL_BLOCKS 2
#define LRU_PULL_RETURN_ITEM 4

struct lru_pull_tail_return {
    item *it;
    uint32_t hv;
};
int lru_pull_tail(const int orig_id, const int cur_lru,
        const uint64_t total_bytes, const uint8_t flags,
        const rel_time_t max_age, struct lru_pull_tail_return *ret_it);

void item_stats(ADD_STAT add_stats, void *c);
void item_stats_totals(ADD_STAT add_stats, void *c);

void item_stats_sizes(ADD_STAT add_stats, void *c);
void item_stats_sizes_init(void);
void item_stats_sizes_enable(ADD_STAT add_stats, void *c);
void item_stats_sizes_disable(ADD_STAT add_stats, void *c);
void item_stats_sizes_add(item *it);
void item_stats_sizes_remove(item *it);
bool item_stats_sizes_status(void);

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


