crawler扫描源代码分析
==================================
1 crawler线程启动/暂停等相关操作
-----------------------------------------
在介绍`crawler`线程源代码之前，我们先看一下`crawler`扫描线程如何进行启动以及初始化其所使用的全局变量。

### 启动函数源代码
```
/* Lock dance to "block" until thread is waiting on its condition:
 * caller locks mtx. caller spawns thread.
 * thread blocks on mutex.
 * caller waits on condition, releases lock.
 * thread gets lock, send signal.
 * caller can't wait, as thread has lock.
 * thread waits on condition, release lock
 * caller makes on condition, releases lock
 * caller makes on condition, gets lock.
 * caller immediately release lock.
 * thread is now safely waiting on condition before the caller returns
 */
int start_item_crawler_thread(void) {
	int ret;
	
	if (settings.lru_crawler)			// 判断是否需要开启crawler线程
		return -1;
	pthread_mutex_lock(&lru_crawler_lock);
	do_run_lru_crawler_thread = 1;	// 标示crawler线程启动
	if ((ret = pthread_create(&item_crawler_tid, NULL, item_crawler_thread, NULL)) != 0) {
		fprintf(stderr, "Can't create LRU crawler thread: %s\n", strerror(ret));
		pthread_mutex_unlock(&lru_crawler_lock);
		return -1;
	}
	/* Avoid returning until the crawler has actually started */
	pthread_cond_wait(&lru_crawler_cond, &lru_crawler_lock);
	pthread_mutex_unlock(&lru_crawler_lock);
	
	return 0;
}
```
上述代码是`cralwer`线程的启动代码，其在`memecached.c`文件中`main`函数中被调用或者`worker`线程中的`process_command`函数调用。

### 关闭线程源代码
```
int stop_item_crawler_thread(void) {
	int ret;
	pthread_mutex_lock(&lru_crawler_lock);
	do_run_lru_crawler_thread = 0;	// 标示crawler线程结束
	pthread_cond_signal(&lru_crawler_cond);
	pthread_mutex_unlock(&lru_crawler_lock);
	if ((ret = pthread_join(item_crawler_tid, NULL)) != 0) {
		fprintf(stderr, "Failed to stop LRU crawler thread: %s\n", strerror(ret));
		return -1;
	}
	settings.lru_crawler = false;
	return 0;
}
```
上述代码是`crawler`线程的结束代码，其在`memcached.c`文件中`worker`线程调用的`process_command`函数中调用。

### 暂停/恢复现成的源代码
```
/* If we hold this lock, crawler can't wake up or move */
void lru_crawler_pause(void) {
	pthread_mutex_lock(&lru_crawler_lock);
}

void lru_crawler_resume(void) {
	pthread_mutex_unlock(&lru_crawler_lock);
}
```

### crawler初始化函数源代码
```
int init_lru_crawler(void *arg) {
	if (lru_crawler_initialized == 0) {
#ifdef EXTSTORE
		storage = arg;
#endif
		if (pthread_cond_init(&lru_crawler_cond, NULL) != 0) {
			fprintf(stderr, "Can't initialize lru crawler condition\n");
			return -1;
		}
		pthread_mutex_init(&lru_crawler_lock, NULL);
		active_crawler_mod.c.c = NULL;
		active_crawler_mod.mod = NULL;
		acitve_crawler_mod.data = NULL;
		lru_crawler_initialized = 1;
	}
	return 0;
}
```

2 crawler线程调用关系
-----------------------------------------
`crawler`线程运行需要设置`crawlers`结构，设置`crawlers`结构是由`do_lru_crawler_start`函数完成。其主要功能是将`crawlers`数组中的伪`ITEM`插入到`LRU`链表的尾部，然后由`crawler`线程将伪`ITEM`从尾到头依次向前运行，检查`item`是否过期。

```
/* 'remaining' is passed in so the LRU maintainer thread can scrub the whole
 * LRU every time.
 */
static int do_lru_crawler_start(uint32_t id, uint32_t remaining) {
	uint32_t sid = id;
	int starts = 0;
	
	pthread_mutex_lock(&lru_locks[sid]);   // 锁住LRU链表
	if (crawlers[sid].it_flags == 0) {     // 判断LRU是否插入伪ITEM
		if (settings.verbose > 2)
			fprintf(stderr, "Kicking LRU crawler off for LRU %u\n", sid);
		crawlers[sid].nbytes = 0;
		crawlers[sid].nkey = 0;
		crawlers[sid].it_flags = 1; // for a crawler, this means enabled
		crawlers[sid].next = 0;
		crawlers[sid].prev = 0;
		crawlers[sid].time = 0;
		if (remaining == LRU_CRAWLER_CAP_REMAINING) {
			remaining = do_get_lru_size(sid);
		}
		/* Values for remaining:
		 * remaining = 0
		 * - scan all elements, until a NULL is reached
		 * - if empty, NULL is reached right away
		 * remaining = n + 1
		 * - first n elements are parsed (or until a NULL is reached)
		 */
		 if (remaining) remaining++;
		 crawlers[sid].remaining = remaining;
		 crawlers[sid].slabs_clsid = sid;
		 crawlers[sid].reclaimed = 0;
		 crawlers[sid].unfetched = 0;
		 crawlers[sid].checked = 0;
		 do_item_linktail_q((item *)&crawlers[sid]); // 将伪ITEM插入到LRU链表尾部
		 crawler_count++;	// 记录crawler线程需要扫描的LRU链表个数
		 starts++;
	}
	pthread_mutex_unlock(&lru_locks[sid]);
	if (starts) {
		STATS_LOCK();
		stats_state.lru_crawler_running = true;
		stats.lru_crawler_starts++;
		STATS_UNLOCK();
	}
	return starts;
}
```

由上述`do_lru_crawler_start`函数的源代码可以看出此函数为`static`函数，为文件静态函数，不能被外界函数调用。而调用此函数的函数为`lru_crawler_start`函数，其源代码如下:

```
// 此函数用于初始化crawler线程所使用的扫描函数，并调用do_lru_crawler_start函数
int lru_crawler_start(uint8_t *ids, uint32_t remaining,
								const enum crawler_run_type type, void *data,
								void *c, const int sfd) {
	int starts = 0;
	bool is_running;
	static rel_time_t block_ae_until = 0; // 用于防止CRAWLER_AUTOEXPIRE工作状态间隔至少60s
	pthread_mutex_lock(&lru_crawler_lock);
	STATS_LOCK();
	is_running = stats_state.lru_crawler_running; // 判断crawler线程是否运行中
	STATS_UNLOCK();
	if (is_running &&
			!(type == CRAWLER_AUTOEXPIRE && active_crawler_type == CRAWLER_AUTOEXPIRE)) {
		pthread_mutex_unlock(&lru_crawler_lock);
		block_ae_until = current_time + 60;
		return -1;
	}
	
	// 判断时间间隔
	if (type == CRAWLER_AUTOEXPIRE && block_ae_until > current_time) {
		pthread_mutex_unlock(&lru_crawler_lock);
		return -1;
	}
	
	// Configure the module
	// 初始化crawler线程所使用的数据结构
	if (!is_running) {
		assert(crawler_mod_regs[type] != NULL);
		active_crawler_mod.mod = crawler_mod_regs[type]; // 设置crawler线程扫描所使用的函数
		active_crawler_type = type;
		if (active_crawler_mod.mod->init != NULL) {
			active_crawler_mod.mod->init(&active_crawler_mod, data);
		}
		if (active_crawler_mod.mod->needs_client) {
			if (c == NULL || sfd == 0) {
				pthread_mutex_unlock(&lru_crawler_lock);
				return -2;
			}
			if (lru_crawler_set_client(&active_crawler_mod, c, sfd) != 0) {
				pthread_mutex_unlock(&lru_crawler_lock);
				return -2;
			}
		}
	}
	
	/* we allow the autocrawler to restart sub-LRU's before completion */
	for (int sid = POWER_SMALLEST; sid < POWER_LARGEST; sid++) {
		if (ids[sid])
			starts += do_lru_crawler_start(sid, remaining); // 添加伪ITEM
	}
	if (starts) {
		pthread_cond_signal(&lru_crawler_cond); // 释放信号量
	}
	pthread_mutex_unlock(&lru_crawler_lock);
	return starts;
}
```
调用`lru_crawler_start`函数的有两处，分别如下:

| 文件  |  函数  | 作用 | 备注 |
| ---  | ----- | ---- | ---- |
| items.c | `lru_maintainer_crawler_check` | 用于检查`LRU`链表中的过期`item` | 无 |
| crawler.c | `lru_crawler_crawl` | 用于管理客户端命令检查`LRU`链表的函数 | 无 |

`lru_crawler_crawl`函数源代码如下，其被`memcached.c`文件中`process_command`函数调用，用于处理客户端发送过来的命令。

```
/*
 * Also only clear the crawlerstats once per sid
 */
enum crawler_result_type lru_crawler_crawl(char *slabs, const eunm crawler_run_type type, void *c, const int sfd, unsigned int remaining) {
	char *b = NULL;
	uint32_t sid = 0;
	int starts = 0;
	uint8_t tocrawl[POWER_LARGEST];
	
	/* FIXME: I added this while debugging. Don't think it's needed? */
	memset(tocrawl, 0, sizeof(uint8_t) * POWER_LARGEST);
	if (strcmp(slabs, "all") == 0) {  // 判断是否所有LRU链表都需要检查
		for (sid = 0; sid < POWER_LARGEST; sid++) {
			tocrawl[sid] = 1;
		}
	} else {				// 解析字符串，判断哪些LRU需要处理
		for (char *p = strtok_r(slabs, ",", &b);
				p != NULL;
				p = strtok_r(NULL, ",", &b)) {
			if (!safe_strtoul(p, &sid) || sid < POWER_SMALLEST
					|| sid >= MAX_NUMBER_OF_SLAB_CLASSES) {
				pthread_mutex_unlock(&lru_crawler_lock);
				return CRAWLER_BADCLASS;
			}
			tocrawl[sid | TEMP_LRU] = 1;
			tocrawl[sid | HOT_LRU] = 1;
			tocrawl[sid | WARM_LRU] = 1;
			tocrawl[sid | COLD_LRU] = 1;
		}
	}
	
	// 添加伪ITEM，运行crawler线程
	starts = lru_crawler_start(tocrawl, remaining, type, NULL, c, sfd);
	if (starts == -1) {
		return CRAWLER_RUNNING;
	} else if (starts == -2) {
		return CRAWLER_ERROR;	// FIXME: not very helpful.
	} else if (starts) {
		return CRAWLER_OK;
	} else {
		return CRAWLER_NOTSTARTED;
	}
}
```

3 crawler使用的结构体
-----------------------------------------
`crawler`线程主要使用的结构体在介绍`lru_maintainer_thread`线程时已经介绍过了，在本文将再介绍一下。

```
#define LRU_CRAWLER_CAP_REMAINING -1

// 每个LRU对应的crawlerstats_t的结构体
typdef struct {
	uint64_t histo[61];		// LRU链表中未来1hour中每分钟要过期的item数量
	uint64_t ttl_hourplus;	// LRU链表中未来1hour过期的item数量
	uint64_t noexp;
	uint64_t reclaimed;
	uint64_t seen;
	rel_time_t start_time;	// LRU链表的开始时间
	rel_time_t end_time;	// LRU链表的结束时间
	bool run_complete;		// LRU链表检查是否完成
} crawlerstats_t;

struct crawler_expired_data {
	pthread_mutex_t lock;
	crawlerstats_t crawlerstats[POWER_LARGEST];  // 每个LRU链表的情况
	// redundant with crawlerstats_t so we can get overall start/stop/done
	rel_time_t start_time;		// crawler线程开始检查时间
	rel_time_t end_time;		// crawler线程结束检查时间
	bool crawl_complete;		// crawler线程扫描是否完成
	bool is_external;			// 此结构体是crawler线程申请还是外界申请
};

// crawler线程的返回值类型
enum crawler_result_type {
	CRAWLER_OK=0, CRAWLER_RUNNING, CRAWLER_BADCLASS, CRAWLER_NOTSTARTED, CRAWLER_ERROR
};

#define LARGEST_ID POWER_LARGEST

typedef struct {
	void *c;	// original connection structure. still with source thread attached.
	int sfd;	// client fd
	bipbuf_t *buf; // output buffer
	char *cbuf;		// current buffer
} crawler_client_t;  // 客户端调用crawler线程行为时使用

typedef struct _crawler_module_t crawler_module_t;

// crawler扫描过程中用的函数指针定义
typedef void (*crawler_eval_func)(crawler_module_t *cm, item *it, uint32_t hv, int slab_cls);	// 判断LUR链表中item是否过期的函数
typedef int (*crawler_init_func)(crawler_module_t *cm, void *data); // 初始化函数
typedef void (*crawler_deinit_func)(crawler_module_t *cm); // TODO: extra args?
typedef void (*crawler_doneclass_func)(crawler_module_t *cm, int slab_cls); // LRU链表完成时调用
typedef void (*crawler_finalize_func)(crawler_module_t *cm); // crawler线程完成时调用

// crawler线程使用的函数指针的结构体
typedef struct {
	crawler_init_func init;		// run before crawl starts
	crawler_eval_func eval;		// runs on an item
	crawler_doneclass_func doneclass;		// runs once per sub-crawler completion.
	crawler_finalize_func finalize;	// runs once when all sub-crawlers are done
	bool needs_lock;	// whether or not we need the LRU lock held when eval is called
	bool needs_client;	// whether or not to grab onto the remote client
} crawler_module_reg_t;

// crawler线程使用的结构体
struct _crawler_module_t {
	void *data;		// opaque data pointer
	crawler_client_t c;
	crawler_module_reg_t *mod;
};
```

4 crawler源代码解析
-----------------------------------------
5 crawler的工作方式以及其对应的函数
-----------------------------------------
6 总结
-----------------------------------------