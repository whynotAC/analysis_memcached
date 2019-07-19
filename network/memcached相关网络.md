memcached网络相关函数
====================================
前面介绍过了基础的网络模型，本文将通过介绍`memcached`的源代码中网络相关函数。
主要分为两个部分:

1. `memcached`主线程中网络设置过程。
2. `memcached`子线程中网络设置过程。
3. `memcached`调用`libevent`的函数。

## 1. `memcached`主线程中网络设置 
主函数中关于网络部分的代码如下所示:

```
// memcached.c文件中main函数中代码
int main (int argc, char **argv) {
	···			// 其他代码
	// 网络设置有两种方式
	// 1. 使用unix socket，使用本地的文件描述符
	// 2. 使用tcp/udp端口，创建网络接口
	
	// 1. unix socket本地文件描述符
	/* create unix mode sockets after dropping privileges */
	if (settings.socketpath != NULL) {
		errno = 0;
		if (server_socket_unix(settings.socketpath, settings.access)) {
			vperror("failed to listen on UNIX socket: %s", settings.socketpath);
			exit(EX_OSERR);
		}
	}
	
	// 2. 创建tcp/udp网络接口
	/* create the listening socket, bind it, and init */
	if (settings.socketpath == NULL) {
		const char *portnumber_filename = getenv("MEMCACHED_PORT_FILENAME");
		char *temp_portnumber_filename = NULL;
		size_t len;
		FILE *portnumber_file = NULL;
		
		// 创建网络接口的保存文件
		if (portnumber_filename != NULL) {
			len = strlen(portnumber_filename)+4+1;
			temp_portnumber_filename = malloc(len);
			snprintf(temp_portnumber_filename,
							len,
							"%s.lck", portnumber_filename);
			portnumber_file = fopen(temp_portnumber_filename, "a");
			if (portnumber_file == NULL) {
				fprintf(stderr, "Failed to open \"%s\": %s\n",
							temp_portnumber_filename, strerror(errno));
			}
		}
		
		// 创建TCP端口
		errno = 0;
		if (settings.port && server_sockets(settings.port, tcp_transport,
											portnumber_file)) {
			vperror("failed to listen on TCP port %d", settings.port);
			exit(EX_OSERR);
		}
		
		/*
		 * initialization order: first create the listening sockets
		 * (may need root on low ports), then drop root if needed,
		 * then daemonize if needed, then init libevent (in some cases
		 * descriptors created by libevent wouldn't survive forking).
		 */
		 
		 /* create the UDP listening socket and bind it */
		 // 创建UDP端口
		 errno = 0;
		 if (settings.udpport && server_sockets(settings.udpport, udp_transport,
		 										pornumber_file)) {
		 	vperror("failed to listen on UDP port %d", settings.udpport);
		 	exit(EX_OSERR);
		 }
		 
		 if (portnumber_file) {
		 	fclose(portnumber_file);
		 	rename(temp_portnumber_filename, portnumber_filename);
		 }
		 if (temp_portnumber_filename)
		 	free(temp_portnumber_filename);
	}
	
	// 其他代码
	···
	
	// enter the event loop
	// 进入事件同步分发处理器
	if (event_base_loop(main_loop, 0) != 0) {
		retval = EXIT_FAILURE;
	}
	
	// 其他代码
	···
}
```
上述代码分为两个部分进行介绍，首先介绍`unix socket`本地文件描述符的设置过程，然后介绍`tcp/udp`网络接口的设置过程。

### 1. `unix socket`设置过程
`main`函数中包含`unix socket`的代码部分如下:

```
if (settings.socketpath != NULL) {
	errno = 0;
	if (server_socket_unix(settings.socketpath, settings.access)) {
		vperror("failed to listen on UNIX socket: %s", settings.socketpath);
		exit(EX_OSERR);
	}
}
```
主要函数`server_socket_unix`的主要内容如下:

```
// server_socket_unix函数中使用的结构体
linger结构体用于控制close函数的断开方式，有两种方式:
1. 优雅断开
2. 强制断开

linger结构体数据结构如下:
#include <arpa/inet.h>
struct linger {
	int l_onoff;
	int l_linger;
};
由l_onoff和l_linger两个结构体变量来控制断开方式。
1. 当l_onoff = 0; l_linger忽略时。close()立刻返回，底层会将未发送完的数据发送完后再释放资源，即优雅断开。
2. 当l_onoff != 0; l_linger = 0时。close()立刻返回，但不会发送未发送完成的数据，而是通过一个REST包强制的关闭socket描述符，即立即断开。
3. 当l_onoff != 0; l_linger > 0时。close()不会立即返回，内核会延迟一段时间(由l_linger的值来决定)。如果超时时间到达之前，发送完未发送的数据(包括FIN包)并得到另一端的确认，close()会返回正确，socket描述符优雅断开。否则，close()会直接返回错误值，未发送的数据丢失，socket描述符被强制性退出。需要注意如果socket描述符被设置为非阻塞型，则close()会直接返回值。

// unix socket的处理代码
static int server_socket_unix(const char *path, int access_mask) {
	int sfd;		// 文件描述符
	struct linger ling = {0, 0};	// 控制文件描述符的关闭方式
	struct sockaddr_un addr;		// 设置文件描述符的地址信息
	struct stat tstat;		// 用于获取文件的状态
	int flags = 1;
	int old_umask;		// 用于获取文件的权限设置
	
	// 文件路径不存在
	if (!path) {
		return 1;
	}
	
	// 获取一个新的socket，并且设置为非阻塞的socket
	if ((sfd = new_socket_unix()) == -1) {		return 1;
	}
	
	/*
	 * Clean up a previous socket file if we left it around
	 */
	// 获取文件的状态
	if (lstat(path, &tstat) == 0) {
		if (S_ISSOCK(tstat.st_mode))
			unlink(path);
	}
	
	// 设置socket的状态
	setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags)); // 将socket文件描述符设置为可复用的
	setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags)); // 将socket文件描述符设置为链接保活
	setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling)); // 设置socket文件描述符的close状态
	
	/*
	 * the memset call clears nonstandard fields in some implementations
	 * that otherwise mess things up.
	 */
	memset(&addr, 0, sizeof(addr));
	
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	assert(strcmp(addr.sun_path, path) == 0);
	old_mask = umask( ~(access_mask&0777));
	// 绑定文件描述符
	if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("bind()");
		close(sfd);
		umask(old_umask);
		return 1;
	}
	umask(old_umask);
	
	// 监听文件描述符
	if (listen(sfd, settings.backlog) == -1) {
		perror("listen()");
		close(sfd);
		return 1;
	}
	
	// 将文件描述符加入到Reactor管理器中,
	// 监听事件为EV_READ|EV_PERSIST, 事件状态为conn_listening
	if (!(listen_conn = conn_new(sfd, conn_listening,
										EV_READ | EV_PERSIST, 1,
										local_transport, main_base))) {
		fprintf(stderr, "failed to create listening connection\n");
		exit(EXIT_FAILURE);
	}
	
	return 0;
}

// new_socket_unix函数，创建socket文件描述符
static int new_socket_unix(void) {
	int sfd;
	int flags;
	
	// 申请文件描述符
	if ((sfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("socket()");
		return -1;
	}
	
	// 设置文件描述符非阻塞标示
	if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
			fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("setting O_NONBLOCK");
		close(fd);
		return -1;
	}
	
	return sfd;
}
```
通过上面的代码可以看出，其主要功能是申请文件描述符并设置其状态，然后将文件描述符加入到`Reactor`管理器中(即调用`conn_new`)。对于`conn_new`函数后续会进行介绍。

### 2. 网络接口设置过程
`memcached`主要被用于网络的`K/V`存储，其提供对外的网络接口。`memcached`不仅提供了`tcp`接口，还提供了`udp`接口。本小节将详细介绍一下从网络套接字申请到注册到`Reactor`管理器为止。`memcached`中`main`函数中`tcp/udp`接口的代码如下:

```
/* create the listening socket, bind it, and init */
if (settings.socketpath == NULL) {
	const char *portnumber_filename = getenv("MEMCACHED_PORT_FILENAME");
	char *temp_portnumber_filename = NULL;
	size_t len;
	FILE *portnumber_file = NULL;
	
	if (partnumber_filename != NULL) {
		len = strlen(portnumber_filename)+4+1;
		temp_portnumber_filename = malloc(len);
		snprintf(temp_portnumber_filename,
					len,
					"%s.lck", portnumber_filename);
					
		portnumber_file = fopen(tem_portnumber_filename, "a");
		if (portnumber_file == NULL) {
			fprintf(stderr, "Failed to open \"%s\": %s\n",
						temp_portnumber_filename, strerror(errno));
		}
	}
	
	// 创建TCP接口服务
	errno = 0;
	if (settings.port && server_sockets(settings.port, tcp_transport,
												portnumber_file)) {
		vperror("failed to listen on TCP port %d", settings.port);
		exit(EX_OSERR);
	}
	
	/*
	 * initialization order: first create the listening sockets
	 * (may need root on low ports), then drop root if needed,
	 * then daemonize if needed, then init libevent (in some cases
	 * descriptors created by libevent wouldn't surivive forking).
	 */
	 
	 /* create the UDP listening socket and bing it */
	 // 创建UDP接口服务
	 errno = 0;
	 if (settings.udpport && server_sockets(settings.udpport, udp_transport,
	 											     portnumber_file)) {
	 	vperror("failed to listen on UDP port %d", settings.udpport);
	 	exit(EX_OSERR);
	 }
	 
	 if (portnumber_file) {
	 	fclose(portnumber_file);
	 	rename(temp_portnumber_filename, portnumber_filename);
	 }
	 if (temp_portnumber_filename)
	 	free(temp_portnumber_filename);
}
```
通过上述的代码可以看出无论创建`TCP`接口还是创建`UDP`接口，都调用`server_sockets`函数来实现接口创建并将套接字加入到`Reactor`管理器中。

函数`server_sockets`的源代码如下:

```
static int server_sockets(int port, enum network_transport transport,
									FILE *portnumber_file) {
	// 判断是否需要解析IP地址
	if (settings.inter == NULL) {
		// 调用server_socket创建套接字，并将其加入到Reactor管理器中
		return server_socket(settings.inter, transport, portnumber_file);
	} else {
		// 截断settings.inter中的网络地址后，调用server_socket
		// tokenize them and bind to each one of them..
		char *b;
		int ret = 0;
		char *list = strdup(settings.inter);
		
		if (list == NULL) {
			fprintf(stderr, "Failed to allocate memory for parsing server interface string\n");
			return 1;
		}
		// 依次取出对应的IP地址进行处理
		for (char *p = strtok_r(list, ";,", &b);
				p != NULL;
				p = strtok_r(NULL, ";,", &b)) {
			int the_port = port;	// 获取端口
			
			// 获取IP地址
			char *h = NULL;
			if (*p == '[') {
				// expecting it to be an IPv6 address enclosed in []
				// i.e. RFC3986 style recommended by RFC5952
				char *e = strchr(p, ']');
				if (e == NULL) {
					fprintf(stderr, "Invalid IPV6 address: \"%s\"", p);
					free(list);
					return 1;
				}
				h = ++p;	// skip then opening '['
				*e = '\0';
				p = ++e;	// skip the closing ']'
			}
			
			// 获取端口
			char *s = strchr(p, ':');
			if (s != NULL) {
				// If no more semicolons - attempt to treat as port number
				// Otherwise the only valid option is an unenclosed IPv6 without port, 
				// until of course there was an RFC3986 IPv6 previously specified -
				// in such a case there is no good option, will just send it to fail
				// as port number.
				if (strchr(s + 1, ':') == NULL || h != NULL) {
					*s = '\0';
					++s;
					if (!safe_strtol(s, &the_port)) {
						fprintf(stderr, "Invalid port number: \"%s\"", s);
						free(list);
						return 1;
					}
				}
			}
			
			if (h != NULL)
				p = h;
				
			if (strcmp(p, "*") == 0) {
				p = NULL;
			}
			ret |= server_socket(p, the_port, transport, pornumber_file);
		}
		free(list);
		return ret;
	}
}

// 网络使用的系统结构体
struct addrinfo {
	int ai_flags;			// input flags
	int ai_family;		// protocol family for socket
	int ai_socktype;		// socket type
	int ai_protocol;		// protocol for socket
	socklen_t ai_addrlen;	// length of socket-address
	struct sockaddr *ai_addr;	// socket-address for sockets
	char *ai_canonname;		// canonical name for service location
	struct addrinfo *ai_next;	// pointer to next in list
};

系统函数getaddrinfo是为了支持IPv6引入的函数，它是协议无关的，既可用于IPv4也可用于IPv6。getaddrinfo函数能够处理名字到地址以及服务到端口这两种转换，返回的是一个addrinfo的结构(列表)指针而不是一个地址清单。这些addrinfo结构随后可由套接口函数直接使用。

// server_socket的源代码
/**
 * Create a socket and bind it to a specific port number
 * @param interface the interface to bind to
 * @param port the port number to bind to
 * @param transport the transport protocol (TCP/UDP)
 * @param portnumber_file A filepointer to write the port numbers to
 * 			when they are successfully added to the list of ports we
 * 		 	listen on.
 */
static int server_socket(const char *interface,
								int port,
								enum network_transport transport,
								FILE *portnumber_file) {
	int sfd;
	struct linger ling = {0, 0};
	struct addrinfo *ai;					// 用于保存获取的IP地址和端口号，从主机名和服务名
	struct addrinfo *next;					
	struct addrinfo hints = { .ai_flags = AI_PASSIVE,  // 设置标记，获取的addrinfo将用于bind函数
									.ai_family = AF_UNSPEC }; // 用于获取addrinfo可以用于各种网络协议
	char port_buf[NI_MAXSERV];
	int error;
	int success = 0;
	int flags = 1;
	
	// 用于设置socket的类型
	hints.ai_socktype = IS_UDP(transport) ? SOCK_DGRAM : SOCK_STREAM;
	
	if (port == -1) {
		port = 0;
	}
	snprintf(port_buf, sizeof(port_buf), "%d", port);
	error = getaddrinfo(interface, port_buf, &hits, &ai);
	if (error != 0) {
		if (error != EAI_SYSTEM)		// 判断是否为系统问题
			fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
		else
			perror("getaddrinfo");
		return 1;
	}
	
	// 循环处理addrinfo结构体
	for (next = ai; next; next = next->ai_next) {
		conn *listen_conn_add;
		// 创建套接字，并设置为非阻塞
		if ((sfd = new_socket(next)) == -1) {
			/* getaddrinfo can return "junk" addresses,
			 * we make sure at least one works before erroring.
			 */
			if (errno == EMFILE) {		// 没有套接字可使用
				/* ...unless we're out of fds */
				perror("server_socket");
				exit(EX_OSERR);
			}
			continue;
		}
		
		// 处理IPv6情况
#ifdef IPV6_V6ONLY
		if (next->ai_family == AF_INET6) {
			// 设置套接字仅支持IPv6进行通信
			error = setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&flags, sizeof(flags));
			if (error != 0) {
				perror("setsockopt");
				close(sfd);
				continue;
			}
		}
#endif

		// 设置套接字可以复用标志
		setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
		// 判断使用协议是UDP还是TCP
		if (IS_UDP(transport)) {
			maximize_sndbuf(sfd);
		} else {
			// 设置TCP的网络套接字
			error = setsockopt(sfd, SOL_SOCKET, SO_KEEPAVLIE, (void *)&flags, sizeof(flags));
			if (error != 0)
				perror("setsockopt");
			
			error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
			if (error != 0)
				perror("setsockopt");
			
			// 禁用Nagle’s Algorithm，防止发送小数据包时被合并发送，导致数据发送变慢
			error = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
			if (error != 0)
				perror("setsockopt");
		}
		
		// 绑定socket
		if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1) {
			if (errno != EADDRINUSE) {
				perror("bind()");
				close(sfd);
				freeaddrinfo(ai);
				return 1;
			}
			close(sfd);
			continue;
		} else {
			// 绑定成功
			success++;
			// 判断socket使用的协议是否为TCP协议
			if (!IS_UDP(transport) && listen(sfd, settings.backlog) == -1) {
				perror("listen()");
				close(sfd);
				freeaddrinfo(ai);
				return 1;
			}
			// 输出绑定信息
			if (portnumber_file != NULL &&
					(next->ai_addr->sa_family == AF_INET ||
						next->ai_addr->sa_family == AF_INET6)) {
				union {
					struct sockaddr_in in;
					struct sockaddr_in6 in6;
				} my_sockaddr;
				socklen_t len = sizeof(my_sockaddr);
				if (getsockname(sfd, (struct sockaddr*)&my_sockaddr, &len) == 0) {
					if (next->ai_addr->sa_family == AF_INET) {
						fprintf(portnumber_file, "%s INET: %u\n",
									IS_UDP(transport) ? "UDP" : "TCP",
									ntohs(my_sockaddr.in.sin_port));
					} else {
						fprintf(portnumber_file, "%s INET6: %u\n",
									IS_UDP(transport) ? "UDP" : "TCP",
									ntohs(my_sockaddr.in6.sin6_port));
					}
				}
			}
		}
		
		// 对于UDP和TCP的socket进行分别处理
		if (IS_UDP(transport)) {
			// udp处理
			int c;
			
			for (c = 0; c < settings.num_threads_per_udp; c++) {
				/* Allocate one UDP file descriptor per worker thread;
				 * this allows "stats conns" to separately list multiple
				 * parallel UDP requests in progress.
				 * The dispatch code round-robins new connection requests
				 * among threads, so this is guaranteed to assign one
				 * FD to each thread.
				 */
				 // 将sfd进行复制, dup函数
				 int per_thread_fd = c ? dup(sfd) : sfd;
				 // 将UDP的套接字复制后分配到子线程中进行处理
				 dispatch_conn_new(per_thread_fd, conn_read,
				 							EV_READ | EV_PERSIST,
				 							UDP_READ_BUFFER_SIZE, transport);
			}
		} else {
			// tcp处理
			if (!(listen_conn_add = conn_new(sfd, conn_listening,
												EV_READ | EV_PERSIST, 1,
												transport, main_base))) {
				fprintf(stderr, "failed to create listening connection\n");
				exit(EXIT_FAILURE);
			}
			// 将监听的socket加入到以listen_conn为首的链表中
			listen_conn_add->next = listen_conn;
			listen_conn = listen_conn_add;
		}
	}
	
	freeaddrinfo(ai);
	
	// Return zero iff we detected no errors in starting up connections
	return success == 0;
}

// 创建socket并设置其为非阻塞
static int new_socket(struct addrinfo *ai) {
	int sfd;
	int flags;
	
	// 创建socket
	if ((sfd = socket(ai->ai_faimly, ai->ai_socktype, ai->ai_protocol)) == -1) {
		return -1;
	}
	
	// 设置未非阻塞
	if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
			fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("setting O_NONBLOCK");
		close(sfd);
		return -1;
	}
	return sfd;
}

```
上面的代码中最终通过`dispatch_conn_new`和`conn_new`函数分别将`UDP`和`TCP`的套接字加入到`Reactor`管理器中，然后等到事件的发生以及处理。

### 3. 加入`Reactor`管理器的源代码
`UDP`套接字加入到`Reactor`管理器中调用`dispatch_conn_new`函数的源代码如下:

```
/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming conneciton.
 */
// 将套接字分配到通信网络自线程中
void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags,
								int read_buffer_size, enum network_transport transport) {
	CQ_ITEM *item = cqi_new();
	char buf[1];
	if (item == NULL) {
		close(sfd);
		// given that malloc failed this may also fail, but let's try
		fprintf(stderr, "Failed to allocate memory for connection object\n");
		return;
	}
	
	// 循环进行分配网络套接字
	int tid = (last_thread + 1) % settings.num_threads;
	
	LIBEVENT_THREAD *thread = threads + tid;
	
	last_thread = tid;
	
	// 设置CQ_ITEM结构内容
	item->sfd = sfd;						// 描述符
	item->init_state = init_state;	// 确定初始化状态
	item->event_flags = event_flags;	// 设置套接字的监听事件
	item->read_buffer_size = read_buffer_size; // readbuffer的缓冲区大小
	item->transport = transport;		// 使用的网络协议
	item->mode = queue_new_conn;		// 网络子线程的处理模式
	
	cq_push(thread->new_conn_queue, item); // 将CQ_ITEM结构体加入到线程中新链接的链表中
	
	MEMCACHED_CONN_DISPATCH(sfd, thread->thread_id);
	buf[0] = 'c';		// 告诉网络通信子线程所需要的操作
	// 向网路通信子线程发送消息
	if (write(thread->notify_send_fd, buf, 1) != 1) {
		perror("Writing to thread notify pipe");
	}
}

// 网络通信子线程使用的事件处理函数
/*
 * Processes on incoming "handle a new conneciton" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
static void thread_libevent_process(int fd, short which, void *arg) {
	LIBEVENT_INFO *me = arg;
	CQ_ITEM *item;
	char buf[1];
	conn *c;
	unsigned int timeout_fd;
	
	// 读取发送过来的信息
	if (read(fd, buf, 1) != 1) {
		if (settings.verbose > 0)
			fprintf(stderr, "Can't read from libevent pipe\n");
		return;
	}
	
	// 处理发送过来的消息
	switch (buf[0]) {
	case 'c':
		// 新传递一个socket需要进行处理
		item = cq_pop(me->new_conn_queue); // 从线程的新链接的链表中获取一个CQ_ITEM
		
		if (NULL == item) {
			break;
		}
		switch (item->mode) {
			case queue_new_conn:
				// 处理新的网络套接字
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
					c->thread =me;
				}
				break;
			// 将网络套接字重新加入到Reactor管理器中
			case queue_redispatch:
				conn_worker_readd(item->c);
				break;
		}
		// 释放使用的CQ_ITEM
		cqi_free(item);
		break;
	// we were told to pause and report in
	case 'p':
		// 通知main thread子线程已经初始化完毕
		register_thread_initialized();
		break;
	// a client socket timed out
	case 't':
		// 处理超时的connection，获取超时的套接字
		if (read(fd, &timeout_fd, sizeof(timeout_fd)) != sizeof(timeout_fd)) {
			if (settings.verbose > 0)
				fprintf(stderr, "Can't read timeout fd from libevent pipe\n");
			return;
		}
		// 关闭connection
		conn_close_idle(conns[timeout_fd]);
		break;
	}
}
```
由上述的代码可以看出`udp`套接字绑定的过程中仍然需要调用`conn_new`函数，同样这个函数也会在本地网络套接字和`tcp`套接字加入到`Reactor`管理器的过程中也需要调用。`conn_new`函数的源代码如下:

```
conn *conn_new(const int sfd, enum conn_states init_state,
					const int event_flags,
					const int read_buffer_size, enum network_transport transport,
					struct event_base *base) {
	conn *c;	// 新的connection结构体
	
	assert(sfd >= 0 && sfd <= max_fds);	// 判断套接字是否在[0,max_fds]的范围内
	c = conns[sfd];		// 获取sfd对应的conn结构
	
	// 判断conn结构是否存在
	if (NULL == c) {
		// sfd对应的conn结构不存在
		if (!(c = (conn *)calloc(1, sizeof(conn)))) {
			STATS_LOCK();
			stats.malloc_fails++;		// 分配失败
			STATS_UNLOCK();
			fprintf(stderr, "Failed to allocate connection object\n");
			return NULL;
		}
		MEMCACHED_CONN_CREATE(c);
		
		// 设置conn的结构体
		c->rbuf = c->wbuf = 0;
		c->ilist = 0;
		c->suffixlist = 0;
		c->iov = 0;
		c->msglist = 0;
		c->hdrbuf = 0;
		// 申请空间的大小
		c->risze = read_buffer_size;
		c->wsize = DATA_BUFFER_SIZE;
		c->isize = ITEM_LIST_INITIAL;
		c->suffixsize = SUFFIX_LIST_INITIAL;
		c->iovsize = IOV_LIST_INITIAL;
		c->msgsize = MSG_LIST_INITIAL;
		c->hdrsize = 0;
		// 空间申请
		c->rbuf = (char *)malloc((size_t)c->rsize);
		c->wbuf = (char *)malloc((size_t)c->wsize);
		c->ilist = (item **)malloc(sizeof(item *) * c->isize);
		c->suffixlist = (char **)malloc(sizeof(char *) * c->suffixsize);
		c->iov = (struct iovec *)malloc(sizeof(struct iovec) * c->iovsize);
		c->msglist = (struct msghdr *)malloc(sizeof(struct msghdr) * c->msgisze);
		
		if (c->rbuf == 0 || c->wbuf == 0 || c->ilist == 0 || c->iov == 0 ||
				c->msglist == 0 || c->suffixsize == 0) {
			conn_free(c); 	// 释放conn结构体
			STATS_LOCK();
			stats.malloc_fails++;	// 分配空间失败
			STATS_UNLOCK();
			fprintf(stderr, "Failed to allocate buffers for connection\n");
			return NULL;
		}
		
		STATS_LOCK();
		stats_state.conn_structs++;	// 记录使用的conn结构体的个数
		STATS_UNLOCK();
		
		c->sfd = sfd;		// 设置conn结构体的套接字
		conns[sfd] = c;	// 将conn结构体加入到conns指向的结构体数组中
	}
	
	c->transport = transport;	// 设置网络协议
	c->protocol = settings.binding_protocol;	
	/* unix socket mode doesn't need this, so zeroed out. but why
	 * is this done for every command? presumably for UDP
	 * mode. */
	if (!settings.socketpath) {
		c->request_addr_size = sizeof(c->request_addr);
	} else {
		c->request_addr_size = 0;
	}
	
	// 获取链接的地址，用于输出
	if (transport == tcp_transport && init_state == conn_new_cmd) {
		if (getpeername(sfd, (struct sockaddr *)&c->request_addr,
							&c->request_addr_size)) {
			perror("getpeername");
			memset(&c->request_addr, 0, sizeof(c->request_addr));
		}
	}
	
	if (settings.verbose > 1) {
		if (init_state == conn_listening) {
			fprintf(stderr, "<%d server listening (%s)\n", sfd, prot_text(c->protocol));
		} else if (IS_UDP(transport)) {
			fprintf(stderr, "<%d server listening (udp)\n", sfd);
		} else if (c->protocol == negotiating_prot) {
			fprintf(stderr, "<%d new auto-negotiating client connection\n", sfd);
		} else if (c->protocol == ascii_prot) {
			fprintf(stderr, "<%d new ascii client connection.\n", sfd);
		} else if (c->protocol == binary_prot) {
			fprintf(stderr, "<%d new binary client connection.\n", sfd);
		} else {
			fprintf(stderr, "<%d new unknown (%d) client connection\n", sfd, c->protocol);
			assert(false);
		}
	}
	
	// 设置conn结构体其他成员
	c->state = init_state;
	c->rlbytes = 0;
	c->cmd = -1;
	c->rbytes = c->wbytes = 0;
	c->wcurr = c->wbuf;
	c->rcurr = c->rbuf;
	c->ritem = 0;
	c->icurr = c->ilist;
	c->suffixcurr = c->suffixlist;
	c->ileft = 0;
	c->suffixleft = 0;
	c->iovused = 0;
	c->msgcurr = 0;
	c->msgused = 0;
	c->authenticated = false;
	c->last_cmd_time = current_time;	// initialize for idle kicker
#ifdef EXTSTORE
	c->io_wraplist = NULL;
	c->io_wrapleft = 0;
#endif
	
	c->write_and_go = init_state;
	c->write_and_free = 0;
	c->item = 0;
	
	c->noreply = false;
	
	// 设置网络套接字关心的事件以及处理函数，将套接字加入到Reactor管理器中
	event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
	event_base_set(base, &c->event);
	c->ev_flags = event_falgs;
	
	if (event_add(&c->event, 0) == -1) {
		perror("event_add");
		return NULL;
	}
	
	STATS_LOCK();
	stats_state.curr_conns++;
	stats.total_conns++;
	STATS_UNLOCK();
	
	MEMCACHED_CONN_ALLOCATE(c->sfd);
	
	return c;
}
```
通过`conn_new`的代码可知，将传递过来的套接字以及初始化状态，关心事件传递过来，将其包装成`conn`结构体，然后加入到对应线程的`Reactor`管理器中，设置事件的处理函数。

## 2. `memcached`子线程中网络设置过程
前面介绍了`memcached`主线程的网络设置，本小节将介绍`memcached`的网络子线程相关的设置操作。其源代码如下所示:

```
// 在thread.c文件中memcached_thread_init函数
void memcached_thread_init(int nthreads, void *arg) {
	// 其他代码
	···
	
	// 网络相关设置代码
	for (i = 0; i < nthreads; i++) {
		int fds[2];
		if (pipe(fds)) {
			perror("Can't create notify pipe");
			exit(1);
		}
		
		threads[i].notify_receive_fd = fds[0];
		threads[i].notify_send_fd = fds[1];
#ifdef EXTSTORE
		threads[i].storage = arg;
#endif
		// 网络相关设置
		setup_thread(&threads[i]);
		/* Reserve three fds for the libevent base, and two for the pipe */
		stats_state.reserved_fds += 5;
	}
	
	// 其他代码
	···
}

// thread.c文件中setup_thread函数
/*
 * Set up a thread's information
 */
static void setup_thread(LIBEVENT_THREAD *me) {
#if defined(LIBEVENT_VERSION_NUMBER) && LIBEVENT_VERSION_NUMBER >= 0x02000101
	struct event_config *ev_config;
	ev_config = event_config_new();
	event_config_set_flag(ev_config, EVENT_BASE_FLAG_NOLOCK);
	me->base = event_base_new_with_config(ev_config);
	event_config_free(ev_config);
#else
	me->base = event_init();
#endif

	// 其他代码
	···
	
	/* Listen for notifications from other threads */
	event_set(&me->notify_event, me->notify_receive_fd,
				EV_READ | EV_PERSIST, thread_libevent_process, me);
	event_base_set(me->base, &me->notify_event);
	
	// 其他代码
	···
} 
```
通过上述代码可知，网络子线程监听`notify_receive_fd`通道的读端，监听事件为`EV_READ|EV_PERSIST`，处理事件的函数为`thread_libevent_process`。

## 3. `memcached`调用`libevent`的函数
`memcached`中使用`libevent`中的`Reactor`管理器以及事件分发函数来管理多个网络套接字，其使用的`libevent`的函数如下:

|  函数名   |  定义   | 作用   | 备注 |
| ------	| ---- | ---- | ---- |
| `event_config_new` | `struct event_config *event_config_new(void)` | 返回一个配置信息的结构体 | 通过修改结构体内容，作为参数传递给`event_base_new_with_config`可以生成目标`event_base` |
| `event_config_set_flag` | `int event_config_set_flag(struct event_config *cfg, int flag)` | 设置`event_config`的标志 | `memcached`中设置`event_config`的配置不设置锁 |
| `event_base_new_with_config` | `struct event_base *event_base_new_with_config(const sturct event_config *cfg)` | 根据`event_config`初始化一个新的`event_base` | 无 |
| `event_config_free` | `void event_config_free(struct event_config *cfg)` | 释放`event_config`对象 | 无 |
| `event_init` | `struct event_base *event_init(void)` | 低版本`libevent`初始化`event_base`结构 | 无 |
| `event_set` | `void event_set(struct event *ev, int fd, short events, void (*callback)(int, short, void *), void *arg)` | 此函数用于设定`socket`对应的观察事件以及对应的事件处理器 | `ev`--保存事件关联关系的结构体,`fd`--文件描述符，`events`--需要观察的事件,`callback`--事件处理器,`arg`--用户自定义的参数(传递给事件处理器的最后一个参数) |
| `event_base_set` | `int event_base_set(struct event_base *base, struct event *ev)` | 将事件相关的信息绑定到`Reactor`管理器 | `listening`事件绑定到`main thread`的`Reactor`管理器中，其他事件绑定到对应的`sub thread`的`Reactor`管理器中 |
| `event_add` | `int event_add(struct event *ev, const struct timeval *tv)` | 将事件增加到等待事件中 | `tv`--等待事件，如果为0则永久等待 |
| `event_base_free` | `void event_base_free(struct event_base *base)` | 释放`Reactor`相关资源 | 无 |
| `event_base_loop` | `int event_base_loop(struct event_base *base, int flags)` | 开启`Reactor`管理器的循环 | 无 |
| `evtimer_set` | `#define evtimer_set(ev, cb, arg) event_set((ev), -1, 0, (cb), (arg))` | 用于初始化定时器 | 无 |
| `evtimer_add` | `#define evtimer_add(ev, tv) event_add((ev), (tv))` | 激活定时器，将定时器加入到`Reactor`管理器中 | `tv`--定时器超时时间 |
| `evtimer_del` | `#define evtimer_del(ev) event_del(ev)` | 删除定时器 | 无 |
上面表格中详细介绍了`memcached`中调用的`libevent`的函数，使用`Reactor`网络模式来管理套接字以及事件分发处理，并且使用`libevent`提供的定时器来实现`current_time`的秒级更新。