memcached哈希表初始化过程
==============================================
memcached中哈希表主要用于查找memcached中的item，以尽快查找到对应的item。hash表的主要文件有以下几个:

1. hash.h/c
2. assoc.h/c
3. jenkins_hash.h/c
4. murmur3_hash.h/c
5. thread.c 

几个重要的结构体
-----------------------------------------

在文件`assoc.c`中，`primary_hashtable`和`old_hashtable`为memorycached的hash表存放结构。

>	// Main hash table. This is where we lock except during expansion
>	
>	static item** primary_hashtable = NULL;
>
>	// Previous hash table. During expansion, we lock here for keys that haven't been
>		moved over to the primary yet.
>
>	static item** old_hashtable = NULL;

表的大小通过变量`hashpower`来设定`hash`表的大小，其默认大小为`HASHPOWER_DEFAULT`。通过宏`hashsize`和`hashmask`来计算`hash`表大小、以及`hash`表的掩码。

>	#define hashsize(n) ((ub4)1<<(n))
>
>	#define hashmask(n) (hashsize(n)-1)

`hash`表在扩展时，通过计算需要访问的`hash bucket`与`expand_bucket`进行大小比较来决定访问`old_hashtable`还是访问`primary_hashtable`。其代码如下:

>	```
>	if (expanding && 	// 是否在扩展hash表
>		(oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket) {
>		it = old_hashtable[oldbucket];
>	} else {
>		it = primary_hashtable[hv & hashmask(hashpower)];
>	}
>	```

`hash`表的扩展是按照左移一位来扩展，即扩展后的大小为扩展前的2倍。其代码如下(`assoc_expand`函数中):

>	```
>	old_hashtable = promary_hashtable;
>	
>	primary_hashtable = (item **)calloc(hashsize(hashpower + 1), sizeof(void *));
>	if (primary_hashtable) {
>		hashpower++;
>		expanding = true;
>		expand_bucket = 0;
>	} else {
>		primary_hashtble = old_hashtable;
>	}
>	```

访问`hash`表时需要通过`hash`锁来查看对应的`hash`表队列。`hash`锁的结构在thread.c文件中定义，如下:

>	```
>	static pthread_mutex_t *item_locks;
>	/* size of the item lock hash table*/
>	unsigned int item_lock_count;
>	```

`hash`函数有两种，分别为`JEKINS_HASH`和`MURMUR3_HASH`。这两种`hash`函数分别在`jenkins_hash.h\c`和`murmur3_hash.h\c`中定义。

**重要一点：**

>	`hash`表锁不会随着`hash`表的扩展而变化，即程序启动后，`hash`表锁的大小就确定了。
>
>	```
>		void *item_trylock(uint32_t hv) {
>			pthread_mutex_t *lock = &item_locks[hv & hashmask(item_lock_hashpower)];
>			if (pthread_mutex_trylock(lock) == 0) {
>				return lock;
>			}
>		}
>	```
>
>	由上面一段代码可以看出，当`hash`表扩展后，多个`hash`表使用同一个`hash`锁。

HASH表扩展线程
---------------------------------
`memcached`中的`hash`表扩展是由`hash`表维护线程操作的。其函数如下:

>	```
>	static void *assoc_maintenance_thread(void *arg) {
>		
>		mutex_lock(&maintenance_lock);
>		while (do_run_maintenance_thread) {
>			int ii = 0;
>		
>			for (ii = 0; ii < hash_bulk_move && expanding; ++ii) {
>				item *it, *next;
>				unsinged int bucket;
>				void *item_lock = NULL;
>			
>				if ((item_lock = item_trylock(expand_bucket))) {
>					for (it = old_hashtable[expand_bucket]; NULL != it; it = next) {
>						next = it->h_next;
>						bucket = hash(ITEM_key(it), it->nkey) & hashmask(hashpower);
>						it->h_next = primary_hashtable[bucket];
>						primary_hashtable[bucket] = it;
>					}
>					
>					old_hashtable[expand_bucket] = NULL;
>
>					expand_bucket++;			// 扩展下一个`hash`表
>					if (expand_bucket == hashsize(hashpower - 1)) {
>						expanding = false;
>						free(old_hashtable);
>					}
>				} else {
>					usleep(10 * 1000);
>				}
>				
>				if (item_lock) {
>					item_trylock_unlock(item_lock);
>					item_lock = NULL;
>				}
>			}
>			
>			if (!expanding) {
>				started_expanding = false;
>				pthread_cond_wait(&maintenance_cond, &maintenance_lock);
>				
>				pause_threads(PAUSE_ALL_THREADS);
>				assoc_expand();		//	为hash表扩展做准备
>				pause_threads(RESUME_ALL_THREADS);
>			}
>		}
>		return NULL;
>	}
>	```

由上面的代码可以，`hash`表扩展线程由`maintenance_cond`条件控制其运行，其设置的函数为`assoc_start_expand`。扩展条件如下:

>	```
>	if (curr_items > (hashsize(hashpower) * 3) / 2 &&
>			hashpower < HASHPOWER_MAX) {	// 当前存放的item足够多，并且hash表为扩展到最大
>		started_expadning = true;
>		pthread_cond_signal(&maintenance_cond);
>	}
>	```

HASH表中涉及到的setting
-------------------------------------------------
`hash`表在初始化过程中需要设置的`settings`中的变量有以下几个:

>	```
>	char *hash_algorithm;	// hash表使用的HASH函数
>	int	hashpower_init;		// hash表初始化的大小
>	int num_threads;			// 工作线程数，由这个值来决定hash锁表的大小
>	```


HASH表初始化过程
-------------------------------------------------
`hash`表在`memcached.c`中初始化，其过程大致如下:

>	```
>	bool start_assoc_maint = true;
>	#ifdef EXTSTORE
>	void *storage = NULL;
>	#endif
>
>	settings_init();
>
>	enum hashfunc_type hash_type = MURMUR3_HASH; // 默认HASH函数
>	
>	if (hash_init(hash_type) != 0) {	// 初始化HASH算法
>		...
>	}
>
>	assoc_init(settings.hashpower_init);	 // 初始化HASH表大小
>	// 初始化HASH锁表的大小
>	#ifdef EXTSTORE
>	memcached_thread_init(settings.num_threads, storage);
>	#else
>	memcached_thread_init(settings.num_threads, NULL);
>	#endif
>
>	// 初始化HASH表维护线程
>	if (start_assoc_maint && start_assoc_maintenance_thread() == -1) {
>		...
>	}
>	
>	// 终止HASH表维护线程
>	stop_assoc_maintenance_thread();
>	```

![HASH表初始化过程图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_hash/hash_init.png)

HASH表扩展前后的图示
-------------------------------------------------

![HASH表扩展访问图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_hash/hash_visit.png)