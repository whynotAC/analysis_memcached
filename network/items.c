
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

// TODO: Third place this code needs to be deduped
static void lru_bump_buf_link_q(lru_bump_buf *b) {
    pthread_mutex_lock(&bump_buf_lock);
    assert(b != bump_buf_head);

    b->prev = 0;
    b->next = bump_buf_head;
    if (b->next) b->next->prev = b;
    bump_buf_head = b;
    if (bump_buf_tail == 0) bump_buf_tail = b;
    pthread_mutex_unlock(&bump_buf_lock);
    return;
}

void *item_lru_bump_buf_create(void) {
    lru_bump_buf *b = calloc(1, sizeof(lru_bump_buf));
    if (b == NULL) {
        return NULL;
    }

    b->buf = bipbuf_new(sizeof(lru_bump_entry) * LRU_BUMP_BUF_SIZE);
    if (b->buf == NULL) {
        free(b);
        return NULL;
    }

    pthread_mutex_init(&b->mutex, NULL);

    lru_bump_buf_link_q(b);
    return b;
}
