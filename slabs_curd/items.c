#include "items.h"

#include <cassert>
#include <pthread.h>

#include "assoc.h"
#include "trace.h"
#include "slabs.h"
#include "logger.h"
#include "bipbuffer.h"
#include "memcached.h"

typedef uint32_t (*hash_func)(const void *key, size_t length);
extern hash_func hash;

static void item_link_q(item *it);
static void item_unlink_q(item *it);
static void do_item_unlink_q(item *it);

static unsigned int lru_type_map[4] = {HOT_LRU, WARM_LRU, COLD_LRU, TEMP_LRU};

typedef struct _lru_bump_buf {
    struct _lru_bump_buf *prev;
    struct _lru_bump_buf *next;
    pthread_mutex_t mutex;
    bipbuf_t *buf;
    uint64_t dropped;
} lru_bump_buf;

typedef struct {
    item *it;
    uint32_t hv;
} lru_bump_entry;

static lru_bump_buf *bump_buf_head = NULL;
static lru_bump_buf *bump_buf_tail = NULL;
static pthread_mutex_t bump_buf_lock = PTHREAD_MUTEX_INITIALIZER;
// TODO: tunable? Need bench results
#define LRU_BUMP_BUF_SIZE 8192

static bool lru_bump_async(lru_bump_buf *b, item *it, uint32_t hv);

#define LARGEST_ID POWER_LARGEST
typedef struct {
    uint64_t evicted;
    uint64_t evicted_nonzero;
    uint64_t reclaimed;
    uint64_t outofmemory;
    uint64_t tailrepairs;
    uint64_t expired_unfetched; // items reclaimed but never touched
    uint64_t evicted_unfetched; // items evicted but never touched
    uint64_t evicted_active;    // items evicted that should have been shuffled
    uint64_t crawler_reclaimed;
    uint64_t crawler_items_checked;
    uint64_t lrutail_reflocked;
    uint64_t moves_to_cold;
    uint64_t moves_to_warm;
    uint64_t moves_within_lru;
    uint64_t direct_reclaims;
    uint64_t hits_to_hot;
    uint64_t hits_to_warm;
    uint64_t hits_to_cold;
    uint64_t hits_to_temp;
    rel_time_t evicted_time;
} itemstats_t;

static item *heads[LARGEST_ID];
static item *tails[LARGEST_ID];
static itemstats_t itemstats[LARGEST_ID];
static unsigned int sizes[LARGEST_ID];
static uint64_t sizes_bytes[LARGEST_ID];
static unsigned int *stats_sizes_hist = NULL;
static uint64_t stats_sizes_cas_min = 0;
static int stats_sizes_buckets = 0;

static pthread_mutex_t cas_id_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stats_sizes_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Get the next CAS id for a new item.
 * TODO: refactor some atomics for this
 */
uint64_t get_cas_id(void) {
    static uint64_t cas_id = 0;
    pthread_mutex_lock(&cas_id_lock);
    uint64_t next_id = ++cas_id;
    pthread_mutex_unlock(&cas_id_lock);
    return next_id;
}

static unsigned int temp_lru_size(int slabs_clsid) {
    int id = CLEAR_LRU(slabs_clsid);
    id |= TEMP_LRU;
    unsigned int ret;
    pthread_mutex_lock(&lru_locks[id]);
    ret = sizes_bytes[id];
    pthread_mutex_unlock(&lru_locks[id]);
    return ret;
}


// Enable this for reference-count debugging
#if 1
#define DEBUG_REFCNT(it,op) \
                fprintf(stderr, "item %x refcnt(%c) %d %c%c%c\n", \
                    it, op, it->refcount, \
                    (it->it_flags & ITEM_LINKED) ? 'L' : ' ', \
                    (it->it_flags & ITEM_SLABBED) ? 'S': ' ')
#else
#define DEBUG_REFCNT(it,op) while(0)
#endif


static void item_unlink_q(item *it) {
    pthread_mutex_lock(&lru_locks[it->slabs_clsid]);
    do_item_unlink_q(it);
    pthread_mutex_unlock(&lru_locks[it->slabs_clsid]);
}

extern volatile rel_time_t current_time;

int do_item_link(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_LINK(ITEM_key(it), it->nkey, it->nbytes);
    assert((it->it_flags & (ITEM_LINKED|ITEM_SLABBED)) == 0);
    it->it_flags |= ITEM_LINKED;
    it->time = current_time;

    STATS_LOCK();
    stats_state.curr_bytes += ITEM_ntotal(it);
    stats_state.curr_items += 1;
    stats.total_items += 1;
    STATS_UNLOCK();

    /* Allocate a new CAS ID on link*/
    ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);
    assoc_insert(it, hv);
    item_link_q(it);
    refcount_incr(it);
    item_stats_sizes_add(it);

    return 1;
}

static void do_item_link_q(item *it) { // item is the new head
    item **head, **tail;
    assert((it->it_flags & ITEM_SLABBED) == 0);

    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];
    assert(it != *head);
    assert((*head && *tail) || (*head == 0 && *tail == 0));
    it->prev = 0;
    it->next = *head;
    if (it->next) it->next->prev = it;
    *head = it;
    if (*tail == 0) *tail = it;
    sizes[it->slabs_clsid]++;
#ifdef EXTSTORE
    if (it->it_flags & ITEM_HDR) {
       sizes_bytes[it->slabs_clsid] += (ITEM_ntotal(it) - it->nbytes) + sizeof(item_hdr);
    } else {
        sizes_bytes[it->slabs_clsid] += ITEM_ntotal(it);
    }
#else
    sizes_bytes[it->slabs_clsid] += ITEM_ntotal(it);
#endif

    return;
}

static void item_link_q(item *it) {
    pthread_mutex_lock(&lru_locks[it->slabs_clsid]);
    do_item_link_q(it);
    pthread_mutex_unlock(&lru_locks[it->slabs_clsid]);
}

static void item_link_q_warm(item *it) {
    pthread_mutex_lock(&lru_locks[it->slabs_clsid]);
    do_item_link_q(it);
    itemstats[it->slabs_clsid].moves_to_warm++;
    pthread_mutex_unlock(&lru_locks[it->slabs_clsid]);
}

static void do_item_unlink_q(item *it) {
    item **head, **tail;
    head = &heads[it->slabs_clsid];
    tail = &tails[it->slabs_clsid];

    if (*head == it) {
        assert(it->prev == 0);
        *head = it->next;
    }
    if (*tail == it) {
        assert(it->next == 0);
        *tail = it->prev;
    }
    assert(it->next != it);
    assert(it->prev != it);

    if (it->next) it->next->prev = it->prev;
    if (it->prev) it->prev->next = it->next;
    sizes[it->slabs_clsid]--;
#ifdef EXTSTORE
    if (it->it_flags & ITEM_HDR) {
        sizes_bytes[it->slabs_clsid] -= (ITEM_ntotal(it) - it->nbytes) + sizeof(item_hdr);
    } else {
        sizes_bytes[it->slabs_clsid] -= ITEM_ntotal(it);
    }
#else
    sizes_bytes[it->slabs_clsid] -= ITEM_ntotal(it);
#endif

    return;
}

void do_item_unlink(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    if ((it->it_flags & ITEM_LINKED) != 0) {
        it->it_flags &= ~ITEM_LINKED;
        STATS_LOCK();
        stats_state.curr_bytes -= ITEM_ntotal(it);
        stats_state.curr_items -= 1;
        STATS_UNLOCK();
        item_stats_sizes_remove(it);
        assoc_delete(ITEM_key(it), it->nkey, hv);
        item_unlink_q(it);
        do_item_remove(it);
    }
}

void do_item_unlink_nolock(item *it, const uint32_t hv) {
    MEMCACHED_ITEM_UNLINK(ITEM_key(it), it->nkey, it->nbytes);
    if ((it->it_flags & ITEM_LINKED) != 0) {
        it->it_flags &= ~ITEM_LINKED;
        STATS_LOCK();
        stats_state.curr_bytes -= ITEM_ntotal(it);
        stats_state.curr_items -= 1;
        STATS_UNLOCK();
        item_stats_sizes_remove(it);
        assoc_delete(ITEM_key(it), it->nkey, hv);
        do_item_unlink_q(it);
        do_item_remove(it);
    }
}

void do_item_remove(item *it) {
    MEMCACHED_ITEM_REMOVE(ITEM_key(it), it->nkey, it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0);
    assert(it->refcount > 0);

    if (refcount_decr(it) == 0) {
        item_free(it);
    }
}

/**
 * Copy/paste to avoid adding two extra branches for all common calls, since
 * _nolock is only used in an uncommon case where we want to relink
 */

void do_item_update_nolock(item *it) {
    MEMCACHED_ITEM_UPDATE(ITEM_key(it), it->nkey, it->nbytes);
    if (it->time < current_time - ITEM_UPDATE_INTERVAL) {
        assert((it->it_flags & ITEM_SLABBED) == 0);

        if ((it->it_flags & ITEM_LINKED) != 0) {
            do_item_unlink_q(it);
            it->time = current_time;
            do_item_link_q(it);
        }
    }
}

// Bump the last accessed time, or relink if we're in compat mode
void do_item_update(item *it) {
    MEMCACHED_ITEM_UPDATE(ITEM_key(it), it->nkey, it->nbytes);

    // Hits to COLD_LRU immediately move to WARM
    if (settings.lru_segmented) {
        assert((it->it_flags & ITEM_SLABBED) == 0);
        if ((it->it_flags & ITEM_LINKED) != 0) {
            if (ITEM_lruid(it) == COLD_LRU && (it->it_flags & ITEM_ACTIVE)) {
                it->time = current_time;
                item_unlink_q(it);
                it->slabs_clsid = ITEM_clsid(it);
                it->slabs_clsid |= WARM_LRU;
                it->it_flags &= ~ITEM_ACTIVE;
                item_link_q_warm(it);
            } else if (it->time < current_time - ITEM_UPDATE_INTERVAL) {
                it->time = current_time;
            }
        }
    } else if (it->time < current_time - ITEM_UPDATE_INTERVAL) {
        assert((it->it_flags & ITEM_SLABBED) == 0);

        if ((it->it_flags & ITEM_LINKED) != 0) {
            it->time = current_time;
            item_unlink_q(it);
            item_link_q(it);
        }
    }
}

int do_item_replace(item *it, item *new_it, const uint32_t hv) {
    MEMCACHED_ITEM_REPLACE(ITEM_key(it), it->nkey, it->nbytes,
                            ITEM_key(new_it), new_it->nkey, new_it->nbytes);
    assert((it->it_flags & ITEM_SLABBED) == 0);

    do_item_unlink(it, hv);
    return do_item_link(new_it, hv);
}

void item_free(item *it) {
    size_t ntotal = ITEM_ntotal(it);
    unsigned int clsid;
    assert((it->it_flags & ITEM_LINKED) == 0);
    assert(it != heads[it->slabs_clsid]);
    assert(it != tails[it->slabs_clsid]);
    assert(it->refcount == 0);

    /* so slab size changer can tell later if item is already free or not*/
    clsid = ITEM_clsid(it);
    DEBUG_REFCNT(it, 'F');
    slabs_free(it, ntotal, clsid);
}

void item_stats_sizes_add(item *it) {
    if (stats_sizes_hist == NULL || stats_sizes_cas_min > ITEM_get_cas(it))
        return;
    int ntotal = ITEM_ntotal(it);
    int bucket = ntotal / 32;
    if ((ntotal % 32) != 0) bucket++;
    if (bucket < stats_sizes_buckets) stats_sizes_hist[bucket]++;
}

/*  I think there's no way for this to be accurate without using the CAS
 *  value.Since items getting their time value bumped will pass this 
 *  validation
 */
void item_stats_sizes_remove(item *it) {
    if (stats_sizes_hist == NULL || stats_sizes_cas_min > ITEM_get_cas(it))
        return;
    int ntotal = ITEM_ntotal(it);
    int bucket = ntotal / 32;
    if ((ntotal % 32) != 0) bucket++;
    if (bucket < stats_sizes_buckets) stats_sizes_hist[bucket]--;
}

/*
 *  Generates the variable-sized part of the header for an object.
 *  
 *  key     - The key
 *  nkey    - The length of the key
 *  flags   - key flags
 *  nbytes  - Number of bytes to hold value and addition CRLF terminator
 *  suffix  - Buffer for the "VALUE" line suffix (flags, size).
 *  nsuffix - The length of the suffix is stored here
 *
 *  Returns the total size of the header
 */
static size_t item_make_header(const uint8_t nkey, const unsigned int flags, const int nbytes,
                               char *suffix, uint8_t *nsuffix) {
    if (settings.inline_ascii_response) {
        /* suffix is defined at 40 chars elsewhere...*/
        *nsuffix = (uint8_t)snprintf(suffix, 40, " %u %d\r\n", flags, nbytes - 2);
    } else {
        if (flags == 0) {
            *nsuffix = 0;
        } else {
            *nsuffix = sizeof(flags);
        }
    }
    return sizeof(item) + nkey + *nsuffix + nbytes;
}

item *do_item_alloc_pull(const size_t ntotal, const unsigned int id) {
    item *it = NULL;
    int i;
    /*  If no memory is available, attempt a direct LRU juggle/eviction */
    /*  This is a race in order to simplify lru_pull_tail; in cases where
     *  locked items are on the tail, you want them to fail out and cause
     *  occasional OOM's, rather than internally work around them.
     *  This also gives one fewer code path for slab alloc/free
     */
    for (i = 0; i < 10; ++i) {
        uint64_t total_bytes;
        /* Try to reclaim memory first */
        if (!settings.lru_segmented) {
            lru_pull_tail(id, COLD_LRU, 0, 0, 0, NULL);
        }
        it = (item *)slabs_alloc(ntotal, id, &total_bytes, 0);

        if (settings.temp_lru)
            total_bytes -= temp_lru_size(id);

        if (it == NULL) {
            if (lru_pull_tail(id, COLD_LRU, total_bytes, LRU_PULL_EVICT, 0, NULL) <= 0) {
                if (settings.lru_segmented) {
                    lru_pull_tail(id, HOT_LRU, total_bytes, 0, 0, NULL);
                } else {
                    break;
                }
            }
        } else {
            break;
        }
    }

    if (i > 0) {
        pthread_mutex_lock(&lru_locks[id]);
        itemstats[id].direct_reclaims += i;
        pthread_mutex_unlock(&lru_locks[id]);
    }

    return it;
}

/**
 * Chain another chunk onto this chunk
 * slab mover: if it finds a chunk without ITEM_CHUNK flag, and no ITEM_LINKED
 * flag, it counts as busy and skips.
 * I think it might still not be safe to do linking outside of the slab lock.
 */
item_chunk *do_item_alloc_chunk(item_chunk *ch, const size_t bytes_remain) {
    // TODO: Should be a cleaner way of finding real size with slabber calls
    size_t size = bytes_remain + sizeof(item_chunk);
    if (size > settings.slab_chunk_size_max)
        size = settings.slab_chunk_size_max;
    unsigned int id = slabs_clsid(size);

    item_chunk *nch = (item_chunk *) do_item_alloc_pull(size, id);
    if (nch == NULL)
        return NULL;

    // link in
    // ITEM_CHUNK[ED] bits need to be protected by the slabs lock
    slabs_mlock();
    nch->head = ch->head;
    ch->next = nch;
    nch->prev = ch;
    nch->next = 0;
    nch->used = 0;
    nch->slabs_clsid = id;
    nch->size = size - sizeof(item_chunk);
    nch->it_flags |= ITEM_CHUNK;
    slabs_munlock();
    return nch;
}

item *do_item_alloc(char *key, const size_t nkey, const unsigned int flags,
                    const rel_time_t exptime, const int nbytes) {
    uint8_t nsuffix;
    item *it = NULL;
    char suffix[40];
    // Avoid potential underflows.
    if (nbytes < 2)
        return 0;

    size_t ntotal = item_make_header(nkey + 1, flags, nbytes, suffix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    unsigned int id = slabs_clsid(ntotal);
    unsigned int hdr_id = 0;
    if (id == 0)
        return 0;

    /*
     *  This is a large item. Allocate a header object now, lazily allocate
     *  chunks while reading the upload.
     */
    if (ntotal > settings.slab_chunk_size_max) {
        /*
         *  We still link this item into the LRU for the larger slab class, but
         *  we're pulling a header from an entirely different slab class. The
         *  free routines handle large items specifically.
         */
        int htotal = nkey + 1 + nsuffix + sizeof(item) + sizeof(item_chunk);
        if (settings.use_cas) {
            htotal += sizeof(uint64_t);
        }
        hdr_id = slabs_clsid(htotal);
        it = do_item_alloc_pull(htotal, hdr_id);
        /* setting ITEM_CHUNKED is fine here because we aren't LINKED yet.*/
        if (it != NULL)
            it->it_flags |= ITEM_CHUNKED;
    } else {
        it = do_item_alloc_pull(ntotal, id);
    }

    if (it == NULL) {
        pthread_mutex_lock(&lru_locks[id]);
        itemstats[id].outofmemory++;
        pthread_mutex_unlock(&lru_locks[id]);
        return NULL;
    }

    assert(it->slabs_clsid == 0);
    //assert(it != heads[id]);
    
    /* Refcount is seeded to 1 by slabs_alloc() */
    it->next = it->prev = 0;

    /*
     * Items are initially loaded into the HOT_LRU. This is '0' but I want at
     * least a note here. Compiler (hopefully?) optimizes this out.
     */
    if (settings.temp_lru &&
            exptime - current_time <= settings.temporary_ttl) {
        id |= TEMP_LRU;
    } else if (settings.lru_segmented) {
        id |= HOT_LRU;
    } else {
        /* There is only COLD in compat-mode */
        id |= COLD_LRU;
    }
    it->slabs_clsid = id;

    DEBUG_REFCNT(it, '*');
    it->it_flags |= settings.use_cas ? ITEM_CAS : 0;
    it->nkey = nkey;
    it->nbytes = nbytes;
    memcpy(ITEM_key(it), key, nkey);
    it->exptime = exptime;
    if (settings.inline_ascii_response) {
        memcpy(ITEM_suffix(it), suffix, (size_t)nsuffix);
    } else if (nsuffix > 0) {
        memcpy(ITEM_suffix(it), &flags, sizeof(flags));
    }
    it->nsuffix = nsuffix;

    /* Initialize internal chunk. */
    if (it->it_flags & ITEM_CHUNKED) {
        item_chunk *chunk = (item_chunk*) ITEM_data(it);

        chunk->next = 0;
        chunk->prev = 0;
        chunk->used = 0;
        chunk->size = 0;
        chunk->head = (item_chunk*)it;
        chunk->orig_clsid = hdr_id;
    }
    it->h_next = 0;

    return it;
}

/*** LRU MAINTENANCE THREAD  ***/
/*  Returns number of items remove, expired, or evicted.
 *  Callable from worker threads or the LRU maintainer thread
 */
int lru_pull_tail(const int orig_id, const int cur_lru,
                  const uint64_t total_bytes, const uint8_t flags, const rel_time_t max_age,
                  struct lru_pull_tail_return *ret_it) {
    item *it = NULL;
    int id = orig_id;
    int removed = 0;
    if (id == 0)
        return 0;

    int tries = 0;
    item *search;
    item *next_it;
    void *hold_lock = NULL;
    unsigned int move_to_lru = 0;
    uint64_t limit = 0;

    id |= cur_lru;
    pthread_mutex_lock(&lru_locks[id]);
    search = tails[id];
    /* We walk up *only* for locked items, and if bottom is expired */
    for (; tries > 0 && search != NULL; tries--, search=next_it) {
        /* we might relink search mid-loop, so search->prev isn't reliable */
        next_it = search->prev;
        if (search->nbytes == 0 && search->nkey == 0 && search->it_flags == 1) {
            /* We are a crawler, ignore it */
            if (flags & LRU_PULL_CRAWL_BLOCKS) {
                pthread_mutex_unlock(&lru_locks[id]);
                return 0;
            }
            tries++;
            continue;
        }
        uint32_t hv = hash(ITEM_key(search), search->nkey);
        /*  Attempt to hash item lock the "search" item. If locked, no
         *  other callers can incr the refcount. Also skip ourselves.
         */
        if ((hold_lock = item_trylock(hv)) == NULL)
            continue;
        /* Now see if the item is refcount locked */
        if (refcount_incr(search) != 2) {
            /* Note pathological case with ref'ed items in tail */
            /* Can still unlink the item, but it won't be reusable yet */
            itemstats[id].lrutail_reflocked++;
            /*  In case of refcount leaks, enable for quick workload.
             *  WARNING: This can cause terrible corruption
             */
            if (settings.tail_repair_time &&
                    search->time + settings.tail_repair_time < current_time) {
                itemstats[id].tailrepairs++;
                search->refcount = 1;
                /* This will call item_remove -> item_free since refcnt is 1 */
                STORAGE_delete(ext_storage, search);
                do_item_unlink_nolock(search, hv);
                item_trylock_unlock(hold_lock);
                continue;
            }
        }

        /* Expired or flushed */
        if ((search->exptime != 0 && search->exptime < current_time)
            || item_is_flushed(search)) {
            itemstats[id].reclaimed++;
            if ((search->it_flags & ITEM_FETCHED) == 0) {
                itemstats[id].expired_unfetched++;
            }
            /* refcnt 2 -> 1 */
            do_item_unlink_nolock(search, hv);
            STORAGE_delete(ext_storage, search);
            /* refcnt 1 -> 0 -> item_free */
            do_item_remove(search);
            item_trylock_unlock(hold_lock);
            removed++;

            /* If all we're finding are expired, can keep going */
            continue;
        }

        /*  If we're HOT_LRU or WARM_LRU and over size limit, send to COLD_LRU
         *  If we're COLD_LRU, send to WARN_LRU unless we need to evict.
         */
        switch (cur_lru) {
        case HOT_LRU:
            limit = total_bytes * settings.hot_lru_pct / 100;
        case WARM_LRU:
            if (limit == 0)
                limit = total_bytes * settings.warm_lru_pct / 100;
            /* Rescue ACTIVE items arrgressively */
            if ((search->it_flags & ITEM_ACTIVE) != 0) {
                search->it_flags &= ~ITEM_ACTIVE;
                removed++;
                if (cur_lru == WARM_LRU) {
                    itemstats[id].moves_within_lru++;
                    do_item_update_nolock(search);
                    do_item_remove(search);
                    item_trylock_unlock(hold_lock);
                } else {
                    /* Active HOT_LRU items flow to WARM */
                    itemstats[id].moves_to_warm++;
                    move_to_lru = WARM_LRU;
                    do_item_unlink_q(search);
                    it = search;
                }
            } else if (sizes_bytes[id] > limit ||
                       current_time - search->time > max_age) {
                itemstats[id].moves_to_cold++;
                move_to_lru = COLD_LRU;
                do_item_unlink_q(search);
                it = search;
                removed++;
                break;
            } else {
                /* Don't want to move to COLD, not active, bail out */
                it = search;
            }
            break;
        case COLD_LRU:
            it = search;    // No matter what, we're stopping
            if (flags & LRU_PULL_EVICT) {
                if (settings.evict_to_free == 0) {
                    // Don't think we need a counter for this. It'll OOM.
                    break;
                }
                itemstats[id].evicted++;
                itemstats[id].evicted_time = current_time - search->time;
                if (search->exptime != 0)
                    itemstats[id].evicted_nonzero++;
                if ((search->it_flags & ITEM_FETCHED) == 0) {
                    itemstats[id].evicted_unfetched++;
                }
                if ((search->it_flags & ITEM_ACTIVE)) {
                    itemstats[id].evicted_active++;
                }
                LOGGER_LOG(NULL, LOG_EVICTIONS, LOGGER_EVICTION, search);
                STORAGE_delete(ext_storage, search);
                do_item_unlink_nolock(search, hv);
                removed++;
                if (settings.slab_automove == 2) {
                    slabs_reassign(-1, orig_id);
                }
            } else if (flags & LRU_PULL_RETURN_ITEM) {
                // Keep a reference to this item and return it
                ret_it->it = it;
                ret_it->hv = hv;
            } else if ((search->it_flags & ITEM_ACTIVE) != 0 
                        && settings.lru_segmented) {
                itemstats[id].moves_to_warm++;
                search->it_flags &= ~ITEM_ACTIVE;
                move_to_lru = WARM_LRU;
                do_item_unlink_q(search);
                removed++;
            }
            break;
        case TEMP_LRU:
            it = search;    // Kill the loop. Parent only interested in reclaim
            break;
        }
        if (it != NULL)
            break;
    }

    pthread_mutex_unlock(&lru_locks[id]);

    if (it != NULL) {
        if (move_to_lru) {
            it->slabs_clsid = ITEM_clsid(it);
            it->slabs_clsid |= move_to_lru;
            item_link_q(it);
        }
        if ((flags & LRU_PULL_RETURN_ITEM) == 0) {
            do_item_remove(it);
            item_trylock_unlock(hold_lock);
        }
    }

    return removed;
}

int item_is_flushed(item *it) {
    rel_time_t oldest_live = settings.oldest_live;
    uint64_t cas = ITEM_get_cas(it);
    uint64_t oldest_cas = settings.oldest_cas;
    if (oldest_live == 0 || oldest_live > current_time)
        return 0;
    if ((it->time <= oldest_live)
            || (oldest_cas != 0 && cas != 0 && cas < oldest_cas)) {
        return 1;
    }
    return 0;
}

/** wrapper around assoc_find which does the lazy expiration logic */
item *do_item_get(const char *key, const size_t nkey, const uint32_t hv, conn *c, const bool do_update) {
    item *it = assoc_find(key, nkey, hv);
    if (it != NULL) {
        refcount_incr(it);
        /* Optimization for slab reassignment. prevents popular items from
         * jamming in busy wait. Can only do this here to satisfy lock order
         * of item_lock, slabs_lock. */
        /* This was made unsafe by removal of the cache_lock:
         * slab_rebalance_signal and slab_rebal.* are modified in a separate
         * thread under slabs_lock.If slab_rebalance_signal = 1, slab_start =
         * NULL(0), but slab_end is still equall to some value, this would end
         * up unlinking every item fetched.
         * This is either an acceptable loss, or if slab_rebalance_signal is 
         * ture, slab_start/slab_end should be put behind the slabs_lock.
         * Which would cause a huge potential slowdown.
         * Could also use a specific lock for slab_rebal.* and 
         * slab_rebalance_signal (shorter lock?)
         */
        /* if (slab_rebalance_signal && 
         *      ((void*)it >= slab_rebal.slab_start && (void *)it < slab_rebal.slab_end)) {
         *      do_item_unlink(it, hv);
         *      do_item_remove(it);
         *      it = NULL;
         * }
         */
    }
    int was_found = 0;

    if (settings.verbose > 2) {
        int ii;
        if (it == NULL) {
            fprintf(stderr, "> NOT FOUND ");
        } else {
            fprintf(stderr, "> FOUND KEY ");
        }
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }
    }

    if (it != NULL) {
        was_found = 1;
        if (item_is_flushed(it)) {
            do_item_unlink(it, hv);
            STORAGE_delete(c->thread->storage, it);
            do_item_remove(it);
            it = NULL;
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.get_flushed++;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            if (settings.verbose > 2) {
                fprintf(stderr, " -nuked by flush ");
            }
            was_found = 2;
        } else if (it->exptime != 0 && it->exptime <= current_time) {
            do_item_unlink(it, hv);
            STORAGE_delete(c->thread->storage, it);
            do_item_remove(it);
            it = NULL;
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.get_expired++;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            if (settings.verbose > 2) {
                fprintf(stderr, " -nuked by expire ");
            }
            was_found = 3;
        } else {
            if (do_update) {
                /* We update the hit markers only during fetches.
                 * An item needs to be hit twice overall to be considered
                 * ACTIVE, but only needs a single hit to maintain activity
                 * afterward.
                 * FETCHED tells if an item has ever been active.
                 */
                if (settings.lru_segmented) {
                    if ((it->it_flags & ITEM_ACTIVE) == 0) {
                        if ((it->it_flags & ITEM_FETCHED) == 0) {
                            it->it_flags |= ITEM_FETCHED;
                        } else {
                            it->it_flags |= ITEM_ACTIVE;
                            if (ITEM_lruid(it) != COLD_LRU) {
                                do_item_update(it); // bump LA time
                            } else if (!lru_bump_async((lru_bump_buf *)c->thread->lru_bump_buf, it, hv)) {
                                // add flag before async bump to avoid race.
                                it->it_flags &= ~ITEM_ACTIVE;
                            }
                        }
                    }
                } else {
                    it->it_flags |= ITEM_FETCHED;
                    do_item_update(it);
                }
            }
            DEBUG_REFCNT(it, '+');
        }
    }

    if (settings.verbose > 2)
        fprintf(stderr, "\n");
    /* For new this is in addition to the above verbose logging. */
    LOGGER_LOG(c->thread->l, LOG_FETCHERS, LOGGER_ITEM_GET, NULL, was_found, key, nkey, 
                (it) ? ITEM_clsid(it) : 0);
    return it;
}

item *do_item_touch(const char *key, size_t nkey, uint32_t exptime, 
                    const uint32_t hv, conn *c) {
    item *it = do_item_get(key, nkey, hv, c, DO_UPDATE);
    if (it != NULL) {
        it->exptime = exptime;
    }
    return it;
}

static bool lru_bump_async(lru_bump_buf *b, item *it, uint32_t hv) {
    bool ret = false;
    refcount_incr(it);
    pthread_mutex_lock(&b->mutex);
    lru_bump_entry *be = (lru_bump_entry *) bipbuf_request(b->buf, sizeof(lru_bump_entry));
    if (be != NULL) {
        be->it = it;
        be->hv = hv;
        if (bipbuf_push(b->buf, sizeof(lru_bump_entry)) == 0) {
            ret = false;
            b->dropped++;
        }
    } else {
        ret = false;
        b->dropped++;
    }
    if (!ret) {
        refcount_decr(it);
    }
    pthread_mutex_unlock(&b->mutex);
    return ret;
}
