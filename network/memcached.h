
#include "protocol_binary.h"

#include "sasl_defs.h"

/* Slab sizing definitions. */
#define POWER_LARGEST 256 // actual cap is 255

/* slab class max is a 6-bit number. -1*/
#define MAX_NUMBER_OF_SLAB_CLASSES (63 + 1)

/** Time relative to server start. Smaller than time_t on 64-bit systems. */
// TODO: Move to sub-header. needed in logger.h
// typedef unsigned int rel_time_t;

/** Use X macros to avoid iterating over the stats fields during reset and
 * aggregation. No longer have to add new stats in 3+ places.
 */

#define SLAB_STATS_FIELDS \
    X(set_cmds) \
    X(get_hits) \
    X(touch_hits) \
    X(delete_hits) \
    X(cas_hits) \
    X(cas_badval) \
    X(incr_hits) \
    X(decr_hits)

/* Stats stored per slab (and per thread.)*/
struct slab_stats {
#define X(name) uint64_t    name;
    SLAB_STATS_FIELDS
#undef X
};

#define THREAD_STATS_FIELDS \
    X(get_cmds) \
    X(get_misses) \
    X(get_expired) \
    X(get_flushed) \
    X(touch_cmds) \
    X(touch_misses) \
    X(delete_misses) \
    X(incr_misses) \
    X(decr_misses) \
    X(cas_misses) \
    X(bytes_read) \
    X(bytes_written) \
    X(flush_cmds) \
    X(conn_yields) /* # of yields for connections (-R option)*/ \
    X(auth_cmds) \
    X(auth_errors) \
    X(idle_kicks) /* idle connections killed */

#ifdef EXTSTORE
#define EXTSTORE_THREAD_STATS_FIELDS \
    X(get_extstore) \
    X(recache_from_extstore) \
    X(miss_from_extstore) \
    X(badcrc_from_extstore)
#endif

/**
 * Stats stored per-thread
 */
struct thread_stats {
    pthread_mutex_t mutex;
#define X(name) uint64_t    name;
    THREAD_STATS_FIELDS
#ifdef EXTSTORE
    EXTSTORE_THREAD_STATS_FIELDS 
#endif
#undef X
    struct slab_stats slab_stats[MAX_NUMBER_OF_SLAB_CLASSES];
    uint64_t lru_hits[POWER_LARGEST];
};

/**
 * When adding a setting, be sure to update process_stat_settings */
/**
 * Globally accessible settings as derived from the commandline.
 */
struct settings {
    size_t maxbytes;        // 最大chunk大小
    int maxconns;           // connection结构体个数
    int port;               // tcp端口
    int udpport;            // dup端口
    char *inter;
    int verbose;            // 输出信息等级 0,1,2
    rel_time_t oldest_live; // ignore existing items older than this 
    uint64_t oldest_cas;    // ignore existing items with CAS values lower than this
    int evict_to_free;
    char *socketpath;       // path to unix socket if using local socket
    int access;             // access mask (a la chmod) for unix domain socket
    double factor;          // chunk size growth factor
    int chunk_size;        
    int num_threads;        // number of worker (without dispatcher) libevent threads to run
    int num_threads_per_udp;// number of worker threads serving each udp socket
    char prefix_delimiter;  // character that marks a key prefix (for stats)
    int detail_enabled;     // nonzero if we're collecting detailed stats
    int reqs_per_event;     // Maximum number of io to process on each io-event

    bool use_cas;
    enum protocol binding_protocol;
    int backlog;
    int item_size_max;      // Maximum item size
    int slab_chunk_size_max;// Upper end for chunks within slab pages
    int slab_page_size;     // Slab's page units.
    bool sasl;              // SASL on/off
    bool maxconns_fast;     // Whether or not to early close connections
    bool lru_crawler;       // Whether or not to early enable the autocrawler thread
    bool lru_maintainer_thread; // LRU maintainer background thread
    bool lru_segmented;     // Use split or flat LRU's
    bool slab_reassign;     // Whether or not slab reassignment is allowed
    int slab_automove;      // Whether or not to automatically move slabs
    double slab_automove_ratio; // youngest must be within pct of oldest
    unsigned int slab_automove_window;  // window mover for algorithm
    int hashpower_init;     // Starting hash power level
    bool shutdown_command;  // allow shutdown command
    int tail_repair_time;   // LRU tail refcount leak repair time
    bool flush_enabled;     // flush_all enabled
    bool dump_enabled;      // whether cachedump/metadump commands work
    char *hash_algorithm;   // Hash algorithm in use
    int lru_crawler_sleep;  // Microsecond sleep between items
    uint32_t lru_crawler_tocrawl;   // Number of items to crawl per run
    int hot_lru_pct;        // percentage of slab space for HOT_LRU
    int warm_lru_pct;       // percentage of slab space for WARM_LRU
    double hot_max_factor;  // HOT tail age relative to COLD tail
    double warm_max_factor; // WARM tail age relative to COLD tail
    int crawls_persleep;    // Number of LRU crawls to run before sleeping
    bool inline_ascii_respone;  // pre-format the VALUE line for ASCII responses
    bool temp_lru;          // TTL < temporary_ttl uses TEMP_LRU
    uint32_t temporary_ttl; // temporary LRU threshold
    int idle_timeout;       // Number of seconds to let connections idle
    unsigned int logger_watcher_buf_size; // size of logger's per-watcher buffer
    unsigned int logger_buf_size;   // size of per-thread logger buffer
    bool drop_privileges;   // Whether or not to drop unnecessary process privileges
    bool relaxed_privileges;// Relax process restrictions when running testapp
#ifdef EXTSTORE
    unsigned int ext_item_size; // minimum size of items to store externally
    unsigned int ext_item_age;  // max age of tail item before storing ext.
    unsigned int ext_low_ttl;   // remainning TTL below this uses own pages
    unsigned int ext_recache_rate; // counter++ % recache_rate == 0 > recache
    unsigned int ext_wbuf_size; // read only note for the engine
    unsigned int ext_compact_under; // when fewer than this many pages, compact
    unsigned int ext_drop_under;// When fewer than this many pages, drop COLD items
    double ext_max_frag;        // ideal maximum page fragmentation
    double slab_automove_freeratio; // % of memory to hold free as buffer
    double ext_drop_unread;     // skip unread itms during compaction
    // per-slab-class free chunk limit
    unsigned int ext_free_memchunks[MAX_NUMBER_OF_SLAB_CLASSES];
#endif
};

/**
 * Structure for storing items within memcached.
 */
typedef struct _stritem {
    /* Protected by LRU locks */
    struct _stritem *next;
    struct _stritem *prev;
    /* Rest are protected by an item lock */
    struct _stritem *h_next;    // hash chain next
    rel_time_t      time;       // least recent access
    rel_time_t      exptime;    // expire time
    int             nbytes;     // size of data
    unsigned short  refcount;
    uint8_t         nsuffix;    // length of flags-and-length string
    uint8_t         it_flags;   // ITEM_* above
    uint8_t         slabs_clsid;// which slab class we're in
    uint8_t         nkey;       // key length, w/terminating null and padding
    /* this odd type prevents type-punning issues when we do
     * the little shuffle to save space when not suing CAS. */
    union {
        uint64_t cas;
        char end;
    } data[];
    /* if it_flags & ITEM_CAS we have 8 bytes CAS
     * then null-terminated key
     * then " flags length\r\n" (to terminating null)
     * then data with terminating \r\n (to terminating null; it's binary!)
     */
} item;

/**
 * NOTE: If you modify this table you _MUST_ update the function state_text
 */
/**
 * Possible states of a connection.
 */
enum conn_states {
    conn_listening,     // < the socket which listens for connections
    conn_new_cmd,       // < Prepare connection for next command
    conn_waiting,       // < waiting for a readable socket
    conn_read,          // < reading in a command line
    conn_parse_cmd,     // < try to parse a command from the input buffer
    conn_write,         // < writing out a simple response
    conn_nread,         // < reading in a fixed number of bytes
    conn_swallow,       // < swallowing unnecessary bytes w/o storing
    conn_closing,       // < closing this connection
    conn_mwirte,        // < writing out many items sequentially
    conn_closed,        // < connection is closed
    conn_watch,         // < held by the logger thread as a watcher
    conn_max_state      // < Max state value (used for assertion)
};

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

// 传输数据使用的协议
enum protocol {
    ascii_prot = 3,     // arbitrary value. ASCII接口
    binary_prot,        // 二进制接口
    negotiating_prot    // Discovering the protocol,接口根据数据来定
};

// 网络通信协议
enum network_transport {
    local_transport,    // Unix sockets 本地通信
    tcp_transport,      // tcp通信
    udp_transport       // udp通信
};

typedef struct {
    pthread_t thread_id;                // unique ID of this thread
    struct event_base *base;            // libevent handle this thread uses
    struct event notify_event;          // listen event for notify pipe
    int notify_receive_fd;              // receiving end of notify pipe
    int notify_send_fd;                 // sending end of notify pipe
    struct thread_stats stats;          // Stats generated by this thread
    struct conn_queue *new_conn_queue;  // queue of new connections to handle
    cache_t *suffix_cache;              // suffix cache
#ifdef EXTSTORE
    cache_t *io_cache;                  // IO objects
    void *storage;                      // data object for storage system
#endif
    logger *l;                          // logger buffer
    void *lru_bump_buf;                 // async LRU bump buffer
} LIBEVENT_THREAD;
typedef struct conn conn;
#ifdef EXTSTORE
typedef struct _io_wrap {
    obj_io io;
    struct _io_wrap *next;
    conn *c;
    item *hdr_it;                       // original header item
    unsigned int iovec_start;           // start of the iovecs for this IO
    unsigned int iovec_count;           // total number of iovecs
    unsigned int iovec_data;            // specific index of data iovec
    bool    miss;                       // signal a miss to unlink hdr_it
    bool    badcrc;                     // signal a crc failure
    bool    active;                     // FIXME: canary for test. remove
} io_wrap;
#endif

/**
 * The structure representing a connection into memcached.
 */
struct conn {
    int sfd;                    // 网络文件描述符
    sasl_conn_t *sasl_conn;     // sasl链接
    bool authenticated;         // 认证
    enum conn_states state;     // 网络接口状态
    enum bin_substates substate;
    rel_time_t last_cmd_time;   // 最后处理cmd的时间
    struct event event;
    short ev_flags;
    short which;    /* which events were just triggered */

    char *rbuf;     /* buffer to read commands into, 读取的buffer */
    char *rcurr;    /* but if we parsed some already, this is where we stopped，当前读取到的位置*/
    int  rsize;     /* total allocated size of rbuf, rbuf的长度 */
    int  rbytes;    /* how much data, starting from rcur, do we have unpares. 还有多少数据未读取 */

    char *wbuf;     // 写入buffer的位置
    char *wcurr;    // 当前写入位置
    int  wsize;     // buffer的总大小
    int  wbytes;    // 写入buffer字节的总大小
    /* which state to go into after finishing current write */
    enum conn_states write_and_go;
    void *write_and_free;   // free this memory after finishing writing

    char *ritem;    // when we read in an item's value, it goes here
    int  rlbytes;
    
    /* data for the nread state */

    /**
     * item is used to hold on item structure created after reading the command
     * line of set/add/replace commands, but before we finished reading the
     * actual data. The data is read into ITEM_data(item) to avoid extra
     * copying.
     */
    // 用于记录set/add/replace操作的item
    void *item;     // for commands seet/add/replace

    /* data for the swallow state */
    int sbytes;     // how many bytes to swallow

    /* data for the mwrite state */
    struct iovec *iov;
    int    iovsize;     // number of elements allocated in iov[]
    int    iovused;     // number of elements used in iiov[]

    struct msghdr *msglist;
    int     msgsize;    // number of elements allocated in msglist[]
    int     msgused;    // number of elements used in msglist[]
    int     msgcurr;    // element in msglist[] being transmitted now
    int     msgbytes;   // number of bytes in current msg

    // 用于记录往外写的item
    item    **ilist;    // list of items to write out
    int     isize;
    item    **icurr;
    int     ileft;

    char    **suffixlist;
    int     suffixsize;
    char    **suffixcurr;
    int     suffixleft;
#ifdef EXTSTORE
    int     io_wrapleft;
    unsigned int recache_counter;
    io_wrap *io_wraplist;   // linked list of io_wraps
    bool    io_queued;      // FIXME: debugging flag
#endif
    enum protocol protocol; // which protocol this connection speaks
    enum network_transport transport;   // what transport is used by this connection

    // data for UDP clients
    int     request_id; // Incoming UDP request ID, if this is a UDP "connection"
    struct sockaddr_in6 request_addr;   // udp: Who sent the most recent request
    socklen_t request_addr_size;
    unsigned char *hdrbuf;  // udp packet headers
    int     hdrsize;    // number of headers' worth of space is allocated

    bool    noreply;    // True if the reply should not be sent.
    // current stats command
    struct {
        char *buffer;
        size_t size;
        size_t offset;
    } stats;

    // Binary protocol stuff
    // This is where the binary header goes
    protocol_binary_request_header binary_header;
    uint64_t cas;   // the cas to return
    short   cmd;      // current command being processed
    int     opaque;
    int     keylen;
    conn    *next;  // Used for generating a list of conn structures
    LIBEVENT_THREAD *thread; // Pointer to the thread object serving this connection
};

// array of conn structures, indexed by file descriptor
extern conn **conns;
