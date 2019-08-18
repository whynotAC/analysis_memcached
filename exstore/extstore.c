/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

// FIXME: config.h?
#include <stdint.h>
#include <stdbool.h>
// end FIXME
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "extstore.h"

// TODO: better if an init option turns this on/off
#ifdef EXTSTORE_DEBUG
#define E_DEBUG(...) \
    do { \
        fprintf(stderr, __VA_ARGS__); \
    } while(0)
#else
#define E_DEBUG(...)
#endif

#define STAT_L(e) pthread_mutex_lock(&e->stats_mutex);
#define STAT_UL(e) pthread_mutex_unlock(&e->stats_mutex);
#define STAT_INCR(e, stat, amount) { \
    pthread_mutex_lock(&e->stats_mutex); \
    e->stats.stat += amount; \
    pthread_mutex_unlock(&e->stats_mutex); \
}

#define STAT_DECR(e, stat, amount) { \
    pthread_mutex_lock(&e->stats_mutex); \
    e->stats.stat -= amount; \
    pthread_mutex_unlock(&e->stats_mutex); \
}

typedef struct __store_wbuf {
    struct __store_wbuf *next;
    char *buf;
    char *buf_pos;
    unsigned int free;
    unsigned int size;
    unsigned int offset; // offset into page this wirte starts at
    bool full;          // doen writing to this page
    bool flushed;       // whether wbuf has been flushed to disk
} _store_wbuf;

typedef struct _store_page {
    pthread_mutex_t mutex;  // Need to be held for most operations
    uint64_t obj_count;     // _delete can decrease post-closing
    uint64_t bytes_used;    // _delete can decrease post-closing
    uint64_t offset;        // starting address of page within fd
    unsigned int version;
    unsigned int refcount;
    unsigned int allocated;
    unsigned int written;   // item offsets can be past written if wbuf not flushed
    unsigned int bucket;    // which bucket the page is linked into
    unsigned int free_bucket; // whtich bucket this page returns to when freed
    int fd;
    unsigned short id;
    bool active;    // actively being written to
    bool closed;    // closed and draining before free
    bool free;      // on freelist
    _store_wbuf *wbuf;  // currently active wbuf from the stack
    struct _store_page *next;
} store_page;

typedef struct store_engine store_engine;
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    obj_io *queue;
    store_engine *e;
    unsigned int depth; // queue depth
} stroe_io_thread;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    store_engine *e;
} store_maint_thread;

struct store_engine {
    pthread_mutex_t mutex;      // covers internal stacks and variables
    store_page *pages;          // directyly addressable page list
    _store_wbuf *wbuf_stack;    // wbuf freelist
    obj_io *io_stack;           // IO's to use with submitting wbuf's
    store_io_thread *io_threads;
    store_maint_thread *maint_thread;
    store_page *page_freelist;
    store_page **page_buckets;  // stack of pages currently allocated to each bucket
    store_page **free_page_buckets; // stack of use-case isolated free pages
    size_t page_size;
    unsigned int version;       // global version counter
    unsigned int last_io_thread;    // round robin the IO threads
    unsigned int io_threadcount;    // count of IO threads
    unsigned int page_count;
    unsigned int page_free;         // unallocated pages
    unsigned int page_bucketcount;  // count of potential page buckets
    unsigned int free_page_bucketcount; // count of free page buckets
    unsigned int io_depth;          // FIXME: might cache into thr struct
    pthread_mutex_t stats_mutex;
    struct extstore_stats stats;
}

static _store_wbuf *wbuf_new(size_t size) {
    _store_wbuf *b = calloc(1, sizeof(_store_wbuf));
    if (b == NULL)
        return NULL;
    b->buf = malloc(size);
    if (b->buf == NULL) {
        free(b);
        return NULL;
    }
    b->buf_pos = b->buf;
    b->free = size;
    b->size = size;
    return b;
}

static store_io_thread *_get_io_thread(store_engine *e) {
    int tid = -1;
    long long int low = LLONG_MAX;
    pthread_mutex_lock(&e->mutex);
    // find smallest queue. ignoring lock since being wrong isn't fatal.
    // TODO: if average queue depth can be quickly tracked, can bread as soon
    // as we see a thread that's less than average, and start from last_io_thread.
    for (int x = 0; x < e->io_threadcount; x++) {
        if (e->io_threads[x].depth == 0) {
            tid = x;
            break;
        } else if (e->io_threads[x].depth < low) {
            tid = x;
            low = e->io_threads[x].depth;
        }
    }
    pthread_mutex_unlock(&e->mutex);

    return &e->io_threads[tid];
}

static uint64_t _next_version(store_engine *e) {
    return e->version++;
}

static void *extstore_io_thread(void *arg);
static void *extstore_maint_thread(void *arg);

/* Copies stats internal to engine and computes any derived values */
void extstore_get_stats(void *ptr, struct extstore_stats *st) {
    store_engine *e = (store_engine *)ptr;
    STAT_L(e);
    memcpy(st, &e->stats, sizeof(struct extstore_stats));
    STAT_UL(e);

    // grab pages_free/pages_used
    pthread_mutex_lock(&e->mutex);
    st->pages_free = e->page_free;
    st->pages_used = e->page_count - e->page_free;
    pthread_mutex_unlock(&e->mutex);
    st->io_queue = 0;
    for (int x = 0; x < e->io_threadcount; x++) {
        pthread_mutex_lock(&e->io_threads[x].mutex);
        st->io_queue += e->io_threads[x].depth;
        pthread_mutex_unlock(&e->io_threads[x].mutex);
    }
    // calculate bytes_fragmented.
    // note that open and yet-filled pages count against fragmentation.
    st->bytes_fragmented = st->pages_used * e->page_size - 
        st->bytes_used;
}

void extstore_get_page_data(void *ptr, struct extstore_stats *st) {
    store_engine *e = (store_engine *)ptr;
    STAT_L(e);
    memcpy(st->page_data, e->stats.page_data,
            sizeof(struct extstore_page_data) * e->page_count);
    STAT_UL(e);
}

const char *extstore_err(enum extstore_res res) {
    char *rv = "unknown error";
    switch (res) {
    case EXTSTORE_INIT_BAD_WBUF_SIZE:
        rv = "page_size must be divisible by wbuf_size";
        break;
    case EXTSTORE_INIT_NEED_MORE_WBUF:
        rv = "wbuf_count must be >= page_buckets";
        break;
    case EXTSTORE_INIT_NEED_MORE_BUCKETS:
        rv = "page_buckets must be > 0";
        break;
    case EXTSTORE_INIT_PAGE_WBUF_ALIGNMENT:
        rv = "page_size and wbuf_size must be divisible by 1024*1024*2";
        break;
    case EXTSTORE_INIT_TOO_MANY_PAGES:
        rv = "page_count must total to < 65526. Increase page_size of lower path sizes";
        break;
    case EXTSTORE_INIT_OOM:
        rv = "failed calloc for engine";
        break;
    case EXTSTORE_INIT_OPEN_FAIL:
        rv = "failed to open file";
        break;
    case EXTSTORE_INIT_THREAD_FAIL:
        break;
    }
    return rv;
}

// TODO: #define's for DEFAULT_BUCKET， FREE_VERSION, etc
void *extstore_init(struct extstore_conf_file *h, struct extstore_conf *cf,
        enum extstore_res *res) {
    int i;
    struct extstore_conf_file *f = NULL;
    pthread_t thread;

    if (cf->page_size % cf->wbuf_size != 0) {
        *res = EXTSTORE_INIT_BAD_WBUF_SIZE;
        return NULL;
    }
    // Should ensure at least one write buffer per potential page
    if (cf->page_buckets > cf->wbuf_count) {
        *res = EXTSTORE_INIT_NEED_MORE_WBUF;
        return NULL;
    }
    if (cf->page_buckets < 1) {
        *res = EXTSTORE_INIT_NEED_MORE_BUCKETS;
        return NULL;
    }

    // TODO: More intelligence around aligenment of flash erasure block sizes
    if (cf->page_size % (1024 * 1024 * 2) != 0 ||
            cf->wbuf_size % (1024 * 1024 * 2) != 0) {
        *res = EXTSTORE_INIT_PAGE_WBUF_ALIGNMENT;
        return NULL;
    }

    store_engine *e = calloc(1, sizeof(store_engine));
    if (e == NULL) {
        *res = EXTSTORE_INIT_OOM;
        return NULL;
    }

    e->page_size = cf->page_size;
    uint64_t temp_page_count = 0;
    for (f = fh; f != NULL; f = f->next) {
        f->fd = open(f->file, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (f->fd < 0) {
            *res = EXTSTORE_INIT_OPEN_FAIL;
#ifdef EXTSTORE_DEBUG
            perror("open");
#endif
            free(e);
            return NULL;
        }
        temp_page_count += f->page_count;
        f->offset = 0;
    }

    if (temp_page_count >= UINT16_MAX) {
        *res = EXTSTORE_INIT_TOO_MANY_PAGES;
        free(e);
        return NULL;
    }
    e->page_count = temp_page_count;

    e->pages = calloc(e->page_count, sizeof(store_page));
    if (e->pages == NULL) {
        *res = EXTSTORE_INIT_OOM;
        // FIXME: loop-close. make error lable
        free(e);
        return NULL;
    }

    // interleave the pages between devices
    f = NULL;   // start at the first device.
    for (i = 0; i < e->page_count; i++) {
        // find next device with available pages
        while (1) {
            // restart the loop
            if (f == NULL || f->next == NULL) {
                f = fh;
            } else {
                f = f->next;
            }
            if (f->page_count) {
                f->page_count--;
                break;
            }
        }
        pthread_mutex_init(&e->pages[i].mutex, NULL);
        e->pages[i].id = i;
        e->pages[i].fd = f->fd;
        e->pages[i].free_bucket = f->free_bucket;
        e->pages[i].offset = f->offset;
        e->pages[i].free = true;
        f->offset += e->page_size;
    }

    // free page buckets allows the app to organize devices by use case
    e->free_page_buckets = calloc(cf->page_buckets, sizeof(store_page *));
    e->page_bucketcount = cf->page_buckets;

    for (i = e->page_count-1; i > 0; i--) {
        e->page_free++;
        if (e->pages[i].free_bucket == 0) {
            e->pages[i].next = e->page_freelist;
            e->page_freelist = &e->pages[i];
        } else {
            int fb = e->pages[i].free_bucket;
            e->pages[i].next = e->free_page_buckets[fb];
            e->free_page_buckets[fb] = &e->pages[i];
        }
    }

    // 0 is magic "page is freed" version
    e->version = 1;

    // scratch data for stats. TODO: malloc failure handle
    e->stats.page_data = 
        calloc(e->page_count, sizeof(struct extstore_page_data));
    e->stats.page_count = e->page_count;
    e->stats.page_size = e->page_size;

    // page buckets lazily have pages assigned into them
    e->page_buckets = calloc(cf->page_buckets, sizeof(store_page *));
    e->page_bucketcount = cf->page_buckets;

    // allocate write buffers
    // also IO's to use for shipping to IO thread
    for (i = 0; i < cf->wbuf_count; i++) {
        _store_wbuf *w = wbuf_new(cf->wbuf_size);
        obj_io *io = calloc(1, sizeof(obj_io));
        /* TODO: on error, loop again and free stack. */
        w->next = e->wbuf_stack;
        e->wbuf_stack = w;
        io->next = e->io_stack;
        e->io_stack = io;
    }

    pthread_mutex_init(&e->mutex, NULL);
    pthread_mutex_init(&e->stats_mutex, NULL);

    e->io_depth = cf->io_depth;

    // spawn threads
    e->io_threads = calloc(cf->io_threadcount, sizeof(store_io_thread));
    for (i = 0; i < cf->io_threadcount; i++) {
        pthread_mutex_init(&e->io_threads[i].mutex, NULL);
        pthread_cond_init(&e->io_threads[i].cond, NULL);
        e->io_threads[i].e = e;
        // FIXME: error handling
        pthread_create(&thread, NULL, extstore_io_thread, &e->io_threads[i]);
    }
    e->io_threadcount = cf->io_threadcount;

    e->maint_thread = calloc(1, sizeof(store_maint_thread));
    e->maint_thread->e = e;
    // FIXME: error handling
    pthread_mutex_init(&e->maint_thread->mutex, NULL);
    pthread_cond_init(&e->maint_thread->cond, NULL);
    pthread_create(&thread, NULL, extstore_maint_thread, e->maint_thread);

    extstore_run_maint(e);

    return (void *)e;
}

void extstore_run_maint(void *ptr) {
    store_engine *e = (store_engine *)ptr;
    pthread_cond_signal(&e->maint_thread->cond);
}

// call with *e locked
static store_page *_allocate_page(store_engine *e, unsigned int bucket,
        unsigned int free_bucket) {
    assert(!e->page_buckets[bucket] || e->page_buckets[bucket]->allocated == e->page_size);
    store_page *tmp = NULL;
    // if a specific free bucket was requested, check there first
    if (free_bucket != 0 && e->free_page_buckets[free_bucket] != NULL) {
        assert(e->page_free > 0);
        tmp = e->free_page_buckets[free_bucket];
        e->free_page_buckets[free_bucket] = tmp->next;
    }
    // failing that, try the global list.
    if (tmp == NULL && e->page_freelist != NULL) {
        tmp = e->page_freelist;
        e->page_freelist = tmp->next;
    }
    E_DEBUG("EXTSTORE: allocating new page\n");
    // page_freelist can be empty if the only free pages are specialized and
    // we didn't just request one.
    if (e->page_free > 0 && tmp != NULL) {
        tmp->next = e->page_buckets[bucket];
        e->page_buckets[bucket] = tmp;
        tmp->active = true;
        tmp->free = false;
        tmp->closed = false;
        tmp->version = _next_version(e);
        tmp->bucket = bucket;
        e->page_free--;
        STAT_INCR(e, page_allocs, 1);
    } else {
        extstore_run_maint(e);
    }
    if (tmp)
        E_DEBUG("EXTSTORE: got page %u\n", tmp->id);
    return tmp;
}

//call with *p locked, locks *e
static void _allocate_wbuf(store_engine *e, store_page *p) {
    _store_wbuf *wbuf = NULL;
    assert(!p->wbuf);
    pthread_mutex_lock(&e->mutex);
    if (e->wbuf_stack) {
        wbuf = e->wbuf_stack;
        e->wbuf_stack = wbuf->next;
        wbuf->next = 0;
    }
    pthread_mutex_unlock(&e->mutex);
    if (wbuf) {
        wbuf->offset = p->allocated;
        p->allocated += wbuf->size;
        wbuf->free = wbuf->size;
        wbuf->buf_pos = wbuf->buf;
        wbuf->full = false;
        wbuf->flushed = false;

        p->wbuf = wbuf;
    }
}

/* callback after wbuf is flushed. can only remove wbuf's from the head onward
 * if successfully flushed, which complicates this routine. each callback
 * attempts to free the wbuf stack, which is finally done when the head wbuf's
 * callback happends.
 * It's rare flushes would happen out of order
 */
static void _wbuf_cb(void *ep, obj_io *io, int ret) {
    store_engine *e = (store_engine *)ep;
    store_page *p = &e->pages[io->page_id];
    _store_wbuf *w = (_store_wbuf *) io->data;

    // TODO: Example return code. Not entirely sure how to handle errors.
    // Navie first-pass should probably cause the page to close/free.
    w->flushed = true;
    pthread_mutex_lock(&p->mutex);
    assert(p->wbuf != NULL && p->wbuf == w);
    assert(p->written == w->offset);
    p->written += w->size;
    p->wbuf = NULL;

    if (p->written == e->page_size)
        p->active = false;

    // return the wbuf
    pthread_mutex_lock(&e->mutex);
    w->next = e->wbuf_stack;
    e->wbuf_stack = w;
    // also return the IO we just used.
    io->next = e->io_stack;
    e->io_stack = io;
    pthread_mutex_unlock(&e->mutex);
    pthread_mutex_unlock(&p->mutex);
}

/* Wraps pages current wbuf in an io and submits to IO thread.
 * Called with p locked, locks e.
 */
static void _submit_wbuf(store_engine *e, store_page *p) {
    _store_wbuf *w;
    pthread_mutex_lock(&e->mutex);
    obj_io *io = e->io_stack;
    e->io_stack = io->next;
    pthread_mutex_unlock(&e->mutex);
    w = p->wbuf;

    // zero out the end of the wbuf to allow blind readback of data.
    memset(w->buf + (w->size - w->free), 0, w->free);

    io->next = NULL;
    io->mode = OBJ_IO_WRITE;
    io->page_id = p->id;
    io->data = w;
    io->offset = w->offset;
    io->len = w->size;
    io->buf = w->buf;
    io->cb = _wbuf_cb;

    extstore_submit(e, io);
}

/* engine write function; takes engine, item_io
 * fast fail if no available write buffer (flushing)
 * lock engine context, find active page, unlock
 * if page full, submit page/buffer to io thread.
 *
 * write is designed to be flaky; if page full, caller must try again to get
 * new page. best if used from a background thread that can harmlessly retry
 */

int extstore_write_request(void *ptr, unsigned int bucket,
        unsigned int free_bucket, obj_io *io) {
    store_engine *e = (store_engine *)ptr;
    store_page *p;
    int ret = -1;
    if (bucket >= e->page_bucketcount)
        return ret;

    pthread_mutex_lock(&e->mutex);
    p = e->page_buckets[bucket];
    if (!p) {
        p = _allocate_page(e, bucket, free_bucket);
    }
    pthread_mutex_unlock(&e->mutex);
    if (!p)
        return ret;

    pthread_mutex_lock(&p->mutex);

    // FIXME: can't null out page_buckets!!!
    // page is full, clear bucket and retry later.
    if (!p->active ||
            ((!p->wbuf || p->wbuf->full) && p->allocated >= e->page_size)) {
        pthread_mutex_unlock(&p->mutex);
        pthread_mutex_lock(&e->mutex);
        _allocate_page(e, bucket, free_bucket);
        pthread_mutex_unlock(&e->mutex);
        return ret;
    }

    // if io won't fit, submit IO for wbuf and find new one.
    if (p->wbuf && p->wbuf->free < io->len && !p->wbuf->full) {
        _submit_wbuf(e, p);
        p->wbuf->full = true;
    }

    if (!p->wbuf && p->allocated < e->page_size) {
        _allocate_wbuf(e, p);
    }

    // hand over buffer for caller to copy into
    // leaves p locked
    if (p->wbuf && !p->wbuf->full && p->wbuf->free >= io->len) {
        io->buf = p->wbuf->buf_pos;
        io->page_id = p->id;
        return 0;
    }

    pthread_mutex_unlock(&p->mutex);
    // p->written is incremented post-wbuf flush
    return ret;
}

/* _must_ be called after a successful write_request.
 * fills the rest of io structure.
 */
void extstore_write(void *ptr, obj_io *io) {
    store_engine *e = (store_engine *)ptr;
    store_page *p = &e->pages[io->page_id];

    io->offset = p->wbuf->offset + (p->wbuf->size - p->wbuf->free);

}
