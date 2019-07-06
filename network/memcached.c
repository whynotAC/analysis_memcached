
#include "memcached.h"

/* exported globals */
conn **conns;

/* file scope variables */
static conn *listen_conn = NULL;
static int max_fds;
static struct event_base *main_base;

/**
 * Initialize the connections array. We don't actually allocate connection
 * structures until they're needed, so as to avoid wasting memory when the 
 * maximum connection count is much higher than the actual number of
 * connections.
 *
 * This does end up wasting a few pointers' worth of memory for FDs that are
 * used for things other than connections, but that's worth it in exchange for
 * being able to directly index the conns array by FD.
 */
static void conn_init(void) {
    /* We're ulinkely to see an FD much higher than maxconns. */
    int next_fd = dup(1);   // 获取第一个未被使用的文件描述符
    int headroom = 10;   /* account for extra unexpected open FDs */
    struct rlimit rl;       // 资源限制

    max_fds = settings.maxconns + headroom + next_fd; // 获取需要connection结构个数

    /* But if possible, get the actual highest FD we can possibly ever see. */
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        max_fds = rl.rlim_max;
    } else {
        fprintf(stderr, "Failed to query maximum file descriptor; "
                        "falling back to maxconns\n");
    }

    // 关闭文件描述符
    close(next_fd);

    // 申请connect数组空间
    if ((conns = calloc(max_fds, sizeof(conn *))) == NULL) {
        fprintf(stderr, "Failed to allocate connection structures\n");
        // This is unrecoverable so bail out early.
        exit(1);
    }
}

int main (int argc, char **argv) {
    
    // 与网络无关部分
    // initialize main thread libevent instance
#if defined(LIBEVENT_VERSION_NUMBER) && LIBEVENT_VERSION_NUMBER >= 0x02000101
    /* If libevent version is larger/equal to 2.0.2-alpha, use newer version */
    struct event_config *ev_config;
    ev_config = event_config_new();
    event_config_set_flag(ev_config, EVENT_BASE_FLAG_NOLOCK);
    main_base = event_base_new_with_config(ev_config);
    event_config_free(ev_config);
#else
    /* Otherwise, use older API */
    main_base = event_init();
#endif

    /* initialize other stuff */
    conn_init();

 #ifdef EXTSTORE
    // 其他处理
    memcached_thread_init(settings.num_threads, storage);
#else
    // 其他处理
    memcached_thread_init(settings.num_threads, NULL);
#endif

    if (settings.idle_timeout && start_conn_timeout_thread() == -1) {
        exit(EXIT_FAILURE);
    }

    /* initialise clock event */
    clock_handler(0, 0, 0);

    /* create unix mode sockets after dropping privileges */
    if (settings.socketpath != NULL) {
        errno = 0;
        if (server_socket_unix(settings.socketpath, settings.access)) {
            vperror("failed to listen on UNIX socket: %s", settings.socketpath);
            exit(EX_OSERR);
        }
    }

    /* create the listening socket, bind it, end init */
    if (settings.socketpath == NULL) {
        const char *portnumber_filename = getenv("MEMCACHED_PORT_FILENAME");
        char *temp_portnumber_filename = NULL;
        size_t len;
        FILE *portnumber_file = NULL;

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

        // 绑定监听tcp端口
        errno = 0;
        if (settings.port && server_sockets(settings.port, tcp_transport,
                                            portnumber_file)) {
            vperror("failed to listen on TCP port %d", settings.port);
            exit(EX_OSERR);
        }

        // 绑定监听udp接口
        /**
         * initialization order: first create the listening sockets
         * (may need root on low ports), then drop root if needed,
         * then daemonize if needed, then init libevent (in some cases
         * descriptors created by libevent wouldn't survive forking)
         */
        
        // create the UDP listening socket and bind it
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

    /* Give the sockets a moment to open. I know this is dumb, but the error
     * is only an advisory.
     */
    usleep(1000);
    if (stats_state.curr_conns + stats_state.reserved_fds >= settings.maxconns - 1) {
        fprintf(stderr, "Maxconns setting is too low, use -c to increase.\n");
        exit(EXIT_FAILURE);
    }

    if (pid_file != NULL) {
        save_pid(pid_file);
    }

    /* Drop privileges to no longer needed */
    if (settings.drop_privileges) {
        drop_privileges();
    }

    // 其他代码
    
    // 主循环
    // enter the event loop
    if (event_base_loop(main_base, 0) != 0) {
        retval = EXIT_FAILURE;
    }

    // 其他代码
    
    // remove the PID file if we're a daemon
    if (do_daemonize)
        remove_pidfile(pid_file);
    /* Clean up strdup() call for bind() address */
    if (settings.inter)
        free(settings.inter);
    if (l_socket)
        free(l_socket);
    if (u_socket)
        free(u_socket);

    /* cleanup base */
    event_base_free(main_base);
}
