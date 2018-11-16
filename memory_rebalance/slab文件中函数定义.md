slabs文件分析
===========================

1 slabs文件中对外接口函数
----------------------------

| 函数名	|	函数定义	|	作用	|	备注  |
| ---- 	| ----- 	| ---- 	| ----- 	  |
| `slabs_init` | `void slabs_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes)` | 用于初始化slabclass结构体 | 无 |
| `slabs_prefill_global` | `void slabs_prefill_global(void)` | 按照`slab_page_size`切分申请的内存空间，存放在`slabclass[0]`中 | 无 |
| `slabs_clsid` | `unsigned int slabs_clsid(const size_t size)` | 给定对象大小，查找其对应的`slabclass`的编号 | 无 |
| `slabs_alloc` | `void *slabs_alloc(const size_t size, unsigned int id, uint64_t *total_bytes, unsigned int flags)` | 锁定`slabs_lock`，调用`do_slabs_alloc`分配内存空间 |无|
| `slabs_free` | `void slabs_free(void *ptr, size_t size, unsigned int id)` | 锁定`slabs_lock`，调用`do_slabs_free`释放申请的内存空间 | 无 |
| `slabs_adjust_mem_requested` | `void slabs_adjust_mem_requested(unsigned int id, size_t old, size_t ntotal)` | 根据`id`来调整`slabclass`的`requested`字段值 | 无 |
| `slabs_adjust_mem_limit` | `bool slabs_adjust_mem_limit(size_t new_mme_limit)` | 锁定`slabs_lock`，调用`do_slabs_adjust_mem_limit`来调整内存大小 | 无 |
| `get_stats` | `bool get_stats(const char *state_type, int nkey, ADD_STAT add_stats, void *c)` | 用与查看`slabclass`的内存空间使用情况 | 无 |
| `fill_slab_stats_automove` | `void fill_slab_stats_automove(slab_stats_automove *am)`| 锁定`slabs_lock`，并根据`slabclass`情况填充`am`数组 | 此函数可能于内存管理线程有关 |
| `global_page_pool_size` | `unsigned int global_page_pool_size(bool *mem_flag)` | 用于检测`mem_malloced`是否超过`mem_limit` | 无 |
| `slabs_stats` | `void slabs_stats(ADD_STAT add_stats, void *c)` | 锁定`slabs_lock`，调用`do_slabs_stats`函数填充`add_stats`参数 | 此函数可以用于观察`slabclass`内存情况 |
| `slabs_available_chunks` | `unsigned int slabs_available_chunks(unsigned int id, bool *mem_flag, uint64_t *total_bytes, unsigned int *chunks_preslab)` | 其用于观察`slabclass`整体内存使用情况 | 这个仅为标记统计 |
| `slabs_mlock` | `void slabs_mlock(void)` | 用于包装锁定`slabs_lock`的函数 | 无 |
| `slabs_munlock` | `void slabs_munlock(void)` | 用于包装解锁`slabs_lock`的函数 | 无 |
| `start_slab_maintenance_thread` | `int start_slab_maintenance_thread(void)` | 用于启动`slabclass`调整线程的函数 | 无 |
| `stop_slab_maintenance_thread` | `void stop_slab_maintenance_thread(void)` | 用于关闭`slabclass`调整线程的函数 | 无 |
| `slabs_reassign` | `enum reassign_result_type slabs_reassign(int src, int dst)` | 调整`id`为`src/dst`的`slabclass`的函数 | 其会抢占`slabs_rebalance_lock`锁 | 
| `slabs_rebalancer_pause` | `void slabs_rebalancer_pause(void)` | 暂停`slabclass`调整线程 | 无 | 
| `slabs_rebalancer_resume` | `void slabs_rebalancer_resume(void)` | 恢复`slabclass`调整线程运行 | 无 | 

2 slabs文件中全局变量
---------------------------
**slabs文件中重要结构体**

>		// powers-of-N allocation structures
>		typedef struct {
>			unsigned int size;		//	sizeof of items
>			unsigned int perslab;	// 	how many items per slab
>			
>			void *slots;				// list of items ptrs
>			unsigned int sl_curr;	//	total free items in list
>
>			unsigned int slabs;		//	how many slabs were allocated for this class
>			
>			void **slab_list;		//	array of slab pointers
>			unsigned int list_size;	//	size of prev array
>
>			size_t requested;		//	The number of requested bytes
>		} slabclass_t;
>
>		#define DEFAULT_SLAB_BULK_CHECK 1
>
>		enum move_status {
>			MOVE_PASS=0, MOVE_FROM_SLAB, MOVE_FROM_LRU, MOVE_BUSY, MOVE_LOCKED
>		};
>
>		#define SLAB_MOVE_MAX_LOOPS 1000

**slabs文件中全局变量**

|	变量名	|	定义	|	是否为静态变量	|	作用    |  备注   |
| ------- | ------- | ------------- | --------- | ------ |
| `slabclass` | `slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES]` | 是 | 用于存放所有`slabclass`，管理内存空间 | 无 |
| `mem_limit` | `size_t mem_limit = 0` | 是 | 用于记录整个内存空间的大小 | 无 |
| `mem_malloced` | `size_t mem_malloced = 0` | 是 | 用于记录已用内存空间的大小 |无|
| `mem_limit_reached` | `bool mem_limit_reached = false` | 是 | 用于标记已用内存是否超过`mem_limit`的值 | 无 |
| `power_largest` | `int power_largest` | 是 | 用于记录最大的`slabclass`的下标|无|
| `mem_base` | `void *mem_base = NULL` | 是 | 用于指向分配内存空间的基地址 | 无 |
| `mem_current` | `void *mem_current = NULL` | 是 | 用于记录当前可分配空间的开始地址| 无 |
| `mem_avail` | `size_t mem_avail = 0` | 是 | 用于记录已分配空间的大小 | 无 |
| `slabs_lock` | `pthread_mutex_t slabs_lock = PTHREAD_MUTEX_INITIALIZER` |是| 用于控制访问`slabclass`的互斥量 | 无 |
| `slabs_rebalance_lock` | `pthread_mutex_t slabs_rebalance_lock = PTHREAD_MUTEX_INITIALIZER` | 是 | 用于控制`slab_rebalance_thread`线程行为的互斥量|无|
| `slab_rebalance_cond` | `pthread_cond_t slab_rebalance_cond = PTHREAD_COND_INITIALIZER` | 是 | 用于控制`slab_rebalance_thread`线程的行为的信号量| 无 |
| `do_run_slab_thread` | `volatile int do_run_slab_thread = 1` | 是 | 无 | 无 | 
| `do_run_slab_rebalance_thread` | `volatile int do_run_slab_rebalance_thread = 1` | 是 | 用于控制`slab_rebalance_thread`是否停止运行 | 无 |
| `slab_bulk_check` | `int slab_bulk_check = 1` | 否 |用于控制`slab_rebalance_thread`线程每次移动`item`的个数 | 无 |
| `rebalance_tid` | `pthread_t rebalance_tid` | 是 | 用于记录`slab_rebalance_thread`线程的`ID` | 无 |

3 slabs文件中静态函数
-----------------------------------

