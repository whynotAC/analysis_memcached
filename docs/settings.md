# 全局设置

memcached提供很多配置参数，从而控制程序的运行流程。

## 1. 所在文件

```c
memcached.h
```

## 2. 定义

```c
/* When adding a setting, be sure to update process_stat_settings */
/**
 * Globally accessible settings as derived from the commandline.
 */
struct settings {
    size_t maxbytes;
    int maxconns;
    int port;
    int udpport;
    char *inter;
    int verbose;
    rel_time_t oldest_live; /* ignore existing items older than this */
    uint64_t oldest_cas; /* ignore existing items with CAS values lower than this */
    int evict_to_free;
    char *socketpath;   /* path to unix socket if using local socket */
    char *auth_file;    /* path to user authentication file */
    int access;  /* access mask (a la chmod) for unix domain socket */
    double factor;          /* chunk size growth factor */
    int chunk_size;
    int num_threads;        /* number of worker (without dispatcher) libevent threads to run */
    int num_threads_per_udp; /* number of worker threads serving each udp socket */
    char prefix_delimiter;  /* character that marks a key prefix (for stats) */
    int detail_enabled;     /* nonzero if we're collecting detailed stats */
    int reqs_per_event;     /* Maximum number of io to process on each
                               io-event. */
    bool use_cas;
    enum protocol binding_protocol;
    int backlog;
    int item_size_max;        /* Maximum item size */
    int slab_chunk_size_max;  /* Upper end for chunks within slab pages. */
    int slab_page_size;     /* Slab's page units. */
    bool sig_hup;           /* a HUP signal was received but not yet handled */
    bool sasl;              /* SASL on/off */
    bool maxconns_fast;     /* Whether or not to early close connections */
    bool lru_crawler;        /* Whether or not to enable the autocrawler thread */
    bool lru_maintainer_thread; /* LRU maintainer background thread */
    bool lru_segmented;     /* Use split or flat LRU's */
    bool slab_reassign;     /* Whether or not slab reassignment is allowed */
    int slab_automove;     /* Whether or not to automatically move slabs */
    double slab_automove_ratio; /* youngest must be within pct of oldest */
    unsigned int slab_automove_window; /* window mover for algorithm */
    int hashpower_init;     /* Starting hash power level */
    bool shutdown_command; /* allow shutdown command */
    int tail_repair_time;   /* LRU tail refcount leak repair time */
    bool flush_enabled;     /* flush_all enabled */
    bool dump_enabled;      /* whether cachedump/metadump commands work */
    char *hash_algorithm;     /* Hash algorithm in use */
    int lru_crawler_sleep;  /* Microsecond sleep between items */
    uint32_t lru_crawler_tocrawl; /* Number of items to crawl per run */
    int hot_lru_pct; /* percentage of slab space for HOT_LRU */
    int warm_lru_pct; /* percentage of slab space for WARM_LRU */
    double hot_max_factor; /* HOT tail age relative to COLD tail */
    double warm_max_factor; /* WARM tail age relative to COLD tail */
    int crawls_persleep; /* Number of LRU crawls to run before sleeping */
    bool temp_lru; /* TTL < temporary_ttl uses TEMP_LRU */
    uint32_t temporary_ttl; /* temporary LRU threshold */
    int idle_timeout;       /* Number of seconds to let connections idle */
    unsigned int logger_watcher_buf_size; /* size of logger's per-watcher buffer */
    unsigned int logger_buf_size; /* size of per-thread logger buffer */
    bool drop_privileges;   /* Whether or not to drop unnecessary process privileges */
    bool relaxed_privileges;   /* Relax process restrictions when running testapp */
#ifdef EXTSTORE
    unsigned int ext_item_size; /* minimum size of items to store externally */
    unsigned int ext_item_age; /* max age of tail item before storing ext. */
    unsigned int ext_low_ttl; /* remaining TTL below this uses own pages */
    unsigned int ext_recache_rate; /* counter++ % recache_rate == 0 > recache */
    unsigned int ext_wbuf_size; /* read only note for the engine */
    unsigned int ext_compact_under; /* when fewer than this many pages, compact */
    unsigned int ext_drop_under; /* when fewer than this many pages, drop COLD items */
    double ext_max_frag; /* ideal maximum page fragmentation */
    double slab_automove_freeratio; /* % of memory to hold free as buffer */
    bool ext_drop_unread; /* skip unread items during compaction */
    /* per-slab-class free chunk limit */
    unsigned int ext_free_memchunks[MAX_NUMBER_OF_SLAB_CLASSES];
#endif
#ifdef TLS
    bool ssl_enabled; /* indicates whether SSL is enabled */
    SSL_CTX *ssl_ctx; /* holds the SSL server context which has the server certificate */
    char *ssl_chain_cert; /* path to the server SSL chain certificate */
    char *ssl_key; /* path to the server key */
    int ssl_verify_mode; /* client certificate verify mode */
    int ssl_keyformat; /* key format , defult is PEM */
    char *ssl_ciphers; /* list of SSL ciphers */
    char *ssl_ca_cert; /* certificate with CAs. */
    rel_time_t ssl_last_cert_refresh_time; /* time of the last server certificate refresh */
    unsigned int ssl_wbuf_size; /* size of the write buffer used by ssl_sendmsg method */
#endif
};

extern struct settings settings;
```
将全局设置按类别划分，主要分为以下几个部分:

### 2.1 网络相关参数

```c
struct settings {
	int maxconns;					// 最大的struct conn结构体数量
    int port;						// tcp端口号
    int udpport;					// udp端口号
    char *inter;					// 监听地址字符串
    char *socketpath;				// UNIX文件套接字地址
    int access;						// UNIX文件访问权限
    int backlog;					// listen队列长度
    enum protocol binding_protocol;	// 内容解析协议
    int idle_timeout;       		// 链接超时时间
};
```

### 2.2 网络子线程相关参数

```c
struct settings {
	int num_threads;        		// IO线程数量
    int num_threads_per_udp; 		// 处理UDP消息的IO线程数量
    int reqs_per_event;     		// 每次event事件处理IO请求的个数
};
```

### 2.3 内存初始化相关参数

```c
struct settings {
	size_t maxbytes;				// memcached使用内存大小
	double factor;					// item大小值的成长因子
	int chunk_size;					// item的初始值
	int item_size_max;        		// item的最大值
    int slab_chunk_size_max;  		// chunk item的最小值
    int slab_page_size;     		// slab页面的大小
};
```

### 2.4 内存管理相关参数
```c
struct settings {
    bool slab_reassign;     			// slab是否允许被重新分配
    int slab_automove;     				// slab转移的等级
    double slab_automove_ratio; 		// slab转移page中使用的比例
    unsigned int slab_automove_window; 	// slab转移线程中使用的统计窗口大小
};
```

### 2.5 Hash管理相关参数
```c
struct settings {
	int hashpower_init;     			// 初始Hash桶大小
	char *hash_algorithm;     			// 使用的Hash算法
};
```

### 2.6 LRU管理相关参数
```c
struct settings {
	rel_time_t oldest_live; 			// item的最大有效期
	bool lru_crawler;        			// 开启LRU链表扫描线程标志
    bool lru_maintainer_thread; 		// 开启LRU链表长度维护线程标志
    bool lru_segmented;     			// 开启LRU分片的标志
	int lru_crawler_sleep; 				// LRU链表扫描线程间隔休息时间
    uint32_t lru_crawler_tocrawl; 		// 每次LRU链表扫描线程扫描的item个数
    int hot_lru_pct; 					// HOT_LRU占比
    int warm_lru_pct; 					// WRAM_LRU占比
    double hot_max_factor; 				// HOT_LRU最多item的比例
    double warm_max_factor; 			// WRAM_LRU最多item的比例
    int crawls_persleep; 				// 需要扫描的LRU链表数
    bool temp_lru; 						// 开启TEMP_LRU标志
};
```

### 2.7 外存相关参数
```c
struct settings {
#ifdef EXTSTORE
    unsigned int ext_item_size; 		// 外存中item的最小长度值
    unsigned int ext_item_age; 			// 外存中item的最长有效期
    unsigned int ext_low_ttl; 			// 最小过期时间
    unsigned int ext_recache_rate; 		
    unsigned int ext_wbuf_size; 		// 读写缓冲区大小
    unsigned int ext_compact_under; 	// 紧缩外存页面的阈值
    unsigned int ext_drop_under; 		// 丢弃外存item的阀值
    double ext_max_frag; 				// 紧缩外存页面的阀值
    double slab_automove_freeratio; 	// 紧缩某个外存页面的阀值
    bool ext_drop_unread; 				// 丢弃外存item的标识
    unsigned int ext_free_memchunks[MAX_NUMBER_OF_SLAB_CLASSES];  // slab在外存空间大小限制
#endif
};
```

### 2.8 其他参数

```c
struct settings {
	int verbose;						// 打印详情的等级
	uint64_t oldest_cas; 				// 使用CAS时，忽略item的CAS最小值
	int evict_to_free;					// 开启evict的标识位
	char *auth_file;    
	char prefix_delimiter;  
    int detail_enabled;     	
    bool use_cas;
    bool sig_hup;           
    bool sasl;              
    bool maxconns_fast;     
    bool shutdown_command; 				// 开启shutdown命令标识
    int tail_repair_time;   			// item锁定的最长时间
    bool flush_enabled;     			// 开启内存清空标识位
    bool dump_enabled;      
    unsigned int logger_watcher_buf_size; 
    unsigned int logger_buf_size; 
    bool drop_privileges;   
    bool relaxed_privileges;   
#ifdef TLS
    bool ssl_enabled; 
    SSL_CTX *ssl_ctx; 
    char *ssl_chain_cert; 
    char *ssl_key; 
    int ssl_verify_mode; 
    int ssl_keyformat; 
    char *ssl_ciphers; 
    char *ssl_ca_cert; 
    rel_time_t ssl_last_cert_refresh_time; 
    unsigned int ssl_wbuf_size; 
#endif
};
```

?>对于未注释的配置，本文档将不会进行介绍。它们不影响您对memcached原理的理解。

## 3. 初始化过程
`memcached`相关配置的初始化过程都在`memcached.c`文件中，其源代码如下:

```C
int main() {
    ...
    /* init settings */
    settings_init();
#ifdef EXTSTORE
    settings.ext_item_size = 512;
    settings.ext_item_age = UINT_MAX;
    settings.ext_low_ttl = 0;
    settings.ext_recache_rate = 2000;
    settings.ext_max_frag = 0.8;
    settings.ext_drop_unread = false;
    settings.ext_wbuf_size = 1024 * 1024 * 4;
    settings.ext_compact_under = 0;
    settings.ext_drop_under = 0;
    settings.slab_automove_freeratio = 0.01;
    ext_cf.page_size = 1024 * 1024 * 64;
    ext_cf.wbuf_size = settings.ext_wbuf_size;
    ext_cf.io_threadcount = 1;
    ext_cf.io_depth = 1;
    ext_cf.page_buckets = 4;
    ext_cf.wbuf_count = ext_cf.page_buckets;
#endif
    ...
}

static void settings_init(void) {
    settings.use_cas = true;
    settings.access = 0700;
    settings.port = 11211;
    settings.udpport = 0;
#ifdef TLS
    settings.ssl_enabled = false;
    settings.ssl_ctx = NULL;
    settings.ssl_chain_cert = NULL;
    settings.ssl_key = NULL;
    settings.ssl_verify_mode = SSL_VERIFY_NONE;
    settings.ssl_keyformat = SSL_FILETYPE_PEM;
    settings.ssl_ciphers = NULL;
    settings.ssl_ca_cert = NULL;
    settings.ssl_last_cert_refresh_time = current_time;
    settings.ssl_wbuf_size = 16 * 1024; // default is 16KB (SSL max frame size is 17KB)
#endif
    /* By default this string should be NULL for getaddrinfo() */
    settings.inter = NULL;
    settings.maxbytes = 64 * 1024 * 1024; /* default is 64MB */
    settings.maxconns = 1024;         /* to limit connections-related memory to about 5MB */
    settings.verbose = 0;
    settings.oldest_live = 0;
    settings.oldest_cas = 0;          /* supplements accuracy of oldest_live */
    settings.evict_to_free = 1;       /* push old items out of cache when memory runs out */
    settings.socketpath = NULL;       /* by default, not using a unix socket */
    settings.auth_file = NULL;        /* by default, not using ASCII authentication tokens */
    settings.factor = 1.25;
    settings.chunk_size = 48;         /* space for a modest key and value */
    settings.num_threads = 4;         /* N workers */
    settings.num_threads_per_udp = 0;
    settings.prefix_delimiter = ':';
    settings.detail_enabled = 0;
    settings.reqs_per_event = 20;
    settings.backlog = 1024;
    settings.binding_protocol = negotiating_prot;
    settings.item_size_max = 1024 * 1024; /* The famous 1MB upper limit. */
    settings.slab_page_size = 1024 * 1024; /* chunks are split from 1MB pages. */
    settings.slab_chunk_size_max = settings.slab_page_size / 2;
    settings.sasl = false;
    settings.maxconns_fast = true;
    settings.lru_crawler = false;
    settings.lru_crawler_sleep = 100;
    settings.lru_crawler_tocrawl = 0;
    settings.lru_maintainer_thread = false;
    settings.lru_segmented = true;
    settings.hot_lru_pct = 20;
    settings.warm_lru_pct = 40;
    settings.hot_max_factor = 0.2;
    settings.warm_max_factor = 2.0;
    settings.temp_lru = false;
    settings.temporary_ttl = 61;
    settings.idle_timeout = 0; /* disabled */
    settings.hashpower_init = 0;
    settings.slab_reassign = true;
    settings.slab_automove = 1;
    settings.slab_automove_ratio = 0.8;
    settings.slab_automove_window = 30;
    settings.shutdown_command = false;
    settings.tail_repair_time = TAIL_REPAIR_TIME_DEFAULT;
    settings.flush_enabled = true;
    settings.dump_enabled = true;
    settings.crawls_persleep = 1000;
    settings.logger_watcher_buf_size = LOGGER_WATCHER_BUF_SIZE;
    settings.logger_buf_size = LOGGER_BUF_SIZE;
    settings.drop_privileges = false;
#ifdef MEMCACHED_DEBUG
    settings.relaxed_privileges = false;
#endif
}
```
在`settings_init`函数中是对全局变量`settings`结构体成员的初始化设置，在后续`main`函数中还会根据环境变量或者运行参数修改全局设置。
