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

## `LRU`链表结构
先看一下`memcached`的整个内存结构图示，主要从三个方面来描述其内存空间结构。

* 从`hash`表角度 ----- 查找`item`。
* 从`LRU`链表角度 ---- 维护`item`的生命周期。
* 从`slabclass`管理角度 ---- 管理`item`。

![memcached内存图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_lru/memcached_memory.png)

