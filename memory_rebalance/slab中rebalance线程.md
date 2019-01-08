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
`slab_rebalance_thread`线程由对外提供的接口`slab_reassign`调用`do_slabs_reassign`函数提供对外服务。

当`src=-1`时，`do_slabs_reassign`调用`slab_reassign_pick_any`函数来轮询来查找合适的`src_slabclass`来提供内存空间。

外界对`slab_reassign`的调用有以下几处:

| 文件名	| 调用方式	| 备注		|
|-----		|-------	|------	|
| items.c	| `slabs_reassign(-1, orig_id);` | 轮询查找`slabclass`来充当`src_slabclass`|
| memcached.c	 | `rv=slabs_reassign(src, dst)`| 通过外界命令调用`slab_rebalance`线程 |

4.`slab_rebalance_thread`中调用的函数代码结构
------------------------------------------
**4.1 `slab_rebalance_start`函数的代码结构**

>	```
>	
>	```
