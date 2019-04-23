/*-*- Mode: C; table-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*-*/
#include "memcached.h"


/* Forward Declarations */
static void item_link_q(item *it);
static void item_unlink_q(item *it);

static unsigned int lru_type_map[4] = {HOT_LRU, WARM_LRU, COLD_LRU, TEMP_LRU};

#define LARGEST_ID POWER_LARGEST
typedef struct {
    uint64_t evicted;
    uint64_t evicted_nonzero;
    uint64_t reclaimed;
    uint64_t outofmemory;
    uint64_t tailrepairs;
    uint64_t expired_unfetched; // items reclaimed but never touched
    uint64_t evicted_unfetched; // items evicted but never touched
    uint64_t evicted_active; // items evicted that should have been shuffled
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

static volatile int do_run_lru_maintainer_thread = 0;
static int lru_maintainer_initialized = 0;
static pthread_mutex_t lru_maintainer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t cas_id_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t stats_sizes_lock = PTHREAD_MUTEX_INITIALIZER;

void item_stats_reset(void) {
    int i;
    for (i = 0; i < LARGEST_ID; i++) {
        pthread_mutex_lock(&lru_locks[i]);
        memset(&itemstats[i], 0, sizeof(itemstats_t));
        pthread_mutex_unlock(&lru_lock[i]);
    }
}

/* called with class lru lock held */
void do_item_stats_add_crawl(const int i, const uint64_t reclaimed,
        const uint64_t unfetched, const uint64_t checked) {
    itemstats[i].crawler_reclaimed += reclaimed;
    itemstats[i].expired_unfetched += unfetched;
    itemstats[i].crawler_items_checked += checked;
}

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
/* TODO: tunable? Need bench results */
#define LRU_BUMP_BUF_SIZE 8192

static bool lru_bump_async(lru_bump_buf *b, item *it, uint32_t hv);
static uint64_t lru_total_bumps_dropped(void);

/* Get the next CAS id for a new item. */
/* TODO: refactor some atomics for this */
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

/* must be locked before call */
unsigned int do_get_lru_size(uint32_t id) {
    return sizes[id];
}

/* Enable this for reference-count debugging. */
#if 0
#define DEBUG_REFCNT(it, op) \
            fprintf(stderr, "item %x refcnt(%c) %d %c%c%c\n", \
                it, op, it->refcount, \
                (it->it_flags & ITEM_LINKED) ? 'L' : ' ', \
                (it->it_flags & ITEM_SLABBED) ? 'S' : ' ')
#else
#define DEBUG_REFCNT(it, op) while(0)
#endif

/**
 * Generates the variable-sized part of the header for an object
 *
 * key          - They key
 * nkey         - The length of the key
 * flags        - key flags
 * nbytes       - Number of bytes to hold value and addition CRLF terminator
 * suffix       - Buffer for the "VALUE" line suffix (flags, size)
 * nsuffix      - The length of the suffix is stored here.
 *
 * Returns the total size of the header.
 */
static size_t item_make_header(const uint8_t nkey, const unsigned int flags, const int nbytes,
                                char *suffix, uint8_t *nsuffix) {
    if (settings.inline_ascii_response) {
        /* suffix is defined at 40 chars elsewhere.. */
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
    /* If no memory is available, attemp a direct LRU juggle/eviction */
    /* This is a race in order to simplify lru_pull_tail; in cases where
     * locked items are on the tail, you want them to fall out and case
     * occasional OOM's, rather than internally work around them.
     * This also gives one fewer code path for slab alloc/free
     */
    for (i = 0; i < 10; i++) {
        uint64_t total_bytes;
        /* Try to reclaim memory first */
        if (!settings.lru_segmented) {
            lru_pull_tail(id, COLD_LRU, 0, 0, 0, NULL);
        }
        it = slabs_alloc(ntotal, id, &total_bytes, 0);

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
