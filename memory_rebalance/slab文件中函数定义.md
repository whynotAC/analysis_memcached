#1 slab文件中对外接口函数

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
