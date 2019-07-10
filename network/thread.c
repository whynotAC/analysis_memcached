
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

// 主线程等待子线程启动完毕
static void wait_for_thread_registration(int nthreads) {
    while (init_count < nthreads) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

// 子线程通知主线程启动完毕
static void register_thread_initialized(void) {
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
    // Force worker threads to pile up if someone wants us to
    pthread_mutex_lock(&worker_hang_lock);
    pthread_mutex_unlock(&worker_hang_lock);
}

/*
 * Initializes a connection queue
 */
static void cq_init(CQ *cq) {
    pthread_mutex_init(&cq->lock, NULL);
    cq->head = NULL;
    cq->tail = NULL;
}

/*
 * Looks for an item on connection queue, but doesn't block if there isn't
 * one.
 * Returns the item, or NULL if no item is available
 */
static CQ_ITEM *cq_pop(CQ *cq) {
    CQ_ITEM *item;

    pthread_mutex_lock(&cq->lock);
    item = cq->head;
    if (NULL != item) {
        cq->head = item->next;
        if (NULL == cq->head)
            cq->tail = NULL;
    }
    pthread_mutex_unlock(&cq->lock);

    return item;
}

/**
 * Adds an item to a connection queue.
 */
static void cq_push(CQ *cq, CQ_ITEM *item) {
    item->next = NULL;

    pthread_mutex_lock(&cq->lock);
    if (NULL == cq->tail)
        cq->head = item;
    else
        cq->tail->next = item;
    cq->tail = item;
    pthread_mutex_unlock(&cq->lock);
}

/*
 * Returns a fresh connection queue item.
 */
static CQ_ITEM *cqi_new(void) {
    CQ_ITEM *item = NULL;
    pthread_mutex_lock(&cqi_freelist_lock);
    if (cqi_freelist) {
        item = cqi_freelist;
        cqi_freelist = item->next;
    }
    pthread_mutex_unlock(&cqi_freelist_lock);

    if (NULL == item) {
        int i;

        // Allocate a bunch of items at once to reduce fragmentation
        item = malloc(sizeof(CQ_ITEM) * ITEMS_PER_ALLOC);
        if (NULL == item) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return NULL;
        }

        /*
         * Link together all the new items except the first one
         * (which we'll return to the caller) for placement on
         * the freelist.
         */
        for (i = 2; i < ITEMS_PER_ALLOC; i++)
            item[i - 1].next = &item[i];

        pthread_mutex_lock(&cqi_freelist_lock);
        item[ITEMS_PER_ALLOC - 1].next = cqi_freelist;
        cqi_freelist = &item[1];
        pthread_mutex_unlock(&cqi_freelist_lock);
    }

    return item;
}

/*
 * Frees a connection queue item (adds it to the freelist.)
 */
static void cqi_free(CQ_ITEM *item) {
    pthread_mutex_lock(&cqi_freelist_lock);
    item->next = cqi_freelist;
    cqi_freelist = item;
    pthread_mutex_unlock(&cqi_freelist_lock);
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
    // 本次不介绍,故隐去
    //me->l = logger_create();
    me->lru_bump_buf = item_lru_bump_buf_create();
    if (me->l == NULL || me->lru_bump_buf == NULL) {
        abort();
    }
    
    // 本次不介绍,故隐去
    /*if (settings.drop_privileges) {
        drop_worker_privileges();
    }*/

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
        if (settings.verbose > 0)
            fpritnf(stderr, "Can't read from libevent pipe\n");
        return;
    }

    // 处理发送过来的信息,发送的信息中第一个字符代表需要什么样的处理
    switch (buf[0]) {
    case 'c':
        item = cq_pop(me->new_conn_queue);

        if (NULL == item) {
            break;
        }
        switch (item->mode) {
            case queue_new_conn:
                c = conn_new(item->sfd, item->init_state, item->event_flags,
                                item->read_buffer_size, item->transport,
                                me->base);
                if (c == NULL) {
                    if (IS_UDP(item->transport)) {
                        fprintf(stderr, "Can't listen for events on UDP socket\n");
                        exit(1);
                    } else {
                        if (settings.verbose > 0) {
                            fprintf(stderr, "Can't listen for events on fd %d\n",
                                    item->sfd);
                        }
                        close(item->sfd);
                    }
                } else {
                    c->thread = me;
                }
                break;

            case queue_redispatch:
                conn_worker_readd(item->c);
                break;
        }
        cqi_free(item);
        break;
    // we were told to pause and report in
    case 'p':
        register_thread_initialized();
        break;
    // a client socket timed out
    case 't':
        if (read(fd, &timeout_fd, sizeof(timeout_fd)) != sizeof(timeout_fd)) {
            if (settings.verbose > 0) {
                fprintf(stderr, "Can't read timeout fd from libevent pipe\n");
            }
            return;
        }
        conn_close_idle(conns[timeout_fd]);
        break;
    }
}

/* Which thread we assigned a connection to most recently. */
// 用于记录connection分配个哪个通信子thread,新accept的connection通过轮转的方法
// 进行分配
static int last_thread = -1;

/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming connection.
 */
// 用于分配新accept的connection
void dispatch_conn_new(int sfd, enum conn_stats init_state, int event_flags,
                        int read_buffer_size, enum network_transport transport) {
    CQ_ITEM *item = cqi_new();
    char buf[1];
    if (item == NULL) {
        close(sfd);
        // given that malloc failed this may also fail, but let's try
        fprintf(stderr, "Failed to allocate memory for connection object\n");
        return ;
    }

    // 计算要将connection分配给哪个通信thread
    int tid = (last_thread + 1) % settings.num_threads;
    
    // 查找对应thread的LIBEVENT_THREAD结构体
    LIBEVENT_THREAD *thread = threads + tid;

    last_thread = tid;

    // 初始化CQ_ITEM结构
    item->sfd = sfd;
    item->init_state = init_state;
    item->event_flags = event_flags;
    item->read_buffer_size = read_buffer_size;
    item->transport = transport;
    item->mode = queue_new_conn;
    
    // 将CQ_ITEM加入到thread对应的链表中
    cq_push(thread->new_conn_queue, item);

    MEMCACHED_CONN_DISPATCH(sfd, thread->thread_id);
    // 向thread监听的文件描述符传递新connection的消息
    buf[0] = 'c';
    if (write(thread->notify_send_fd, buf, 1) != 1) {
        perror("Writing to thread notify pipe");
    }
}

/*
 * Re-dispatches a connection back to the original thread. Can be called from
 * any sid thread borrowing a connection.
 */
// 将connection重新分配到thread上
void redispath_conn(conn *c) {
    CQ_ITEM *item = cqi_new();
    char buf[1];
    if (item == NULL) {
        // Can't cleanly redispatch connection. close it forcefully
        c->state = conn_closed;
        close(c->sfd);
        return;
    }
    LIBEVENT_THREAD *thread = c->thread;
    item->sfd = c->sfd;
    item->init_state = conn_new_comd;
    item->c = c;
    item->mode = queue_redispatch;

    cq_push(thread->new_conn_queue, item);

    buf[0] = 'c';
    if (write(thread->notify_send_fd, buf, 1) != 1) {
        perror("Writing to thread notify pipe");
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
