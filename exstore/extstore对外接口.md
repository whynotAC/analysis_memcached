extstore对外接口
========================================
外存管理的相关代码主要集中于`extstore.h/c`文件中，下面将详细介绍文件中的主要内容。外存管理使用的重要结构体已经在前文《[外存管理](https://github.com/whynotAC/analysis_memcached/blob/master/exstore/%E5%A4%96%E5%AD%98%E7%AE%A1%E7%90%86.md)》已经介绍过，本文将介绍`extstore.h/c`中对外提供的接口以及自身的函数。

## 1. 对外接口
`extstore.h/c`文件中对外提供的函数如下表格所示:

|  函数名  | 函数定义  |  函数说明  |  备注  |
| ------- | -------- | -------- | ------ |
| `extstore_err` | `const char *extstore_err(enum extstore_res res)` | 获取`extstore_res`对应的解释字符串 | 无 |
| `extstore_init` | `void *extstore_init(struct extstore_conf_file *fh, struct extstore_conf * cf, enum extstore_res *res)` | 外存初始化函数 | 前面文档已经叙述过,本文不作描述 |
| `extstore_write_request` | `int extstore_write_request(void *ptr, unsigned int bucket, unsigned int free_bucket, obj_io *io)` | 发送写入外存文件的请求 | 无 |
| `extstore_write` | `void extstore_write(void *ptr, obj_io *io)` | 当调用完写入请求后填充`IO`结构体 | 无 |
| `extstore_submit` | `int extstore_submit(void *ptr, obj_io *io)` | 将`IO`结构体链入到`IO`线程的队列中，提交外存操作任务 |  无 |
| `extstore_check` | `int extstore_check(void *ptr, unsigned int page_id, uint64_t page_version)` | 检查`page_id`指定的外存页的版本是否等于`page_version` | 无 |
| `extstore_delete` | `int extstore_delete(void *ptr, unsigned int page_id, uint64_t page_version, unsigned int count, unsigned int bytes)` | 当删除外存`item`时，仅用检查指定`page`的版本,并减少对应`page`中`obj_count`值 | 无 |
| `extstore_get_stats` | `void extstore_get_stats(void *ptr, struct extstore_stats *st)` | 获取外存的状态 | 无 |
| `extstore_get_page_data` | `void extstore_get_page_data(void *ptr, struct extstore_stats *st)` | 获取外存页面的状态 | 无 |
| `extstore_run_maint` | `void extstore_run_maint(void *ptr)` | 释放外存管理等待的`cond`条件变量,使`extstore_maint_thread`线程运行 | 无 |
| `extstore_close_page` | `void extstore_close_page(void *ptr, unsigned int page_id, uint64_t page_version)` | 释放`page_id`指定的外存页面,调用`extstore_maint_thread`线程释放页面 | 无 |

上面介绍了外存管理文件对外提供的接口函数以及其定义、作用等等，下面将介绍其函数的具体定义。

```
typedef void (*obj_io_cb)(void *e, obj_io *io, int ret);  // 回调函数

const char *extstore_err(enum extstore_res res) {
	char *rv = "unknown error";
	switch (res) {
		case EXTSTORE_INIT_BAD_WBUF_SIZE:
			rv = "page_size must be divisible by wbuf_size";
			break;
		case EXTSTORE_INIT_NEED_MORE_WBUF:
			rv = "wbuf_count must be >= page_buckets";
			break;
		case EXTSTORE_INIT_NEED_MORE_BUCKETS:
			rv = "page_buckets must be > 0";
			break;
		case EXTSTORE_INIT_PAGE_WBUF_ALIGNMENT:
			rv = "page_size and wbuf_size must be divisible by 1024*1024*2";
			break;
		case EXTSTORE_INIT_TOO_MANY_PAGES:
			rv = "page_count must total to < 65536. Increase page_size of lower path sizes";
			break;
		case EXTSTORE_INIT_OOM:
			rv = "failed calloc for engine";
			break;
		case EXTSTORE_INIT_OPEN_FAIL:
			rv = "failed to open file";
			break;
		case EXTSTORE_INIT_THREAD_FAIL:
			break;
	}
	return rv;
}

/* engine write function; takes engine, item_io.
 * fast fail if no available write buffer (flushing)
 * lock engine context, find active page. unlock
 * if page full, submit page/buffer to io thread.
 *
 * write is designed to be flaky; if page full, caller must try again to get
 * new page. best if used from a background thread that can harmlessly retry.
 */
int extstore_write_request(void *ptr, unsigned int bucket, unsigned int free_bucket, obj_io *io) {
	// 判断是否有页面存在未被使用的空间
	store_engine *e = (store_engine *)ptr;
	store_page *p;
	int ret = -1;
	// 判断页面所属的bucket是否超出范围
	if (bucket >= e->page_bucketcount)
		return ret;
	
	// 获取空闲页面
	pthread_mutex_lock(&e->mutex);
	p = e->page_buckets[bucket];
	if (!p) {
		p = _allocate_page(e, bucket, free_bucket);
	}
	pthread_mutex_unlock(&e->mutex);
	if (!p)
		return ret;
		
	pthread_mutex_lock(&p->mutex);
	
	// FIXME: can't null out page_buckets!!!
	// page is full. clear bucket and retry later
	// 判断页面是否可以使用
	if (!p->active || 
			((!p->wbuf || p->wbuf->full) && p->allocated >= e->page_size)) {
		pthread_mutex_unlock(&p->mutex);
		// 申请新的页面
		pthread_mutex_lock(&e->mutex);
		_allocate_page(e, bucket, free_bucket);
		pthread_mutex_unlock(&e->mutex);
		
		return ret;
	}
	
	// if io won't fit, submit IO for wbuf and find new one.
	// 页面对应的wbuf没有足够多的空间
	if (p->wbuf && p->wbuf->free < io->len && !p->wbuf->full) {
		_submit_wbuf(e, p);
		p->wbuf->full = true;
	}
	
	// 页面没有wbuf，则申请新空间
	if (!p->wbuf && p->allocated < e->page_size) {
		_allocate_wbuf(e, p);
	}
	
	// hand over buffer for caller to copy into
	// leaves p locked.
	// 当外存页面对应的wbuf拥有足够的空间,使用io记录写入的空间的起始位置
	if (p->wbuf && !p->wbuf->full && p->wbuf->free >= io->len) {
		io->buf = p->wbuf->buf_pos;
		io->page_id = p->id;
		return 0;
	}
	
	pthread_mutex_unlock(&p->mutex);
	// p->written is incremented post-wbuf flush
	return ret;
}

/* _must_ be called after a successful write_request.
 * fills the rest of structure.
 */
void extstore_write(void *ptr, obj_io *io) {
	store_engine *e = (store_engine *)ptr;
	store_page *p = &e->pages[io->page_id];
	// 记录IO结构体与Page的对应关系
	io->offset = p->wbuf->offset + (p->wbuf->size - p->wbuf->free);  // 记录IO结构体内容与wbuf缓冲区的偏移量
	io->page_version = p->version;	// 记录写出页面的版本号
	p->wbuf->buf_pos += io->len;		// 修改写出缓冲区的大小
	p->wbuf->free -= io->len;			// 修改缓冲区的空闲大小
	p->bytes_used += io->len;			// 修改页面的使用字节数
	p->obj_count++;						// 页面中的item个数
	// 修改外存的统计状态
	STAT_L(e);
	e->stats.bytes_written += io->len;
	e->stats.bytes_used += io->len;
	e->stats.objects_written++;
	e->stats.objects_used++;
	STAT_UL(e);
	
	pthread_mutex_unlock(&p->mutex);
}

/* engine submit function: takes engine, item_io stack.
 * lock io_thread context and add stack?
 * signal io thread to wake.
 * return success.
 */
int extstore_submit(void *ptr, obj_io *io) {
	store_engine *e = (store_engine *)ptr;
	store_io_thread *t = _get_io_thread(e);  // 轮流获取IO线程
	
	pthread_mutex_lock(&t->mutex);
	// 将IO结构体加入到IO线程的QUEUE中
	if (t->thread == NULL) {
		t->queue = io;
	} else {
		/* Have to put the *io stack at the end of current queue.
		 * FIXME: Optimize by tracking tail.
		 */
		// 将IO结构体加入到队列尾部
		obj_io *tmp = t->queue;
		while (tmp->next != NULL) {
			tmp = tmp->next;
			assert(tmp != t->queue);
		}
		tmp->next = io;
	}
	// TODO: extstore_submit(ptr, io, count)
	obj_io *tio = io;
	while (tio != NULL) {
		t->depth++;
		tio = tio->next;
	}
	pthread_mutex_unlock(&t->mutex);
	
	// pthread_mutex_lock(&t->mutex);
	pthread_cond_signal(&t->cond);
	// pthread_mutex_unlock(&t->mutex);
	return 0;
}

int extstore_check(void *ptr, unsigned int page_id, uint64_t page_version) {
	store_engine *e = (store_engine *)ptr;
	store_page *p = &e->pages[page_id];
	int ret = 0;
	
	pthread_mutex_lock(&p->mutex);
	if (p->version != page_version)
		ret = -1;
	pthread_mutex_unlock(&p->mutex);
	return ret;
}

/* engine note delete function: takes engine, page id, size?
 * note that an item in this page is no longer valid
 */
int extstore_delete(void *ptr, unsigned int page_id, uint64_t page_version, unsigned int count, unsigned int bytes) {
	store_engine *e = (store_engine *)ptr;
	// FIXME: validate page_id in bounds
	store_page *p = &e->pages[page_id];
	int ret = 0;
	
	// 减少外存page的页面记录
	pthread_mutex_lock(&p->mutex);
	if (!p->closed && p->version == page_version) {
		// 减少外存页面使用的字节数
		if (p->bytes_used >= bytes) {
			p->bytes_used -= bytes;
		} else {
			p->bytes_used = 0;
		}
		
		// 减少外存页面中item个数
		if (p->obj_count >= count) {
			p->obj_count -= count;
		} else {
			p->obj_count = 0;		// caller has bad accounting?
		}
		
		// 修改外存记录
		STAT_L(e);
		e->stats.bytes_used -= bytes;
		e->stats.objects_used -= count;
		STAT_UL(e);
		
		// 如果页面没有存在item,则释放此页面
		if (p->obj_count == 0) {
			extstore_run_maint(e);
		}
	} else {
		ret = -1;
	}
	pthread_mutex_unlock(&p->mutex);
	return ret;
}

// Copies stats internal to engine and computes any derived values
void extstore_get_stats(void *ptr, struct extstore_stats *st) {
	store_engine *e = (store_engine *)ptr;
	STAT_L(e);
	memcpy(st, &e->stats, sizeof(struct extstore_stats));
	STAT_UL(e);
	
	// grab pages_free/pages_used
	pthread_mutex_lock(&e->mutex);
	st->pages_free = e->page_free;
	st->pages_used = e->page_count - e->page_free;
	pthread_mutex_unlock(&e->mutex);
	st->io_queue = 0;
	for (int x = 0; x < e->io_threadcount; x++) {
		pthread_mutex_lock(&e->io_threads[x].mutex);
		st->io_queue += e->io_threads[x].depth;
		pthread_mutex_unlock(&e->io_threads[x].mutex);
	}
	// calculate bytes_fragmented.
	// note that open and yet-filled pages count against fragmentation.
	st->bytes_fragmented = st->pages_used * e->page_size - st->bytes_used;
}

void extstore_get_page_data(void *ptr, struct extstore_stats *st) {
	store_engine *e = (store_engine *)ptr;
	STAT_L(e);
	memcpy(st->page_data, e->stats.page_data, sizeof(struct extstore_page_data) * e->page_count);
	STAT_UL(e);
}

void extstore_run_maint(void *ptr) {
	store_engine *e = (store_engine *)ptr;
	pthread_cond_signal(&e->maint_thread->cond);
}

// allows a compactor to say "we're done with this page. kill it."
void extstore_close_page(void *ptr, unsigned int page_id, uint64_t page_version) {
	store_engine *e = (store_engine *)ptr;
	store_page *p = &e->pages[page_id];
	
	pthread_mutex_lock(&p->mutex);
	if (!p->closed && p->version == page_version) {
		p->closed = true;
		extstore_run_maint(e);
	}
	pthread_mutex_unlock(&p->mutex);
}
```

## 2. 内部函数(静态函数)
`extstore.h/c`文件中静态函数如下表格所示:

| 函数名 | 函数定义 | 函数说明 | 备注 |
| ----- | ------- | ------- | ----- |
| `wbuf_new` | `static _store_wbuf *buf_new(size_t size)` | 申请写出空间 | 无 |
| `_get_io_thread` | `static store_io_thread *_get_io_thread(store_engine *e)` | 用于获取`IO`结构体分配给的线程`ID` | 使用轮询操作 |
| `_next_version` | `static uint64_t _next_version(store_engine *e)` | 用于获取下一个版本号 | 无 |
| `_allocate_page` | `static store_page *_allocate_page(store_engine *e, unsigned int bucket, unsigned int free_bucket)` | 用于申请新外存页面 | 无 |
| `_allocate_wbuf` | `static void _allocate_wbuf(store_engine *e, store_page *p)` | 分配新的缓冲空间 | 从数组中获取缓冲空间 |
| `_wbuf_cb` | `static void _wbuf_cb(void *ep, obj_io *io, int ret)` | 用于写出缓冲区结束后的回调函数 | 用于写出缓冲空间 |
| `_submit_wbuf` | `static void _submit_wbuf(store_engine *e, store_page *p)` | 提交写出缓冲区空间 | 无 |
| `_read_from_wbuf` | `static inline int _read_from_wbuf(store_page *p, obj_io *io)` | 从`wbuf`中读取`item`的值 | 无 |
| `extstore_io_thread` | `static void *extstore_io_thread(void *arg)` | `IO`线程的函数体 | 本文不再做详细介绍 |
| `_free_page` | `static void _free_page(store_engine *e, store_page *p)` | 释放外存空间页面 | 无 |
| `extstore_maint_thread` | `static void extstore_maint_thread(void *arg)` | 外存空间管理线程函数体 | 本文不再做详细介绍 |

上面的函数都是静态函数，用于外存管理方面的函数。其对应的代码如下所示:

```
// 申请新的缓冲区函数
static _store_wbuf *wbuf_new(size_t size) {
	_store_wbuf *b = calloc(1, sizeof(_store_wbuf));
	if (b == NULL)
		return NULL;
	b->buf = malloc(size);
	if (b->buf == NULL) {
		free(b);
		return NULL;
	}
	// 初始化成员变量
	b->buf_pos = b->buf;
	b->free = size;
	b->size = size;
	return b;
}

// 轮询获取IO线程分配IO操作结构体
static store_io_thread *_get_io_thread(store_engine *e) {
	int tid = -1;
	long long int low = LLONG_MAX;
	pthread_mutex_lock(&e->mutex);
	// find smallest queue. ignoring lock since being wrong isn't fatal.
	// TODO: if average queue depth can be quickly tracked. can break as soon
	// as we see a thread that's less than average, and start from last_io_thread
	// 采用轮询模式来分配IO操作
	for (int x = 0; x < e->io_threadcount; x++) {
		if (e->io_threads[x].depth == 0) {
			tid = x;
			break;
		} else if (e->io_threads[x].depth < low) {
			tid = x;
			low = e->io_threads[x].depth;
		}
	}
	pthread_mutex_unlock(&e->mutex);
	
	return &e->io_threads[tid];
}

// 获取下一个页面编号
static uint64_t _next_version(store_engine *e) {
	return e->version++;
}

// call with *e locked
// 申请新的页面空间
static store_page *_allocate_page(store_engine *e, unsigned int bucket, unsigned int free_bucket) {
	assert(!e->page_buckets[bucket] || e->page_buckets[bucket]->allocated == e->page_size);
	store_page *tmp = NULL;
	// if a specific free bucket was requested, check there first
	// 判断bucket是否有空闲的页面
	if (free_bucket != 0 && e->free_page_buckets[free_bucket] != NULL) {
		assert(e->page_free > 0);
		tmp = e->free_page_buckets[free_bucket];
		e->free_page_buckets[free_bucket] = tmp->next;
	}
	// failling that, try the global list.
	// 从全局的空闲页面链表中分配空间
	if (tmp == NULL && e->page_freelist != NULL) {
		tmp = e->page_freelist;
		e->page_freelist = tmp->next;
	}
	E_DEBUG("EXTSTORE: allocating new page\n");
	// page_freelist can be empty if the only free pages are specialized and
	// we didn't just request one.
	// 判断是否有空闲页面
	if (e->page_free > 0 && tmp != NULL) {
		tmp->next = e->page_buckets[bucket];
		e->page_buckets[bucket] = tmp;
		tmp->active = true;
		tmp->free = false;
		tmp->closed = false;
		tmp->version = _next_version(e);	// 每次分配都拥有一个新的版本
		tmp->bucket = bucket;
		e->page_free--;
		STAT_INCR(e, page_allocs, 1);		// 记录分配页面数
	} else {
		extstore_run_maint(e);				// 回收外存页面空间
	}
	if (tmp)
		E_DEBUG("EXTSTORE: got page %u\n", tmp->id);
	return tmp;
}

// call with *p locked. locks *e
static void _allocate_wbuf(store_engine *e, store_page *p) {
	_store_wbuf *wbuf = NULL;
	assert(!p->wbuf);
	pthread_mutex_lock(&e->mutex);
	if (e->wbuf_stack) {
		wbuf = e->wbuf_stack;
		e->wbuf_stack = wbuf->next;
		wbuf->next = 0;
	}
	pthread_mutex_unlock(&e->mutex);
	if (wbuf) {
		// 初始化缓冲区与页面之间的对应关系
		wbuf->offset = p->allocated;
		p->allocated += wbuf->size;
		wbuf->free = wbuf->size;
		wbuf->buf_pos = wbuf->buf;
		wbuf->full = false;
		wbuf->flushed = false;
		
		p->wbuf = wbuf;		// 将缓冲区链接入外存页面的缓冲区链表中
	}
}

/* callback after wbuf is flushed. can only remove wbuf's from the head onward
 * if successfully flushed, which complicates this routine. each callback
 * attempts to free the wbuf stack. which is finally done when the head wbuf's
 * callback happens.
 * It's rare flushes would happen out of order.
 */
// 写出缓冲区后的回调函数
static void _wbuf_cb(void *ep, obj_io *io, int ret) {
	store_engine *e = (store_engine *)ep;
	store_page *p = &e->pages[io->page_id];
	_store_wbuf *w = (_store_wbuf *) io->data; // 获取写出缓冲区
	
	// TODO: Examine return code. Not entirely sure how to handle errors.
	// Naive first-pass should probably cause the page to close/free.
	// 设置写出缓冲区与页面
	w->flushed = true;
	pthread_mutex_lock(&p->mutex);
	assert(p->wbuf != NULL && p->wbuf == w);
	assert(p->written == w->offset);
	p->written += w->size;
	p->wbuf = NULL;
	
	// 当页面写满时，设置页面状态为不活跃
	if (p->written == e->page_size)
		p->active = false;
		
	// return the wbuf
	// 归还缓冲区到缓冲区池中
	pthread_mutex_lock(&e->mutex);
	w->next = e->wbuf_stack;
	e->wbuf_stack = w;
	
	// also return the IO we just used.
	// 归还IO操作结构体
	io->next = e->io_stack;
	e->io_stack = io;
	pthread_mutex_unlock(&e->mutex);
	pthread_mutex_unlock(&p->mutex);
}

// Wraps pages current wbuf in an io and submits to IO thread.
// Called with a locked, locks e.
static void _submit_wbuf(store_engine *e, store_page *p) {
	// 获取IO操作结构体
	_store_wbuf *w;
	pthread_mutex_lock(&e->mutex);
	obj_io *io = e->io_stack;
	e->io_stack = io->next;
	pthread_mutex_unlock(&e->mutex);
	w = p->wbuf;
	
	// 设置IO操作结构体成员
	// zero out the end of the wbuf to allow blind readback of data.
	memset(w->buf + (w->size - w->free), 0, w->free);
	
	// 设置成员变量
	io->next = NULL;
	io->mode = OBJ_IO_WRITE;
	io->page_id = p->id;
	io->data = w;
	io->offset = w->offset;
	io->len = w->size;
	io->buf = w->buf;
	io->cb = _wbuf_cb;		// 设置回调函数
	
	// 提交IO操作结构体
	extstore_submit(e, io);
}

/* Finds an attached wbuf that can satisfy the read.
 * Since wbufs can potentially be flushed to disk out of order, they are only
 * removed as the head of the list successfully flushed to disk.
 */
// call with *p locked
// FIXME: protect from reading past wbuf
static inline int _read_from_wbuf(store_page *p, obj_io *io) {
	// 从缓冲区读取item
	_store_wbuf *wbuf = p->wbuf;
	assert(wbuf != NULL);
	assert(io->offset < p->written + wbuf->size);
	if (io->iov == NULL) {
		memcpy(io->buf, wbuf->buf + (io->offset - wbuf->offset), io->len);
	} else {
		int x;
		unsigned int off = io->offset - wbuf->offset;
		// need to loop fill iovecs
		for (x = 0; x < io->iovcnt; x++) {
			struct iovec *iov = &io->iov[x];
			memcpy(iov->iov_base, wbuf->buf + off, iov->iov_len);
			off += iov->iov_len;
		}
	}
	return io->len;
}

// call with *p locked.
// 释放页面
static void _free_page(store_engine *e, store_page *p) {
	store_page *tmp = NULL;
	store_page *prev = NULL;
	E_DEBUG("EXTSTORE: freeing page %u\n", p->id);
	STAT_L(e);
	e->stats.objects_used -= p->obj_count;
	e->stats.bytes_used -= p->bytes_used;
	e->stats.page_reclaims++;
	STAT_UL(e);
	pthread_mutex_lock(&e->mutex);
	// unlink page from bucket list
	tmp = e->page_buckets[p->bucket];
	while (tmp) {
		if (tmp == p) {
			if (prev) {
				prev->next = tmp->next;
			} else {
				e->page_buckets[p->bucket] = tmp->next;
			}
			tmp->next = NULL;
			break;
		}
		prev = tmp;
		tmp = tmp->next;
	}
	// reset most values
	p->version = 0;
	p->obj_count = 0;
	p->bytes_used = 0;
	p->allocated = 0;
	p->written = 0;
	p->written = 0;
	p->bucket = 0;
	p->active = false;
	p->closed = false;
	p->free = true;
	// add to page stack
	// TODO: free_page_buckets first class and remove redundancy?
	if (p->free_bucket != 0) {
		p->next = e->free_page_buckets[p->free_bucket];
		e->free_page_buckets[p->free_bucket] = p;
	} else {
		p->next = e->page_freelist;
		e->page_freelist = p;
	}
	e->page_free++;
	pthread_mutex_unlock(&e->mutex);
}
```

上面的代码就是所有`extstore.h/c`文件中提供的函数，基本都是围绕着`store_engine`结构体进行设计与编写的。
