
/* An item in the connection queue */
enum conn_queue_item_modes {
    queue_new_conn,     // brand new connection
    queue_redispatch    // redispatching from side thread
};
typedef struct conn_queue_item CQ_ITEM;
struct conn_queue_item {
    int                 sfd;
    enum conn_states    init_state;
    int                 event_flags;
    int                 read_buffer_size;
    enum network_transport      transport;
    enum conn_queue_item_modes  mode;
    conn *c;
    CQ_ITEM         *next;
};

/* A connection queue */
typedef struct conn_queue CQ;
struct conn_queue {
    CQ_ITEM *head;
    CQ_ITEM *tail;
    pthread_mutex_t lock;
};

/* Lock to cause worker threads to hong up after being woken */
static pthread_mutex_t worker_hang_lock;

/* Free list of CQ_ITEM structs */
static CQ_ITEM *cqi_freelist;
static pthread_mutex_t cqi_freelist_lock;

/*
 * Each libevent instance has a wakeup pip, which other threads
 * can use to signal that they've put a new connection on its queue.
 */
static LIBEVENT_THREAD *threads;

/*
 * Number of worker threads that have finished setting themselves up.
 */
static int init_count = 0;
static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;

/*
 * Initializes a connection queue
 */
static void cq_init(CQ *cq) {
    pthread_mutex_init(&cq->lock, NULL);
    cq->head = NULL;
    cq->tail = NULL;
}

/*
 * Creates a worker thread.
 */
static void create_worker(void *(*func)(void *), void *arg) {
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&((LIBEVENT_THREAD*)arg)->thread_id, &attr, func, arg)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
        exit(1);
    }
}

/*************************************LIBEVENT THREADS**************/

/**
 * set up a thread's information
 */
// 初始化线程结构体
static void setup_thread(LIBEVENT_THREAD *me) {
#if defined(LIBEVENT_VERSION_NUMBER) && LIBEVENT_VERSION_NUMBER >= 0x02000101
    struct event_config *ev_config;
    ev_config = event_config_new();
    event_config_set_flag(ev_config, EVENT_BASE_FLAG_NOLOCK);
    me->base = event_base_new_with_config(ev_config);
    event_config_free(ev_config);
#else
    me->base = event_init();
#endif

    if (! me->base) {
        fprintf(stderr, "Can't allocate event base \n");
        exit(1);
    }

    // 设置监听的文件描述符以及其对应的监听事件和处理函数
    event_set(&me->notify_event, me->notify_receive_fd,
                EV_READ | EV_PERSIST, thread_libevent_process, me);
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1) {
        fprintf(stderr, "Can't monitor libevent notify pipe\n");
        exit(1);
    }

    me->new_conn_queue = malloc(sizeof(struct conn_queue));
    if (me->new_conn_queue == NULL) {
        perror("Failed to allocate memory for connection conn_queue");
        exit(EXIT_FAILURE);
    }
    cq_init(me->new_conn_queue);

    if (pthread_mutex_init(&me->stats.mutex, NULL) != 0) {
        perror("Failed to initialize mutex");
        exit(EXIT_FAILURE);
    }

    me->suffix_cache = cache_create("suffix", SUFFIX_SIZE, sizeof(char*), NULL, NULL);

    if (me->suffix_cache == NULL) {
        fprintf(stderr, "Failed to create suffix cache\n");
        exit(EXIT_FAILURE);
    }
#ifdef EXTSTORE
    me->io_cache = cache_create("io", sizeof(io_wrap), sizeof(char*), NULL, NULL);
    if (me->io_cache == NULL) {
        fprintf(stderr, "Failed to create IO object cache\n");
        exit(EXIT_FAILURE);
    }
#endif
}

/*
 * Worker thread: main event loop
 */
static void *worker_libevent(void *arg) {
    LIBEVENT_THREAD *me = arg;

    /* Any per-thread setup can happen here; memcached_thread_init() will block
     * until all threads have finished initializing.
     */
    me->l = logger_create();
    me->lru_bump_buf = item_lru_bump_buf_create();
    if (me->l == NULL || me->lru_bump_buf == NULL) {
        abort();
    }

    if (settings.drop_privileges) {
        drop_worker_privileges();
    }

    register_thread_initialized();

    event_base_loop(me->base, 0);

    event_base_free(me->base);
    return NULL;
}

/*
 * Processes on incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
static void thread_libevent_process(int fd, short which, void *arg) {
    LIBEVENT_THREAD *me = arg;
    CQ_ITEM *item;
    char buf[1];
    conn *c;
    unsigned int timeout_fd;

    if (read(fd, buf, 1) != 1) {
        
    }
}


/*
 * Initialize the thread subsystem, creating various worker threads.
 *
 * nthreads Number of worker event handler threads to spawn
 */
void memcached_thread_init(int nthreads, void *arg) {
    int i;
    // 其他代码
    pthread_mutex_init(&worker_hang_lock, NULL);

    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);

    pthread_mutex_init(&cqi_freelist_lock, NULL);
    cqi_freelist = NULL;

    // 其他代码
    // 申请记录每个线程信息的结构体
    threads = calloc(nthreads, sizeof(LIBEVENT_THREAD));
    if (!threads) {
        perror("Can't allocate thread descriptors");
        exit(1);
    }
    // 初始化线程配置
    for (i = 0; i < nthreads; i++) {
        // 创建线程之间的通信管道
        int fds[2];
        if (pipe(fds)) {
            perror("Can't create notify pipe");
            exit(1);
        }

        threads[i].notify_receive_fd = fds[0];  // 读文件描述符
        threads[i].notify_send_fd = fds[1];     // 写文件描述符
#ifdef EXTSTORE
        threads[i].storage = arg;
#endif
        setup_thread(&threads[i]);      // 初始化线程的LIBEVENT_THREAD结构体
        // Reserve three fds for the libevent base, and two for the pipe
        stats_state.reserved_fds += 5;
    }

    // Create threads after we've done all the libevent setup.
    // 创建线程
    for (i = 0; i < nthreads; i++) {
        create_worker(worker_libevent, &threads[i]);
    }
    
    // 等待所有线程创建完毕
    // Wait for all the threads to set themselves up before returning
    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthreads);
    pthread_mutex_unlock(&init_lock);
}
