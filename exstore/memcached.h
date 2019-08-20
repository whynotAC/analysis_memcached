
#ifdef EXTSTORE
typedef struct {
    unsigned int page_version;  // from IO header
    unsigned int offset;        // from IO header
    unsigned short page_id;     // from IO header
} item_hdr;
#endif

#ifdef EXTSTORE
typedef struct _io_wrap {
    obj_io io;
    struct _io_wrap *next;
    conn *c;
    item *hdr_it;               // original header item.
    unsigned int iovec_start;   // start of the iovecs for this IO
    unsigned int iovec_count;   // total number of iovecs
    unsigned int iovec_data;    // specific index of data iovec
    bool miss;                  // signal a miss to unlink hdr_it
    bool badcrc;                // signal a crc failure
    bool active;                // tells if IO was dispatched or not
} io_wrap;
#endif

/**
 * The stucture representing a conection into memcached.
 */
struct conn {
    int sfd;
#ifdef TLS
    SSL *ssl;
    char *ssl_wbuf;
    bool ssl_enabled;
#endif
    sasl_conn_t *sasl_conn;
    bool sasl_started;
    bool authenticated;
    enum conn_states state;
    enum bin_substates substate;
    rel_time_t last_cmd_time;
    struct event event;
    short ev_flags;
    short which; /* which events were just triggered*/

    char    *rbuf;      // buffer to read commands into
    char    *rcurr;     // but if we parsed some already, this is where we stopped
    int     rsize;      // total allocated size of rbuf
    int     rbytes;     // how much data, starting from rcur, do we have unparsed

    char    *wbuf;
    char    *wcurr;
    int     wisze;
    int     wbytes;
    // which state to go into after finishing current write
    enum conn_states write_and_go;
    void    *write_and_free;    // free this memory after finishing writing

    char    *ritem;     // when we read in an item's value, it goes here
    int     rlbytes;

    // data for the nread state
    //
    
    /**
     * item is used to hold an item structure created after reading the command
     * line of set/add/replace commands, but before we finished reading the
     * actual data. The data is read into ITEM_dat(item) to avoid extra copying.
     */

    void *item;     // for commands set/add/replace

    /* data for the swallow state */
    int   sbytes;   // how many bytes to swallow

    /* data for the mwrite state*/
    struct iovec *iov;
    int     iovsize;    // number of elements allocated in iov[]
    int     iovused;    // number of elements used in iov[]

    struct msghdr *msglist;
    int     msgsize;    // number of elements allocated in msglist[]
    int     msgused;    // number of elements used in msglist[]
    int     msgcurr;    // element in msglist[] being transmitted now
    int     msgbytes;   // number of bytes in current msg

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

    /* data for UDP clients */
    int     request_id; // Incoming UDP request ID, if this is a UDP connection
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

    /* Binary protocol stuff */
    /* This is where the binary header goes */
    protocol_binary_request_header binary_header;
    uint64_t cas;   // the cas to return
    short cmd;      // current command being processed
    int opaque;
    int keylen;
    conn *next;     // Used for generating a list of conn structures
    LIBEVENT_THREAD *thread;    // Pointer to the thread object serving this connection
    int (*try_read_command)(conn *c); // pointer for top level input parser
    ssize_t (*read)(conn *c, void *buf, size_t count);
    ssize_t (*sendmsg)(conn *c, struct msghdr *msg, int flags);
    ssize_t (*write)(conn *c, void *buf, size_t count);
};

#ifdef EXTSTORE
extern void *ext_storage;
#endif
