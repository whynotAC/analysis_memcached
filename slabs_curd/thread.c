#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "assoc.h"
#include "items.h"
#include "memcached.h"

// Locks for cache LRU operations
pthread_mutex_t lru_locks[POWER_LARGEST];

typedef uint32_t (*hash_func)(const void *key, size_t length);
extern hash_func hash;

// lock for global stats
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *item_locks;
/* size of the item lock hash table */
static uint32_t item_lock_count;
unsigned int item_lock_hashpower;
#define hashsize(n) ((unsigned long int)1<<(n))
#define hashmask(n) (hashsize(n)-1)

/**
 * Each libevent instance has a wakeup pipe, which other threads
 * can use to signal that they've put a new connection on this queue
 */
static LIBEVENT_THREAD *threads;

/*  item_lock() must be held for an item before any modifications to either its
 *  associated hash bucket,or the structure itself.
 *  LRU modifications must hold the item lock, and the LRU lock.
 *  LRU's accessing items must item_trylock() before modifying an item.
 *  Items accessible from an LRU must not be freed or modified
 *  without first locking and removing from the LRU.
 */
// 初始化时hash表的item_locks已经初始化确定其值大小，在后续hash表扩展时不会更改item_locks
// 的大小，只会通过hashmask(item_lock_hashpower)的与操作来抢锁执行
void item_lock(uint32_t hv) {
    mutex_lock(&item_locks[hv & hashmask(item_lock_hashpower)]);
}

void *item_trylock(uint32_t hv) {
    pthread_mutex_t *lock = &item_locks[hv & hashmask(item_lock_hashpower)];
    if (pthread_mutex_trylock(lock) == 0) {
        return lock;
    }
    return NULL;
}

void item_trylock_unlock(void *lock) {
    mutex_unlock((pthread_mutex_t *)lock);
}

void item_unlock(uint32_t hv) {
    mutex_unlock(&item_locks[hv & hashmask(item_lock_hashpower)]);
}

// Must not be called with any deeper locks held
void pause_threads(enum pause_thread_types type) {
    char buf[0];
    int i;

    buf[0] = 0;
    switch (type) {
        case PAUSE_ALL_THREADS:
        case PAUSE_WORKER_THREADS:
        case RESUME_ALL_THREADS:
        case RESUME_WORKER_THREADS:
            break;
        default:
            fprintf(stderr, "Unkown lock type: %d\n", type);
            assert(1 == 0);
            break;
    }

    return;
}

/************************************* LIBEVENT THREADS ***********************/

/**
 * Set up a thread's information
 */
static void setup_thread(LIBEVENT_THREAD *me) {
    
    if (pthread_mutex_init(&me->stats.mutex, NULL) != 0) {
        perror("Failed to initialize mutex");
        exit(EXIT_FAILURE);
    }
}

/**
 * Worker thread: main event loop
 */
static void *worker_libevent(void *arg) {
    LIBEVENT_THREAD *me = (LIBEVENT_THREAD*)arg;

    /**
     * Any per-thread setup can happen here; memcached_thread_init() will block
     * until all threads have finished initializing.
     */
    me->lru_bump_buf = item_lru_bump_buf_create();
    if (me->lru_bump_buf == NULL) {
        abort();
    }
}

/************************************* ITEM ACCESS ****************************/

/*
 * Allocates a new item.
 */
item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes) {
    item *it;
    /* do_item_alloc handles its own locks*/
    it = do_item_alloc(key, nkey, flags, exptime, nbytes);
    return it;
}

/*
 *  Return an item if it hasn't marked as expired
 *  lazy-expiring as needed.
 */
item *item_get(const char *key, const size_t nkey, conn *c, const bool do_update) {
    item *it;
    uint32_t hv;
    hv = hash(key, nkey);
    item_lock(hv);
    it = do_item_get(key, nkey, hv, c, do_update);
    item_unlock(hv);
    return it;
}

item *item_touch(const char *key, size_t nkey, uint32_t exptime, conn *c) {
    item *it;
    uint32_t hv;
    hv = hash(key, nkey);
    item_lock(hv);
    it = do_item_touch(key, nkey, exptime, hv, c);
    item_unlock(hv);
    return it;
}

/*
 * Links an item into the LRU and hashtable
 */
int item_link(item *item) {
    int ret;
    uint32_t hv;

    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    ret = do_item_link(item, hv);
    item_unlock(hv);
    return ret;
}

/*
 * Decrements the reference count on an item and adds it to the freelist if
 * needed.
 */
void item_remove(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);

    item_lock(hv);
    do_item_remove(item);
    item_unlock(hv);
}

/*
 * Replaces one item with another in the hashtable.
 * Unprotected by a mutex lock since the core server does not require
 * it to be thread-safe.
 */
int item_replace(item *old_it, item *new_it, const uint32_t hv) {
    return do_item_replace(old_it, new_it, hv);
}

/*
 * Unlinks an item from the LRU and hashtable
 */
void item_unlink(item *item) {
    uint32_t hv;
    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    do_item_unlink(item, hv);
    item_unlock(hv);
}

/*
 * Does arithmetic on a numberic item value.
 */
enum delta_result_type add_delta(conn *c, const char *key,
                                 const size_t nkey, bool incr,
                                 const int64_t delta, char *buf,
                                 uint64_t *cas) {
    enum delta_result_type ret;
    uint32_t hv;

    hv = hash(key, nkey);
    item_lock(hv);
    ret = do_add_delta(c, key, nkey, incr, delta, buf, cas, hv);
    item_unlock(hv);
    return ret;
}

/*
 * Stores an item in the cache  (high level, obeys set/add/replace semantics)
 */
enum store_item_type store_item(item *item, int comm, conn *c) {
    enum store_item_type ret;
    uint32_t hv;

    hv = hash(ITEM_key(item), item->nkey);
    item_lock(hv);
    ret = do_store_item(item, comm, c, hv);
    item_unlock(hv);
    return ret;
}

/********************************* GLOBAL STATS ****************************/

void STATS_LOCK() {
    pthread_mutex_lock(&stats_lock);
}

void STATS_UNLOCK() {
    pthread_mutex_unlock(&stats_lock);
}

/*
 * Initializes the thread subsytem, creating various worker threads
 *
 * nthreads Number of worker event handler threds to spawn.
 */
void memcached_thread_init(int nthreads, void *arg) {
    int i;
    int power;

    for (i = 0; i < POWER_LARGEST; ++i) {
        pthread_mutex_init(&lru_locks[i], NULL);
    }

    /*
     * Want a wide lock table, but don't wate memory
     */
    if (nthreads < 3) {
        power = 10;
    } else if (nthreads < 4) {
        power = 11;
    } else if (nthreads < 5) {
        power = 12;
    } else if (nthreads <= 10) {
        power = 13;
    } else if (nthreads <= 20) {
        power = 14;
    } else {
        // 32 buckets, just under the hashpower default
        power = 15;
    }

    if (power >= hashpower) {
        fprintf(stderr, "Hash table power size (%d) cannot be equal to or less than item lock table (%d)\n", hashpower, power);
        fprintf(stderr, "Item lock table grows with `-t N` (worker threadcount)\n");
        fprintf(stderr, "Hash table grows with `-o hashpower=N`\n");
        exit(1);
    }

    item_lock_count = hashsize(power);
    item_lock_hashpower = power;

    item_locks = (pthread_mutex_t *)calloc(item_lock_count, sizeof(pthread_mutex_t));
    if (! item_locks) {
        perror("Can't allocate item locks");
        exit(1);
    }
    for (i = 0; i < item_lock_count; ++i) {
        pthread_mutex_init(&item_locks[i], NULL);
    }

    threads = (LIBEVENT_THREAD *)calloc(nthreads, sizeof(LIBEVENT_THREAD));
    if (! threads) {
        perror("Can't allocate thread descriptors");
        exit(1);
    }

    for (i = 0; i < nthreads; ++i) {
        setup_thread(&threads[i]);
        // Reserve three fds for the libevent base, and two for the pipe
        stats_state.reserved_fds += 5;
    }

    // Create threads after  we've done all the lievent setup
    for (i = 0; i < nthreads; ++i) {
        worker_libevent(&threads[i]);
    }
}
