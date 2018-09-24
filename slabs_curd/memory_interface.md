memcached内存CURD接口
======================================

memcached的大致水平分层架构
-------------------------------------------

memcached的水平分层可以理解成如下图所示：

![memcached水平分层图示](https://github.com/whynotAC/analysis_memcached/blob/master/slabs_curd/memory_interface.png)

因此我们现在只需要掌握memcached对外提供服务接口，便可以轻松的知晓memcached的内存管理架构。此外，后面对网络部分的分析时，再认真讨论网络自线程是如何使用这些接口对外提供CURD服务，从而透彻分析memcached的整体架构。

对外提供的接口
--------------------------------------------
memcached内存管理对外提供的服务接口有以下几个(thread.c文件中):

1. item_alloc
2. item_get
3. item_touch
4. item_link
5. item_remove
6. item_replace
7. item_unlink
8. add_delta
9. store_item

接下来便对这些接口进行分析，以便了解内存管理对外提供服务的流程。

**item_alloc函数**

函数定义:
>		item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes) {
>			item *it;
>			it = do_item_alloc(key, nkey, flags, exptime, nbytes);
>			return it;
>		}

函数功能: 	**在对应的`slabclass`中分配一个空闲的`item`。**

函数流程图如下:
