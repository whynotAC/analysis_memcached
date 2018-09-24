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

PS:对于`item`中的`it_flags`字段进行着重介绍一下，这个字段主要用于标示`item`所处的状态。
其对应的状态下有如下几种:

>		#define ITEM_LINKED 	1  // 用于标示item是否处于LRU链表和Hash表中
>		#define ITEM_CAS			2  // 用于标示item是否开启CAS
>		#define ITEM_SLABBED	4 	// 用于标示item是否位于slots队列中，即空闲队列
>		#define ITEM_FETCHED	8	// 待定
>		#define ITEM_ACTIVE		16	// 待定
>		#define ITEM_CHUNKED	32	// 当item后面存储的是大对象(超过settings.slab_chunk_size_max)时，需要该标记标示
>		#define ITEM_CHUNK		64	// 待定
>		#define ITEM_HDR			128	// 待定 

对`item`中`data`字段进行分析，来介绍一下`item`存放一个数据时的内存构造情况
![item的内存结构图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_init/item_struct.png)

为了获取上面内存结构中的字段值，`memcached`提供了一系列的函数来获取其对应的值
> ``` 	
> 	#define ITEM_get_cas(item) (((item)->it_flags & ITEM_CAS) ? \
>			(item)->data->cas : (uint64_t)0)
> 
> 	#define ITEM_set_cas(item, v) { \
> 		if ((item)->it_flags & ITEM_CAS) { \
> 				(item)->data->cas = v;	\
> 		}	\
> 	}
> 
> 	#define ITEM_key(item) (((char*)&((item)->data)) \
> 				+ (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))
> 
> 	#define ITEM_suffix(item) ((char*) &((item)->data) + (item)->nkey + 1 \
> 				+ (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))
> 
> 	#define ITEM_data(item) ((char*) &((item)->data) + (item)->nkey + 1 \
> 				+ (item)->nsuffix + (item)->nbytes \
> 				+ (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))
> 
> 	#define ITEM_ntotal(item) (sizeof(struct _stritem) + (item)->nkey + 1 \
> 				+ (item)->nsuffix + (item)->nbytes \
> 				+ (((item)->it_flags & ITEM_CAS) ? sizeof(uint64_t) : 0))
> ```

上述的几个宏定义专门负责从`item`的`data`中获取到相应的值。

在创建`item`时，函数如下所示：
>```
>	static size_t item_make_header(const uint8_t nkey, const unsigned int flags, char *suffix, uint8_t *nsuffix) {
>		if (settings.inline_ascii_response) {
>			*nsuffix = (uint8_t)snprintf(suffix, 40, " %u %d\r\n", flags, nbytes - 2);
>		} else {
>			if (flags == 0) {
>				*nsuffix = 0;
>			} else {
>				*nsuffix = sizeof(flags);
>			}
>		}
>		return sizeof(item) + nkey + *nsuffix + nbytes;
>	}
>
>	item *do_item_alloc(char *key, const size_t nkey, const unsigned int flags,
>							const rel_time_t exptime, const int nbytes) {
>		···
>		size_t ntotal = item_make_header(nkey + 1, flags, nbytes, suffix, &nsuffix);
>		
>		unsigned int id = slabs_clsid(ntotal);
>		···
>	}
>```

当需要存储的`item`的大小超过`settings.slab_chunk_size_max`时，需要用到以下的结构体。
>```
>	// Header when an item is octually a chunk of another item
>	typedef struct _strchunk {
>		struct _strchunk	*next;			// points within its own chain
>		struct _strchunk *prev;			// can potentially point to the head
>		struct _strchunk *head;			// alawys points to the owner chunk(一般只向开头的item)
>		int					size;			// avaliable chunk space in bytes
>		int 				used;			// chunk space used
>		int					nbytes;		// used
>		unsigned short	refcount;		// used?
>		uint8_t			orig_clsid;	// for obj hdr chunks slabs_clsid is fake
>		uint8_t			it_flags;		// ITEM_* above
>		uint8_t			slabs_clsid;	// same as above
>		char				data[];		
>	} item_chunk;
>```

其图示如下:

![初始化后的内存图示](https://github.com/whynotAC/analysis_memcached/blob/master/memory_init/item_chunk.png)

`item_chunk`所占用哪个`slabclass`的计算方法如下:
>```
>	item *do_item_alloc(char *key, const size_t nkey, const unsigned int flags, const rel_time_t exptime, const int nbytes) {
>		···
>		if (ntotal > settings.slab_chunk_size_max) {
>			int htotal = nkey + 1 + nsuffix + sizeof(item) + sizeof(item_chunk);
>			hdr_id = slabs_clsid(htotal);
>			···
>		}
>		···
>	}
>```

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

