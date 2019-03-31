#include "memcached.h"



extern pthread_mutex_t lru_locks[POWER_LARGEST];
extern stats_state stats_state;
extern stats stats;

static unsigned int lru_type_map[4] = {HOT_LRU, WARM_LRU, COLD_LRU, TEMP_LRU};

#define LARGEST_ID POWER_LARGEST
typedef struct {
    uint64_t evicted;
    uint64_t evicted_nonzero;
    uint64_t reclaimed;
    uint64_t outofmemory;
    uint64_t tailrepairs;
    uint64_t expired_unfetched; /* items reclaimed but nerver touched */
    uint64_t evicted_unfetched; /* items evicted but never touched */
    uint64_t evicted_active; /* items evicted that should have been shuffled */
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

static int lru_maintainer_initialized = 0;
static pthread_mutex_t lru_maintainer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cas_id_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    unsigned long int size;

    /* region A */
    unsigned int a_start, a_end;

    /* region B */
    unsigned int b_end;

    /* is B inuse? */
    int b_inuse;

    unsigned char data[];
} bipbuf_t;

typedef struct _lru_bump_buf {
    struct _lru_bump_buf *prev;
    struct _lru_bump_buf *next;
    pthread_mutex_t mutex;
    bipbuf_t *buf;
    uint64_t dropped;
} lru_bump_buf;

static bool lru_bump_async(lru_bump_buf *b, item *it, uint32_t hv);
static uint64_t lru_total_bumps_dropped(void) {
    return 0;
}

/* Get the next CAS id for a new item. */
uint64_t get_cas_id(void) {
    static uint64_t cas_id = 0;
    pthread_mutex_lock(&cas_id_lock);
    uint64_t next_id = ++cas_id;
    pthread_mutex_unlock(&cas_id_lock);
    return next_id;
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

static unsigned int temp_lru_size(int slabs_clsid) {
    int id = CLEAR_LRU(slabs_clsid);
    id |= TEMP_LRU;
    unsigned int ret;
    pthread_mutex_lock(&lru_locks[id]);
    ret = sizes_bytes[id];
    pthread_mutex_unlock(&lru_locks[id]);
    return ret;
}

/**
 * Generates the variable-sized part of the header for an object
 *
 * key          - The key
 * nkey         - The length of the key
 * flags        - key flags
 * nbytes       - Number of bytes to hold value and addition CRLF terminator
 * suffix       - Buffer for the "VALUE" line suffix (flags, size).
 * nsuffix      - The length of the suffix is stored here.
 *
 * Returns the total size of the header.
 */
static size_t item_make_header(const uint8_t nkey, const unsigned int flags, 
        const int nbytes, char *suffix, uint8_t *nsuffix) {
    if (settings.inline_ascii_response) {
        *nsuffix = (uint8_t) snprintf(suffix, 40, " %u %d\r\n", flags, nbytes - 2);
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
    /**
     * If no memory is available, attempt a direct LRU juggle/eviction
     * This is a race in order to simplify lru_pull_tail; in cases where
     * locked items are on the tail, you want them to fall out and cause
     * occasional OOM's, rather than internally work around them.
     * This also gives one fewer code path for slab alloc/free
     */
    for (i = 0; i < 10; i++) {
        uint64_t total_bytes;
        // Try to reclaim memory first
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

/* Chain another chunk onto this chunk.
 * slab mover: if it finds a chunk without ITEM_CHUNK flag, and no ITEM_LINKED
 * flag, it counts as busy and skips.
 * I think it might still not be safe to do linking outside of the slab lock
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

    // link in.
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

    // Avoid potential underflows
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

    /** This is a large item. Allocate a header object now, lazily allocate
     * chunks while reading the upload.
     */
    // 如果分配的内存空间大于settings.slab_chunk_size_max时,内存空间将使用两部分组成
    // item ----- item_chunk
    if (ntotal > settings.slab_chunk_size_max) {
        /** We still link this item into LRU for the larger slab class, but
         * we're pulling a header from an entirely different slab class.
         * The free routines handle large items specifically.
         */
        int htotal = nkey + 1 + nsuffix + sizeof(item) 
                        + sizeof(item_chunk);
        if (settings.use_cas) {
            htotal += sizeof(uint64_t);
        }
        hdr_id = slabs_clsid(htotal);
        it = do_item_alloc_pull(htotal, hdr_id);
        /* setting ITEM_CHUNKED is fine here because we aren't LINKED yet*/
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
    // assert(it != heads[id]);
    
    // Refcount is seeded to 1 by slabs_alloc()
    it->next = it->prev = 0;

    /**
     * Items are initially loaded into the HOT_LRU. This is '0' but I want
     * at least a note here. Compiler (hopefully?) optimizes this out.
     */
    if (settings.temp_lru &&
            exptime - current_time <= settings.temporary_ttl) {
        id |= TEMP_LRU;
    } else if (settings.lru_segmented) {
        id |= HOT_LRU;
    } else {
        // There is only COLD in compat-mode
        id |= COLD_LRU;
    }
    it->slabs_clsid = id;
    
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

    // Initialize internal chunk.
    if (it->it_flags & ITEM_CHUNKED) {
        item_chunk *chunk = (item_chunk *) ITEM_data(it);

        chunk->next = 0;
        chunk->prev = 0;
        chunk->used = 0;
        chunk->size = 0;
        chunk->head = it;
        chunk->orig_clsid = hdr_id;
    }
    it->h_next = 0;

    return it;
}

void item_free(item *it) {
    size_t ntotal = ITEM_ntotal(it);
    unsigned int clsid;
    assert((it->it_flags & ITEM_LINKED) == 0);
    assert(it != heads[it->slabs_clsid]);
    assert(it != tails[it->slabs_clsid]);
    assert(it->refcount == 0);

    /*
     * so slab size changer can tell later if item is already free or not.
     */
    clsid = ITEM_clsid(it);
    slabs_free(it, ntotal, clsid);
}

/**
 * Returns true if an item will fit in the cache (its size does not exceed
 * the maximum for a cache entry.)
 */
bool item_size_ok(const size_t nkey, const int flags, const int nbytes) {
    char prefix[40];
    uint8_t nsuffix;
    if (nbytes < 2)
        return false;

    size_t ntotal = item_make_header(nkey + 1, flags, nbytes,
                                     prefix, &nsuffix);
    if (settings.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    return slabs_clsid(ntotal) != 0;
}

static void do_item_link_q(item *it) { // item is the now head
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

static void item_unlink_q(item *it) {
    pthread_mutex_lock(&lru_locks[it->slabs_clsid]);
    do_item_unlink_q(it);
    pthread_mutex_unlock(&lru_locks[it->slabs_clsid]);
}

int do_item_link(item *it, const uint32_t hv) {
    assert((it->it_flags & (ITEM_LINKED|ITEM_SLABBED)) == 0);
    it->it_flags |= ITEM_LINKED;
    it->time = current_time;

    STATS_LOCK();
    stats_state.curr_bytes += ITEM_ntotal(it);
    stats_state.curr_items += 1;
    stats.total_items += 1;
    STATS_UNLOCK();

    // Allocate a new CAS ID on link
    ITEM_set_cas(it, (settings.use_cas) ? get_cas_id() : 0);
    assoc_insert(it, hv);
    item_link_q(it);
    refcount_incr(it);
    item_stats_sizes_add(it);

    return 1;
}

void do_item_unlink(item *it, const uint32_t hv) {
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

/* FIXME: Is it necessary to keep this copy/pasted code? */
void do_item_unlink_nolock(item *it, const uint32_t hv) {
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

void od_item_remove(item *it) {
    assert((it->it_flags & ITEM_SLABBED) == 0);
    assert(it->refcount > 0);

    if (refcount_decr(it) == 0) {
        item_free(it);
    }
}

/**
 * Copy/paste to avoid adding two extra branches for all common calls,
 * since _nolock is only used in an uncommon case where we want to relink.
 */
void do_item_update_nolock(item *it) {
    if (it->time < current_time - ITEM_UPDATE_INTERVAL) {
        assert((it->it_flags & ITEM_SLABBED) == 0);

        if ((it->it_flags & ITEM_LINKED) != 0) {
            do_item_unlink_q(it);
            it->time = current_time;
            do_item_link_q(it);
        }
    }
}

/* Bump the last accessed time, or relink if we're in compat mode */
void do_item_update(item *it) {
    if (settings.lru_segmented) {
        assert((it->it_flags & ITEM_SLABBED) == 0);
        if ((it->it_flags & ITEM_LINKED) != 0) {
            if (ITEM_lruid(it) == COLD_LRU 
                    && (it->it_flags & ITEM_ACTIVE)) {
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
    assert((it->it_flags & ITEM_SLABBED) == 0);

    do_item_unlink(it, hv);
    return do_item_link(new_it, hv);
}



void item_stats_totals(ADD_STAT add_stats, void *c) {
    itemstats_t totals;
    memset(&totals, 0, sizeof(itemstats_t));
    int n;
    for (n = 0; n < MAX_NUMBER_OF_SLAB_CLASSES; n++) {
        int x;
        int i;
        for (x = 0; x < 4; x++) {
            i = n | lru_type_map[x];
            pthread_mutex_lock(&lru_locks[i]);
            totals.expired_unfetched += itemstats[i].expired_unfetched;
            totals.evicted_unfetched += itemstats[i].evicted_unfetched;
            totals.evicted_active += itemstats[i].evicted_active;
            totals.evicted += itemstats[i].evicted;
            totals.reclaimed += itemstats[i].reclaimed;
            totals.crawler_reclaimed += itemstats[i].crawler_reclaimed;
            totals.crawler_items_checked += itemstats[i].crawler_items_checked;
            totals.lrutail_reflocked += itemstats[i].lrutail_reflocked;
            totals.moves_to_cold += itemstats[i].moves_to_cold;
            totals.moves_to_warm += itemstats[i].moves_to_warm;
            totals.moves_within_lru += itemstats[i].moves_within_lru;
            totals.direct_reclaims += itemstats[i].direct_reclaims;
            pthread_mutex_unlock(&lru_locks[i]);
        }
    }
    APPEND_STAT("expired_unfetched", "%llu", 
            (unsigned long long)totals.expired_unfetched);
    APPEND_STAT("evicted_unfetched", "%llu",
            (unsigned long long)totals.evicted_unfetched);
    if (settings.lru_maintainer_thread) {
        APPEND_STAT("evicted_active", "%llu",
            (unsigned long long)totals.evicted_active);
    }
    APPEND_STAT("evictions", "%llu",
            (unsigned long long)totals.evicted);
    APPEND_STAT("reclaimed", "%llu",
            (unsigned long long)totals.reclaimed);
    APPEND_STAT("crawler_reclaimed", "%llu",
            (unsigned long long)totals.crawler_reclaimed);
    APPEND_STAT("crawler_items_checked", "%llu",
            (unsigned long long)totals.crawler_items_checked);
    APPEND_STAT("lrutail_reflocked", "%llu",
            (unsigned long long)totals.lrutail_reflocked);
    if (settings.lru_maintainer_thread) {
        APPEND_STAT("moves_to_cold", "%llu",
            (unsigned long long)totals.moves_to_cold);
        APPEND_STAT("moves_to_warm", "%llu",
            (unsigned long long)totals.moves_to_warm);
        APPEND_STAT("moves_within_lru", "%llu",
            (unsigned long long)totals.moves_within_lru);
        APPEND_STAT("direct_reclaims", "%llu",
            (unsigned long long)totals.direct_reclaims);
        APPEND_STAT("lru_bumps_dropped", "%llu",
            (unsigned long long)lru_total_bumps_dropped());
    }
}

void item_stats(ADD_STAT add_stats, void *c) {
}

bool item_stats_sizes_status(void) {
}

void item_stats_sizes_init(void) {
}

void item_stats_sizes_enable(ADD_STAT add_stats, void *c) {
}

void item_stats_sizes_diable(ADD_STAT add_stats, void *c) {
}

void item_stats_sizes_add(item *it) {
}

void item_stats_sizes_remove(item *it) {
}

/**
 * dumps out a list of objects of each size, with granularity of 32 bytes
 * Locks are correct based on a technicality. Holds LRU lock while doing the
 * work, so items can't go invalid, and it's only looking at header sizes
 * which don't change.
 */
void item_stats_sizes(ADD_STAT add_stats, void *c) {
}

/**
 * wrapper around assoc_find which does the lazy expiration logic 
 */
item *do_item_get(const char *key, const size_t nkey, const uint32_t hv,
                    conn *c, const bool do_update) {
    item *it = assoc_find(key, nkey, hv);
    if (it != NULL) {
        refcount_incr(it);
        /* Optimization for slab reassignment. prevents popular items from
         * jamming in busy wait. Can only do this here to satisfy lock 
         * order of item_lock, slabs_lock.
         * This was made unsafe by removal of the cache_lock:
         * slab_rebalance_signal and slab_rebal.* are modified in a
         * separate thread under slabs_lock. If slab_rebalance_signal = 1,
         * slab_start = NULL(0), but slab_end is still equal to some value
         * this would end up unlinking every item fetched.
         * This is either an acceptable loss, or if slab_rebalance_signal
         * is true, slab_start/slab_end should be put behind the slabs_lock
         * Which would cause a huge potential slowdown
         * Could also use a specific lock for slab_rebal.* and
         * slab_rebalance_signal (shorter lock?)
         */
        /* if (slab_rebalance_signal &&
         *      ((void *)it >= slab_rebal.slab_start && 
         *       (void *)it < slab_rebal.slab_end)) {
         *    do_item_unlink(it, hv);
         *    do_item_remove(it);
         *    it = NULL;
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
            do_item_remove(it);
            it = NULL;
            was_found = 2;
         } else if (it->exptime != 0 && it->exptime <= current_time) {
            do_item_unlink(it, hv);
            do_item_remove(it);
            it = NULL;
            was_found = 3;
         } else {
            if (do_update) {
                /* We update the hit markers only during fetches.
                 * An item needs tb be hit twice overall to be considered
                 * ACTIVE, but only needs a single hit to maintain 
                 * activity afterward.
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
                            } else if (!lru_bump_async((lru_bump_buf *)(c->thread->lru_bump_buf), it, hv)) {
                                // and flag before async bump to avoid race
                                it->it_flags &= ~ITEM_ACTIVE;
                            }
                        }
                    }
                } else {
                    it->it_flags |= ITEM_FETCHED;
                    do_item_update(it);
                }
            }
         }
    }

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

int lru_pull_tail(const int orig_id, const int cur_lru,
        const uint64_t total_bytes, const uint8_t flags, 
        const rel_time_t max_age, struct lru_pull_tail_return *ret_it) {
    return NULL;
}

/**
 * With refactoring of the various stats code the automover won't need a 
 * custom function here.
 */
void fill_item_stats_automove(item_stats_automove *am) {
    int n;
    for (n = 0; n < MAX_NUMBER_OF_SLAB_CLASSES; n++) {
        item_stats_automove *cur = &am[n];

        int i = n | HOT_LRU;
        pthread_mutex_lock(&lru_locks[i]);
        cur->outofmemory = itemstats[i].outofmemory;
        pthread_mutex_unlock(&lru_locks[i]);

        // evictions and tail age are from COLD
        i = n | COLD_LRU;
        pthread_mutex_lock(&lru_locks[i]);
        cur->evicted = itemstats[i].evicted;
        if (tails[i]) {
            cur->age = current_time - tails[i]->time;
        } else {
            cur->age = 0;
        }
        pthread_mutex_unlock(&lru_locks[i]);
    }
}

int init_lru_maintainer(void) {
    if (lru_maintainer_initialized == 0) {
        pthread_mutex_init(&lru_maintainer_lock, NULL);
        lru_maintainer_initialized = 1;
    }
    return 0;
}

static bool lru_bump_async(lru_bump_buf *b, item *it, uint32_t hv) {
    return false;
}
