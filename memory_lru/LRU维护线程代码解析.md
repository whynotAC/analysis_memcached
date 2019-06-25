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

![lru_pull_tail函数流程图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_lru/gaitubao_lru_pull_tail函数流程.png)