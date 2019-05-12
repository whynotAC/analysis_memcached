items文件内容分析
=================================
1 items文件中对外接口函数
---------------------------------------

| 函数名     	| 	函数定义	|	作用	|	备注      |
|----------	| ----------	| ------	| -------    |
| `get_cas_id` | `uint64_t get_cas_id(void)` | 获取原子序号 | 需要获取`cas_id_lock`锁 |
| `do_item_alloc` | `item *do_item_alloc(char *key, const size_t nkey, const unsigned int flags, const rel_time_t exptime, cont int nbytes)` | 用于分配`item` | 调用`do_item_alloc_pull`获取空间，并将`item`插入到不同的队列 |
| `do_item_alloc_chunk` | `item_chunk *do_item_alloc_chunk(item_chunk *ch, const size_t bytes_remain)` | 用于分配`item_chunk`的空间 | 调用`do_item_alloc_pull`来获取空间 |
| `do_item_alloc_pull` | `item *do_item_alloc_pull(const size_t ntotal, const unsigned int id)` | 用于分配`slab`空间 | 调用`lru_pull_tail`和`slab_alloc`来分配空间 |
| `item_free` | `void item_free(item *it)` | 用于释放`item`的空间 | 无 |
| `item_size_ok` | `bool item_size_ok(const size_t nkey, const int flags, const int nbytes` | 判断`item`的大小是否小于1M | 无 |
| `do_item_link` | `int do_item_link(item *it, const uint32_t hv)` | 将`item`链接入`HASH`链表和`LRU`链表 | 无 |
| `do_item_unlink` | `void do_item_unlink(item *it, const uint32_t hv)` | 将`item`从`HASH`链表和`LRU`链表中释放，并根据`item.refcount`来判断是否需要释放空间 | 无 |
| `do_item_unlink_nolock` | `void do_item_unlink_nolock(item *it, const uint32_t hv)` | 与`do_item_unlink`函数一致 | 已经准备废除的代码 |
| `do_item_remove` | `void do_item_remove(item *it)` | 根据`item.refcount`的值来判断是否需要释放`item` | 无 |
| 	`do_item_update` | `void do_item_update(item *it)` | 更新`item.time`时间，并根据需要切换`LRU`队列 | 每次`item`跟换时间需要`ITEM_UPDATE_INTERVAL`的冷却期 |
| `do_item_update_nolock` | `void do_item_update_nolock(item *it)` | 更新`item.time`，将其链入头部 | 已经准备废除的代码 | 
| `do_item_replace` | `int do_item_replace(item *it, item *new_it, const uint32_t hv)` | 替换`item`操作 | 无 |
| `item_is_flushed` | `int item_is_flushed(item *it)` | 判断`item`是否已经被删除 | 无 |
| `do_get_lru_size` | `unsigned int do_get_lru_size(uint32_t id)` | 获取`LRU`的链接长度 | 无 |
| `do_item_linktail_q` | `void do_item_linktail_q(item *it)` | 将`item`链接到`LRU`的尾部 | 无 |
| `do_item_unlinktail_q` | `void do_item_unlinktail_q(item *it)` | 将`item`从`LRU`链接中去链 | 无 |
| `do_item_carl_q` | `item *do_item_crawl_q(item *it)` | 将`item`向前移动一个位置，用于扫描整个`LRU`队列 | 无 |
| `item_lru_bump_buf_create` | `void *item_lru_bump_buf_create(void)` | 创建`lru_bump_buf`结构 | 无 |
| `lru_pull_tail` | `int lru_pull_tail(const int orig_id, const int cur_lru, const uint64_t total_bytes, const uint8_t flags, const rel_time_t max_age, struct lru_pull_tail_return *ret_it)` | 将`item`删除、判断`item`是否过期等操作 |无|
| `item_cachedump` | `char *item_cachedump(const unsigned int slabs_clsid, const unsigned int limit, unsigned int *bytes)` | 将`item`按照`LRU`顺序进行展示存储 | 无 |
| `item_stats` | `void item_stats(ADD_STAT add_stats, void *c)` | 获取整个`item`的运行情况 | 无 |
| `do_item_stats_add_crawl` | `void do_item_stats_add_crawl(const int i, const uint64_t reclaimed, const uint64_t unfetched, const uint64_t checked)` | 增加`itemstats`中对应的`classslab`的数据 | 无 |
| `item_stats_totals` | `void item_stats_totals(ADD_STAT add_stats, void *c)` | 获取所有的`item`的状态 | 无 |
| `item_stats_sizes` | `void item_stats_sizes(ADD_STAT add_stats, void *c)` | 获取`stats_size_hist`中的内容 | 无 |
| `item_stats_sizes_init` | `void item_stats_sizes_init(void)` | 初始化`stats_sizes_buckets`和`stats_sizes_hist`等信息 | 无 |
| `item_stats_sizes_enable` | `void item_stats_sizes_enable(ADD_STAT add_stats, void *c)` | 是否启用`stats_sizes_hist`等记录信息 | 无 |
| `item_stats_sizes_disable` | `void item_stats_sizes_disable(ADD_STAT add_stats, void *c)` | 用于释放`stats_sizes_hist`的内存空间 | 无 |
| `item_stats_sizes_add` | `void item_stats_sizes_add(item *it)` | 增加`stats_sizes_hist`对应的`item`的`bucket`的数值 | 无 |
| `item_stats_sizes_remove` | `void item_stats_sizes_remove(item *it)` | 其作用与`item_stats_sizes_add`相反 | 无 |
| `item_stats_sizes_status` | `bool item_stats_sizes_status(void)` | 用于统计信息 | 无 |
| `fill_item_automove` | `void fill_item_stats_automove(item_stats_automove *am)` | 将`itemstats`中的信息填充到`am`中，用于判断是否需要`slab_rebalance`操作 | 无|
| `do_item_get` | `item *do_item_get(const char *key, const size_t nkey, const uint32_t hv, conn *c, const bool do_update)` | 根据`key`值来获取`item` | 无 |
| `do_item_touch` | `item *do_item_touch(const char *key, const size_t nkey, uint32_t exptime, const uint32_t hv, conn *c)` | 调用`do_item_get`函数，并将`item`的日期进行更行 | 无 |
| `item_stats_reset` | `void item_stats_reset(void)` | 重置`itemstats`中的状态 | 无 |
| `start_lru_maintainer_thread` | `int start_lru_maintainer_thread(void *arg)` | 开启`LRU maintainer`线程 | 无 |
| `stop_lru_maintainer_thread` | `int stop_lru_maintainer_thread(void)` | 关闭`LRU maintainer`线程 | 无 |
| `init_lru_maintainer` | `int init_lru_maintainer(void)` | 初始化`LRU maintainer`线程使用的全局变量 | 无 |
| `lru_maintainer_pause` | `void lru_maintainer_pause(void)` | 暂停`LRU maintainer`线程 | 无 |
| `lru_maintainer_resume` | `void lru_maintainer_resume(void)` | 恢复`LRU maintainer`线程 | 无 |

2 全局使用的结构体
-------------------------------------------
>```
>	// 此结构体用于lru_pull_tail函数
>	struct lru_pull_tail_return {
>		item *it;
>		uint32_t hv;
>	};
>
>	// 此结构体用于automover
>	typedef struct {
>		int64_t evicted;
>		int64_t outofmemory;
>		uint32_t age;
>	} item_stats_automove;
>
>	// 用于统计item的信息
>	typedef struct {
>		uint64_t evicted;
>		uint64_t evicted_nonzero;
>		uint64_t reclaimed;
>		uint64_t outofmemory;
>		uint64_t tailrepairs;
>		uint64_t expired_unfetched; // items reclaimed but never touched
>		uint64_t evicted_unfetched; // items evicted but never touched
>		uint64_t evicted_active;	// items evicted that should been shuffled
>		uint64_t crawler_reclaimed;
>		uint64_t crawler_items_checked;
>		uint64_t lrutail_reflocked;
>		uint64_t moves_to_cold;
>		uint64_t moves_to_warm;
>		uint64_t direct_reclaims;
>		uint64_t hits_to_hot;
>		uint64_t hits_to_warm;
>		uint64_t hits_to_cold;
>		uint64_t hits_to_cold;
>		uint64_t hits_to_temp;
>		rel_time_t evicted_time;
>	} itemstats_t;
>
>	typedef struct _lru_bump_buf {
>		struct _lru_bump_buf *prev;
>		struct _lru_bump_buf *next;
>		pthread_mutex_t mutex;
>		bipbuf_t *buf;
>		uint64_t dropped;
>	} lru_bunp_buf;
>
>	typedef struct {
>		item *it;
>		uint32_t hv;
>	} lru_bump_entry;
>
>	// 完成slab_rebalance数据结构的函数结构体
>	typedef struct {
>		slab_automove_init_func init;
>		slab_automove_free_func free;
>		slab_automove_run_func run;
>	} slab_automove_reg_t;
>```

3 全局变量
--------------------------------
全局的宏定义如下:
>	```
>		#define HOT_LRU 0
>		#define WARM_LRU 64
>		#define COLD_LRU 128
>		#define TEMP_LRU 192   // WARM_LRU|COLD_LRU
>		#define CLEAR_LRU(id) (id & ~(3<<6))
>		#define GET_LRU(id) (id & (3<<6))
>
>		#define LRU_PULL_EVICT 1
>		#define LRU_PULL_CRAWL_BLOCKS 2
>		#define LRU_PULL_RETURN_ITEM 4
>
>	#ifdef EXTSTORE
>		#define STORAGE_delete(e, it) \
>			do {	\
>				if (it->it_flags & ITEM_HDR) { \
>					item_hdr *hdr = (item_hdr *)ITEM_data(it); \
>					extstore_delete(e, hdr->page_id, hdr->page_version, \
>							1,	ITEM_ntotal(it)); \
>				} \
>			} while(0)
>	#else
>		#define STORAGE_delete(...)
>	#endif
>
>		#define LARGEST_ID POWER_LARGEST
>
>		#define LRU_BUMP_BUF_SIZE 8192
>
>		#define MAX_MAINTCRAWL_WAIT 60 * 60
>
>		#define MAX_LRU_MAINTAINER_SLEEP 1000000
>		#define MIN_LRU_MAINTAINER_SLEEP 1000
>	```

全局使用的变量如下:

| 变量名 | 定义 | 作用 | 备注 |
| ------|-----|-----|-----|
| `lru_locks` | `pthread_mutex_t lru_locks[POWER_LARGEST]` | `LRU`队列的锁数组 | 无 |
| `lru_type_map` | `static unsigned int lru_type_map[4] = {HOT_LRU, WARM_LRU, COLD_LRU, TEMP_LRU}` | 所有`LRU`队列对应的值 | 无 |
| `heads` | `static item *heads[LARGEST_ID]` | `LRU`队列的头数组 | 无 |
| `tails` | `static item *tails[LARGEST_ID]` | `LRU`队列的尾数组 | 无 |
| `itemstats` | `static itemstats_t itemstats[LARGEST_ID]` | `LRU`队列的信息 | 用于后续的`slab_rebalance`等处理操作 |
| `sizes` | `static unsigned int sizes[LARGEST_ID]` | 用于存放`LRU`中`item`的个数 | 无 |
| `sizes_bytes` | `static uint64_t sizes_bytes[LARGEST_ID]` | `LRU`中`item`的总子节大小 | 无 |
| `stats_sizes_hist` | `static unsigned int *stats_sizes_hist = NULL` | 用于存放历史的`LRU`队列的状态 | 无 |
| `stats_sizes_cas_min` | `static uint64_t stats_sizes_cas_min = 0` | 用于保存最小的`CAS`的数值 | 无 |
| `stats_sizes_buckets` | `static int stats_sizes_buckets = 0` | `stats bucket`的个数 | 无 |
| `do_run_lru_maintainer_thread` | `static volatile int do_run_lru_maintainer_thread = 0` | 用于判断是否运行`lru maintainer`线程 | 无 |
| `lru_maintainer_initialized` | `static int lru_maintainer_initialized = 0` | 用于初始化`lru maintainer`线程使用的全局变量 | 无 |
| `lru_maintainer_lock` | `static pthread_mutex_t lru_maintainer_lock = PTHREAD_MUTEX_INITAIALIZER` | `lru maintainer`线程中使用的锁 | 无 |
| `cas_id_lock` | `static pthread_mutex_t cas_id_lock = PTHREAD_MUTEX_INITALIZER` | 用于获取`cas`值的锁 | 无 |
| `stats_sizes_lock` | `static pthrad_mutex_t stats_sizes_lock = PTHREAD_MUTEX_INITIALIZER` | 用于获取或设置`stats_sizes_hist`的锁 | 无 |
| `bump_buf_head` | `static lru_bump_buf *bump_buf_head = NULL` | 用于工作线程 | 无 |
| `bump_buf_tail` | `static lru_bump_buf *bump_buf_tail = NULL` | 用于工作线程 |
无 |
| `bump_buf_lock` | `static pthread_mutex_t bump_buf_lock = PTHREAD_MUTEX_INITIALIZER` | 用于保护`bump_buf_head`和`bump_buf_tail`信息 | 无 |
| `slab_automove_default` | `slab_automove_reg_t slab_automove_default` | 用于判断`slab`是否需要转移`slab rebalance` | 根据是否定义`EXTSTORE`来决定是否用的函数 |
| `lru_maintainer_tid` | `static pthread_t lru_maintainer_tid` | 用于保存`lru maintainer`线程的`ID` | 无 |

4 文件静态函数
--------------------------------------
| 函数名 | 定义 | 作用 | 备注 |
| ----- | ---- | ---- | ----- |
| `item_link_q` | `static void item_link_q(item *it)` | 获取`lru_locks`中`item`对应的锁，将`item`链接到`LRU`链表中 | 调用`do_item_link_q`实现具体功能 | 
| `item_unlink_q` | `static void item_unlink_q(item *it)` | 获取`lru_locks`中`item`对应的锁，将`item`从`LRU`链表中去除 | 调用`do_item_unlink_q`实现具体功能 |
| `lru_bump_async` | `static bool lru_bump_async(lru_bump_buf *b, item *it, uint32_t hv)` | 没看懂 | 无 |
| `lru_total_bumps_dropped` | `static uint64_t lru_total_bumps_dropped(void)` | 没看懂 | 无 |
| `temp_lru_size` | `static unsigned int temp_lru_size(int slabs_clsid)` | 获取`slabs_clsid`对应的`sizes_bytes`的大小 | 无 |
| `item_make_header` | `static size_t item_make_header(const uint8_t nkey, const unsigned int flags, const int nbytes, char *suffix, uint8_t nsuffix)` | 根据`neky`等大小来设置`item`所占的内存大小 | 无 |
| `do_item_link_q` | `static void do_item_link_q(item *it)` | 将`item`链接到`LRU`链表中 | 设置全局`sizes`和`sizes_bytes`的值 |
| `item_link_q_warm` | `static void item_link_q_warm(item *it)` | 将`item`链接到`LRU warm`链表中 | 设置全局`itemstats`中的`moves_to_warm`的值 | 
| `do_item_unlink_q` | `static void do_item_unlink_q(item *it)` | 将`item`从`LRU`链表中去除 | 设置全局`sizes`和`sizes_bytes`的值 |
| `lru_bump_buf_link_q` | `static void lru_bump_buf_link_q(lru_bump_buf *b)` | 未知 | 无 |
| `lru_bump_async` | `static bool lru_bump_async(lru_bump_buf *b, item *it, uint32_t hv)` | 未知 | 无 |
| `lru_maintainer_bumps` | `static bool lru_maintainer_bumps(void)` | 未知 | 无 |
| `lru_total_bumps_dropped` | `static uint64_t lru_total_bumps_dropped(void)` | 未知 | 无 |
| `lru_maintainer_juggle` | `static int lru_maintainer_juggle(const int slabs_clsid)` | 此函数用于调整`slabs_clsid`对应的`LRU`链表的个数 | 无 |
| `lru_maintainer_crawler_check` | `static void lru_maintainer_crawler_check(struct crawler_expired_data *cdata, logger *l)` |未知 | 无 |
| `lru_maintainer_thread` | `static void *lru_maintainer_thread(void *arg)` | `lru maintainer`线程的函数 | 分析的主要对象 |
 

