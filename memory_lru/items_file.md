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