#include "memcached.h"
#include "assoc.h"
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef uint32_t (*hash_func)(const void *key, size_t length);
extern hash_func hash;

static pthread_mutex_t *item_locks;
/* size of the item lock hash table*/
static uint32_t item_lock_count;
unsigned int item_lock_hashpower;
#define hashsize(n) ((unsigned long int)1<<(n))
#define hashmask(n) (hashsize(n)-1)


/*
 * item_lock() must be held for an item before any modifications to either its
 * associated hash bucket, or the structure itself.
 * LRU modifications must hold the item lock, and the LRU lock.
 * LRU's accessing items must item_trylock() before modifying an item.
 * Items accessible from an LRU must not be freed or modified
 * without first locking and removing from the LRU.
 */
// 初始化时hash表的item_locks已经初始化确定其值大小，在后续hash表扩展时不会更改item_locks
// 的大小，只会通过hashmask(item_lock_hashpower)的与操作来抢锁执行。
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
    mutex_unlock((pthread_mutex_t *) lock);
}

void item_unlock(uint32_t hv) {
    mutex_unlock(&item_locks[hv & hashmask(item_lock_hashpower)]);
}

/* Must not be called with any deeper locks held */
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

/*
 * Initializes the thread subsystem, creating various worker threads.
 *
 * nthreads Number of worker event handler threads to spawn.
 */
void memcached_thread_init(int nthreads, void *arg) {
    int i;
    int power;

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
        // 32K buckets. just under the hashpower default
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
    for (i = 0; i < item_lock_count; i++) {
        pthread_mutex_init(&item_locks[i], NULL);
    }
}
