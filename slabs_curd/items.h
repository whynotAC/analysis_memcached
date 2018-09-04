#ifndef ITEM_H
#define ITEM_H

#include <cstdint>
#include <string.h>
#include <pthread.h>

#include "memcached.h"

extern pthread_mutex_t lru_locks[POWER_LARGEST];

/*@null@*/
item *do_item_alloc(char *key, const size_t nkey, const unsigned int flags, 
                    const rel_time_t exptime, const int nbytes);
item *do_item_alloc_pull(const size_t ntotal, const unsigned int id);
void item_free(item *it);

int do_item_link(item *it, const uint32_t hv); // may fail if transgresses limits
void do_item_unlink(item *it, const uint32_t hv);
void do_item_unlink_nolock(item *it, const uint32_t hv);
void do_item_remove(item *it);
int do_item_replace(item *it, item *new_it, const uint32_t hv);

int item_is_flushed(item *it);

struct lru_pull_tail_return {
    item *it;
    uint32_t hv;
};

int lru_pull_tail(const int orig_id, const int cur_lru,
                  const uint64_t total_bytes, const uint8_t flags, const rel_time_t max_age,
                  struct lru_pull_tail_return *ret_it);

void item_stats_sizes_add(item *it);
void item_stats_sizes_remove(item *it);

item *do_item_get(const char *key, const size_t nkey, const uint32_t hv, conn *c, const bool do_update);
item *do_item_touch(const char *key, const size_t nkey, uint32_t exptime, const uint32_t hv, conn *c);

#ifdef EXTSTORE
#define STORAGE_delete(e, it) \
    do {  \
        if (it->it_flags & ITEM_HDR) { \
            item_hdr *hdr = (item_hdr *)ITEM_data(it);  \
            extstore_delete(e, hdr->page_id, hdr->page_version, \
                            1, ITEM_ntotal(it));    \
        } \
    } while (0)
#else
#define STORAGE_delete(...)
#endif

#endif
