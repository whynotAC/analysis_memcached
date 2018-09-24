memcached内存初始化函数调用过程
===================================
memcached中关于内存管理函数主要在`slabs.h/c`文件中，包括对内存的初始化以及`slabclass`的管理等信息。但本文暂不介绍关于`slabclass`访问等具体情况，仅负责介绍内存管理以及对外提供的接口。

重要的数据变量
-------------------------------------
其中`slab.c`中包含的重要静态变量如下:
>```
>	#define MAX_NUMBER_OF_SLAB_CLASSES (63 + 1)
>	static slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES]; // 整个slab内存管理的结构体
>	static size_t mem_limit = 0; // 可分配的内存总大小
>	static size_t mem_malloced = 0; // 已经分配出去的内存大小
>	
>	static bool mem_limit_reached = false; // 是否已经无法对外分配内存空间
>	staitc int power_largest;		// 最大的slabclass的id号
>	
>	static void *mem_base = NULL;	// 可分配内存的基地址
>	static void *mem_current = NULL;	// 现在可以分配内存的基地址
>	static size_t mem_avail = 0;	// 可用内存大小
>```

主要的函数
--------------------------------------
此处主要介绍`slab_init`函数，这个函数是`memcached`内存管理的初始化函数。其定义如下:
>```
>	// 函数参数:
>	//	limit: 使用内存大小
>	// factor: slabclass的成长因子
>	// prealloc: 是否预分配内存空间
>	// slab_sizes: slabclass大小数组
>	void slab_init(const size_t limit, const double factor, const bool prealloc, const uint32_t *slab_sizes) {
>		int i = POWER_SMALLEST - 1;	// 默认认为slabclass能够达到最大的空间
>		// 计算最小item所占用的字节数
>		// sizeof(item) = 48 (64位CPU)
>		// settings.chunk_size的默认大小也为48
>		unsigned int size = sizeof(item) + settings.chunk_size;
>	
>		//	申请的内存大小，默认为64M
>		mem_limit = limit;
>		
>		// 是否预申请内存
>		if (prealloc) {
>			mem_base = malloc(mem_limit);
>			if (mem_base != NULL) {
>				mem_current = mem_base;
>				mem_avail = mem_limit;
>			} else {
>				···
>			} 
>		}
>
>		// 初始化slabclass数组
>		memset(slabclass, 0, sizeof(slabclass));
>		
>		// 按照size*factor或者slab_sizes中数据的大小来决定slabclass每个id的大小
>		// 保证不能超过MAX_NUMBER_OF_SLAB_CLASSES - 1
>		while (++i < MAX_NUMBER_OF_SLAB_CLASSES-1) {
>			if (slab_sizes != NULL) {
>				if (slab_sizes[i-1] == 0) {
>					break;
>				}
>				size = slab_sizes[i-1];
>			} else if (size >= settigs.slab_chunk_size_max / factor) {
>				break;
>			}
>			// 保证8字节对齐
>			if (size % CHUNK_ALIGN_BYTES) {
>				size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
>			}
>			// 每个slabclass数组可存放item的大小
>			slabclass[i].size = size;
>			// 每个page下可以分配的item个数
>			slabclass[i].perslab = settings.slab_page_size / slabclass[i].size;
>			if (slab_sizes == NULL) {
>				size *= factor;
>			}
>		}
>		
>		// 对slabclass最后一个slab进行分配
>		power_largest = i;
>		slabclass[power_largest].size = settings.slab_chunk_size_max;
>		slabclass[power_largest].persize = settings.slab_page_size / settings.slab_chunk_size_max;
>		
>		// 对预申请的内存进行处理
>		if (prealloc) {
>			slab_preallocate(power_largest);
>		}
>	}
>```

对某一个`id`分配一个页面的过程如下:
>```
>	static int do_slabs_newslab(const unsigned id) {
>		slabclass_t *p = &slabclass[id];	// 根据索引取出slabclass的位置
>		slabclass_t *g = &slabclass[SLAB_GLOBAL_PAGE_POOL];
>		
>		// 获取申请chunk大小，理论上每个chunk <= 1M
>		// 但是有些情况size * perslab不会正好等于1M而是小于1M
>		// 开启settings.slab_reassign设置为ture，可以用于slabclass之间页面转移
>		int len = (settings.slab_reassign || settings.slab_chunk_size_max != settings.slab_page_size) ? settings.slab_page_size : p->size * p->perslab;
>		char *ptr;
>		
>		// 判断内存使用情况是否超过最大设定
>		if ((mem_limit && mem_malloced + len > mem_limit && p->slabs > 0 && g->slabs == 0)) {
>			mem_limit_reached = true;
>			return 0;
>		}
>		
>		// grow_slab_list申请chunk/slab指针数组，就是void **slab_list、list_size
>		// get_page_from_global_pool从slabclass数组SLAB_GLOBAL_PAGE_POOL位置查看是否有空闲空间
>		// memory_allocate从预申请的内存中划分出一个slab/chunk(1M空间)
>		if ((grow_slab_list(id) == 0) ||
>				(((ptr = (char*)get_page_from_global_pool()) == NULL) &&
>				((ptr = (char*)memory_allocate((size_t)len)) == 0) {
>			return 0;
>		}
>		// 初始化slab/chunk中item的指针域等信息
>		memset(ptr, 0, (size_t)len);
>		split_slab_page_into_freelist(ptr, id);
>		
>		// 保存当前chunk/slab的指针，并更新slabs的大小
>		p->slab_list[p->slabs++] = ptr;
>		return 1;
>	}
>```

其他函数:
>```
>	static int grow_slab_list(const unsigned int id); // 用于增加slabclass中的slab_list的大小
>	static void *memory_allocate(size_t size); // 用于从mem_base中获取内存空间
>	static void do_slabs_free(void *ptr, const size_t size, unsigned int id); // 用于释放item或者item_chunk申请的空间
>	static void slabs_preallocate(const unsigned int maxslabs); // 申请预分配内存
>	static void *get_page_from_global_pool(void); // 从slabclass[0]获取内存空间
>	static void spilt_slab_page_into_freelist(char *ptr, const unsigned int id); // 初始化分配来的空间内存
>	static void do_slabs_free_chunked(item *it, const size_t size); // 释放item_chunk
>```

以上便是在初始化过程中的主要函数。