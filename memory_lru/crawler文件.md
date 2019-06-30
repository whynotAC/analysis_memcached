crawler文件内容分析
===========================================
1 crawler文件中对外接口函数
---------------------------------------------------
| 函数名 | 函数定义 | 作用 | 备注 |
| ----- | ------ | ---- | --- |
| `start_item_crawler_thread` | `int start_item_crawler_thread(void);` | 开启`LRU`扫描线程 | 无 |
| `open_item_crawler_thread` | `int stop_item_crawler_thread(void);` | 关闭`LRU`扫描线程 | 无 |
| `init_lru_crawler` | `int init_lru_crawler(void *arg);` | 初始化`LRU`扫描线程所使用的全局变量 | 无 |
| `lru_crawler_crawl` | `enum crawler_result_type lru_crawler_crawl(char *slabs, enum crawler_run_type, void *c, const int sfd, unsigned int remaining)` | 根据客户端发送来的信息，来设置需要扫描的`LRU`链表 | 无 |
| `lru_crawler_start` | `int lru_crawler_start(uint8_t *ids, uint32_t remaining, const enum crawler_run_type type, void *data, void *c, const int sfd);` | `LRU`维护线程根据`LRU`的信息来调用此函数来执行`LRU`扫描 | 无 |
| `lru_crawler_pause` | `void lru_crawler_pause(void);` | 用于暂停`LRU`扫描线程 | 无 |
| `lru_crawler_resume` | `void lru_crawler_resume(void);` | 用于恢复`LRU`扫描线程 | 无 |

2 全局使用的结构体
---------------------------------------------------
```
#define LRU_CRAWLER_CAP_REMAINING -1

// 每个LRU都拥有一个crawlerstats_t结构体，用于记录LRU中item过期的情况。
typedef struct {
	uint64_t histo[61];	     //将LRU未来一小时过期的item按分钟记录在数组中
	uint64_t ttl_hourplus;   //LRU未来一小时过期的item个数
	uint64_t noexp;
	uint64_t reclaimed;
	uint64_t seen;
	rel_time_t start_time;	 // 此LRU扫描的开始时间
	rel_time_t end_time;    // 此LRU扫描的结束时间
	bool run_complete; 		 // 此LRU扫描是否完成
} crawlerstats_t;

// LRU扫描线程速序使用的结构体
struct crawler_expired_data {
	pthread_mutex_t lock;			// 锁
	crawlerstats_t crawlerstats[POWER_LARGEST]; // LRU对应的扫描信息
	/* redundant with crawlerstats_t so we can get overall start/stop/done */
	rel_time_t start_time;			// 整个任务的开始时间
	rel_time_t end_time;			// 整个任务的结束时间
	bool crawl_complete;			// 整个任务是否完成
	bool is_external; /* whether this was an alloc local or remote to the module */
};

// 控制LRU扫描线程行为的客户端所使用的结构体
typedef struct {
	// 链接的connect结构体
	void *c; /* original connection structure. still with source thread attached. */
	int sfd; /* client fd. 连接的文件描述符 */
	bipbuf_t *buf; /* output buffer */
	char *cbuf; 	/* current buffer */
} crawler_client_t;

typedef struct _crawler_module_t crawler_module_t;

// 用于定义过期函数指针
typedef void (*crawler_eval_func)(crawler_module_t *cm, item *it, uint32_t hv, int slab_cls);
typedef int (*crawler_init_func)(crawler_module_t *cm, void *data); // TODO: init args?
typedef void (*crawler_deinit_func)(crawler_module_t *cm); // TODO: extra args?
typedef void (*crawler_doneclass_func)(crawler_module_t *cm, int slab_cls);
typedef void (*crawler_finalize_func)(crawler_module_t *cm);

typedef struct {
	crawler_init_func init;		// run before crawl starts (初始化函数)
	crawler_eval_func eval;		// runs on an item (用于判断item是否过期)
	crawler_doneclass_func doneclass; // runs once per sub-crawler completion (此slabclass对应的LRU链表完成时调用)
	crawler_finalize_func finalize;	// runs once when all sub-crawlers are done (当LRU扫描任务完成时调用)
	bool needs_lock;	// whether or not we need the LRU lock held when eval is called (判断是否需要加锁)
	bool needs_client; // whether or not to grab onto the remote client (判断是否需要client控制)
} crawler_module_reg_t;

struct _crawler_module_t {
	void *data;		// opaque data pointer (用于指向数据指针)
	crawler_client_t c;	// 客户端
	crawler_module_reg_t *mod;	// 用于LRU扫描线程使用的函数指针
};
```

3 全局变量
--------------------------------------------------
```
// LRU扫描线程以CRAWLER_AUTOEXPIRE/CRAWLER_EXPIRED工作方式时，使用的函数指针
crawler_module_reg_t crawler_expired_mod = {
	.init = crawler_expired_init,
	.eval = crawler_expired_eval,
	.doneclass = crawler_expired_doneclass,
	.finalize = crawler_expired_finalize,
	.needs_lock = true,
	.needs_client = false
};

// LRU扫描线程以CRAWLER_METADUMP工作方式时，使用的函数指针
crawler_module_reg_t crawler_metadump_mod = {
	.init = NULL,
	.eval = crawler_metadump_eval,
	.doneclass = NULL,
	.finalize = crawler_metadump_finalize,
	.needs_lock = false,
	.needs_client = true
};

// LRU扫描线程所有工作方式下的函数指针数组
crawler_module_reg_t *crawler_mod_regs[3] = {
	&crawler_expired_mod,	// CRAWLER_AUTOEXPIRE
	&crawler_expired_mod,	// CRAWLER_EXPIRED
	&crawler_metadump_mod	// CRAWLER_METADUMP
};

crawler_module_t active_crawler_mod;	// LRU扫描线程正在使用的方法
enum crawler_run_type active_crawler_type; // LRU扫描线程正在进行的工作方式

static crawler crawlers[LARGEST_ID];		// 存放每个LRU链表扫描使用的伪ITEM

static int crawler_count = 0; 	// LRU扫描线程需要扫描的LRU链表个数
static volatile int do_run_lru_crawler_thread = 0; // 判断LRU扫描线程是否在运行
static int lru_crawler_initialized = 0; // LRU扫描线程使用的结构体是否初始化
static pthread_mutex_t lru_crawler_lock = PTHREAD_MUTEX_INITIALIZER; // LRU扫描线程所使用的锁
static pthread_cond_t lru_crawler_cond = PTHREAD_COND_INITIALIZER; // LRU扫描线程所使用的条件变量

/* Will crawl all slab classes a minimum of once per hour */
#define MAX_MAINTCRAWL_WAIT 60 * 60 // 每个LRU链表最长等待时间为1hour

#define LRU_CRAWLER_WRITEBUF 8192		// 用于连接客户端的buf长度

static pthread_t item_crawler_tid;	// LRU扫描线程的ID
```

3 文件静态函数
---------------------------------------------------
| 函数名 | 定义 | 作用 | 备注 |
| ----- | ---- | ---- | ---- |
| `lru_crawler_close_client` | `static void lru_crawler_close_client(crawler_client_t *c);` | 用于关闭连接的`client` | 关闭用于控制`LRU`扫描线程行为的`client` |
| `lru_crawler_release_client` | `static void lru_crawler_release_client(crawler_client_t *c);` | 释放客户端申请的内存空间 | 无 |
| `crawler_epired_init` | `static int crawler_expired_init(crawler_module_t *cm, void *data)` | 用于初始化`LRU`扫描线程使用的结构体 | 无 |
| `crawler_expired_doneclass` | `static void crawler_expired_doneclass(crawler_module_t *cm, int slab_cls)` | 当`LRU`链表扫描完成时调用的函数 | 无 |
| `crawler_expired_finalize` | `static void crawler_expired_finalize(crawler_module_t *cm)` | 当`LRU`扫描线程完成所有`LRU`链表扫描后调用的函数 | 无 |
| `crawler_expired_eval` | `static void crawler_expired_eval(crawler_module_t *cm, item *search, uint32_t hv, int i)` | 用于判断`LRU`链表中`item`是否符合`evicted`要求 | `CRAWLER_AUTOEXPIRE/CRAWLER_EXPIRED`工作方式 |
| `crawler_metadump_eval` | `static void crawler_metadump_eval(crawler_module_t *cm, item *it, uint32_t hv, int i)` | 用于判断`LRU`链表中`item`是否符合`evicted`要求  | `CRAWLER_METADUMP`工作方式 |
| `crawler_metadump_finalize` | `static void crawler_metadump_finalize(crawler_module_t *cm)` | 当`LRU`扫描线程完成所有`LRU`链表扫描后调用的函数 | `CRAWLER_METADUMP`工作方式 |
| `lru_crawler_poll` | `static int lru_crawler_poll(crawler_client_t *c)` | 暂无 | 无 |
| `lru_crawler_client_getbuf` | `static int lru_crawler_client_getbuf(crawler_client_t *c)` | 暂无 | 无 |
| `lru_crawler_class_done` | `static void lru_crawler_class_done(int i)` | 当`LRU`链表扫描后调用函数,去除伪`ITEM`,记录所描的过程中记录的`ITEM`信息 | 无 |
| `item_crawler_thread` | `static void *item_crawler_thread(void *arg)` | `LRU`扫描线程的函数主体 | 无 |
| `do_lru_crawler_start` | `static int do_lru_crawler_start(uint32_t id, uint32_t remaining)` | 用于初始化`LRU`扫描线程所用的伪`ITEM`，并将它加入`LRU`链表尾部 | 无 |
| `lru_crawler_set_client` | `static int lru_crawler_set_client(crawler_module_t *cm, void *c, const int sfd)` | 用于初始化`client`的内容 | 无 |

