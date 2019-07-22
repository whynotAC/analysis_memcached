memcached网络模型
==================================
前面通过两篇文章介绍了基础的网络模型以及`memcached`相关的网络设置，本文将着重介绍`memcached`网络模型。本文将从以下几个方面详细进行介绍:

1. 使用的结构体以及全局变量
2. `main thread`的事件以及处理器
3. `sub thread`的事件以及处理器
4. `connection`的生命周期
5. `memcached`网络模型
6. `memcached`的定时器
7. 总结

## 1. 使用的结构体以及全局变量
本小节将详细介绍一下`memcached`使用的网络相关的结构体，并介绍它们成员的作用。

```
// 系统提供的多向量的IO缓冲区
struct msghdr {
	void 			*msg_name;		// optional address
	socklen_t		msg_namelen;		// size of address
	struct iovec *msg_iov;			// scatter/gather array
	int 			msg_iovlen;		// elements in msg_iov
	void 			*msg_control;		// ancillary data, see below
	socklen_t		msg_controllen;	// ancillary data buffer len
	int				msg_flags;		// flags on received message
};

struct iovec {
	char 	*iov_base;			// Base address
	size_t iov_len;				// Length
};

// 结构体

// 使用的保存connection结构体
/*
 * The structure representing a connection into memcached.
 */
struct conn {
	int		sfd;						// 套接字
	sasl_conn_t *sasl_conn;			// 使用sasl
	bool	authenticated;			// 是否认证
	enum conn_states state;			// 链接的状态
	enum bin_substates substate;	//	链接的子状态
	rel_time_t last_cmd_time;		// 最后执行cmd的时间
	struct event event;				// 监听的事件
	short ev_flags;					// 事件的标示
	short which;	// which events were just triggered 哪种事件会被处理
	
	char *rbuf;	// buffer to read commands into 读取缓冲区
	char *rcurr;	// but if we parsed some already, this is where we stopped 目前读取缓冲区的位置
	int  rsize;	// total allocated size of rbuf 读取缓冲区的大小
	int  rbytes;	// how much data, starting from rcurr, do we have unparsed 读取缓冲区内还有多少字节未被处理
	
	char *wbuf;	// 写缓冲区
	char *wcurr;	// 写缓冲区目前写入位置
	int	  wsize;	// 写缓冲区的大小
	int	  wbytes;	// 写缓冲区的字符个数
	
	/* which state to go into after finishing current write */
	enum conn_states wirte_and_go;	// 写入完成后，connection的状态
	void *write_and_free;	// free this memory after finishing writing 写入完毕后释放空间
	
	char *ritem;		// when we read in an item's value, it goes here 读取item的值保存位置
	int rlbytes;		// 读取字节个数
	
	/* data for the nread state */
	
	/**
	 * item is used to hold an item structure created after reading the command
	 * line of set/add/replace commands, but before we finished reading the actual
	 * data. The data is read into ITEM_data(item) to avoid extra copying.
	 */
	void *item;	// for commands set/add/replace set/add/replace操作的item保存位置
	
	/* data for the swallow state */
	int  sbytes;	// how many bytes to swallow 多少个字节被保存
	
	/* data for the mwrite state */
	struct iovec *iov;		// IO缓冲区向量
	int		iovsize;			// number of elements allocated in iov[] IO缓冲区向量大小
	int		iovused;			// number of elements used in iov[] IO缓冲区向量使用大小
	
	struct msghdr 	*msglist;		// 多级IO缓冲区向量
	int					msgsize;		// number of elements allocated in msglist[] 大小
	int					msgused;		// number of elements used in msglist[] 使用的大小
	int					msgcurr;		// element in msglist[] being transmitted now 传输到的地址
	int 				msgbytes;		// number of bytes in current msg 消息的大小
	
	item				**ilist;		// list of items to write out 写出item的链表
	int					isize;			// 大小
	item				**icurr;		// 使用到的位置
	int					ileft;			// 使用的链表个数
	
	char				**suffixlist;	// 缓冲区列表
	int					suffixsize;	// 大小
	char				**suffixcurr;	// 使用到的位置
	int					suffixleft;	// 已用的个数
#ifdef EXTSTORE
	int					io_wrapleft;
	unsigned int		recache_counter;
	io_wrap 			*io_wraplist;	// linked list of io_wraps
	bool				io_queued;	// FIXME: debugging flag
#endif
	enum protocol		protocol;	// which protocol this connection speaks 字节协议
	enum network_transport transport;	// what transport is used by this conneciotn 网络协议
	
	/* data for UDP clients */
	// udp协议使用的结构成员
	int 				request_id; // Incoming UDP request ID, if this is a UDP "conneciton"
	struct	sockaddr_in6	request_addr;	// udp: Who sent the most recent request
	socklen_t request_addr_size;
	unsigned char *hdrbuf;	// udp packet headers
	int hdrsize;	// number of headers' worth of space is allocated
	
	bool noreply;	// True if the reply should not be sent
	// current stats command
	struct {
		char *buffer;
		size_t size;
		size_t offset;
	} stats;
	
	// Binary protocol stuff
	// This is where the binary header goes
	protocol_binary_request_header binary_hreader; // 二进制通信协议头	uint64_t cas;		// the cas to return
	short cmd;	// current command being processed 处理cmd的个数
	int opaque;
	int keylen;
	conn *next;	// Used for generating a list of conn structrues
	LIBEVENT_THREAD *thread;	// Pointer to the thread object serving this conneciton
};

// 链接的枚举变量，事件处理器会根据connection的状态来进行处理
/*
 * NOTE: If you modify this table you _MUST_ update the function state_text
 */
/*
 * Possible states of a connection.
 */
enum conn_states {
	conn_listening,	// the socket which listens for connections
	conn_new_cmd,		// Prepare connection for next command
	conn_waiting,		// waiting for a readable socket
	conn_read,		// reading in a command line
	conn_parse_cmd,	// try to parse a command from the input buffer
	conn_write,		// writing out a simple response
	conn_nread,		// reading in a fixed number of bytes
	conn_swallow,		// swallowing unnecessary bytes w/o storing
	conn_closing,		// closing this connection
	conn_mwrite,		// writing out many items sequentially
	conn_closed,		// connection is closed
	conn_watch,		// held by the logger thread as a watcher
	conn_max_state	// Max state value (used for assertion)
};

// 如果字节解析协议为binary，有以下几个分状态
enum bin_substates {
	bin_no_state,
	bin_reading_set_header,
	bin_reading_cas_header,
	bin_read_set_value,
	bin_reading_get_key,
	bin_reading_stat,
	bin_reading_del_header,
	bin_reading_incr_header,
	bin_read_flush_exptime,
	bin_reading_sasl_auth,
	bin_reading_sasl_auth_data,
	bin_reading_touch_key,
};

// 通信协议
enum protocol {
	ascii_prot = 3,	// arbitrary value.	字节码通信
	binary_port,		// 二进制通信
	negotiating_prot	// Discovering the protocol 根据内容来判断协议类型
};

// 网络协议
enum network_transport {
	local_transport,		// Unix sockets
	tcp_transport,
	udp_transport
};

// 线程使用的结构体
typedef struct {
	pthread_t thread_id;		// unique ID of this thread	struct event_base *base;	// libevent handle this thread uses
	struct event notify_event;	// listen event for notify pipe
	int notify_receive_fd;		// receiving end of notify pipe
	int notify_send_fd;			// sending end of notify pipe
	struct thread_stats stats; // Stats generated by this thread
	struct conn_queue *new_conn_queue;	// queue of new connecitons to handle
	cache_t *suffix_cache;		// suffix cache
#ifdef EXTSTORE
	cache_t *io_cache;			// IO objects;
	void *storage;				// data object for storage system.
#endif
	logger *l;					// logger buffer
	void *lru_bump_buf;			// async LRU bump buffer
} LIBEVENT_THREAD;

// 新connection使用的结构体
/* An item in the conneciton queue. */
enum conn_queue_item_modes {
	queue_new_conn,		// brand new connection
	queue_redispatch		// redispatching from side thread
};
typedef struct conn_queue_item CQ_ITEM;
struct conn_queue_item {
	int								sfd;	// 文件描述符
	enum conn_states				init_state; // 初始化状态
	int								event_flags; // 监听的事件
	int								read_buffer_size; // 读缓冲大小
	enum network_transport		transport;  // 网络协议
	enum conn_queue_item_modes	mode;		 // 新connection的状态
	conn 							*c;			// conn结构体的指针
	CQ_ITEM						*next;    // 下一个CQ_ITEM
};

/* A connection queue */
typedef struct conn_queue CQ;
struct conn_queue {
	CQ_ITEM *head;		// 链表头
	CQ_ITEM *tail;		// 链表尾
	pthread_mutex_t lock;	// 互斥量
};

// 全局变量
static conn *listen_conn = NULL;		// TCP接口的conn链表
static struct event_base *main_base;	// 主函数中使用的`Reactor`具柄
static int max_fds;						// 最大的文件描述符

static volatile bool allow_new_conns = true; // 用于判断是否可以分配conn，用于新链接的接收

/* Free list of CQ_ITEM structs */
static CQ_ITEM *cqi_freelist;			// 指向CQ_ITEM的空闲链表
static pthread_mutex_t cqi_freelist_lock; // 访问空闲CQ_ITEM链表的互斥量

// 网络子线程的结构体
/*
 * Each libevent instance has a wakeup pipe, which other threads
 * can use to signal that they've put a new connection on its queue.
 */
static LIBEVENT_THREAD *threads;

```
上面代码部分列出了将要使用的所有全局变量以及其对应的定义，后面将介绍这些变量是如何在`memcached`程序中使用的。

## 2. `main thread`的事件以及处理器
`main thread`网络设置过程如下图所示:

![main thread网络设置流程图示](https://github.com/whynotAC/analysis_memcached/blob/master/network/主线程网络设置流程图.png)

| 网络通信方式 |  `conn`结构体的`state` | 监听事件 | 处理事件函数  | 绑定的线程
| ---------- | --------------------- | ------- | ----------- | ------- |
| `linux socket` | `conn_listening` | `EV_READ|EV_PERSIST` | `event_handler` | 绑定到主线程`main_base`的`Reactor`事件管理器中 |
| `tcp socket` | `conn_listening` | `EV_READ|EV_PERSIST` | `event_handler` | 绑定到主线程`main_base`的`Reactor`事件管理器中 |
| `udp socket` | `conn_read` | `EV_READ|EV_PERSIST` | `event_handler` | 绑定到通信网络子线程的`Reactor`事件管理器中 |

事件处理器`event_handler`源代码如下:

```
// memcached.c文件中event_handler函数
// 参数解释:
/* fd -- 绑定的网络通信描述符
 * which -- 关注事件的类型
 * arg -- 自己注册的conn结构体的指针
 */
void event_handler(const int fd, const short which, void *arg) {
	conn *c;
	
	c = (conn *)arg;
	assert(c != NULL);
	
	c->which = which;
	
	/* sanity */
	if (fd != c->sfd) {
		if (settings.verbose > 0)
			fprintf(stderr, "Catastrophic: event fd doesn't match conn fd!\n");
		conn_close(c);
		return;
	}
	
	drive_machine(c);		// 进入状态机进行信息处理
	
	/* wait for next event */
	return;
}

// 状态机的程序代码
static void drive_machine(conn *c) {
	bool stop = false;
	int sfd;
	socklen_t addrlen;
	struct sockaddr_storage addr;
	int nreqs = settings.reqs_per_event;
	int res;
	const char *str;
#ifdef HAVE_ACCEPT4
	static int use_accept4 = 1;
#else
	static int use_accept4 = 0;
#endif
	
	assert(c != NULL);
	
	while (!stop) {
		
		switch(c->state) {
		case conn_listening:		// 主线程监听的事件
			addrlen = sizeof(addr);
#ifdef HAVE_ACCEPT4
			if (use_accept4) {
				sfd = accept4(c->sfd, (struct sockaddr *)&addr, &addrlen, SOCK_NONBLOCK);
			} else {
				sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
			}
#else
			sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
#endif
			if (sfd == -1) {
				// 判断是否accept成功
				if (use_accept4 && errno == ENOSYS) {
					use_accept4 = 0;
					continue;
				}
				perror(use_accept4 ? "accept4()" : "accept()");
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					/* these are transient, so don't log anything */
					stop = true;
				} else if (errno == EMFILE) {
					// 判断是否是系统没有空闲的文件描述符了，不能接受更多的connection链接
					if (settings.verbose > 0)
						fprintf(stderr, "Too many open connections\n");
					accept_new_conns(false); // 表示程序不接受新的connection链接
					stop = true;
				} else {
					perror("accept()");
					stop = true;
				}
				break;
			}
			if (!use_accept4) {
				// 设置新链接的套接字为非阻塞套接字
				if (fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL) | O_NONBLOCK) < 0) {
					perror("setting O_NONBLOCK");
					close(sfd);
					break;
				}
			}
			// 判断是否超过了程序设置的最大链接数
			if (settings.maxconns_fast &&
					stats_state.curr_conns + stats_state.reserved_fds >= settings.maxconns - 1) {
				str = "ERROR Too many open connection\r\n";				res = write(sfd, str, strlen(str));
				close(sfd);
				STATS_LOCK();
				stats.rejected_conns++;
				STATS_UNLOCK();
			} else {
				// 接收到的connection链接，分配到网络通信子线程中
				dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST,
					DATA_BUFFER_SIZE, c->transport);
			}
			
			stop = true;
			break;
		case xxx:
			// 其他状态代码
		
		case conn_read:	
			res = IS_UDP(c->transport) ? try_read_udp(c) : try_read_network(c);
			
			switch (res) {
			case READ_NO_DATA_RECEIVED:
				conn_set_state(c, conn_waiting);
				break;
			case READ_DATA_RECEIVED:
				conn_set_state(c, conn_parse_cmd);
				break;
			case READ_ERROR:
				conn_set_state(c, conn_closing);				break;
			case READ_MEMORY_ERROR: // Failed to allocate more memory
				/* State already set by try_read_network */
				break;
			}
			break;
			
		case xxx:
			// 其他状态代码
		}
	}
}
```
由此可以发现当主线程中`linux socket`和`tcp`方式的都只会调用`drive_machine`中的`conn_listening`分支，用来接受新的`conneciton`链接，然后将`connection`链接分配到网络子线程中。新`connection`的注册事件以及`connection`状态如下所示:

|  `conn`结构体的`state` | 监听事件 | 处理事件函数  | 绑定的线程 |
| --------------------- | ------- | ----------- | ------- |
| `conn_new_cmd` | `EV_READ|EV_PERSIST` | `event_handler` | 绑定到网络通信线程的`Reactor`事件管理器中 |

对于主线程使用的是`udp`通信方式时会调用`driver_machine`中的`conn_read`分支，然后根据读取套接字的数据状态来进行不同的处理方式。

## 3. `sub thread`的事件以及处理器
前面介绍了主线程的网络设置，下面将讲述网络通信子线程的事件设置以及对应的事件处理器,其图示如下:

![sub thread网络设置流程图示](https://github.com/whynotAC/analysis_memcached/blob/master/network/网络子线程网络设置流程图.jpg)

| 通信端口 | 监听事件 | 处理事件函数 | 绑定的线程 |
| `pipe`函数创建的通道的读端 | `EV_READ | EV_PERSIST` | `thread_libevent_process` | 绑定到网络通信子线程的`Reactor`事件管理器中 |

事件处理器的函数代码如下:

```
/* 
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
static void thread_libevent_process(int fd, short which, void *arg) {
	LIBEVENT_THREAD *me = arg;
	CQ_ITEM *item;
	char buf[1];
	conn *c;
	unsigned int timeout_fd;
	// 读取发送过来的命令
	if (read(fd, buf, 1) != 1) {
		if (settings.verbose > 0)
			fprintf(stderr, "Can't read from libevent pipe\n");
		return;
	}
	
	// 根据发送过来的命令来进行处理
	switch (buf[0]) {
	case 'c':			// 创建新的connection
		item = cq_pop(me->new_conn_queue);
		
		if (NULL == item) {
			break;
		}
		switch (item->mode) {
			case queue_new_conn:   // 新链接
				// 将新来的connection连接到Reactor管理器上。
				// init_state为 conn_new_cmd
				// event_flags为 EV_READ | EV_PERSIST
				// 处理函数为 event_handler
				c = conn_new(item->sfd, item->init_state, item->event_flags,
									item->read_buffer_size, item->transport,
									me->base);
				if (c == NULL) {
					if (IS_UDP(item->transport)) {
						fprintf(stderr, "Can't listen for events on UDP socket\n");
						exit(1);
					} else {
						if (settings.verbose > 0) {
							fprintf(stderr, "Can't listen for events on fd %d\n",
								item->sfd);
						}
						close(item->sfd);
					}
				} else {
					c->thread = me;
				}
				break;
			
			case queue_redispatch:	// 重新绑定connection
				conn_worker_readd(item->c);
				break;
		}
		cqi_free(item);
		break;
	/* we were told to pause and report in */
	case 'p':		// 网络子线程初始化完成
		register_thread_initialized();
		break;
	/* a client socket timed out */
	case 't':		// connection超时处理
		if (read(fd, &timeout_fd, sizeof(timeout_fd)) != sizeof(timeout_fd)) {
			if (settings.verbose > 0)
				fprintf(stderr, "Can't read timeout fd from libevent pipe\n");
			return;
		}
		conn_close_idle(conns[timeout_fd]);
		break;
	}
}
```
由上述代码可以看出网络通信子线程中事件处理器，处理三种命令`c`、`p`、`t`。它们分别对应新链接的到来，通信子线程的初始化完成、`connection`的超时处理。

## 4. `connection`的生命周期
前面介绍了主线程和网络子线程的网络设置，由源代码可以看出主线程中进行监听，接收网络链接后，通过调用`dispatch_conn_new`函数，将新的`connection`分配到网络通信子线程的`Reactor`管理器上，从而开启了`connection`的生命周期。

![connection生命周期图示](https://github.com/whynotAC/analysis_memcached/blob/master/network/connection的生命周期.png)

下面将从四个方面对`connection`的生命周期进行介绍:

1. 新`connection`的处理操作。
2. `connection`的主动退出。
3. `connection`的超时退出。
4. `connection`是否允许接收判断。

对于新`connection`接收的相关代码前面已经介绍过很多次，这里不再进行介绍了。其他几个方面将进行逐一介绍。

### `connection`的主动退出
当网络通信结束时，网络子线程会调用`drive_machine`函数中的`case conn_closing`分支进行处理，其源代码如下:

```
// drive_machine函数中
case conn_closing:
	if (IS_UDP(c->transport))
		conn_cleanup(c);
	else
		conn_close(c);
	stop = true;
	break;

// 对于UDP的套接字进行的处理conn_cleanup函数
static void conn_cleanup(conn *c) {
	assert(c != NULL);
	
	conn_release_items(c);	// 清空conn链接的item
	
	if (c->write_and_free) {		// 清空写出缓存空间
		free(c->write_and_free);
		c->write_and_free = 0;
	}
	
	if (c->sasl_conn) {				// 清空认证信息
		assert(settings.sasl);
		sasl_dispose(&c->sasl_conn);
		c->sasl_conn = NULL;
	}
	
	if (IS_UDP(c->transport)) {		// 重新设置主线程中的UDP状态
		conn_set_state(c, conn_read);
	}
}

// 对于TCP的套接字进行的处理conn_close函数

```