memcached内存初始化过程
======================================

memcached是用C语言编写而成，学习memcached时应该从struct结构体开始了解其整体架构。

几个重要的结构体
----------------------------------------------

memcached中使用的关键结构体slabclass_t，用于管理已经分配给slab的内存。

>		typedef struct {
>			unsigned int size; 			// item区域的大小
>			unsigned int perslab;		// chunk/slab中item的个数
>		
>			void *slots;					// 用于链接空闲的item
>			unsigned int sl_curr;		// 用于记录空闲item的个数多少
>
>			unsigned int slabs;			// 用于记录分配多少个slab/chunk
>
>			void **slab_list;			// 用于记录每个chunk/slab的位置指针
>			unsigned int list_size;		// slab_list的大小，当list_size == slabs时，需要需要扩容
>		
>			size_t requested;			// slabclass_t使用的字节数
>		} slabclass_t;

memcached中使用的关键结构体item，用于管理key-value数据的结构体。

>		typedef struct _stritem {
>			// Proctected by LRU locks
>			struct _stritem *next;
>			struct _stritem *prev;
>			// Reset are protected by an item lock
>			struct _stritem *h_next;		// hash chain next
>			rel_time_t time;					// least recent access
>			rel_time_t exptime;				// expire time
>			int nbytes;						// size of data
>			unsigned short refcount;		// 引用计数器，当线程操作该item时，refcount会加1
>			uint8_t nsuffix;					// length of flags-and-length string
>			uint8_t it_flags;				// ITEM_* above
>			uint8_t slabs_clsid;			// which slab class we're in
>			uint8_t nkey;						// key length, w/terminating null and padding
>			/*
>			 * this add type prevents type-punning isssuses when we do.
>			 * the little shuffle to save space when not using CAS.
>			 */
>			union {
>				uint64_t cas;
>				char end;
>			} data[];
>			/*
>			 * if it_flags & ITEM_CAS we have 8 bytes CAS
>			 *	then null-terminated key
>			 *	then " flags length\r\n" (no terminating null)
>			 * then data with terminating \r\n (no terminating null; it's binary!)
>			 */
>		} item;

上面就是在初始化过程中使用的主要结构体。

内存模型图示
--------------------------------------------

![初始化后的内存图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_init/memcached_init_memory.png)


初始化参数
--------------------------------------------

对于结构体settings只选择初始化需要的参数进行说明:

>		struct settings {
>			size_t maxbytes;			// memcached所占用的空间大小
>			int	verbose;				// 查看内存的分配情况，可将此值设置为2
>			double factor;			// item的成长因子
>			int chunk_size;			// 最小的item中数据所占的字节数
>
>			bool use_cas;
>			int slab_chunk_size_max;	// 最大chunk的大小，默认为slab_page_size/2
>			int slab_page_size;		// 默认划分内存空间的单位大小，默认为1M
>			bool slab_reassign;		// 是否对内存进行紧缩，减少浪费的空间
>		};

