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

// crawler线程以CRAWLER_AUTOEXPIRE/CRAWLER_EXPIRED方式进行工作使用的结构体
crawler_module_reg_t crawler_expired_mod = {
	.init = crawler_expired_init,			// 初始化函数
	.eval = crawler_expired_eval,			// 判断item是否过期函数
	.doneclass = crawler_expired_doneclass, // LRU链表完成时调用的函数
	.finalize = crawler_expired_finalize,   // crawler线程所描完所有LRU链表结束时调用
	.needs_lock = true,			// 是否使用锁
	.needs_client = false		// 是否使用客户端
};

// crawler线程以CRAWLER_METADUMP方式进行工作使用的结构体
crawler_module_reg_t crawler_metadump_mod = {
	.init = NULL,
	.eval = crawler_metadump_eval,
	.doneclass = NULL,
	.finalize = crawler_metadump_finalize,
	.needs_lock = false,
	.needs_client = true
};

// 所有工作方式使用的函数指针
crawler_module_reg_t *crawler_mod_regs[3] = {
	&crawler_expired_mod,		//	CRAWLER_AUTOEXPIRE工作方式
	&crawler_expired_mod,		// CRAWLER_EXPIRED工作方式
	&crawler_metadump_mod		// CRAWLER_METADUMP工作方式
};

// 全局变量
crawler_module_t active_crawler_mod;	// crawler线程使用的函数方式
enum crawler_run_type active_crawler_type; // crawler线程的工作方式

static crawler crawlers[LARGEST_ID];	 // crawler线程使用的伪ITEM存放的数组

static int crawler_count = 0; // crawler需要扫描的LRU链表的个数
static volatile int do_run_lru_crawler_thread = 0; // 判断线程是否在运行
static int lru_crawler_initialized = 0; // crawler线程是否初始化使用的变量
static pthread_mutex_t lru_crawler_lock = PTHEAD_MUTEX_INITIALIZER; // 锁
static pthread_cond_t lru_crawler_cond = PTHREAD_COND_INITIALIZER; // 变量
```

4 crawler源代码解析
-----------------------------------------
前面介绍了`cralwer`线程的各个方面，包括线程的启动\关闭、其被调用的方式、使用的全局变量等等信息。下面将着重介绍`cralwer`线程本身，将介绍其工作方式以及整个函数的工作流程。

```
static void *item_crawler_thread(void *arg) {
	int i;
	int crawls_persleep = settings.crawls_persleep;
	
	pthread_mutex_lock(&lru_crawler_lock);
	pthread_cond_signal(&lru_crawler_cond);
	settings.lru_crawler = true;
	if (settings.verbose > 2)
		fprintf(stderr, "Starting LRU crawler background thread\n");
	while (do_run_lru_crawler_thread) {	 // 进入crawler线程工作循环中
	pthread_cond_wait(&lru_crawler_cond, &lru_crawler_lock); // 等待条件变量运行
	
	while (crawler_count) {    // 根据需要扫描的LRU链表个数来循环，当所有需要扫描的LRU链表扫描完成后，退出循环
		item *search = NULL;	 // 用于记录需要判断的item
		void *hold_lock = NULL;	  // item对应的hash锁
		
		for (i = POWER_SMALLEST; i < LARGEST_ID; i++) {  // 逐一扫描LRU链表
			if (crawlers[i].it_flags != 1) { // 根据伪ITEM来判断此LRU链表是否需要扫描
				continue;
			}
			
			// Get memory from bipbuf, if client has no space, flush
			// crawler线程以CRAWLER_METADUMP方式工作时需要考虑client客户端
			if (active_crawler_mod.c.c != NULL) {
				// 判断客户端的发送信息失败时将LRU链表扫描结束
				int ret = lru_crawler_client_getbuf(&active_crawler_mod.c);
				if (ret != 0) {
					lru_crawler_class_done(i);
					continue;
				}
			} else if (active_crawler_mod.mod->needs_client) {
				// 如果client关闭了，则此LRU链表扫描结束
				lru_crawler_class_done(i);
				continue;
			}
			pthread_mutex_lock(&lru_locks[i]);
			// 移动伪ITEM,返回伪ITEM后的item用于判断
			search = do_item_crawl_q((item *)&crawlers[i]);
			// 判断此LRU链表是否扫描结束
			if (search == NULL ||
					(crawlers[i].remaining && --crawlers[i].remaining < 1) {
				if (settings.verbose > 2)
					fprintf(stderr, "Nothing left to crawl fro %d\n", i);
				lru_crawler_class_done(i);
				continue;
			}
			uint32_t hv = hash(ITEM_key(search), search->nkey);
			/* Attempt to hash item lock the 'search' item. If locked, no
			 * other callers can incr the refcount
			 */
			 // 获取此item对应的hash锁
			 if ((hold_lock = item_trylock(hv)) == NULL) {
			 	pthread_mutex_unlock(&lru_locks[i]);
			 	continue;
			 }
			 // Now see if the item is refcount locked
			 // 判断此item是否被其它线程使用中
			 if (refcount_incr(search) != 2) {
			 	refcount_decr(search);
			 	if (hold_lock)
			 		item_trylock_unlock(hold_lock);
			 	pthread_mutex_unlock(&lru_locks[i]);
			 	continue;
			 }
			 
			 crawlers[i].checked++;  // 通过伪ITEM的checked字段来记录扫描的item个数
			 /* Frees the item or decrements the refcount. */
			 /* Interface for this could improve: do the free/decr here
			  * instead? */
			 // 判断item是否过期时，是否需要加锁
			 if (!active_crawler_mod.mod->needs_lock) {
			 	pthread_mutex_unlock(&lru_locks[i]);
			 }
			 // 判断item是否满足过期条件
			 active_crawler_mod.mod->eval(&active_crawler_mod, search, hv, i);
			 
			 // 释放锁
			 if (hold_lock)
			 	item_trylock_unlock(hold_lock);
			 if (active_crawler_mod.mod->needs_lock) {
			 	pthread_mutex_unlock(&lru_locks[i]);
			 }
			 // 判断crawler线程是否需要usleep
			 if (crawls_persleep-- <= 0 && settings.lru_crawler_sleep) {
			 	pthread_mutex_unlock(&lru_crawler_lock);
			 	usleep(settings.lru_crawler_sleep);
			 	pthread_mutex_lock(&lru_crawler_lock);
			 	crawls_persleep = settings.crawls_persleep;
			 } else if (!settings.lru_crawler_sleep) {
			 	// TODO: only cycle lock every N？
			 	pthread_mutex_unlock(&lru_crawler_lock);
			 	pthread_mutex_lock(&lru_crawler_lock);
			 }
		}
	} // while (crawler_count) 所有需要扫描的LRU链表扫描结束
	// 清空crawler线程所使用的active_crawler_mod全局变量
	if (active_crawler_mod.mod != NULL) {
		// crawler线程扫描完所有LRU链表时使用finalize函数
		if (active_crawler_mod.mod->finalize != NULL)
			active_crawler_mod.mod->finalize(&active_crawler_mod);
		// 判断client是否需要清空结束
		while (active_crawler_mod.c.c != NULL && bipbuf_used(active_crawler_mod.c.buf)) {
			lru_crawler_poll(&active_crawler_mod.c);
		}
		// Double checking in case the client closed during the poll
		if (active_crawler_mod.c.c != NULL) {
			lru_crawler_release_client(&active_crawler_mod.c);
		}
		active_crawler_mod.mod = NULL;
	}
	
	if (settings.verbose > 2)
		fprintf(stderr, "LRU crawler thread sleeping\n");
	
	STATS_LOCK();
	stats_state.lru_crawler_running = false; // crawler线程本次扫描LRU链表结束
	STATS_UNLOCK();
	} // while (do_run_lru_crawler_thread)
	pthread_mutex_unlock(&lru_crawler_lock);
	if (settings.verbose > 2)
		fprintf(stderr, "LRU crawler thread stopping\n");
		
	return NULL;
}
```
从上面的代码可以看出当`crawler`线程以`CRAWLER_AUTOEXPIRE/CRAWLER_EXPIRED`方式工作时，不需要使用`client`，此时代码较为简单:仅仅从`LRU`链表尾部往前逐一移动伪`ITEM`，然后返回伪`ITEM`后面的`item`，当伪`ITEM`移动到`LRU`链表头部时扫描结束。

### `crawler`函数流程图示

![crawler线程流程图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_lru/crawler线程流程图示.png)

### `crawler`中伪`ITEM`移动流程图示

![crawler线程ITEM移动流程图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_lru/crawler线程中执行图示.png)

### 线程中调用的函数
在`crawler`扫描线程中使用了大量函数，本小节将介绍一下其使用的函数。

```
// lru_crawler_client_getbuf函数,获取client的buf，如果没有空间，则清空原来的空间。
static int lru_crawler_client_getbuf(crawler_client_t *c) {
	void *buf = NULL;
	if (c->c == NULL) return -1;
	/* not enough space. */
	while ((buf = bipbuf_request(c->buf, LRU_CRAWLER_WRITEBUF)) == NULL) {
		// TODO: max loops before closing
		int ret = lru_crawler_poll(c);  // 清空client的buf
		if (ret < 0) return ret;
	}
	
	c->cbuf = buf;
	return 0;
}

// 当LRU链表被扫描万时调用
static void lru_crawler_class_done(int i) {
	crawlers[i].it_flags = 0;		// 将伪ITEM的it_flags置为0,用于标示此LRU链表不需要LRU链表扫描
	crawler_count--;	// 减少需要扫描的LRU链表的个数
	do_item_unlinktail_q((item *)&crawlers[i]); // 将伪ITEM从LRU链表中去除
	// 将LRU扫描的结果保存到itemstats中LRU链表对应的位置中
	do_item_stats_add_crawl(i, crawlers[i].reclaimed, crawlers[i].unfetched, crawlers[i].checked);
	pthread_mutex_unlock(&lru_locks[i]);
	if (active_crawler_mod.mod->doneclass != NULL)
		active_crawler_mod.mod->doneclass(&active_crawler_mod, i);
}

// 将伪ITEM在LRU链表中从后往前逐步移动
/* This is too convoluted, but it's a difficult shuffle. Try to rewrite it
 * more clearly.
 */
item *do_item_crawl_q(item *it) {
	item **head, **tail;
	assert(it->it_flags == 1);
	assert(it->nbytes == 0);
	head = &heads[it->slabs_clsid];
	tail = &tails[it->slabs_clsid];
	
	/* We've hit the head, pop off */
	if (it->prev == 0) {
		assert(*head == it);
		if (it->next) {
			*head = it->next;
			assert(it->next->prev == it);
			it->next->prev = 0;
		}
		return NULL; // Done
	}
	
	/* Swing ourselves in front of the next item */
	/* NB: If there is a prev, we can't be the head */
	assert(it->prev != it);
	if (it->prev) {
		if (*head == it->prev) {
			/* Prev was the head, now we're the head */
			*head = it;
		}
		if (*tail == it) {
			/* We are the tail, now they are the tail */
			*tail = it->prev;
		}
		assert(it->next != it);
		if (it->next) {
			assert(it->prev->next == it);
			it->prev->next = it->next;
			it->next->prev = it->prev;
		} else {
			/* Tail. Move this above? */
			it->prev->next = 0;
		}
		/* prev->prev's next is it->prev */
		it->next = it->prev;
		it->prev = it->next->prev;
		it->next->prev = it;
		/* New it->prev now, if we're not at the head. */
		if (it->prev) {
			it->prev->next = it;
		}
	}
	assert(it->next != it);
	assert(it->prev != it);
	
	return it->next; /* success */
}

// 判断client中buf是否存在空闲空间
int bipbuf_used(const bipbuf_t *me) {
	return (me->a_end - me->a_start) + me->b_end;
}

// crawler线程中的poll函数
static int lru_crawler_poll(crawler_client_t *c) {
	unsigned char *data;
	unsigned int data_size = 0;
	struct pollfd to_poll[1];
	to_poll[0].fd = c->sfd;
	to_poll[0].events = POLLOUT;
	
	int ret = poll(to_poll, 1, 1000);
	
	if (ret < 0) {
		// fatal.
		return -1;
	}
	
	if (ret == 0) return 0;
	
	if (to_poll[0].revents & POLLIN) {
		char buf[1];
		int res = read(c->sfd, buf, 1);
		if (res == 0 || (res == -1 && (errno != EAGAIN && errno != EWOULDBLOCK))) {
			lru_crawler_close_client(c);
			return -1;
		}
	}
	if ((data = bipbuf_peek_all(c->buf, &data_size)) != NULL) {
		if (to_poll[0].revents & (POLLHUP|POLLERR)) {
			lru_crawler_close_client(c);
			return -1;
		} else if (to_poll[0].revents & POLLOUT) {
			int total = write(c->sfd, data, data_size);
			if (total == -1) {
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					lru_crawler_close_client(c);
					return -1;
				}
			} else if (total == 0) {
				lru_crawler_close_client(c);
				return -1;
			} else {
				bipbuf_poll(c->buf, total);
			}
		}
	}
	return 0;
}

// 关闭链接和客户端
static void lru_crawler_release_client(crawler_client_t *c) {
	// 释放链接
	redispatch_conn(c->c);
	c->c = NULL;
	c->cbuf = NULL;
	bipbuf_free(c->buf);
	c->buf = NULL;
}
```

5 crawler的工作方式以及其对应的函数
-----------------------------------------
前面介绍了`crawler`的工作流程以及工作函数，本小节将介绍`crawler`线程的工作方式以及其对应的使用的函数。

`crawler`线程工作方式有以下三种:

```
// TODO: If we eventually want user loaded modules, we can't use an enum
enum crawler_run_type {
	CRAWLER_AUTOEXPIRE=0,
	CRAWLER_EXPIRED,
	CRAWLER_METADUMP
};

// CRAWLER_AUTOEXPIRE/CRAWLER_EXPIRED工作方式时，使用的函数指针
crawler_module_reg_t crawler_expired_mod = {
	.init = crawler_expired_init,
	.eval = crawler_expired_eval,
	.doneclass = crawler_expired_doneclass,
	.finalize = crawler_expired_finalize,
	.needs_lock = true,
	.needs_client = false
};

// CRAWLER_METADUMP工作方式时，使用的函数指针
crawler_module_reg_t crawler_metadump_mod = {
	.init = NULL,
	.eval = crawler_metadump_eval,
	.doneclass = NULL,
	.finalize = crawler_metadump_finalize,
	.needs_lock = false,
	.needs_client = true
};
```
`crawler`线程以`CRAWLER_AUTOEXPIRE/CRAWLER_EXPIRED`工作方式时，使用的函数指针如下所示:

```
// 初始化函数
static int crawler_expired_init(crawler_module_t *cm, void *data) {
	struct crawler_expired_data *d;
	if (data != NULL) {
		d = data;
		d->is_external = true;
		cm->data = data;
	} else {
		// allocate data
		d = calloc(1, sizeof(struct crawler_expired_data));
		if (d == NULL) {
			return -1;
		}
		// init lock.
		pthread_mutex_init(&d->lock, NULL);
		d->is_external = false;
		d->start_time = current_time;
		
		cm->data = d;
	}
	pthread_mutex_lock(&d->lock);
	memset(&d->crawlerstats, 0, sizeof(crawlerstats_t) * POWER_LARGEST);
	for (int x = 0; x < POWER_LARGEST; x++) {
		d->crawlerstats[x].start_time = current_time;
		d->crawlerstats[x].run_complete = false;
	}
	pthread_mutex_unlock(&d->lock);
	return 0;
}

// 判断item函数
/* I pulled this out to make the main thread clearer, but it reaches into the
 * main thread's values too much. Should rethink again.
 */
static void crawler_expired_eval(crawler_module_t *cm, item *search, uint32_t hv, int i) {
	struct crawler_expired_data *d = (struct crawler_expired_data *)cm->data;
	pthread_mutex_lock(&d->lock);
	crawlerstats_t *s = d->crawlerstats[i]; // 获取LRU链表对应的crawlerstats_t
	int is_flushed = item_is_flushed(search); // 判断是否接受过客户端flushed命令
#ifdef EXTSTORE
	bool is_valid = true;
	if (search->it_flags & ITEM_HDR) {
		item_hdr *hdr = (item_hdr *)ITEM_data(search);
		if (extstore_check(storage, hdr->page_id, hdr->page_version) != 0)
			is_valid = false;
	}
#endif
	// 判断item是否过期
	if ((search->exptime != 0 && search->exptime < current_time)
			|| is_flushed
#ifdef EXTSTORE
			|| is_valid
#endif
	) {
		crawlers[i].reclaimed++;	// 记录item过期的个数
		s->reclaimed++;
		
		if (setting.verbose > 1) {
			int ii;
			char *key = ITEM_key(search);
			fprintf(stderr, "LRU crawler found on expired item (flags: %d, slab: %d): ", search->it_flags, search->slabs_clsid);
			for (ii = 0; ii < search->nkey; ++ii) {
				fprintf(stderr, "%c", key[ii]);
			}
			fprintf(stderr, "\n");
		}
		if ((search->it_flags & ITEM_FETCHED) == 0 && !is_flushed) {
			crawlers[i].unfetched++; // 记录item在被移除之前未被访问过的个数
		}
#ifdef EXTSTORE
		STORAGE_delete(storage, search);
#endif
		do_item_unlink_nolock(search, hv); // item从链接中去除
		do_item_remove(search); // 释放item
		assert(search->slabs_clsid == 0);
	} else {
		// item未过期
		s->seen++;	//	item中未过期的item个数
		refcount_decr(search);
		if (search->exptime == 0) {
			s->noexp++;		// item永不过期的个数
		} else if (search->exptime - current_time > 3599) {
			s->ttl_hourplus++;		// item的超时时间超过1hour的个数
		} else {
			// 判断item在未来一小时中那一分钟过期
			rel_time_t ttl_remain = search->exptime - current_time;
			// 将item放入对应的过期桶中
			int bucket = ttl_remain / 60;	// 判断那一分钟过期
			if (bucket <= 60) {
				s->histo[bucket]++;  // 记录每分钟过期的item个数
			}
		}
	}
	pthread_mutex_unlock(&d->lock);
}
```

6 总结
-----------------------------------------