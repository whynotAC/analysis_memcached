slabRebalanceThread线程
============================
本文档主要用于介绍`slab_rebalance_thread`线程的代码结构以及其对应线程逻辑。

1.线程代码
----------------------------------
>	```
>	void* slab_rebalance_thread(void *arg) {
>		//用于判断item是否处于无法转移的状态
>		int was_busy = 0;
>		mutex_lock(&slab_rebalance_lock);
>		while (do_run_slab_rebalance_thread) {
>			// 用于判断slab_rebalance_thread线程是否可以进行slab_rebalance操作
>			if (slab_rebalance_signal == 1) {
>				//	slab_rebalance_start函数用于判断是否满足slab_rebalance条件
>				if (slab_rebalance_start() < 0) {
>					// 不符合slab_rebalance的条件
>					slab_rebalance_signal = 0;
>				}
>				was_busy = 0;
>			} else if (slab_rebalance_signal && slab_rebal.slab_start != NULL) {
>				// 满足slab_rebalance的条件，对item进行移动，腾出空间
>				was_busy = slab_rebalance_move();
>			}
>			// 判断slab_rebalance是否转移完成
>			if (slab_rebal.done) {
>				// 转移完成进行清理操作
>				slab_rebalance_finish();
>			} else if (was_busy) {
>				// 当要转移的item正在忙碌时，线程进行休息后继续工作
>				usleep(1000);
>			}
>			// 判断slab_rebalance线程是否需要停止，等待触发
>			if (slab_rebalance_signal == 0) {
>				// 等待slab_rebalance条件变量为真
>				pthread_cond_wait(&slab_rebalance_cond, &slab_rebalance_lock);
>			}
>		}
>		return NULL;
>	}
>	```

上述为`slab_rebalance_thread`线程的代码，其中调用了`slab_rebalance_start()`、`slab_rebalance_move()`和`slab_rebalance_finish()`函数来完成`slab_rebalance`的过程。

`slab_rebalance_thread`线程的代码中`slab_rebalance_signal`的取值以及其表示含义,如下所示:

|	取值	|	备注	|
|--------	|--------	|
|	0		| 停止`slab_rebalance_move`操作	 |
|	1		| 开始检测是否符合`slab_rebalance_move`条件 |
|	2		| 开始调用`slab_rebalance_move`函数进行内存转移 |

2.`slab_rebalance_thread`使用的结构体
--------------------------------------
>	```
>	struct slab_rebalance {
>		void 		*slab_start;			// slab_rebalance开始位置
>		void 		*slab_end;			// slab_rebalance结束位置
>		void 		*slab_pos;			// slab_rebalance当前转移位置
>		int			s_clsid;				// 源slabclass的id
>		int 		d_clsid;				// 目的slabclass的id
>		uint32_t 	busy_items;			//	
>		uint32_t 	rescues;				//
>		uint32_t	evictions_nomem;		//
>		uint32_t	inline_reclaim;		//
>		uint32_t	chunk_rescues;		//
>		uint32_t	busy_deletes;			//
>		uint32_t	busy_loops;			//
>		uint8_t	done;					//	是否rebalance完成
>	}
>	```

外界调用`slab_rebalance_thread`线程时，先设定要上述结构体的初始值，然后再释放信号量`slab_rebalance_cond`来调用此线程。

3.`slab_rebalance_thread`线程对外提供方法和外界调用
-----------------------------------
**3.1 `slab_rebalance_thread`对外提供接口**

`slab_rebalance_thread`线程由对外提供的接口`slabs_reassign`调用`do_slabs_reassign`函数提供对外服务。

当`src=-1`时，`do_slabs_reassign`调用`slab_reassign_pick_any`函数来轮询来查找合适的`src_slabclass`来提供内存空间。

外界对`slab_reassign`的调用有以下几处:

| 文件名	| 调用方式	| 函数  | 备注		|
|-----		|-------	| ------ | ------	|
| items.c	| `slabs_reassign(-1, orig_id);`| `lru_pull_tail` | 轮询查找`slabclass`来充当`src_slabclass`|
| items.c | `slabs_reassign(src, dst)` | `lru_maintainer_thread` | 通过每个`slabclass`的状态来进行判断 |
| memcached.c	 | `rv=slabs_reassign(src, dst)`| `process_command` | 通过外界命令调用`slab_rebalance`线程 |

**3.2 `slabs_reassign `的代码结构**

>```
>	enum reassign_result_type slabs_reassign(int src, int dst) {
>		enum reassign_result_type ret;
>		// 此处应当注意,通过`slabs_rebalance_lock`来防止线程二次进入
>		if (pthread_mutex_trylock(&slabs_rebalance_lock) != 0) {
>			return REASSIGN_RUNNING;
>		}
>		ret = do_slabs_reassign(src, dst);
>		pthread_mutex_unlock(&slabs_rebalance_lock);
>		return ret;
>	}
>```

**3.3 `do_slabs_reassign`的代码结构**

>```
>	static enum reassign_result_type do_slabs_reassign(int src, int dst) {
>		// 此函数用于设置slab_rebal的结构体的值
>		bool nospare = false;
>		// 使用slab_rebalance_signal是否为0来判断slab_rebalance_thread线程是否运行中
>		if (slab_rebalance_signal != 0)
>			return REASSIGN_RUNNING;
>		
>		if (src == dst)
>			return REASSIGN_SRC_DST_SAME;
>		
>		if (src == -1) {
>			// 轮流坐庄的模式来寻找合适的src
>			src = slabs_reassign_pick_any(dst);
>		}
>	
>		if (src < SLAB_GLOBAL_PAGE_POOL || src > power_largest ||
>			 dst < SLAB_GLOBAL_PAGE_POOL || dst > power_largest)
>			return REASSIGN_BADCLASS;
>
>		pthread_mutex_lock(&slabs_lock);
>		if (slabclass[src].slabs < 2)
>			nospare = true;
>		pthread_mutex_unlock(&slabs_lock);
>		if (nospare)
>			return REASSIGN_NOSPARE;
>		
>		slab_rebal.s_clsid = src;
>		slab_reabl.d_clsid = dst;
>	
>		slab_rebalance_signal = 1;
>		pthread_cond_signal(&slab_rebalance_cond);
>
>		return REASSIGN_OK;
>	}
>```

**3.4 `slabs_reassign_pick_any`函数的代码结构**
>```
>	static int slabs_reassign_pick_any(int dst) {
>		static int cur = POWER_SMALLEST - 1;
>		int tries = power_largest - POWER_SMALLEST + 1;
>		for (; tries > 0; tries--) {
>			cur++;
>			if (cur > power_largest)
>				cur = POWER_SMALLEST;
>			if (cur == dst)
>				continue;
>			if (slabclass[cur].slabs > 1) {
>				return cur;
>			}
>		}
>		return -1;
>	}
>```

由上述代码显示可以，程序采用轮流坐庄的模式进行选择源`slabclass`。

4.`slab_rebalance_thread`中调用的函数代码结构
------------------------------------------
**4.1 `slab_rebalance_start`函数的代码结构**

>	```
>	static int slab_rebalance_start(void) {
>		slabclass_t *s_cls;
>		int no_go = 0;
>	
>		pthread_mutex_lock(&slabs_lock);
>		// 首先判断源slabclass和目的slabclass是否符合要求
>		if (slab_rebal.s_clsid < SLAB_GLOBAL_PAGE_POOL ||
>			 slab_rebal.s_clsid > power_largest ||
>			 slab_rebal.d_clsid < SLAB_GLOBAL_PAGE_POOL ||
>			 slab_rebal.d_clsid > power_largest ||
>			 slab_rebal.s_clsid == slab_rebal.d_clsid)
>			no_go = -2;
>		
>		s_cls = &slabclass[&slab_rebal.s_clsid];
>		
>		if (!grow_slab_list(slab_rebal.d_clsid)) {
>			no_go = -1;
>		}
>
>		if (s_cls->slabs < 2) {
>			no_go = -3;
>		}
>	
>		if (no_go != 0) { //判断是否满足转移条件
>			pthread_mutex_unlock(&slabs_lock);
>			return no_go;		// Should use a wrapper function
>		}
>		
>		/**
>		 * Always kill the first available slab page as it is most likely to
>		 *	contain the oldest items.
>		 */
>		// 总是那第一个slab page用于转移，它应该包含更老的item
>		// 设定转移的slab_rebal结构中开始和结束以及当前的slab item位置
>		slab_rebal.slab_start = s_cls->slab_list[0];
>		slab_rebal.slab_end = (char *)slab_rebal.slab_start +
>				(s_cls->size * s_cls->perslab);
>		slab_rebal.slab_pos = slab_rebal.slab_start;
>		slab_rebal.done = 0;	// 用于判断转移是否完成
>		// Don't need to do chunk move work if page is in global pool
>		if (slab_rebal.s_clsid == SLAB_GLOBAL_PAGE_POOL) {
>			slab_rebal.done = 1;
>		}
>		// 设置开始调整slabclass item的信号
>		slab_rebalance_signal = 2;
>	
>		pthread_mutex_unlock(&slab_lock);	
>	
>		return 0;
>	}
>	```

此函数用于判断`slab_rebalance_thread`线程所接受的`slab_rebal`结构体中的源`slabclass`和目的`slabclass`是否符合要求。

**4.2 `slab_rebalance_move`函数的代码结构**

此函数的步骤如下:

* 先取出来`item`的`it_flags`值。
* 根据`item`中`it_flags`的值来设置`status`的值。
* 根据`status`的值来决定如何转移`item`。

*PS:由于代码太长，因此分文档进行介绍。*

**4.3 `slab_rebalance_finish`函数的代码结构**

>	```
>	static void slab_rebalance_finish(void) {
>		// 此函数氛围两部分：
>		// 1.设置源slabclass和目的slabclass中slab_list值。
>		// 2.设置slab_rebal结构体中的值用于返回。
>		slabclass_t *s_cls;
>		slabclass_t *d_cls;
>		int x;
>		uint32_t rescues;
>		uint32_t evictions_nomem;
>		uint32_t inline_reclaim;
>		uint32_t chunk_rescues;
>		uint32_t busy_deletes;
>		
>		pthread_mutex_lock(&slab_lock);
>		
>		s_cls = &slabclass[slab_rebal.s_clsid];
>		d_cls = &slabclass[slab_rebal.d_clsid];
>
>		/** At this point the stolen slab is completely clear.
>		 *  We always kill the "first"/"oldest" slab page in the slab_list,
>		 *  so shuffle the page list backwards and decrement.
>		 */
>		s_cls->slabs--;
>		for (x = 0; x < s_cls->slabs; x++) {
>			s_cls->slab_list[x] = s_cls->slab_list[x+1];
>		}
>
>		d_cls->slab_list[d_cls->slabs++] = slab_rebal.slab_start;
>		/* Don't need to split the page into chunks if we're just storing it */
>		if (slab_rebal.d_clsid > SLAB_GLOBAL_PAGE_POOL) {
>			memset(slab_rebal.slab_start, 0, (size_t)settings.slab_page_size);
>			split_slab_page_into_freelist(slab_rebal.slab_start, slab_rebal.d_clsid);
>		} else if (slab_rebal.d_clsid == SLAB_GLOBAL_PAGE_POOL) {
>			// mem_malloc'ed might be higher than mem_limit
>			mem_limt_reached = false;
>			memory_release();
>		}
>
>		slab_rebal.busy_loops = 0;
>		slab_rebal.done = 0;
>		slab_rebal.s_clsid = 0;
>		slab_rebal.d_clsid = 0;
>		slab_rebal.slab_start = NULL;
>		slab_rebal.slab_end = NULL;
>		slab_rebal.slab_pos = NULL;
>		evictions_nomem = slab_rebal.evictions_nomem;
>		inline_reclaim = slab_rebal.inline_reclaim;
>		rescues = slab_rebal.rescues;
>		chunk_rescues = slab_rebal.chunk_rescues;
>		busy_deletes = slab_rebal.busy_deletes;
>		slab_rebal.evictions_nomem = 0;
>		slab_rebal.inline_reclaim = 0;
>		slab_rebal.rescues = 0;
>		slab_rebal.chunk_rescues = 0;
>		slab_rebal.busy_deletes = 0;
>
>		slab_rebalance_signal = 0;
>
>		pthread_mutex_unlock(&slab_lock);
>	}
>	```




