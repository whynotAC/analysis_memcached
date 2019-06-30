# `LRU`维护线程代码解析
本文将从如下几个部分对`LRU`维护线程的源代码进行解析:

1. `LRU`维护线程的源代码。
2. `LRU`维护线程源代码中维护`LRU`链表长度的源代码。
3. `LRU`维护线程源代码中维护`worker`线程中`lru_bump_buf`结构的源代码。
4. `LRU`维护线程源代码中调用`LRU`扫描线程的源代码。
5. `LRU`维护线程源代码中调用`slab_rebalance_thread`线程的源代码。

## `LRU`维护线程的源代码
其源代码结构如下:

```
static void *lru_maintainer_thread(void *arg) {
	slab_automove_reg_t *sam = &slab_automove_default; // 为调用`slab_rebalance_thread`做准备
#ifdef EXTSTORE				//	有关EXTSTORE的章节后面会单独写文档
	void *storage = arg;
	if (storage != NULL)
		sam = &slab_automove_extstore;
	int x;
#endif
	int i;
	useconds_t to_sleep = MIN_LRU_MAITAINER_SLEEP;
	useconds_t last_sleep = MIN_LRU_MAITAINER_SLEEP;
	rel_time_t last_crawler_check = 0; // 上一次调用lru扫描线程的时间
	rel_time_t last_automove_check = 0; // 上一次调用`slab`维护线程的时间
	// lru队列长度调整的时间记录
	useconds_t next_juggles[MAX_NUMBER_OF_SLAB_CLASSES] = {0};
	useconds_t backoff_juggles[MAX_NUMBER_OF_SLAB_CLASSES] = {0};
	// lru扫描线程使用的结构体
	struct crawler_expired_data *cdata = calloc(1, sizeof(struct crawler_expired_data));
	if (cdata == NULL) {
		fprintf(stderr, "Failed to allocate crawler data for LRU maintainer thread\n");
		abort();
	}
	pthread_mutex_init(&cdata->lock, NULL);
	cdata->crawl_complete = true; // kick off the crawler
	logger *l = logger_create();
	if (l == NULL) {
		fprintf(stderr, "Failed to allocate logger for LRU maitainer thread\n");
		abort();
	}
	
	double last_ratio = settings.slab_automove_ratio;
	void *am = sam->init(&settings);
	
	pthread_mutex_lock(&lru_maintainer_lock);
	if (settings.verbose > 2)
		fprintf(stderr, "Starting LRU maintainer background thread\n");
	// 开启LRU维护线程的循环
	while (do_run_lru_maintainer_thread) {
		pthread_mutex_unlock(&lru_maintainer_lock);
		if (to_sleep)
			usleep(to_sleep);
		pthread_mutex_lock(&lru_maintainer_lock);
		// A sleep of zero counts as a minimum of a 1ms wait
		last_sleep = to_sleep > 1000 ? to_sleep : 1000;
		to_sleep = MAX_LRU_MAINTAINER_SLEEP;
		
		STATS_LOCK();
		stats.lru_maintainer_juggles++;
		STATS_UNLOCK();
		
		// 1.调整lru队列长度的代码开启
		/* Each slab class gets its own sleep to avoid hammering locks */
		// 每个slab都拥有自己的检测时间
		for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
			next_juggles[i] = next_juggles[i] > last_sleep ? next_juggles[i] - last_sleep : 0;
			
			if (next_juggles[i] > 0) {
				// Sleep the thread just for the minimum amount (or not at all)
				if (next_juggles[i] < to_sleep)
					to_sleep = next_juggles[i];
				continue;
			}
			
			// 调整lru队列长度的主要函数
			int did_moves = lru_maitainer_juggle(i);
#ifdef EXTSTORE
			// Deeper loop to speed up pushing to storage
			if (storage) {
				for (x = 0; x < 500; x++) {
					int found;
					found = lru_maintainer_store(storage, i);
					if (found) {
						did_moves += found;
					} else {
						break;
					}
				}
			}
#endif
			if (did_moves == 0) {
				if (backoff_juggles[i] != 0) {
					backoff_juggles[i] += backoff_juggles[i] / 8;
				} else {
					backoff_juggles[i] = MIN_LRU_MAITAINER_SLEEP;
				}
				if (backoff_juggles[i] > MAX_LRU_MAINTAINER_SLEEP)
					backoff_juggles[i] = MAX_LRU_MAINTAINER_SLEEP;
			} else if (backoff_juggles[i] > 0) {
				backoff_juggles[i] /= 2;
				if (backoff_juggles[i] < MIN_LRU_MAINTAINER_SLEEP) {
					backoff_juggles[i] = 0;
				}
			}
			next_juggles[i] = backoff_juggles[i];
			// 将to_sleep设置成最近需要调整LRU的时间
			if (next_juggles[i] < to_sleep)
				to_sleep = next_juggles[i];
		}
		// 1.调整LRU队列长度的代码结束
		
		// 2.维护`worker`线程中`lru_bump_buf`结构的源代码开始
		if (settings.lru_segmented && lru_maintainer_bumps() && to_sleep > 1000) {
			to_sleep = 1000;
		}
		// 2.维护`worker`线程中`lru_bump_buf`结构的源代码结束
		
		// 3.调用`LRU`扫描线程的源代码开始
		// Once per second at most
		if (settings.lru_crawler && last_crawler_check != current_time) {
			lru_maitainer_crawler_check(cdata, l);
			last_crawler_check = current_time;
		}
		// 3.调用`LRU`扫描线程的源代码结束
		
		// 4.调用`slab_rebalance_thread`线程的源代码开始
		if (settings.slab_automove == 1 && last_automove_check != current_time) {
			if (last_ratio != settings.slab_automove_ratio) {
				sam->free(am);
				am = sam->init(&settings);
				last_ratio = settings.slab_automove_ratio;
			}
			int src, dst;
			sam->run(am, &src, &dst);
			if (src != -1 && dst != -1) {
				slabs_reassign(src, dst);
				LOGGER_LOG(l, LOG_SYSEVENTS, LOGGER_SLAB_MOVE, NULL, src, dst);
			}
			// dst == 0 means reclaim to global pool, be more aggressive
			if (dst != 0) {
				last_automove_check = current_time;
			} else if (dst == 0) {
				// also ensure we minimize the thread sleep
				to_sleep = 1000;
			}
		}
		// 4.调用`slab_rebalance_thread`线程的源代码结束
	}
	pthread_mutex_unlock(&lru_maintainer_lock);
	sam->free(am);
	// LRU crawler *must* be stopped
	free(cdata);
	if (settings.verbose > 2)
		fprintf(stderr, "LRU maintainer thread stopping\n");
	
	return NULL;
}
```

## `LRU`维护线程中维护`LRU`链表长度的源代码

其函数调用链为: `lru_maintainer_thread` --> `lru_maintainer_juggle` --> `lru_pull_tail`。从而完成对`LRU`链表的长度维护。

其代码如下:

```
lru_maintainer_thread函数中的代码:
// Each slab class gets its own sleep to avoid hammering locks
for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
	// 根据上次调整时间来判断本次slabid对应的LRU是否需要调整
	next_juggles[i] = next_juggles[i] > last_sleep ? next_juggles[i] - last_sleep : 0;
	
	// 判断slabid对应的LRU链表是否需要调整
	if (next_juggles[i] > 0) {
		// Sleep the thread just for the minimum amout (or not at all)
		if (next_juggles[i] < to_sleep)
			to_sleep = next_juggles[i];
		continue;
	}
	
	// 对slabid对应的LRU链表进行调整
	int did_moves = lru_maintainer_juggle(i);
#ifdef EXTSTORE
	// Deeper loop to speed up pushing to storage
	if (storage) {
		for (x = 0; x < 500; x++) {
			int found;
			found = lru_maintainer_store(storage, i);
			if (found) {
				did_moves += found;
			} else {
				break;
			}
		}
	}
#endif
	// 判断slabid对应的LRU链表是否移动了item
	// 如果某个slabid对应的LRU链表没有移动item，会等待时间越来越长。
	// 如果某个slabid对应的LRU链表一直移动item，其一直不会等待，每次都要尝试移动item。
	if (did_moves == 0) {
		// 没有item移动
		if (backoff_juggles[i] != 0) {
			backoff_juggles[i] += backoff_juggles[i] / 8;
		} else {
			backoff_juggles[i] = MIN_LRU_MAINTAINER_SLEEP;
		}
		if (backoff_juggles[i] > MAX_LRU_MAINTAINER_SLEEP)
			backoff_juggles[i] = MAX_LRU_MAINTAINER_SLEEP;
	} else if (backoff_juggles[i] > 0) {
		backoff_juggles[i] /= 2;
		if (backoff_juggles[i] < MIN_LRU_MAINTAINER_SLEEP) {
			backoff_juggles[i] = 0;
		}
	}
	next_juggles[i] = backoff_juggles[i];
	// to_sleep是求出最近需要调整的slabid对应LRU的时间
	if (next_juggles[i] < to_sleep)
		to_sleep = next_juggles[i];
}

/* Loop up to N times:
 * If too many items are in HOT_LRU, push to COLD_LRU
 * If too many items are in WARM_LRU, push to COLD_LRU
 * If too many items are in COLD_LRU, poke COLD_LRU tail
 * 1000 loops with 1ms min sleep gives us under 1m items shifted/sec. The
 * locks can't handle much more than that. Leaving a TODO for how to
 * autoadjust in the future.
 */
static int lru_maintainer_juggle(const int slabs_clsid) {
	int i;
	int did_moves = 0;
	uint64_t total_bytes = 0;
	unsigned int chunks_perslab = 0;
	// TODO: if free_chunks below high watermark, increase aggressiveness
	// 获取slabs_clsid对应消耗的总字节数和每个slab/chunk分配的item个数
	slabs_available_chunks(slabs_clsid, NULL, &total_bytes, &chunks_perslab);
	// 处理TEMP LRU队列
	if (settings.temp_lru) {
		// only looking for reclaims. Run before we size the LRU.
		for (i = 0; i < 500; i++) {
			if (lru_pull_tail(slabs_clsid, TEMP_LRU, 0, 0, 0, NULL) <= 0) {
				break;
			} else {
				did_moves++;
			}
		}
		// slabs_clsid对应的字节总数减去TEMP队列中的item的字节数
		total_bytes -= temp_lru_size(slabs_clsid);
	}
	
	// 设置HOT\WARM\COLD尾部item的超时时间
	rel_time_t cold_age = 0;
	rel_time_t hot_age = 0;
	rel_time_t warm_age = 0;
	// if LRU is in flat mode, force items to drain into COLD via max age
	if (settings.lru_segmented) {
		pthread_mutex_lock(&lru_locks[slabs_clsid|COLD_LRU]);
		if (tails[slabs_clsid|COLD_LRU]) {
			// 获取COLD队列中最后一个item的超期时间
			cold_age = current_time - tails[slabs_clsid|COLD_LRU]->time;
		}
		pthread_mutex_unlock(&lru_locks[slabs_clsid|COLD_LRU]);
		// 根据cold_age的超时时间来设置hot_age和warm_age的超时时间
		hot_age = cold_age * settings.hot_max_factor;
		warm_age = cold_age * settings.warm_max_factor;
	}
	
	// juggle HOT/WARM up to N times
	// 处理500次HOT/WARM/COLD队列的尾部item
	for (i = 0; i < 500; i++) {
		int do_more = 0;
		if (lru_pull_tail(slabs_clsid, HOT_LRU, total_bytes, LRU_PULL_CRAWL_BLOCKS, hot_age, NULL) || 
			lru_pull_tail(slabs_clsid, WARM_LRU, total_bytes, LRU_PULL_CRAWL_BLOCKS, warm_age, NULL)) {
			do_more++;
		}
		if (settings.lru_segmented) {
			do_more += lru_pull_tail(slabs_clsid, COLD_LRU, total_bytes, LRU_PULL_CRAWL_BLOCKS, 0, NULL);
		}
		if (do_more == 0)
			break;
		did_moves++;
	}
	return did_moves;
} 

/* Returns number of items reomove, expired, or evicted.
 * Callable from worker threads or the LRU maintainer thread.
 */
 int lru_pull_tail(const int orig_id, const int cur_lru, const uint64_t total_bytes,
 						const uint8_t flags, const rel_time_t max_age, struct lru_pull_tail_return *ret_it) {
 	item *it = NULL;
 	int id = orig_id;
 	int removed = 0;
 	if (id == 0)
 		return 0;
 	
 	int tries = 5;
 	item *search;
 	item *next_it;
 	void *hold_lock = NULL;				// item对应的hash锁
 	unsigned int move_to_lru = 0;		// item将要转移的LRU队列
 	uint64_t limit = 0;
 	
 	id |= cur_lru;						// 找到slab_id对应的LRU队列ID
 	pthread_mutex_lock(&lru_locks[id]); // 锁上对应的LRU队列锁
 	search = tails[id];					// 获取LRU队列的最后一个item
 	// we walk up *only* for locked items, and if bottom is expired.
 	for (; tires > 0 && search != NULL; tries--, search=next_it) {
 		// we might relink search mid-loop, so search->prev isn't reliable
 		next_it = search->prev;
 		// 判断此item是否为LRU扫描线程添加的伪item(其特征为item的it_flags为1)
 		if (search->nbytes == 0 && search->nkey == 0 && search->it_flags == 1) {
 			// We are a crawler, ignore it
 			// 如果此LRU队列被LRU扫描线程扫描过程中，将不再对LRU队列进行其他处理。
 			if (flags & LRU_PULL_CRAWL_BLOCKS) {
 				pthread_mutex_unlock(&lru_locks[id]);
 				return 0;
 			}
 			tries++;
 			continue;
 		}
 		uint32_t hv = hash(ITEM_key(search), search->nkey);
 		/* Attempt to hash item lock the "search" item. If locked, no
 		 * other callers can incr the refcount. Also skip ourselves. */
 		// 尝试获取此item的hash锁
 		if ((hold_lock = item_trylock(hv)) == NULL)
 			continue;
 		
 		// 判断此item是否被其他worker线程使用
 		if (refcount_incr(search) != 2) {
 			/* Note pathological case with ref'ed items in tail.
 			 * Can still unlink the item, but it won't be reuseable yet */
 			itemstats[id].lrutail_reflocked++;
 			/* In case of refcount leaks, enable for quick workaround. */
 			if (settings.tail_repair_time && 
 					search->time + settings.tail_repair_time < current_time) {
 				// 如果item长时间没有被访问也没有被释放，则强行释放其item
 				itemstats[id].tailrepairs++;
 				search->refcount = 1;
 				// This will call item_remove -> item_free since refcnt is 1
 				STORAGE_delete(ext_storage, search);
 				do_item_unlink_nolock(search, hv);
 				item_trylock_unlock(hold_lock);
 				continue;
 			}
 		}
 		
 		// item过期或者被flushed出去。
 		if ((search->exptime != 0 && search->exptime < current_time) 
 				|| item_is_flushed(search)) {
 			itemstats[id].reclaimed++;
 			if ((search->it_flags & ITEM_FETCHED) == 0) {
 				itemstats[id].expired_unfetched++;
 			}
 			// 将item从LRU和hash链中释放
 			/* refcnt 2 -> 1*/
 			do_item_unlink_nolock(search, hv);
 			STORAGE_delete(ext_storage, search);
 			/* refcnt 1 -> 0 -> item_free */
 			do_item_remove(search);
 			item_trylock_unlock(hold_lock);
 			removed++;
 			
 			/* If all we're finding are expired, can keep going */
 			continue;
 		}
 		
 		// 当item处于HOT\WARM队列时，如果队列长度超过字节数限制，将它们转移到COLD队列中。
 		// 当item处于COLD_LRU队列时，除非item处于active状态(会被转移到WARM队列)，其它情况将被LRU过期扫描线线程慢慢回收或者被分配出去。
 		switch (cur_lru) {
 			case HOT_LRU:
 				limit = total_bytes * settings.hot_lru_pct / 100; // HOT队列的总字节数限制
 			case WARM_LRU:
 				if (limit == 0)
 					limit = total_bytes * settings.warm_lru_pct / 100; // WARM队列的总字节数限制
 				if ((search->it_flags & ITEM_ACTIVE) != 0) {
 					// 判断item是否处于active状态
 					search->it_flags &= ~ITEM_ACTIVE; // 去除item的active状态
 					removed++;
 					// 根据item所处的队列来进行处理
 					if (cur_lru == WARM_LRU) {
 						// 将WARM队列中active状态的item转移到WARM队列头部
 						itemstats[id].moves_within_lru++;
 						do_item_update_nolock(search);
 						do_item_remove(search);
 						item_trylock_unlock(hold_lock);
 					} else {
 						// HOT队列中active状态的item转移到WARM队列中
 						itemstats[id].moves_to_warm++;
 						move_to_lru = WARM_LRU;  // 记录item将要转移到的队列
 						do_item_unlink_q(search);
 						it = search;
 					}
 				} else if (sizes_bytes[id] > limit || 
 								current_time - search->time > max_age) {
 					// 如果HOT/WARM队列中，非active状态的item转移到COLD队列中
 					itemstats[id].moves_to_cold++;
 					move_to_lru = COLD_LRU;	   // 记录item将要转移到的队列
 					do_item_unlink_q(search);
 					it = search;
 					removed++;
 					break;
 				} else {
 					// 不需要对item进行处理时走的分支
 					// Don't want to move to COLD, not active, bail out.
 					it = search;
 				}
 				break;
 			case COLD_LRU:
 				it = search;
 				// 如果调用lru_pull_tail函数时，flags是LRU_PULL_EVCIT时调用
 				if (flags & LRU_PULL_EVICT) {
 					if (settings.evict_to_free == 0) {
 						// Don't think we need a counter for this. It'll OOM.
 						break;
 					}
 					itemstats[id].evicted++;
 					itemstats[id].evicted_time = current_time - search->time;
 					if (search->exptime != 0)
 						itemstats[id].evicted_nonzero++;
 					if (search->it_flags & ITEM_FETCHED) == 0) {
 						itemstats[id].evicted_unfetched++;
 					}
 					if ((search->it_flags & ITEM_ACTIVE)) {
 						itemstats[id].evicted_active++;
 					}
 					LOGGER_LOG(NULL, LOG_EVICTIONS, LOGGER_EVICTION, search);
 					STORAGE_delete(ext_storage, search);
 					do_item_unlink_nolock(search, hv);
 					removed++;
 					// 如果某个SLAB被换出的item个数较多，则需要考虑是否需要添加slab/chunk
 					if (settings.slab_automove == 2) {
 						// 调用slab rebalance线程
 						slabs_reassign(-1, orig_id);
 					}
 				} else if (flags & LRU_PULL_RETURN_ITEM) {
 					// 如果调用lru_pull_tail函数时是为了获取item时调用
 					ret_it->it = it;
 					ret_it->hv = hv;
 				} else if ((search->it_flags & ITEM_ACTIVE) != 0 && settings.lru_segmented) {
 					// 如果item处于active状态，则将item转移到WARM队列中
 					itemstats[id].moves_to_warm++;
 					search->it_flags &= ~ITEM_ACTIVE; // 除去item的active标示
 					move_to_lru = WARM_LRU; // 记录item将要转移到的队列
 					do_item_unlink_q(search);
 					removed++;
 				}
 				break;
 			case TEMP_LRU:
 				it = search; // TEMP队列中的item不会被处理
 				break;
 		}
 		if (it != NULL)	// 查找到了需要处理的item
 			break;
 	}
 	
 	pthread_mutex_unlock(&lru_locks[id]);
 	
 	// 处理item
 	if (it != NULL) {
 		// 将item转移到对应的move_to_lru队列中
 		if (move_to_lru) {
 			it->slabs_clsid = ITEM_clsid(it);
 			it->slabs_clsid |= move_to_lru;
 			item_link_q(it);
 		}
 		// 如果只是获取item时调用
 		if ((flags & LRU_PULL_RETURN_ITEM) == 0) {
 			do_item_remove(it);
 			item_trylock_unlock(hold_lock);
 		}
 	}
 	
 	return removed;
 }

```

### 使用的结构体
上述代码中使用了大量了结构体或者变量，本小节将逐一介绍:

```
lru_maintainer_thread函数中:

useconds_t next_juggles[MAX_NUMBER_OF_SLAB_CLASSES] = {0}; // 用于记录下次调整的时间
useconds_t backoff_juggles[MAX_NUMBER_OF_SLAB_CLASSES] = {0}; // 用于根据是否转移item来调整next_juggles的时间

lru_maintainer_juggle函数中:

pthread_mutex_t lru_locks[POWER_LARGEST]; // LRU分段锁的数组
// 调用lru_pull_tail的三种flag方式
#define LRU_PULL_EVICT 1 	// 用于将COLD队列中的item逐出队列
#define LRU_PULL_CRAWL_BLOCKS 2 // 用于扫描LRU队列尾部item，并进行转换队列处理
#define LRU_PULL_RETURN_ITEM 4 // 用于获取一个COLD队列中的item

lru_pull_tail函数中:

// 用于返回一个COLD队列中的item
struct lru_pull_tail_return {
	item *it;
	uint32_t hv;
};

// 记录LRU队列的头尾
static item *heads[LARGEST_ID];
static item *tails[LARGEST_ID];

typedef struct {
	uint64_t evicted;					// COLD队列中被剔除的item个数
	uint64_t evicted_nonzero;			// COLD队列中被剔除的item中exptime非0的个数(未到期的item个数)
	uint64_t reclaimed;					// 超期或者被flushed操作的item个数
	uint64_t outofmemory;				// slab无法分配item的次数
	uint64_t tailrepairs;				// repair item的个数
	uint64_t expired_unfetched;		// 超期或者被flushed操作的item中未被访问的个数
	uint64_t evicted_unfetched;		// COLD队列中被剔除的item中未被访问过的个数
	uint64_t evicted_active;			// COLD队列中被剔除的item中active状态的个数
	uint64_t crawler_reclaimed;		// 记录crawler线程检查出的过期item个数
	uint64_t crawler_items_checked;	// 记录crawler线程检查item的个数
	uint64_t lrutail_reflocked;		// item被锁住的次数
	uint64_t moves_to_cold;				// 将转移到COLD队列的item个数
	uint64_t moves_to_warm;				// 将转移到WARM队列的item个数
	uint64_t moves_within_lru;			// 记录从WARM LRU队列转移出的item个数
	uint64_t direct_reclaims;			// 记录不是第一次就获取item的次数
	uint64_t hits_to_hot;				// 命中hot队列的次数
	uint64_t hits_to_warm;				// 命中warm队列的次数
	uint64_t hits_to_cold;				// 命中cold队列的次数
	uint64_t hits_to_temp;				// 命中temp队列的次数
	rel_time_t evicted_time;			// 最后被evicted的item的时间
} itemstats_t;

```

### 流程图示

![lru_pull_tail函数流程图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_lru/lru_pull_tail函数流程.png)

## `LRU`维护线程源代码中维护`worker`线程中`lru_bump_buf`结构的源代码

在`LRU`维护线程中调用`lru_maintainer_bumps()`函数来实现维护`worker`线程中的`lru_bump_buf`结构。其源代码如下:

```
//lru_maintainer_thread函数中:
//处理worker线程中的lru_bump_buf结构
/* Minimize the sleep if we had async LRU bumps to process */
if (settings.lru_segmented && lru_maintainer_bumps() && to_sleep > 1000) {
	to_sleep = 1000;
}

//lru_maintainer_bumps函数
//处理各个worker线程中的lru_bump_buf结构,此结构记录了worker线程最近被访问HOT/WARM队列的item。
//当worker线程中的lru_bump_buf结构中item被释放时，会将最近访问的item移动到队列头部，保证被访问的item不会被换出。
/* TODO: Might be worth a micro-optimization of having bump buffers link
 * themselves back into the central queue when queue goes from zero to 
 * non-zero, then remove from list if zero more than N times.
 * If very few hits on cold this would avoid extra memory barriers from LRU
 * maintainer thread. If many hits, they'll just stay in the list.
 */
static bool lru_maitainer_bumps(void) {
	lru_bump_buf *b;
	lru_bump_entry *be;
	unsigned int size;
	unsigned int todo;
	bool bumped = false;
	pthread_mutex_lock(&bump_buf_lock); // 对所有worker线程中lru_bump_buf组成的双向链表加锁
	// 开始循环遍历每个worker线程的lru_bump_buf结构
	for (b = bump_buf_head; b != NULL; b = b->next) {
		pthread_mutex_lock(&b->mutex);
		// 获取已经添加的item的数量，以及开始的位置
		be = (lru_bump_entry *) bipbuf_peek_all(b->buf, &size);
		pthread_mutex_unlock(&b->mutex);
		
		if (be == NULL) {
			continue;
		}
		todo = size;
		bumped = true;
		
		// 逐个释放item回到队列的头部
		while (todo) {
			item_lock(be->hv);
			do_item_update(be->it); // 更新item队列位置
			do_item_remove(be->it); // 删除引用
			item_unlock(be->hv);
			be++; // 下一个item
			todo -= sizeof(lru_bump_entry);
		}
		
		pthread_mutex_lock(&b->mutex);
		// 将刚才释放的item所占lru_bump_buf的空间都是放掉
		be = (lru_bump_entry *) bipbuf_poll(b->buf, size);
		pthread_mutex_unlock(&b->mutex);
	}
	pthread_mutex_unlock(&bump_buf_lock);
	return bumped;
}

```

### 使用的结构体
上述的函数中使用了大量的结构体，本小节将把上述的结构体逐一拆解，详细描述其功能以及调用关系。

```
typedef struct _lru_bump_buf {
	// 构成双向链表的指针
	struct _lru_bump_buf *prev;
	strcut _lru_bump_buf *next;
	pthread_mutex_t mutex;		// 使用此结构体的锁结构
	bipbuf_t *buf;				// 真正存放数据的结构
	uint64_t dropped;
} lru_bump_buf;

typedef struct {		// 此结构在bipbuffer.h文件中
	unsigned long int size;
	
	/* region A */
	unsigned int a_start, a_end;
	
	/* region B */
	unsigned int b_end;
	
	/* is B insue? */
	int b_inuse;
	
	unsigned char data[];
} bipbuf_t;

typedef struct { 	// 此结构用于保存worker线程访问的WARM队列中的item
	item *it;
	uint32_t hv;
} lru_bump_entry;

// lru_bump_buf链表使用的全局变量
static lru_bump_buf *bump_buf_head = NULL; // 用于记录lru_bump_buf链表的队头
static lru_bump_buf *bump_buf_tail = NULL; // 用于记录lru_bump_buf链表的队尾
static pthread_mutex_t bump_buf_lock = PTHREAD_MUTEX_INITAILIZER; // 用于控制lru_bump_buf链表的访问互斥

#define LRU_BUMP_BUF_SIZE 8192 // worker线程中lru_bump_buf结构中buf存储lru_bump_entry的个数
```

### 创建`lru_bump_buf`链表
上面讲述了`lru_bump_buf`链表的结构体，每个`worker`线程中都有存储此结构体的指针。下面介绍`lru_bump_buf`链表的创建过程，以及如何跟`worker`线程关联起来。

在`worker`线程创建过程中，将会调用`worker_libevent`函数，此函数中将调用`item_lru_bump_buf_create`函数来完成`worker`线程中的`lru_bump_buf`空间申请，以及`lru_bump_buf`链表的创建。

```
// thread.c文件中worker线程创建过程中worker_libevent函数:
static void *worker_libevent(void *arg) {
	LIBEVENT_THREAD *me = arg;
	
	···
	me->lru_bump_buf = item_lru_bump_buf_create(); // 此函数用于创建`lru_bump_buf`链表
	···
}

// items.c文件中item_lru_bump_buf_create函数:
void *item_lru_bump_buf_create(void) {
	lru_bump_buf *b = calloc(1, sizeof(lru_bump_buf)); // 创建一个lru_bump_buf结构
	if (b == NULL) {
		return NULL;
	}
	
	b->buf = bipbuf_new(sizeof(lru_bump_entry) * LRU_BUMP_BUF_SIZE); // lru_bump_buf结构体中buf申请LRU_BUMP_BUF_SIZE个lru_bump_entry结构体
	if (b->buf == NULL) {
		free(b);
		return NULL;
	}
	
	pthread_mutex_init(&b->mutex, NULL); // 初始化lru_bump_buf结构体的互斥量
	
	lru_bump_buf_link_q(b); // 将worker线程中的lru_bump_buf结构链接成双向链表
	return b;
}

// 将worker线程中的lru_bump_buf链接成双向链表
static void lru_bump_buf_link_q(lru_bump_buf *b) {
	pthread_mutex_lock(&bump_buf_lock); // 获取链表的互斥量
	assert(b != bump_buf_head);
	
	// 构建双向链表 --- 头插法
	b->prev = 0;
	b->next = bump_buf_head;
	if (b->next) b->next->prev = b;
	bump_buf_head = b;
	if (bump_buf_tail == 0) bump_buf_tail = b;
	pthread_mutex_unlock(&bump_buf_lock);
	return;
}
```
整个`lru_bump_buf`链表的结构图示如下所示:

![lru_bump_buf链表的图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_lru/lru_bump_buf链表.png)

## `LRU`维护线程源代码中调用`LRU`扫描线程的源代码
上面两个小节分别解读了`LRU`维护线程中关于维护`LRU`队列长度、`worker`线程组成的`lru_bump_buf`双向链表结构。本小节将着重介绍一下`LRU`维护线程中关于调用`LRU`扫描线程的代码部分。其源代码如下:

```
// items.c文件中lru_maintainer_thread函数，调用LRU扫描线程的代码
if (settings.lru_crawler && last_crawler_check != current_time) { // 判断是否可以调用LRU扫描线程
	lru_maintainer_crawler_check(cdata, l); // 调用LRU扫描线程代码
	last_crawler_check = current_time;
}

// item.c文件中lru_maintainer_crawler_check函数
/* Will crawl all slab classes a minimum of once per hour */
#define MAX_MAINTCRAWL_WAIT 60 * 60

/* Hoping user input will improve this function. This is all a wild guess.
 * Operation: Kicks crawler for each slab id. Crawlers take some statistics as
 * to items with nonzero expirations. It then buckets how many items will
 * expire per minute for the next hour.
 * This function checks the results of a run, and if it things more than 1% of
 * expirable objects are ready to go, kick the crawler again to reap.
 * It will also kick the crawler once per minute regardless, waiting a minute
 * longer for each time it has no work to do, up to an hour wait time.
 * The latter is to avoid newly started daemons from waiting too long before
 * retrying a crawl.
 */
 static void lru_maitainer_crawler_check(struct crawler_expired_data *cdata, logger *l) {
 	int i;
 	static rel_time_t next_crawls[POWER_LARGEST]; //下次调整的时间
 	static rel_time_t next_crawl_wait[POWER_LARGEST]; //下次调整的等待时间
 	uint8_t todo[POWER_LARGEST]; // 记录LRU是否需要进行LRU扫描
 	memset(todo, 0, sizeof(uint8_t) * POWER_LARGEST);
 	bool do_run = false;
 	unsigned int tocrawl_limit = 0; // 每个LRU链表中item的字节数
 	
 	// TODO: If not segmented LRU, skip non-cold
 	// 依次判断每个LRU是否需要进行LRU过期扫描
 	for (i = POWER_SMALLEST; i < POWER_LARGEST; i++) {
 		crawlerstats_t *s = &cdata->crawlerstats[i]; // 每个LRU的状态信息记录
 		// We've not successfully kicked off a crawl yet.
 		if (s->run_complete) {				// 判断LRU链表是否完成过期扫描
 			char *lru_name = "na";
 			pthread_mutex_lock(&cdata->lock);
 			int x;
 			// Should we crawl again?
 			uint64_t possible_reclaims = s->seen - s->noexp; // 可能存在的过期item数量
 			uint64_t available_reclaims = 0;
 			// Need to think we can free at least 1% of the items before
 			// crawling.
 			// FIXME: Configurable?
 			uint64_t low_watermark = (possible_reclaims / 100) + 1; // 最低水平线
 			rel_time_t since_run = current_time - s->end_time; // 距离上次LRU检查的时间
 			// Don't bother if the payoff is too low.
 			// crawlerstats_t结构体中存在histo数组，此数组用来保存此LRU链表中下一个hour中每分钟过期的item数量。
 			// 根据下一个hour中每分钟过期的item数量，来判断什么时候开始扫描LRU队列。
 			for (x = 0; x < 60; x++) {
 				available_reclaims += s->histo[x];
 				if (available_reclaims > low_watermark) {
 					if (next_crawl_wait[i] < (x * 60)) {
 						next_crawl_wait[i] += 60;
 					} else if (next_crawl_wait[i] >= 60) {
 						next_crawl_wait[i] -= 60;
 					}
 					break;
 				}
 			}
 			
 			if (available_reclaims == 0) {
 				next_crawl_wait[i] += 60;
 			}
 			
 			if (next_crawl_wait[i] > MAX_MAINTCRAWL_WAIT) {
 				// 最长等待时间为1hour
 				next_crawl_wait[i] = MAX_MAINTCRAWL_WAIT;
 			}
 			// next_crawl_wait是距离下次LRU扫描的时间
 			// next_crawls用于保存下次LRU扫描的时间
 			next_crawls[i] = current_time + next_crawl_wait[i] + 5;
 			// 用于记录LRU的扫描信息
 			switch (GET_LRU(i)) {
 				case HOT_LRU:
 					lru_name = "hot";
 					break;
 				case WARM_LRU:
 					lru_name = "warm";
 					break;
 				case COLD_LRU:
 					lru_name = "cold";
 					break;
 				case TEMP_LRU:
 					lru_name = "temp";
 					break;
 			}
 			LOGGER_LOG(l, LOG_SYSEVENTS, LOGGER_CRAWLER_STATUS, NULL,
 						CLEAR_LRU(i),
 						lru_name,
 						(unsigned long long)low_watermark,
 						(unsigned long long)available_reclaims,
 						(unsigned int)since_run,
 						next_crawls[i] - current_time,
 						s->end_time - s->start_time,
 						s->seen,
 						s->reclaimed);
 		}
 		// 判断LRU链表是否要进行LRU过期扫描
 		if (current_time > next_crawls[i]) {
 			pthread_mutex_lock(&lru_locks[i]);
 			if (sizes[i] > tocrawl_limit) {
 				tocrawl_limit = sizes[i];
 			}
 			pthread_mutex_unlock(&lru_locks[i]);
 			todo[i] = 1;
 			do_run = true;
 			next_crawls[i] = current_time + 5; // minimum retry wait.
 		}
 	}
 	if (do_run) {
 		if (settings.lru_crawler_tocrawl && settings.lru_crawler_tocrawl < tocrawl_limit) {
 			tocrawl_limit = settings.lru_crawler_tocrawl;
 		}
 		// 开启LRU过期扫描线程
 		lru_crawler_start(todo, tocrawl_limit, CRAWLER_AUTOEXPIRE, cdata, NULL, 0);
 	}
 }
 
// crawler.c文件中通知LRU扫描线程工作的lru_crawler_start函数
int lru_crawler_start(uint8_t *ids, uint32_t remaining, const enum crawler_run_type type, void *data, void *c, const int sfd) {
	int starts = 0;
	bool is_running;
	static rel_time_t block_ae_until = 0; // 每隔60s检查LRU过期扫描是否完成
	pthread_mutex_lock(&lru_crawler_lock);
	STATS_LOCK();
	is_running = stats_state.lru_crawler_running;
	STATS_UNLOCK();
	// 当LRU扫描线程正在进行中，并且又有过期扫描任务进入。则等待60s后再进行判断。
	if (is_running &&
			!(type == CRAWLER_AUTOEXPIRE && active_crawler_type == CRAWLER_AUTOEXPIRE) {
		pthread_mutex_unlock(&lru_crawler_lock);
		block_ae_until = current_time + 60;
		return -1;
	}
	// 判断LRU过期扫描是否需要进行
	if (type == CRAWLER_AUTOEXPIRE && block_ae_until > current_time) {
		pthread_mutex_unlock(&lru_crawler_lock);
		return -1;
	}
	
	/* Configure the module */
	// 如果LRU线程还没有开始运行则初始化LRU过期扫描线程所需的配置
	if (!is_running) {
		assert(crawler_mod_regs[type] != NULL);
		// 判断LRU过期扫描线程所使用的方法
		active_crawler_mod.mod = crawler_mod_regs[type];
		active_crawler_type = type;
		if (active_crawler_mod.mod->init = NULL) {
			active_crawler_mod.mod->init(&active_crawler_mod, data);
		}
		// 判断是否需要客户端
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
	
	/* we allow the qutocrawler to restart sub-LRU's before completion */
	// 判断哪个LRU队列是否需要进行LRU过期扫描
	for (int sid = POWER_SMALLEST; sid < POWER_LARGEST; sid++) {
		if (ids[sid])
			starts += do_lru_crawler_start(sid, remaining); // 向LRU队列添加扫描伪item
	}
	// 向LRU扫描线程释放信号
	if (starts) {
		pthread_cond_signal(&lru_crawler_cond);
	}
	pthread_mutex_unlock(&lru_crawler_lock);
	return starts;
}

// crawler.c文件中通知LRU扫描线程工作的do_lru_crawler_start函数
/* 'remaining' is passed in so the LRU maintainer thread can scrub the whole
 * LRU every time.
 */
static int do_lru_crawler_start(uint32_t id, uint32_t remaining) {
	uint32_t sid = id;
	int starts = 0;
	
	pthread_mutex_lock(&lru_locks[sid]);
	// crawlers数组中每个LRU队列对应一个数组下标
	if (crawlers[sid].it_flags == 0) {	// 判断LRU队列对应的item是否正在使用
		if (settings.verbose > 2)
			fprintf(stderr, "Kicking LRU crawler off for LRU %u\n", sid);
		// 设置crawlers中sid对应的item值，并将其加入到LRU队列中，从后往前依次扫描
		// 伪item的标识为it_flags为1，并且item其它的值为0
		crawlers[sid].nbytes = 0;
		crawlers[sid].nkey = 0;
		crawlers[sid].it_flags = 1; // For a crawler, this means enabled.
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
		 crawlers[sid].remaining = remaining; // LRU队列保存的字节数
		 crawlers[sid].slabs_clsid = sid;
		 crawlers[sid].reclaimed = 0;
		 crawlers[sid].unfetched = 0;
		 crawlers[sid].checked = 0;
		 do_item_linktail_q((item *)&crawlers[sid]); // 将伪item加入到LRU链表中
		 crawler_count++;	// 全局变量，用于判断扫描LRU链表的个数
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

上面的代码中显示了`LRU`维护线程中关于`LRU`过期扫描线程的调用链:

`lru_maintainer_thread`函数--->`lru_maintainer_crawler_check`函数--->`lru_crawler_start`函数--->`do_lru_crawler_start`函数

### 使用的结构体
通过代码可以看出此调用链中存在大量的结构体和全局变量，这些结构体/变量在函数中起了很重要的作用。我们将对这些信息进行逐一解释和分析。

```
// 重要的全局变量
#define LRU_CRAWLER_CAP_REMAINING -1

// 每一个LRU链表都拥有一个crawlerstats_t结构体
typedef struct {
	uint64_t histo[61];		// 在未来1hour中每分钟过期的item数量
	uint64_t ttl_hourplus;
	uint64_t noexp;
	uint64_t reclaimed;
	uint64_t seen;
	rel_time_t start_time;	// LRU链表的扫描开始时间
	rel_time_t end_time; // LRU链表的扫描结束时间
	bool run_complete;	// 此LRU链表是否完成
} crawlerstats_t;

// LRU扫描线程使用的重要结构
struct crawler_expired_data {
	pthread_mutex_t lock;	// 结构锁
	crawlerstats_t crawlerstats[POWER_LARGEST]; // 每个LRU链表的信息
	/* redundant with crawlerstats_t so we can get overall start/stop/done */
	rel_time_t start_time;	// LRU扫描线程的开始时间
	rel_time_t end_time;	// LRU扫描线程的结束时间
	bool crawl_complete;	// LRU扫描线程是否完成
	// 判断本结构体是由LRU线程申请还是其他线程申请的。
	bool is_external; /* whether this was an alloc local or remote to the module */
};

// LRU过期扫描线程的工作方法,本小节使用的工作方法为CRAWLER_AUTOEXPIRE
enum crawler_run_type {
	CRAWLER_AUTOEXPIRE=0, CRAWLER_EXPIRED, CRAWLER_METADUMP
};

// LRU扫描线程使用的结构体，以及相关函数
// 用于控制crawler线程使用的client结构体
typedef struct {
	// 用于crawler_client链接的connect结构体
	void *c; // original connection structure. still with source thread attached.
	int sfd; // client fd; connect链接后返回的文件描述符
	bipbuf_t *buf;	// output buffer,客户端对应的bipbuf_t结构，跟worker线程中的一样
	char *cbuf;	// current buffer,client对应的缓冲区
} crawler_client_t;

typedef struct _crawler_module_t crawler_module_t;

typedef void (*crawler_eval_func)(crawler_module_t *cm, item *it, uint32_t hv, int slab_cls); // 用于判断item在crawler线程过期检查中是否要移除的判断函数指针
typedef int (*crawler_init_func)(crawler_module_t *cm, void *data); // 用于初始化crawler线程使用的crawler_expired_datae结构体
typedef void (*crawler_deinit_func)(crawler_module_t *cm); // TODO: extra args?
typedef void (*crawler_doneclass_func)(crawler_module_t *cm, int slab_cls); // 当crawler线程扫描完某个LRU时调用
typedef void (*crawler_finalize_func)(crawler_module_t *cm); // 整个crawler扫描结束时调用

// 用于存放crawler线程扫描过程中使用函数的结构体--不同的工作方法对应不同的函数
typedef struct {
	crawler_init_func init;		// run before crawl starts
	crawler_eval_func eval;		// runs on an item
	crawler_doneclass_func doneclass;	// runs once per sub-crawler completion
	crawler_finalize_func finalize;	// runs once when all sub-crawlers are done.
	bool needs_lock;	// whether or not we need the LRU lock held when eval is called
	bool needs_client;	// whether or not to grab onto the remote client
} crawler_module_reg_t;

// crawler线程扫描过程中所使用的所有方法以及数据的存储结构
struct _crawler_module_t {
	void *data;						// 数据
	crawler_client_t c;				// 客户端
	crawler_module_reg_t *mod;		// 方法
};

// crawler线程扫描过程中使用的伪item的结构
typedef struct {
	struct _stritem *next;		// 双向链表的链接指针
	struct _stritem *prev;
	struct _stritem *h_next;	// hash chain next
	rel_time_t	time;			// least recent access
	rel_time_t	exptime;		// expire time
	int				nbytes;		// size of data
	unsigned short refcount;
	uint8_t		nsuffix;		// length of flags-and-length string
	uint8_t		it_flags;		// it_flags = 1时，表示此LRU正在被扫描
	uitn8_t		slabs_clsid;	// which salb class we're in
	uint8_t		nkey;			// key length, w/terminating null and padding
	uint8_t		remaining;	// Max keys to crawl per slab invocation
	uint8_t		unfetched;	// items reclaimed unfetched during this crawl
	uint8_t		checked;		// items examined during this crawl
} crawler;

// 上面介绍完了所有需要使用的结构体以及函数指针，下面将介绍全局变量
// crawler扫描线程CRAWLER_AUTOEXPIRE/CRAWLER_EXPIRED工作方式下的工作函数
crawler_module_reg_t crawler_expired_mod = {
	.init = crawler_expired_init,		// 初始化函数
	.eval = crawler_expired_eval,		// 判断函数
	.doneclass = crawler_expired_doneclass, // 完成一个LRU链表检测时调用
	.finalize = crawler_expired_finalize,  // crawler终结时调用
	.needs_lock = true,
	.needs_client = false
};
// crawler扫描线程CRAWLER_METADUMP工作方式下的工作函数
crawler_module_reg_t *crawler_metadump_mod = {
	.init = NULL,
	.eval = crawler_metadump_eval,
	.doneclass = NULL,
	.finalize = crawler_metadump_finalize,
	.needs_lock = false,
	.needs_client = true
};

// 所有crawler线程工作方式的数组函数
crawler_module_reg_t *crawler_mod_regs[3] = {
	&crawler_expired_mod,			// CRAWLER_AUTOEXPIRE工作方式使用的函数，本小节将使用的
	&crawler_expired_mod,			// CRAWLER_EXPIRED工作方式使用的函数
	&crawler_metadump_mod			// CRAWLER_METADUMP工作方式使用的函数
};

// 用于保存正在使用的crawler工作方式以及对应函数等信息
crawler_module_t active_crawler_mid;	// crawler扫描线程当前使用的结构
enum crawler_run_type active_crawler_type; // crawler扫描线程的工作方式

// 用于存放crawler扫描线程使用的伪item
static crawler crawlers[LARGEST_ID];

static int crawler_count = 0; // crawler需要扫描的LRU链表的个数
static volatile int do_run_lru_crawler_thread = 0; // crawler线程是否在运行
static int lru_crawler_initialized = 0;	// crawler初始化静态变量
static pthread_mutex_t lru_crawler_lock = PTHREAD_MUTEX_INITIALIZER; // 锁
static ptherad_cond_t  lru_crawler_cond = PTHREAD_COND_INITIALIZER; // 条件变量
```

### 流程图示

上面讲述完了`crawler`所使用的结构体，本小节将整个调用流程的主要功能以图示的方式来展现调用链。

![伪ITEM插入LRU链表的图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_lru/crawler扫描线程的伪ITEM.png)






