memcached网络模型
==================================
前面通过两篇文章介绍了基础的网络模型以及`memcached`相关的网络设置，本文将着重介绍`memcached`网络模型。本文将从以下几个方面详细进行介绍:

1. 使用的结构体以及全局变量
2. `connection`的生命周期
3. `main thread`的事件以及处理器
4. `sub thread`的事件以及处理器
5. `memcached`网络模型
6. `memcached`的定时器
7. 总结

## 使用的结构体以及全局变量
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

// 全局变量
static struct event_base *main_base;	// 主函数中使用的`Reactor`具柄
static int max_fds;						// 最大的文件描述符


```