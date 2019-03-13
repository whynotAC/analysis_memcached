slab\_rebalance\_move函数
================================
1 函数的流程
-------------------------------
函数的整个流程如下:

* 获取`item`的`it_flags`标记，从而设置`status`。
* 根据`status`的不同从而采取不同的策略，来转移`item`。
* 根据`slab_rebal`的`slab_pos`来判断是否清空了整个`slab`。
* 如果`slab`中存在未完成的`item`，则从`slab`的头部重新进行扫描。
* 如果整个`slab`清空，设置`slab_rebal`的`done`字段，然后函数退出。

2 函数的使用的全局变量
--------------------------------

| 变量名 	| 定义 		| 设置			| 备注 	  |
| ---- 	| ----- 	| ----- 		| ----   |
| `slab_bulk_check` | `int slab_bulk_check = DEFAULT_BULK_CHECK` | 此变量可以通过设置环境变量`MEMCACHED_SLAB_BULK_CHECK`来设置其大小 | `DEFAULT_BULK_CHECK`值为1，默认每次只转移`slab`中的一个`item` |
| `SLAB_MOVE_MAX_LOOPS ` | `#define SLAB_MOVE_MAX_LOOPS 1000` | 此变量用于控制每次循环时转移`item`的数量 | 无 |

3 函数的定义
---------------------------------
>```
>	static int slab_rebalance_move(void) {
>		slabclass_t *s_cls;
>		int x;
>		int was_busy = 0;
>		int refcount = 0;
>		uint32_t hv;
>		void *hold_lock;
>		enum move_status status = MOVE_PASS; // 用于记录item的转移状态
>
>		pthread_mutex_lock(&slabs_lock);
>			
>		// 每次对slab_bulk_check个数的item进行操作
>		for (x = 0; x < slab_bulk_check; x++) {
>			hv = 0;
>			hold_lock = NULL;
>			item *it = slab_rebal.slab_pos;
>			item_chunk *ch = NULL;
>			status = MOVE_PASS;
>			if (it->it_flags & ITEM_CHUNK) {
>				// This chunk is a chained part of a larger item
>				ch = (item_chunk *)it;
>				/* Instead, we use the head chunk to find the item and effectively
>				 * lock the entire structure. if a chunk has ITEM_CHUNK flag, its
>				 *	head cannot be slabbed, so the normal routine is safe.
>				 */
>				it = ch->head;
>				assert(it->it_flags & ITEM_CHUNKED);
>			}
>			
>			/* ITEM_FETCHED when ITEM_SLABBED is overloaded to mean we've cleared
>			 * the chunk for move. Only these two flags should exist.
>			 */
>			if (it->it_flags != (ITEM_SLABBED|ITEM_FETCHED)) {
>				// ITEM_SLABBED can only be added/removed under the slabs_lock
>				if (it->it_flags & ITEM_SLABBED) {
>					// item空闲，可以直接转移
>					assert(ch == NULL);
>					slab_rebalance_cut_free(s_cls, it);
>					status = MOVE_FROM_SLAB;
>				} else if ((it->it_flags & ITEM_LINKED) != 0) {
>					/* If it doesn't have ITEM_SLABBED, the item could be in any
>					 * state on its way to being freed or written to. If no
>					 * ITEM_SLABBED, but it's had ITEM_LINKED, it must be active
>					 * and have the key written to it already.
>					 */
>					// 如果it_flags不包含ITEM_SLABBED和ITEM_LINKED，这表明item在释放或者写入状态
>					// 如果it_flags不包含ITEM_SLABBED但包含ITEM_LINKED，这表明item在活动队列或者已经写完
>					// 判断item的hash表来获取对应的item
>					hv = hash(ITEM_key(it), it->nkey);
>					if ((hold_lock = item_trylock(hv)) == NULL) {
>						status = MOVE_LOCKED;
>					} else {
>						bool is_linked = (it->it_flags & ITEM_LINKED);
>						refcount = refcount_incr(it);
>						if (refcount == 2) { // item is linked but not busy
>							//Double check ITEM_LINKED flag here, since we're
>							//past a memory barrier from the mutex.
>							if (is_linked) {
>								// 表示此item可以从LRU转移
>								status = MOVE_FROM_LRU;
>							} else {
>								/* refcount == 1 + !ITEM_LINKED means the item is
>								 *	being uploaded to, or was just unlinked but
>								 * hasn't been freed yet. Let it bleed off on its 
>								 *	own and try again later.
>								 */
>								status = MOVE_BUSY;
>							}
>						} else if (refcount > 2 && is_linked) {
>							// TODO: Mark items for delete/rescue and process
>							// outside of the main loop
>							if (slab_rebal.busy_loops > SLAB_MOVE_MAX_LOOPS) {
>								slab_rebal.busy_deletes++;
>								// Only safe to hold slabs lock because refcount
>								// can't drop to 0 util we release item lock
>								STORAGE_delete(storage, it);
>								pthread_mutex_unlock(&slab_lock);
>								do_item_unlink(it, hv);
>								pthread_mutex_lock(&slab_lock);
>							}
>							status = MOVE_BUSY;
>						} else {
>							status = MOVE_BUSY;
>						}
>						// Item lock must be held while modifying refcount
>						if (status == MOVE_BUSY) {
>							refcount_decr(it);
>							item_try_unlock(hold_lock);
>						}
>					}
>				} else {
>					/* See above comment. No ITEM_SLABBED or ITEM_LINKED. Mark
>					 * busy and wait for item to complete its upload.
>					 */
>					status = MOVE_BUSY;
>				}
>			}
>			
>			// 根据status来判断item如何进行操作
>			int save_item = 0;
>			item *new_it = NULL;	// 用于保存item的转移地址
>			size_t ntotal = 0;
>			switch (status) {
>				case MOVE_FROM_LRU:
>					/* Lock order is LRU locks -> slabs_lock. unlink uses LRU lock.
>					 * We only need to hold the slabs_lock while initially looking
>					 * at an item, and at this point we have an exclusive refcount
>					 * (2) + the item is locked.Drop slabs lock, drop item to
>					 * refcount 1 (just our own, then fail through and wipe it)
>					 */
>					// Check if expired or flushed
>					ntotal = ITEM_ntotal(it);
>#ifdef EXTSTORE
>					if (it->it_flags & ITEM_HDR) {
>						ntotal = (ntotal - it->nbytes) + sizeof(item_hdr);
>					}
>#endif
>					/* REQUIRES slabs_lock: CHECK FOR cls->sl_curr > 0 */
>					if (ch == NULL && (it->it_flags & ITEM_CHUNKED)) {
>						/* Chunked should be identical to non-chunked, except we need
>						 * to swap out ntotal for the head-chunk-total.
>						 */
>						ntotal = s_cls->size;
>					}
>					if ((it->exptime != 0 && it->exptime < current_time)
>							|| item_is_flushed(it)) {
>						/* Expired, don't save. */
>						// 此item已经过期，不用转移
>						save_item = 0;
>					} else if (ch == NULL &&
>							(new_it = slab_rebalance_alloc(ntotal, slab_rebal.s_clsid)) == NULL) {
>						// Not a chunk of an item, and nomem
>						save_item = 0;
>						slab_rebal.evictions_nomem++;
>					} else if (ch != NULL &&
>							(new_it = slab_rebalance_alloc(s_cls->size, slab_rebal.s_clsid)) == NULL) {
>						// Is a chunk of an item, and nomem
>						save_item = 0;
>						slab_rebal.evictions_nomem++;
>					} else {
>						// Was whatever it was, and we have memory for it.
>						save_item = 1;
>					}
>					pthread_mutex_unlock(&slab_lock);
>					unsigned int requested_adjust = 0;
>					if (save_item) {
>						if (ch == NULL) {
>							assert((new_it->flags & ITEM_CHUNKED) == 0);
>							// if free memory, memcpy. clear prev/next/h_bucket
>							memcpy(new_it, it, ntotal);
>							new_it->prev = 0;
>							new_it->next = 0;
>							new_it->h_next = 0;
>							// These are definitely requeired. else fails assert.
>							new_it->it_flags &= ~ITEM_LINKED;
>							new_it->refcount = 0;
>							// 新旧item进行替换
>							do_item_replace(it, new_it, hv);
>							// Need to walk the chunks and repoint head
>							if (new_it->it_flags & ITEM_CHUNKED) {
>								item_chunk *fch = (item_chunk *)ITEM_data(new_it);
>								fch->next->prev = fch;
>								while (fch) {
>									fch->head = new_it;
>									fch = fch->next;
>								}
>							}
>							it->refcount = 0;
>							it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
>#ifdef DEBUG_SLAB_MOVER
>							memcpy(ITEM_key(it), "deadbeef", 8);
>#endif
>							slab_rebal.rescues++;
>							requested_adjust = ntotal;
>						} else {
>							item_chunk *nch = (item_chunk *)new_it;
>							// Chunks always have head chunk (the main it)
>							// 整个item是一个chunk。将chunk转移出去
>							ch->prev->next = nch;
>							if (ch->next)
>								ch->next->prev = nch;
>							memcpy(nch, ch, ch->used + sizeof(item_chunk));
>							ch->refcount = 0;
>							ch->it_flags = ITEM_SLABBED|ITEM_FETCHED;
>							slab_rebal.chunk_rescues++;
>#ifdef DEBUG_SLAB_MOVER
>							memcpy(ITEM_key((item *)ch), "deadbeef", 8);
>#endif
>							refcount_decr(it);
>							requested_adjust = s_cls->size;
>						}
>					} else {
>						// 此处处理不用转移的item
>						// restore ntotal in case we tried saving a head chunk.
>						ntotal = ITEM_ntotal(it);
>						STORAGE_delete(storage, it);
>						do_item_unlink(it, hv);
>						slabs_free(it, ntotal, slab_rebal.s_clsid);
>						// Swing around again later to remove it from the freelist
>						slab_rebal.busy_items++;
>						was_busy++;
>					}
>					item_trylock_unlock(hold_lock);
>					pthread_mutex_lock(&slab_lock);
>					/* Always remove the ntotal, as we added it in during
>					 * do_slabs_alloc() when copying the item.
>					 */
>					s_cls->requested -= requested_adjust;
>					break;
>				case MOVE_FROM_SLAB:
>					it->refcount = 0;
>					it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
>#ifdef DEBUG_SLAB_MOVER
>					memcpy(ITEM_key(it), "deadbeef", 8);
>#endif
>					break;
>				case MOVE_BUSY:
>				case MOVE_LOCKED:
>					slab_rebal.busy_items++;
>					was_busy++;
>					break;
>				case MOVE_PASS:
>					break;
>			}
>			
>			slab_rebal.slab_pos = (char *)slab_rebal.slab_pos + s_cls->size;
>			if (slab_rebal.slab_pos >= slab_rebal.slab_end)
>				break;
>		}
>		// 判断是否转移完成
>		if (slab_rebal.slab_pos >= slab_rebal.slab_end) {
>			// Some items were busy, start again from the top
>			if (slab_rebal.busy_items) {
>				slab_rebal.slab_pos = slab_rebal.slab_start;
>				STATS_LOCK();
>				stats.slab_reassign_busy_items += slab_rebal.busy_items;
>				STATS_UNLOCK();
>				slab_rebal.busy_items = 0;
>				slab_rebal.busy_loops++;
>			} else {
>				slab_rebal.done++;
>			}
>		}
>		
>		pthread_mutex_unlock(&slabs_lock);
>
>		return was_busy;
>	}
>```

4 函数结构定义
-----------------------------------
此章节用于描述函数代码结构，体现`slab_rebalance_move`运行过程。

此函数分为两部分：

* 获取`c_cls`的`item`中的`it_flags`来判断怎样转移。
* 根据`item`的`status`来进行转移。

> PS: 首先看一下`item`中`it_flags`中的标志位。
> 
> | 名称 | 定义 | 备注 |
> | --- | --- | ---- |
> |ITEM_LINKED | #define ITEM_LINKED 1 | 用于表示item在LRU链接中 |
> |ITEM_CAS	 | #define ITEM_CAS 2	| 用于表示item可以使用原子锁 |
> |ITEM_SLABBED | #define ITEM_SLABBED 4 | 用于表示item在空闲的SLAB队列中 |
> |ITEM_FETCHED | #define ITEM_FETCHED 8 | 用于表示item正在操作中 |
> |ITEM_ACTIVE | #define ITEM_ACTIVE 16 | 用于表示item处于活跃状态 |
> |ITEM_CHUNKED | #define ITEM_CHUNKED 32 | 用于表示item是否用于指向chunk |
> |ITEM_CHUNK | #define ITEM_CHUNK 64 | 用于表示item本身是否为chunk |
> |ITEM_HDR | #define ITEM_HDR 128 | 暂时不知道 |

整个`slab_rebalance_move`函数的流程图形如下：

![slab_rebalance_move流程图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_rebalance/slab_rebalance_move.png)