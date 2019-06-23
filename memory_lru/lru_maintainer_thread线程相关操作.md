`lru_maintainer_thread`线程相关操作
======================================
## **LRU维护线程具有以下四部分功能**

* 保持各个`slabclass`对应的`lru`链表的长度。
* 维护`worker`线程的`lru_bump_buf`结构。
* 根据`crawler_expried_data`结构，调用`LRU`过期扫描线程。
* 根据`itemstats`和`slabclass`的内容，来调用`slab_reassign`函数来进一步调用`slab_rebalance_thread`线程。

## `lru_maintainer_thread`线程相关操作

### 线程初始化
此函数由`memcached.c`中`main`函数调用。

```
static int lru_maintainer_lock = 0;

int init_lru_maintainer(void) {
	if (lru_maintainer_initialized == 0) {
		pthread_mutex_init(&lru_maintainer_lock, NULL);
		lru_maintainer_initialized = 1;
	}
	return 0;
}
```
由上述函数可以看出，此函数用于初始化`lru_maintainer_lock`和`lru_maintainer_lock`两个全局变量，其中`lru_maintainer_lock`后面的过程都需要使用。

### 线程启动
知道了`LRU`维护线程初始化过程，下面看一下其启动的过程。其函数由`memcached.c`中`main`函数调用。

```
static volatile int do_run_lru_maintainer_thread = 0; // 用于判断LRU维护线程是否在运行
static pthread_t lru_maintainer_tid; // LRU维护线程的ID 

int start_lru_maintainer_thread(void *arg) {
	int ret;
	
	pthread_mutex_lock(&lru_maintainer_lock);
	do_run_lru_maintainer_thread = 1;
	settings.lru_maintainer_thread = true;
	if ((ret = pthread_create(&lru_maintainer_tid, NULL,
			lru_maintainer_thread, arg)) != 0) {
		fprintf(stderr, "Can't create LRU maintainer thread: %s\n",
			strerror(ret));
		pthread_mutex_unlock(&lru_maintainer_lock);
		return -1;
	}
	pthread_mutex_unlock(&lru_maintainer_lock);
	
	return 0;
}
```
由上述函数可以看出，函数`start_lru_maintainer_thread`启动`LRU`维护线程时，设置`do_run_lru_maintainer_thread`和`settings.lru_maintainer_thread`的值。

### 线程暂停与恢复
这两个函数较为简单，主要用于线程的暂停与恢复。这两个线程使用`lru_maintainer_lock`来控制线程的行为。

```
void lru_maintainer_pause(void) {
	pthread_mutex_lock(&lru_maintainer_lock);
}

void lru_maintainer_resume(void) {
	pthread_mutex_unlock(&lru_maintainer_lock);
}
```

### 线程关闭
当程序关闭时将调用此函数(通过`do_run_lru_maintainer_thread`变量来控制线程退出)，用于终止`LRU`维护线程。

```
int stop_lru_maintainer_thread(void) {
	int ret;
	pthread_mutex_lock(&lru_maintainer_lock);
	/* LRU thread is a sleep loop, will die on its own */
	do_run_lru_maintainer_thread = 0;
	pthread_mutex_unlock(&lru_maintainer_lock);
	if ((ret = pthread_join(lru_maintainer_tid, NULL)) != 0) {
		fprintf(stderr, "Failed to stop LRU maintainer thread: %s\n", strerror(ret));
		return -1;
	}
	settings.lru_maintainer_thread = false;
	return 0;
}
```

## `memcached`内存结构
先看一下`memcached`的整个内存结构图示，主要从三个方面来描述其内存空间结构。

* 从`hash`表角度 ----- 查找`item`。
* 从`LRU`链表角度 ---- 维护`item`的生命周期。
* 从`slabclass`管理角度 ---- 管理`item`。

![memcached内存图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_lru/memcached_memory.png)

从上图可以看出:

1. `slabclass`通过`slab_list`来管理`slabclass`申请的`page/chunk`(1M大小)。
2. `slabclass`通过`slots`来管理空闲的`item`(通过`item`的`prev`和`next`指针)链表。
3. `memcached`的`slabclass`数组个数最大为`MAX_NUMBER_OF_SLAB_CLASSES`(64)个。
4. `memcached`每个`slabclass`对应4种`LRU`链表。
5. `memcached`的`LRU`链表有四种，分别为: `HOT_LRU`、`WARM_LRU`、`COLD_LRU`、`TEMP_LRU`。
6. `memcached`中使用`heads`和`tails`来管理`LRU`,`heads`和`tails`的大小为`LARGEST_ID`(256)。
7. `slabclass`对应的`heads`和`tails`的下表为: `slabid | HOT_LRU`、`slabid | WARM_LRU`、`slabid | COLD_LRU`、 `slabid | TEMP_LRU`。
8. `memcached`中`LRU`链表通过`item`中的`prev`和`next`来形成`LRU`链表。
9. `memcached`中查找`item`时，使用`primary_hashtable`管理`hash`桶。
10. `hash`桶中的链表采用拉链式，通过`item`的`h_next`指针来形成`hash`链表。
11. `memcached`中的整个内存使用`mem_base`和`mem_current`来管理整个申请的内存。
12. `slabclass`数组中下表为0的`slabclass`为`SLAB_GLOBAL_PAGE_POOL`(内存管理池)。
13. 每个结构都有对应的锁结构，防止多线程重入。`primary_hashtable`的锁全局变量为`item_locks`数组,`slabclass`数组的锁全局变量为`slabs_lock`、`LRU`的锁全局变量为`lru_locks`数组。
14. `item_locks`锁数组的大小为2的`item_lock_hashpower`次方大小。
15. `lru_locks`锁数组的大小为`POWER_LARGEST`(256)个。

## `Segmented LRU`结构
每个`item`有两种状态来表示活跃状态:

* `FETCHED`: 当某个`item`被访问时设置。
* `ACTIVE`: 当某个`item`被再次访问时设置。当`item`被检测到或者转移队列时移除其状态。

![segmented lru队列转换图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_lru/segmented_lru.jpeg)

对于上图中队列的解释如下:

1. `HOT lru`充当试用队列，因为`item`可能表现出强烈的时间局部性或非常短的TTL。当`item`到达`HOT`队列尾时:当`item`处于`active`状态时，转移到`WARM`队列中(3);否则，`item`转移到`COLD`队列中(5)。
2. `WARM lru`是主要的`LRU`扫描队列。只有至少被访问两次的`item`才能够进入`WARM`队列。`WARM`队列中的`item`有更多的机会完成其生命周期，同时也减少了锁的争用。当`item`到达`WARM`队列尾时:`item`处于`active`状态时，将其放在`WARM`队列头部(4);否则,将`item`转移到`COLD`队列中(7)。
3. `COLD lru`中的`item`处于不活跃状态。非活跃`item`从`HOT`(5)和`WARM`(7)转移到`COLD`队列中。一旦内存满时，`item`将从`COLD`队列尾部逐出。当`item`变成活跃状态，它将异步转移到`WARM`队列中。
4. `TEMP lru`保存新的`item`处于非常短的TTL(2)。在`TEMP`队列中的`item`不会流转到其他`LRU`队列中。
5. `HOT`队列和`WARM`队列会限制其内存使用比例，但是`COLD`队列和`TEMP`队列不会被限制。
6. 根据`COLD`尾部`item`的过期时间，来限制`HOT`和`WARM`尾部`item`的过期时间。
