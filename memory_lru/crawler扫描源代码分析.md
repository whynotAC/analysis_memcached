crawler扫描源代码分析
==================================
1 crawler线程启动/暂停等相关操作
-----------------------------------------
在介绍`crawler`线程源代码之前，我们先看一下`crawler`扫描线程如何进行启动以及初始化其所使用的全局变量。

### 启动函数源代码
```
/* Lock dance to "block" until thread is waiting on its condition:
 * caller locks mtx. caller spawns thread.
 * thread blocks on mutex.
 * caller waits on condition, releases lock.
 * thread gets lock, send signal.
 * caller can't wait, as thread has lock.
 * thread waits on condition, release lock
 * caller makes on condition, releases lock
 * caller makes on condition, gets lock.
 * caller immediately release lock.
 * thread is now safely waiting on condition before the caller returns
 */
int start_item_crawler_thread(void) {
	int ret;
	
	if (settings.lru_crawler)			// 判断是否需要开启crawler线程
		return -1;
	pthread_mutex_lock(&lru_crawler_lock);
	do_run_lru_crawler_thread = 1;	// 标示crawler线程启动
	if ((ret = pthread_create(&item_crawler_tid, NULL, item_crawler_thread, NULL)) != 0) {
		fprintf(stderr, "Can't create LRU crawler thread: %s\n", strerror(ret));
		pthread_mutex_unlock(&lru_crawler_lock);
		return -1;
	}
	/* Avoid returning until the crawler has actually started */
	pthread_cond_wait(&lru_crawler_cond, &lru_crawler_lock);
	pthread_mutex_unlock(&lru_crawler_lock);
	
	return 0;
}
```
上述代码是`cralwer`线程的启动代码，其在`memecached.c`文件中`main`函数中被调用或者`worker`线程中的`process_command`函数调用。

### 关闭线程源代码
```
int stop_item_crawler_thread(void) {
	int ret;
	pthread_mutex_lock(&lru_crawler_lock);
	do_run_lru_crawler_thread = 0;	// 标示crawler线程结束
	pthread_cond_signal(&lru_crawler_cond);
	pthread_mutex_unlock(&lru_crawler_lock);
	if ((ret = pthread_join(item_crawler_tid, NULL)) != 0) {
		fprintf(stderr, "Failed to stop LRU crawler thread: %s\n", strerror(ret));
		return -1;
	}
	settings.lru_crawler = false;
	return 0;
}
```
上述代码是`crawler`线程的结束代码，其在`memcached.c`文件中`worker`线程调用的`process_command`函数中调用。

### 暂停/恢复现成的源代码
```
/* If we hold this lock, crawler can't wake up or move */
void lru_crawler_pause(void) {
	pthread_mutex_lock(&lru_crawler_lock);
}

void lru_crawler_resume(void) {
	pthread_mutex_unlock(&lru_crawler_lock);
}
```

### crawler初始化函数源代码
```
int init_lru_crawler(void *arg) {
	if (lru_crawler_initialized == 0) {
#ifdef EXTSTORE
		storage = arg;
#endif
		if (pthread_cond_init(&lru_crawler_cond, NULL) != 0) {
			fprintf(stderr, "Can't initialize lru crawler condition\n");
			return -1;
		}
		pthread_mutex_init(&lru_crawler_lock, NULL);
		active_crawler_mod.c.c = NULL;
		active_crawler_mod.mod = NULL;
		acitve_crawler_mod.data = NULL;
		lru_crawler_initialized = 1;
	}
	return 0;
}
```


2 crawler线程调用关系
-----------------------------------------
3 crawler使用的结构体
-----------------------------------------
4 crawler源代码解析
-----------------------------------------
5 crawler的工作方式以及其对应的函数
-----------------------------------------
6 总结
-----------------------------------------