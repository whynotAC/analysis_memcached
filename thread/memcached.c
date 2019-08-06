#include "memcached.h"

enum try_read_result {
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,         /* an error occurred (on the socket) (or client closed connection) */
    READ_MEMORY_ERROR   /* failed to allocate more memory */
};

/** file scope variables **/
static conn *listen_conn = NULL;
static int max_fds;
static struct event_base *main_base;

enum transmit_result {
    TRANSMIT_COMPLETE,      /** All done writing */
    TRANSMIT_INCOMPLETE,    /** More data remaining to write */
    TRANSMIT_SOFT_ERROR,    /** Can't write any more right now. */
    TRANSMIT_HEAD_ERROR     /** Can't write (c->state is set to conn_closing)*/
};

/* This reduces the latency without adding lots of extra wiring to be able to
 * notify the listener thread of when to listen again.
 * Also, the clock timer could be broken out into its own thread and we
 * can block the listener via a condition.
 */
static volatile bool allow_new_conns = true;
static struct event maxconnsevent;
static void maxconns_handler(const int fd, const short which, void *arg) {
    struct timeval t = {.tv_sec = 0, .tv_usec = 10000};

    if (fd == -42 || allow_new_conns == false) {
        /* reschedule in 10ms if we need to keep polling */
        evtimer_set(&maxconnsevent, maxconns_handler, 0);
        event_base_set(main_base, &maxconnsevent);
        evtimer_add(&maxconnsevent, &t);
    } else {
        evtimer_del(&maxconnsevent);
        accept_new_conns(true);
    }
}

/*
 * Adds a message header to a connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int add_msghdr(conn *c)
{
    struct msghdr *msg;

    assert(c != NULL);

    if (c->msgsize == c->msgused) {
        msg = realloc(c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
        if (! msg) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->msglist = msg;
        c->msgsize *= 2;
    }

    msg = c->msglist + c->msgused;

    /* this wipes msg_iovlen, msg_control, msg_controllen, and
     * msg_flags, the last 3 of which aren't defined on solaris: */
    memset(msg, 0, sizeof(struct msghdr));

    msg->msg_iov = &c->iov[c->iovused];

    if (IS_UDP(c->transport) && c->request_addr_size > 0) {
        msg->msg_name = &c->request_addr;
        msg->msg_namelen = c->request_addr_size;
    }

    c->msgbytes = 0;
    c->msgused++;

    if (IS_UDP(c->transport)) {
        /* Leave room for the UDP header, which we'll fill in later. */
        return add_iov(c, NULL, UDP_HEADER_SIZE);
    }

    return 0;
}

/*
 * Adds data to the list of pending data that will be written out to a
 * connection.
 *
 * Return 0 on success, -1 on out-of-memory.
 * Note: This is a hot path for at least ASCII protocol. While there is
 * redundant code in spliting TCP/UDP handing, any reduction in steps has a
 * large impact for TCP connections.
 */
static int add_iov(conn *c, const void *buf, int len) {
    struct msghdr *m;
    int leftover;

    assert(c != NULL);

    if (IS_UDP(c->transport)) {
        do {
            m = &c->msglist[c->msgused - 1];

            /*
             * limit UDP packets to UDP_MAX_PAYLOAD_SIZE bytes.
             */

            /* We may need to start a new msghdr if this one is full. */
            if (m->msg_iovlen == IOV_MAX ||
                    (c->msgbytes >= UDP_MAX_PAYLOAD_SIZE)) {
                add_msghdr(c);
                m = &c->msglist[c->msgused - 1];
            }

            if (ensure_iov_space(c) != 0)
                return -1;

            /* If the fragment is too big to fit in the datagram, split it up */
            if (len + c->msgbytes > UDP_MAX_PAYLOAD_SIZE) {
                leftover = len + c->msgbytes - UDP_MAX_PAYLOAD_SIZE;
                len -= leftover;
            } else {
                leftover = 0;
            }

            m = &c->msglist[c->msgused - 1];
            m->msg_iov[m->msg_iolen].iov_base = (void *)buf;
            m->msg_iov[m->msg_iovlen].iov_len = len;

            c->msgbytes += len;
            c->iovused++;
            m->msg_iovlen++;

            buf = ((char *)buf) + len;
            len = leftover;
        } while (leftover > 0);
    } else {
        /* Optimized path for TCP connections */
        m = &c->msglist[c->msgused - 1];
        if (m->msg_iovlen == IOV_MAX) {
            add_msghdr(c);
            m = &c->msglist[c->msgused - 1];
        }

        if (ensure_iov_space(c) != 0)
            return -1;

        m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
        m->msg_iov[m->msg_iovlen].iov_len = len;
        c->msgbytes += len;
        c->iovused++;
        m->msg_iovlen++;
    }

    return 0;
}

static void out_string(conn *c, const char *str) {
    size_t len;

    assert(c != NULL);

    if (c->noreply) {
        if (settings.verbose > 1)
            fprintf(stderr, ">%d NOREPLY %s\n", c->sfd, str);
        c->noreply = false;
        conn_set_state(c, conn_new_cmd);
        return;
    }

    if (settings.verbose > 1)
        fprintf(stderr, ">%d %s\n", c->sfd, str);

    /* Nuke a partial output... */
    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    add_msghdr(c);

    len = strlen(str);
    if ((len + 2) > c->wsize) {
        /* ought to be always enough, just fail for simplicity */
        str = "SERVER_ERROR output line too long";
        len = strlen(str);
    }

    memcpy(c->wbuf, str, len);
    memcoy(c->wbuf + len, "\r\n", 2);
    c->wbytes = len + 2;
    c->wcurr = c->wbuf;

    conn_set_state(c, conn_write);
    c->write_and_go = conn_new_cmd;
    return;
}

/*
 * Outputs a protocol-specific "out of memory" error. For ASCII clients,
 * this is equivalent to out_string().
 */
static void out_of_memory(conn *c, char *ascii_error) {
    const static char error_prefix[] = "SERVER_ERROR ";
    const static int error_prefix_len = sizeof(error_prefix) - 1;

    if (c->protocol == binary_prot) {
        /* Strip off the generic error prefix; it's irrelevant in binary */
        if (!strncmp(ascii_error, error_prefix, error_prefix_len)) {
            ascii_error += error_prefix_len;
        }
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_ENOMEM, ascii_error, 0);
    } else {
        out_string(c, ascii_error);
    }
}

/*
 * we get here after reading the value in set/add.replace commands.The command
 * has been stored in c->cmd, and the item is ready in c->item.
 */
static void complete_nread_ascii(conn *c) {
    assert(c != NULL);

    item *it = c->item;
    int comm = c->cmd;
    enum store_item_type ret;
    bool is_valid = false;

    pthread_mutex_lock(&c->thread->stats.mutex);
    c->thread->stats.slab_stats[ITEM_clsid(it)].set_cmds++;
    pthread_mutex_unlock(&c->thread->stats.mutex);

    if ((it->it_flags & ITEM_CHUNKED) == 0) {
        if (strncmp(ITEM_data(it) + it->nbytes - 2, "\r\n", 2) == 0) {
            is_valid = true;
        }
    } else {
        char buf[2];
        /* should point to the final item chunk */
        item_chunk *ch = (item_chunk *)c->ritem;
        assert(ch->used != 0);
        /* :( We need to look at the last two bytes. This could span two
         * chunks.
         */
        if (ch->used > 1) {
            buf[0] = ch->data[ch->used - 2];
            buf[1] = ch->data[ch->used - 1];
        } else {
            assert(ch->prev);
            assert(ch->used == 1);
            buf[0] = ch->prev->data[ch->prev->used - 1];
            buf[1] = ch->data[ch->used - 1];
        }
        if (strncmp(buf, "\r\n", 2) == 0) {
            is_valid = true;
        } else {
            assert(1 == 0);
        }
    }

    if (!is_valid) {
        out_string(c, "CLIENT_ERROR bad data chunk");
    } else {
        ret = store_item(it, comm, c);

#ifdef ENABLE_DTRACE
        uint64_t cas = ITEM_get_cas(it);
        switch (c->cmd) {
        case NREAD_ADD:
            MEMCACHED_COMMAND_ADD(c->sfd, ITEM_key(it), it->nkey, 
                    (ret == 1) ? it->nbytes : -1, cas);
            break;
        case NREAD_REPLACE:
            MEMCACHED_COMMAND_REPLACE(c->sfd, ITEM_key(it), it->nkey,
                    (ret == 1) ? it->nbytes : -1, cas);
            break;
        case NREAD_APPEND:
            MEMCACHED_COMMAND_APPEND(c->sfd, ITEM_key(it), it->nkey,
                    (ret == 1) ? it->nbytes : -1, cas);
            break;
        case NREAD_PREPEND:
            MEMCACHED_COMMAND_PREPEND(c->sfd, ITEM_key(it), it->nkey,
                    (ret == 1) ? it->nbytes : -1, cas);
            break;
        case NREAD_SET:
            MEMCACHED_COMMAND_SET(c->sfd, ITEM_key(it), it->nkey,
                    (ret == 1) ? it->nbytes : -1, cas);
            break;
        case NREAD_CAS:
            MEMCACHED_COMMAND_CAS(c->sfd, ITEM_key(it), it->nkey, it->nbytes,
                    cas);
            break;
        }
#endif

        switch (ret) {
        case STORED:
            out_string(c, "STORED");
            break;
        case EXISTS:
            out_string(c, "EXISTS");
            break;
        case NOT_FOUND:
            out_string(c, "NOT_FOUND");
            break;
        case NOT_STORED:
            out_string(c, "NOT_STORED");
            break;
        defalut:
            out_string(c, "SERVER_ERROR Unhandled storage type");
        }
    }

    item_remove(c->item);   // release the c->item reference
    c->item = 0;
}

static const char *prot_text(enum protocol prot) {
    char *rv = "unknown";
    switch (prot) {
        case ascii_prot:
            rv = "ascii";
            break;
        case binary_prot:
            rv = "binary";
            break;
        case negotiating_prot:
            rv = "auto-negotiate";
            break;
    }
    return rv;
}

static void add_bin_header(conn *c, uint16_t err, uint8_t hdr_len, uint16_t key_len, uint32_t body_len) {
    protocol_binary_response_header* header;

    assert(c);

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0) {
        /* This should never run out of memory because iov and msg lists
         * have minimum sizes big enough to hold an error response.
         */
        out_of_memory(c, "SERVER_ERROR out of memory adding binary header");
        return;
    }

    header = (protocol_binary_response_header *)c->wbuf;

    header->response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    header->response.opcode = c->binary_header.request.opcode;
    header->response.keylen = (uint16_t)htons(key_len);

    header->response.extlen = (uint8_t)hdr_len;
    header->response.datatype = (uint8_t)PROTOCOL_BINARY_RAW_BYTES;
    header->response.status = (uint16_t)htons(err);

    header->response.bodylen = htonl(body_len);
    header->response.opaque = c->opaque;
    header->response.cas = htonll(c->cas);

    if (settings.verbose > 1) {
        int ii;
        fprintf(stderr, ">%d Writing bin response:", c->sfd);
        for (ii = 0; ii < sizeof(header->bytes); ++ii) {
            if (ii % 4 == 0) {
                fprintf(stderr, "\n>%d  ", c->sfd);
            }
            fprintf(stderr, " 0x%02x", header->bytes[ii]);
        }
        fprintf(stderr, "\n");
    }

    add_iov(c, c->wbuf, sizeof(header->response));
}

/**
 * Writes a binary error response. If errstr is supplied, it is used as the 
 * error text; otherwise a generic description of the error status code is
 * included.
 */
static void write_bin_error(conn *c, protocol_binary_response_status err,
                                const char *errstr, int swallow) {
    size_t len;

    if (!errstr) {
        switch (err) {
        case PROTOCOL_BINARY_RESPONSE_ENOMEM:
            errstr = "Out of memory";
            break;
        case PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND:
            errstr = "Unknown command";
            break;
        case PROTOCOL_BINARY_RESPONSE_KEY_ENOENT:
            errstr = "Not found";
            break;
        case PROTOCOL_BINARY_RESPONSE_EINVAL:
            errstr = "Invalid arguments";
            break;
        case PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS:
            errstr = "Data exists for key. ";
            break;
        case PROTOCOL_BINARY_RESPONSE_E2BIG:
            errstr = "Too large.";
            break;
        case PROTOCOL_BINARY_RESPONSE_DELTA_BADVAL:
            errstr = "Non-numeric server-side value for incr or decr";
            break;
        case PROTOCOL_BINARY_RESPONSE_NOT_STORED:
            errstr = "Not stored.";
            break;
        case PROTOCOL_BINARY_RESPONSE_AUTH_ERROR:
            errstr = "Auth failure.";
            break;
        default:
            assert(false);
            errstr = "UNHANDLED ERROR";
            fprintf(stderr, ">%d UNHANDLED ERROR: %d\n", c->sfd, err);
        }
    }

    if (settings.verbose > 1) {
        fprintf(stderr, ">%d Writing an error: %s\n", c->sfd, errstr);
    }

    len = strlen(errstr);
    add_bin_header(c, err, 0, 0, len);
    if (len > 0) {
        add_iov(c, errstr, len);
    }
    conn_set_state(c, conn_mwrite);
    if (swalow > 0) {
        c->sbytes = swallow;
        c->write_and_go = conn_swallow;
    } else {
        c->write_and_go = conn_new_cmd;
    }
}

/* From and send a response to a command over the binary protocol */
static void write_bin_response(conn *c, void *d, int hlen, int keylen, int dlen) {
    if (!c->noreply || c->cmd == PROTOCOL_BINARY_CMD_GET ||
            c->cmd == PROTOCOL_BINARY_CMD_GETK) {
        add_bin_header(c, 0, hlen, keylen, dlen);
        if (dlen > 0) {
            add_iov(c, d, dlen);
        }
        conn_set_state(c, conn_mwrite);
        c->write_and_go = conn_new_cmd;
    } else {
        conn_set_state(c, conn_new_cmd);
    }
}

/* Just write an error message and disconnect the client */
static void handle_binary_protocol_error(conn *c) {
    write_bin_error(c, PROTOCOL_BINARY_RESPONSE_EINVAL, NULL, 0);
    if (settings.verbose) {
        fprintf(stderr, "Protocol error (opcode %02x), close connection %d\n",
                    c->binary_header.request.opcode, c->sfd);
    }
    c->write_and_go = conn_closing;
}

static void bin_read_key(conn *c, enum bin_substates next_substate, int extra) {
    assert(c);
    c->substate = next_substate;
    c->rlbytes = c->keylen + extra;

    /* OK... do we have room for the extras and the key in the input buffer?*/
    ptrdiff_t offset = c->rcurr + sizeof(protocol_binary_request_header) - c->rbuf;
    if (c->rlbytes > c->rsize - offset) {
        size_t nsize = c->rsize;
        size_t size = c->rlbytes + sizeof(protocol_binary_request_header);

        while (size > nsize) {
            nsize *= 2;
        }

        if (nsize != c->rsize) {
            if (settings.verbose > 1) {
                fprintf(stderr, "%d: Need to grow buffer from %lu to %lu\n",
                    c->sfd, (unsigned long)c->rsize, (unsigned long)nsize);
            }
            char *newm = realloc(c->rbuf, nsize);
            if (newm == NULL) {
                STATS_LOCK();
                stats.malloc_fails++;
                STATS_UNLOCK();
                if (settings.verbose) {
                    fprintf(stderr, "%d: Failed to grow buffer.. closing connection\n",
                                c->sfd);
                }
                conn_set_state(c, conn_closing);
                return;
            }

            c->rbuf = newm;
            /* rcurr should point to the same offset in the packet */
            c->rcurr = c->rbuf + offset - sizeof(protocol_binary_request_header);
            c->rsize = nsize;
        }
        if (c->rbuf != c->rcurr) {
            memmove(c->rbuf, c->rcurr, c->rbytes);
            c->rcurr = c->rbuf;
            if (settings.verbose > 1) {
                fprintf(stderr, "%d: Repack input buffer\n", c->sfd);
            }
        }
    }

    /* preserve the header in the buffer */
    c->ritem = c->rcurr + sizeof(protocol_binary_request_header);
    conn_set_state(c, conn_nread);
}

static void init_sasl_conn(conn *c) {
    assert(c);
    /* should something else be returned? */
    if (!settings.sasl)
        return;

    c->authenticated = false;

    if (!c->sasl_conn) {
        int result = sasl_server_new("memcached",
                            NULL,
                            my_sasl_hostname[0] ? my_sasl_hostname : NULL,
                            NULL, NULL,
                            NULL, 0, &c->sasl_conn);
        if (result != SASL_OK) {
            if (settings.verbose) {
                fprintf(stderr, "Failed to initialize SASL conn.\n");
            }
            c->sasl_conn = NULL;
        }
    }
}

static void bin_list_sasl_mechs(conn *c) {
    // Guard against a disabled SASL.
    if (!settings.sasl) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL,
                            c->binary_header.request.bodylen - 
                            c->binary_header.request.keylen);
        return;
    }

    init_sasl_conn(c);
    const char *result_string = NULL;
    unsigned int string_length = 0;
    int result = sasl_listmech(c->sasl_conn, NULL,
                                "",
                                " ",
                                "",
                                &result_string, &string_length,
                                NULL);
    if (result != SASL_OK) {
        /* Perhaps there's a better error for this... */
        if (settings.verbose) {
            fprintf(stderr, "Failed to list SASL mechanisms.\n");
        }
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
        return;
    }
    write_bin_response(c, (char *)result_string, 0, 0, string_length);
}

static void dispatch_bin_command(conn *c) {
    int protocol_error = 0;

    uint8_t extlen = c->binary_header.request.extlen;
    uint16_t keylen = c->binary_header.request.keylen;
    uint32_t bodylen = c->binary_header.request.bodylen;

    if (keylen > bodylen || keylen + extlen > bodylen) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL, 0);
        c->write_and_go = conn_closing;
        return;
    }

    if (settings.sasl && !authenticated(c)) {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
        c->wirte_and_go = conn_closing;
        return;
    }

    MEMCACHED_PROCESS_COMMAND_START(c->sfd, c->rcurr, c->rbytes);
    c->noreply = true;

    /* binport supports 16bit keys, but internals are still 8bit */
    if (keylen > KEY_MAX_LENGTH) {
        handle_binary_protocol_error(c);
        return;
    }

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_SETQ:
        c->cmd = PROTOCOL_BINARY_CMD_SET;
        break;
    case PROTOCOL_BINARY_CMD_ADDQ:
        c->cmd = PROTOCOL_BINARY_CMD_ADD;
        break;
    case PROTOCOL_BINARY_CMD_REPLACEQ:
        c->cmd = PROTOCOL_BINARY_CMD_REPLACE;
        break;
    case PROTOCOL_BINARY_CMD_DELETEQ:
        c->cmd = PROTOCOL_BINARY_CMD_DELETE;
        break;
    case PROTOCOL_BINARY_CMD_INCREMENTQ:
        c->cmd = PROTOCOL_BINARY_CMD_INCREMENT;
        break;
    case PROTOCOL_BINARY_CMD_DECREMENTQ:
        c->cmd = PROTOCOL_BINARY_CMD_DECREMENT;
        break;
    case PROTOCOL_BINARY_CMD_QUITQ:
        c->cmd = PROTOCOL_BINARY_CMD_QUIT;
        break;;
    case PROTOCOL_BINARY_CMD_FLUSHQ:
        c->cmd = PROTOCOL_BINARY_CMD_FLUSH;
        break;
    case PROTOCOL_BINARY_CMD_APPENDQ:
        c->cmd = PROTOCOL_BINARY_CMD_APPEND;
        break;
    case PROTOCOL_BINARY_CMD_PREPENDQ:
        c->cmd = PROTOCOL_BINARY_CMD_PREPEND;
        break;
    case PROTOCOL_BINARY_CMD_GETQ:
        c->cmd = PROTOCOL_BINARY_CMD_GET;
        break;
    case PROTOCOL_BINARY_CMD_GETKQ:
        c->cmd = PROTOCOL_BINARY_CMD_GETK;
        break;
    case PROTOCOL_BINARY_CMD_GATQ:
        c->cmd = PROTOCOL_BINARY_CMD_GAT;
        break;
    case PROTOCOL_BINARY_CMD_GATKQ:
        c->cmd = PROTOCOL_BINARY_CMD_GATK;
        break;
    default:
        c->noreply = false;
    }
    
    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_VERSION:
        if (extlen == 0 && keylen == 0 && bodylen == 0) {
            write_bin_response(c, VERSION, 0, 0, strlen(VERSION));
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_FLUSH:
        if (keylen == 0 && bodylen == extlen && (extlen == 0 || extlen == 4)) {
            bin_read_key(c, bin_read_flush_exptime, extlen);
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_NOOP:
        if (extlen == 0 && keylen == 0 && bodylen == 0) {
            write_bin_response(c, NULL, 0, 0, 0);
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_SET:       // FALLTHROUGH
    case PROTOCOL_BINARY_CMD_ADD:       // FALLTHROUGH
    case PROTOCOL_BINARY_CMD_REPLACE:
        if (extlen == 0 && keylen != 0 && bodylen >= (keylen + 8)) {
            bin_read_key(c, bin_reading_set_header, 8);
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_GETQ:
    case PROTOCOL_BINARY_CMD_GET:
    case PROTOCOL_BINARY_CMD_GETKQ:
    case PROTOCOL_BINARY_CMD_GETK:
        if (extlen == 0 && bodylen == keylen && keylen > 0) {
            bin_read_key(c, bin_reading_get_key, 0);
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_DELETE:
        if (keylen > 0 && extlen == 0 && bodylen == keylen) {
            bin_read_key(c, bin_reading_del_header, extlen);
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_INCREMENT:
    case PROTOCOL_BINARY_CMD_DECREMENT:
        if (keylen > 0 && extlen == 20 && bodylen == (keylen + extlen)) {
            bin_read_key(c, bin_reading_incr_header, 20);
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_APPEND:
    case PROTOCOL_BINARY_CMD_PREPEND:
        if (keylen > 0 && extlen == 0) {
            bin_read_key(c, bin_reading_set_header, 0);
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_STAT:
        if (extlen == 0) {
            bin_read_key(c, bin_reading_stat, 0);
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_QUIT:
        if (keylen == 0 && extlen == 0 && bodylen == 0) {
            write_bin_response(c, NULL, 0, 0, 0);
            c->write_and_go = conn_closing;
            if (c->noreply) {
                conn_set_state(c, conn_closing);
            }
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_SASL_LIST_MECHS:
        if (extlen == 0 && keylen == 0 && bodylen == 0) {
            bin_list_sasl_mechs(c);
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_SASL_AUTH:
    case PROTOCOL_BINARY_CMD_SASL_STEP:
        if (extlen == 0 && keylen != 0) {
            bin_read_key(c, bin_reading_sasl_auth, 0);
        } else {
            protocol_error = 1;
        }
        break;
    case PROTOCOL_BINARY_CMD_TOUCH:
    case PROTOCOL_BINARY_CMD_GAT:
    case PROTOCOL_BINARY_CMD_GATQ:
    case PROTOCOL_BINARY_CMD_GATK:
    case PROTOCOL_BINARY_CMD_GATKQ:
        if (extlen == 4 && keylen != 0) {
            bin_read_key(c, bin_reading_touch_key, 4);
        } else {
            protocol_error = 1;
        }
        break;
    default:
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND, NULL,
                            bodylen);
    }

    if (protocol_error)
        handle_binary_protocol_error(c);
}

/*
 * if we have a complete line in the buffer, process it.
 */
static int try_read_command(conn *c) {
    assert(c != NULL);
    assert(c->rcurr <= (c->rbuf + c->rsize));
    assert(c->rbytes > 0);

    if (c->protocol == negotiating_prot || c->transport == udp_transport) {
        if ((unsigned char)c->rbuf[0] == (unsigned char)PORTOCOL_BINARY_REQ) {
            c->protocol = binary_prot;
        } else {
            c->protocol = ascii_prot;
        }

        if (settings.verbose > 1) {
            fprintf(stderr, "%d: Client using the %s protocol\n", c->sfd,
                    prot_text(c->protocol));
        }
    }

    if (c->protocol == binary_prot) {
        /* Do we have the complete packet header? */
        if (c->rbytes < sizeof(c->binary_header)) {
            /* need more data! */
            return 0;
        } else {
#ifdef NEED_ALIGN
            if (((long)(c->rcurr)) % 8 != 0) {
                /* must realign input buffer */
                memmove(c->rbuf, c->rcurr, c->rbytes);
                c->rcurr = c->rbuf;
                if (settings.verbose > 1) {
                    fprintf(stderr, "%d: Realign input buffer\n", c->sfd);
                }
            }
#endif
            protocol_binary_request_header* req;
            req = (protocol_binary_request_header*)c->rcurr;

            if (settings.verbose > 1) {
                /* Dump the packet before we convert it to host order */
                int ii;
                fprintf(stderr, "<%d Read binary protocol data:", c->sfd);
                for (ii = 0; ii < sizeof(req->bytes); ++ii) {
                    if (ii % 4 == 0) {
                        fprintf(stderr, "\n<%d  ", c->sfd);
                    }
                    fprintf(stderr, " 0x%02x", req->bytes[ii]);
                }
                fprintf(stderr, "\n");
            }

            c->binary_header = *req;
            c->binary_header.request.keylen = ntohs(req->request.keylen);
            c->binary_header.request.bodylen = ntohl(req->request.bodylen);
            c->binary_header.request.cas = ntohll(req->request.cas);

            if (c->binary_header.request.magic != PROTOCOL_BINARY_REQ) {
                if (settings.verbose) {
                    fprintf(stderr, "Invalid magic: %x\n",
                            c->binary_header.request.magic);
                }
                conn_set_state(c, conn_closing);
                return -1;
            }

            c->msgcurr = 0;
            c->msgused = 0;
            c->iovused = 0;
            if (add_msghdr(c) != 0) {
                out_of_memory(c, 
                    "SERVER_ERROR Out of memory allocating headers");
                return 0;
            }

            c->cmd = c->binary_header.request.opcode;
            c->keylen = c->binary_header.request.keylen;
            c->opaque = c->binary_header.request.opaque;
            /* clear the returned cas value */
            c->cas = 0;

            dispatch_bin_command(c);

            c->rbytes -= sizeof(c->binary_header);
            c->rcurr += sizeof(c->binary_header);
        }
    } else {
        char *el, *cont;

        if (c->rbytes == 0)
            return 0;

        el = memchr(c->rcurr, '\n', c->rbytes);
        if (!el) {
            if (c->rbytes > 1024) {
                /*
                 * We didn't have a '\n' in the first k. This _has_ to be a
                 * large multiget, if not we should just nuke the connection.
                 */
                char *ptr = c->rcurr;
                while (*ptr == ' ') { /* ignore leading whitespaces */
                    ++ptr;
                }

                if (ptr - c->rcurr > 100 ||
                        (strncmp(ptr, "get ", 4) && strncmp(str, "gets ", 5))) {
                    
                    conn_set_state(c, conn_closing);
                    return 1;
                }
            }

            return 0;
        }
        cont = el + 1;
        if ((el - c->rcurr) > 1 && *(el - 1) == '\r') {
            el--;
        }
        *el = '\0';

        assert(cont <= (c->rcurr + c->rbytes));

        c->last_cmd_time = current_time;
        process_command(c, c->rcurr);

        c->rbytes -= (cont - c->rcurr);
        c->rcurr = cont;

        assert(c->rcurr <= (c->rbuf + c->rsize));
    }

    return 1;
}

static void process_bin_update(conn *c) {
    char *key;
    int nkey;
    int vlen;
    item *it;
    protocol_binary_request_set *req = binary_get_request(c);

    assert(c != NULL);

    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;

    /* fix byteorder in the request */
    req->message.body.flags = ntohl(req->message.body.flags);
    req->message.body.expiration = ntohl(req->message.body.expiration);

    vlen = c->binary_header.request.bodylen - (nkey + c->binary_header.request.extlen);

    if (settings.verbose > 1) {
        int ii;
        if (c->cmd == PROTOCOL_BINARY_CMD_ADD) {
            fprintf(stderr, "<%d ADD ", c->sfd);
        } else if (c->cmd == PROTOCOL_BINARY_CMD_SET) {
            fprintf(stderr, "<%d SET ", c->sfd);
        } else {
            fprintf(stderr, "<%d REPLACE ", c->sfd);
        }
        for (ii = 0; ii < nkey; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }

        fprintf(stderr, " value len is %d", vlen);
        fprintf(stderr, "\n");
    }

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    it = item_alloc(key, nkey, req->message.body.flags,
            realtime(req->message.body.expiration), vlen+2);

    if (it == 0) {
        enum store_item_type status;
        if (! item_size_ok(nkey, req->message.body.flags, vlen + 2)) {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_E2BIG, NULL, vlen);
            status = TOO_LARGE;
        } else {
            out_of_memory(c, "SERVER_ERROR Out of mmeory allocating item");
            /* This error generating method eats the swallow value. Add here.*/
            c->sbytes = vlen;
            status = NO_MEMORY;
        }
        /* FIXME: losing c->cmd since it's translated below. refactor?*/
        LOGGER_LOG(c->thread->l, LOG_MUTATIONS, LOGGER_ITEM_STORE,
                NULL, status, 0, key, neky, req->message.body.expiration,
                ITEM_clsid(it));

        /* Avoid stale data persisting in cache because we failed alloc.
         * Unacceptable for SET. Anywhere else too? */
        if (c->cmd == PROTOCOL_BINARY_CMD_SET) {
            it = item_get(key, nkey, c, DONT_UPDATE);
            if (it) {
                item_unlink(it);
                STORAGE_delete(c->thread->storage, it);
                item_remove(it);
            }
        }

        /* swallow the data line */
        c->write_and_go = conn_swallow;
        return;
    }

    ITEM_set_cas(it, c->binary_header.request.cas);

    switch (c->cmd) {
        case PROTOCOL_BINARY_CMD_ADD:
            c->cmd = NREAD_ADD;
            break;
        case PROTOCOL_BINARY_CMD_SET:
            c->cmd = NREAD_SET;
            break;
        case PROTOCOL_BINARY_CMD_REPLACE:
            c->cmd = NREAD_REPLACE;
            break;
        default:
            assert(0);
    }

    if (ITEM_get_cas(it) != 0) {
        c->cmd = NREAD_CAS;
    }

    c->item = it;
    c->ritem = ITEM_data(it);
    c->rlbytes = vlen;
    conn_set_state(c, conn_nread);
    c->substate = bin_read_set_value;
}

static void process_bin_append_prepend(conn *c) {
    char *key;
    int neky;
    int vlen;
    item *it;

    assert(c != NULL);

    key = binary_get_key(c);
    nkey = c->binary_header.request.keylen;
    vlen = c->binary_header.request.bodylen - nkey;

    if (settings.verbose > 1) {
        fprintf(stderr, "Value len is %d\n", vlen);
    }

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    it = item_alloc(key, nkey, 0, 0, vlen+2);

    if (it == 0) {
        if (! item_size_ok(nkey, 0, vlen+2)) {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_E2BIG, NULL, vlen);
        } else {
            out_of_memory(c, "SERVER_ERROR Out of memory allocating item");
            /* OOM calls eat the swallow value. Add here. */
            c->sbytes = vlen;
        }
        /* swallow the data line*/
        c->write_and_go = conn_swallow;
        return;
    }

    ITEM_set_cas(it, c->binary_header.request.cas);

    switch (c->cmd) {
    case PROTOCOL_BINARY_CMD_APPEND:
        c->cmd = NREAD_APPEND;
        break;
    case PROTOCOL_BINARY_CMD_PREPEND:
        c->cmd = NREAD_PREPEND;
        break;
    default:
        assert(0);
    }

    c->item = it;
    c->ritem = ITEM_data(it);
    c->rlbytes = vlen;
    conn_set_state(c, conn_nread);
    c->substate = bin_read_set_value;
}

static void process_bin_flush(conn *c) {
    time_t exptime = 0;
    protocol_binary_request_flush* req = binary_get_request(c);
    rel_time_t new_oldest = 0;

    if (!settings.flush_enabled) {
        /* flush_all is not allowed but we log it on stats*/
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR, NULL, 0);
        return;
    }

    if (c->binary_header.request.extlen == sizeof(req->message.body)) {
        exptime = ntohl(req->message.body.expiration);
    }

    if (exptime > 0) {
        new_oldest = realtime(exptime);
    } else {
        new_oldest = current_time;
    }
    if (settings.use_cas) {
        settings.oldest_live = new_oldest - 1;
        if (settings.oldest_live <= current_time) 
            settings.oldest_cas = get_cas_id();
    } else {
        settings.oldest_live = new_oldest;
    }

    pthread_mutex_lock(&c->thread->stats.mutex);
    c->thread->stats.flush_cmds++;
    pthread_mutex_unlock(&c->thread->stats.mutex);

    write_bin_response(c, NULL, 0, 0, 0);
}

static void process_bin_delete(conn *c) {
    item *it;

    protocol_binary_request_delete *req = binary_get_request(c);

    char *key = binary_get_key(c);
    size_t nkey = c->binary_header.request.keylen;

    assert(c != NULL);

    if (settings.verbose > 1) {
        int ii;
        fprintf(stderr, "Deleting ");
        for (ii = 0; ii < neky; ++ii) {
            fprintf(stderr, "%c", key[ii]);
        }
        fprintf(stderr, "\n");
    }

    if (settings.detail_enabled) {
        stats_prefix_record_delte(key, nkey);
    }

    it = item_get(key, nkey, c, DONT_UPDATE);
    if (it) {
        uint64_t cas = ntohll(req->message.header.request.cas);
        if (cas == 0 || cas == ITEM_get_cas(it)) {
            MEMCACHED_COMMAND_DELETE(c->sfd, ITEM_key(it), it->nkey);
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.slab_stats[ITEM_clsid(it)].delete_hits++;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            item_unlink(it);
            STORAGE_delete(c->thread->storage, it);
            write_bin_response(c, NULL, 0, 0, 0);
        } else {
            write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS, NULL, 0);
        }
        item_remove(it);    // release out reference
    } else {
        write_bin_error(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT, NULL, 0);
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.delete_misses++;
        pthread_mutex_unlock(&c->thread->stats.mutex);
    }
}

static void complete_nread_binary(conn *c) {
    assert(c != NULL);
    assert(c->cmd >= 0);

    switch(c->substate) {
    case bin_reading_set_header:
        if (c->cmd == PROTOCOL_BINARY_CMD_APPEND || 
                c->cmd == PROTOCOL_BINARY_CMD_PREPEND) {
            process_bin_append_prepend(c);
        } else {
            process_bin_update(c);
        }
        break;
    case bin_read_set_value:
        complete_update_bin(c);
        break;
    case bin_reading_get_key:
    case bin_reading_touch_key:
        process_bin_get_or_touch(c);
        break;
    case bin_reading_stat:
        process_bin_stat(c);
        break;
    case bin_reading_del_header:
        process_bin_delete(c);
        break;
    case bin_reading_incr_header:
        process_incr_bin(c);
        break;
    case bin_read_flush_exptime:
        process_bin_flush(c);
        break;
    case bin_reading_sasl_auth:
        process_bin_sasl_auth(c);
        break;
    case bin_reading_sasl_auth_data:
        process_bin_complete_sasl_auth(c);
        break;
    default:
        printf(stderr, "Not handling substate %d\n", c->substate);
        assert(0);
    }
}

static void reset_cmd_handler(conn *c) {
    c->cmd = -1;
    c->substate = bin_no_state;
    if (c->item != NULL) {
        item_remove(c->item);
        c->item = NULL;
    }
    conn_shrink(c);
    if (c->rbytes > 0) {
        conn_set_state(c, conn_parse_cmd);
    } else {
        conn_set_state(c, conn_waiting);
    }
}

static void complete_nread(conn *c) {
    assert(c != NULL);
    assert(c->protocol == ascii_prot ||
            c->protocol == binary_prot);

    if (c->protocol == ascii_prot) {
        complete_nread_ascii(c);
    } else if (c->protocol == binary_prot) {
        complete_nread_binary(c);
    }
}

typedef struct token_s {
    char *value;
    size_t length;
} token_t;

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1
#define KEY_TOKEN 1

#define MAX_TOKENS 8

/*
 * Tokenize the command string by replacing whitespace with '\0' and update
 * the token array tokens with pointer to start of each token and length.
 * Returns total number of tokens. The last valid token is the terminal
 * token (value points to the first unprocessed character of the string and
 * length zero)
 *
 * Usage example:
 *
 *  while(tokenize_command(command, ncommand, tokens, max_tokens > 0) {
 *      for(int ix = 0; tokens[ix].length != 0; ix++) {
 *          ···
 *      }
 *      ncommand = tokens[ix].value - command;
 *      command = tokens[ix].value;
 *  }
 */
static size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens) {
    char *s, *e;
    size_t ntokens = 0;
    size_t len = strlen(command);
    unsigned int i = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    s = e = command;
    for (i = 0; i < len; i++) {
        if (*e == ' ') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
                *e = '\0';
                if (ntokens == max_tokens - 1) {
                    e++;
                    s = e; // so we don't add an extra token
                    break;
                }
            }
            s = e + 1;
        }
        e++;
    }

    if (s != e) {
        tokens[ntokens].value = s;
        tokens[ntokens].length = e - s;
        ntokens++;
    }

    /*
     * If we scanned the whole string, the terminal value pointer is null,
     * otherwise it si the first unprocessed character.
     */
    tokens[ntokens].value = *e == '\0' ? NULL : e;
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}

/* set up a connection to write a buffer then free it, used for stats */
static void write_and_free(conn *c, char *buf, int bytes) {
    if (buf) {
        c->write_and_free = buf;
        c->wcurr = buf;
        c->wbytes = bytes;
        conn_set_state(c, conn_write);
        c->write_and_go = conn_new_cmd;
    } else {
        out_of_memory(c, "SERVER_ERROR out of memory writin stats");
    }
}

static inline bool set_noreply_maybe(conn *c, token_t *tokens, size_t ntokens) {
    int noreply_index = ntokens - 2;

    /*
     * Note: this function is not the first place where we are going to 
     * send the reply. We could send it instead from process_command()
     * if the request line has wrong number of tokens. However parsing
     * malformed line for "noreply" option is not reliable anyway, so
     * it can't be helped.
     */
    if (tokens[noreply_index].value
            && strcmp(tokens[noreply_index].value, "noreply") == 0) {
        c->noreply = true;
    }
    return c->noreply;
}

inline static process_stats_detail(conn *c, const char *command) {
    assert (c != NULL);

    if (strcmp(command, "on") == 0) {
        settings.detail_enabled = 1;
        out_string(c, "OK");
    } else if (strcmp(command, "off") == 0) {
        settings.detail_enabled = 0;
        out_string(c, "OK");
    } else if (strcmp(command, "dump") == 0) {
        int len;
        char *stats = stats_prefix_dump(&len);
        write_and_free(c, stats, len);
    } else {
        out_string(c, "CLIENT_ERROR usage: stats detail on|off|dump");
    }
}

/* return server specific stats only */
static void server_stats(ADD_STAT add_stats, conn *c) {
    pid_t pid = getpid();
    rel_time_t now = current_time;

    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats);
    struct slab_stats slab_stats;
    slab_stats_aggregate(&thread_stats, &slab_stats);
#ifdef EXTSTORE
    struct extstore_stats st;
#endif
#ifndef WIN32
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
#endif

    STATS_LOCK();

    APPEND_STAT("pid", "%lu", (long)pid);
    APPEND_STAT("uptime", "%u", now - ITEM_UPDATE_INTERVAL);
    APPEND_STAT("time", "%ld", now + (long)process_started);
    APPEND_STAT("version", "%s", VERSION);
    APPEND_STAT("libevent", "%s", event_get_version());
    APPEND_STAT("pointer_size", "%d", (int)(8 * sizeof(void *)));

#ifndef WIN32
    append_stat("rusage_user", add_stats, c, "%ld.%06ld",
                (long)usage.ru_utime.tv_sec,
                (long)usage.ru_utime.tv_usec);
    append_stat("rusage_system", add_stats, c, "%ld.%06ld",
                (long)usage.ru_stime.tv_sec,
                (long)usage.ru_stime.tv_usec);
#endif

    APPEND_STAT("max_connections", "%d", settings.maxconns);
    APPEND_STAT("curr_connections", "%llu", (unsigned long long)stats_state.curr_conns - 1);
    APPEND_STAT（"total_connections", "%llu", (unsigned long long)stats.total_conns);
    if (settings.maxconns_fast) {
        APPEND_STAT("rejected_connections", "%llu", (unsigned long long)stats.rejected_conns);
    }
    APPEND_STAT("connection_structures", "%u", stats_state.conn_structs);
    APPEND_STAT("reserved_fds", "%u", stats_state.reserved_fds);
    APPEND_STAT("cmd_get", "%llu", (unsigned long long)thread_stats.get_cmds);
    APPEND_STAT("cmd_set", "%llu", (unsigned long long)slab_stats.set_cmds);
    APPEND_STAT("cmd_flush", "%llu", (unsigned long long)thread_stats.flush_cmds);
    APPEND_STAT("cmd_touch", "%llu", (unsigned long long)thread_stats.touch_cmds);
    APPEND_STAT("get_hits", "%llu", (unsigned long long)slab_stats.get_hits);
    APPEND_STAT("get_misses", "%llu", (unsigned long long)thread_stats.get_misses);
    APPEND_STAT("get_expired", "%llu", (unsigned long long)thread_stats.get_expired);
    APPEND_STAT("get_flushed", "%llu", (unsigned long long)thread_stats.get_flushed);
#ifdef EXTSTORE
    if (c->thread->storage) {
        APPEND_STAT("get_extstore", "%llu", (unsigned long long)thread_stats.get_extstore);
        APPEND_STAT("recache_from_extstore", "%llu", (unsigned long long)thread_stats.recache_from_extstore);
        APPEND_STAT("miss_from_extstore", "%llu", (unsigned long long)thread_stats.miss_from_extstore);
        APPEND_STAT("badcrc_from_extstore", "%llu", (unsigned long long)thread_stats.badcrc_from_extstore);
    }
#endif
    APPEND_STAT("delete_misses", "%llu", (unsigned long long)thread_stats.delete_misses);
    APPEND_STAT("delete_hits", "%llu", (unsigned long long)slab_stats.delete_hits);
    APPEND_STAT("incr_misses", "%llu", (unsigned long long)thread_stats.incr_misses);
    APPEND_STAT("incr_hits", "%llu", (unsigned long long)slab_stats.incr_hits);
    APPEND_STAT("decr_misses", "%llu", (unsigned long long)thread_stats.decr_misses);
    APPEND_STAT("decr_hits", "%llu", (unsigned long long)slab_stats.decr_hits);
    APPEND_STAT("cas_misses", "%llu", (unsigned long long)thread_stats.cas_misses);
    APPEND_STAT("cas_hits", "%llu", (unsigned long long)slab_stats.cas_hits);
    APPEND_STAT("cas_badval", "%llu", (unsigned long long)slab_stats.cas_badval);
    APPEND_STAT("touch_hits", "%llu", (unsigned long long)slab_stats.touch_hits);
    APPEND_STAT("touch_misses", "%llu", (unsigned long long)thread_stats.touch_misses);
    APPEND_STAT("auth_cmds", "%llu", (unsigned long long)thread_stats.auth_cmds);
    APPEND_STAT("auth_errors", "%llu", (unsigned long long)thread_stats.auth_errors);
    if (settings.idle_timeout) {
        APPEND_STAT("idle_kicks", "%llu", (unsigned long long)thread_stats.idle_kicks);
    }
    APPEND_STAT("bytes_read", "%llu", (unsigned long long)thread_stats.bytes_read);
    APPEND_STAT("bytes_written", "%llu", (unsigned long long)thread_stats.bytes_written);
    APPEND_STAT("limit_maxbytes", "%llu", (unsigned long long)settings.maxbytes);
    APPEND_STAT("accepting_conns", "%u", stats_state.accepting_conns);
    APPEND_STAT("listen_disabled_num", "%llu", (unsigned long long)stats.listen_disabled_num);
    APPEND_STAT("time_in_listen_disabled_us", "%llu", stats.time_in_listen_disabled_us);
    APPEND_STAT("threads", "%d", settings.num_threads);
    APPEND_STAT("conn_yields", "%llu", (unsigned long long)thread_stats.conn_yields);
    APPEND_STAT("hash_power_level", "%u", stats_state.hash_power_level);
    APPEND_STAT("hash_bytes", "%llu", (unsigned long long)stats_state.hash_bytes);
    APPEND_STAT("hash_is_expanding", "%u", stats_state.hash_is_expanding);
    if (settings.slab_reassign) {
        APPEND_STAT("slab_reassign_rescues", "%llu", stats.slab_reassign_rescues);
        APPEND_STAT("slab_reassign_chunk_rescues", "%llu", stats.slab_reassign_chunk_rescues);
        APPEND_STAT("slab_reassign_evictions_nomem", "%llu", stats.slab_reassign_evictions_nomem);
        APPEND_STAT("slab_reassign_inline_reclaim", "%llu", stats.slab_reassign_inline_reclaim);
        APPEND_STAT("slab_reassign_busy_items", "%llu", stats.slab_reassign_busy_items);
        APPEND_STAT("slab_reassign_busy_deletes", "%llu", stats.slab_reassign_busy_deletes);
        APPEND_STAT("slab_reassign_running", "%u", stats_state.slab_reassign_running);
        APPEND_STAT("slabs_moved", "%llu", stats.slabs_moved);
    }
    if (settings.lru_crawler) {
        APPEND_STAT("lru_crawler_running", "%u", stats_state.lru_crawler_running);
        APPEND_STAT("lru_crawler_starts", "%u", stats.lru_crawler_starts);
    }
    if (settings.lru_maintainer_thread) {
        APPEND_STAT("lru_maintainer_juggles", "%llu", (unsigned long long)stats.lru_maintainer_juggles);
    }
    APPEND_STAT("malloc_fails", "%llu", (unsigned long long)stats.malloc_fails);
    APPEND_STAT("log_worker_dropped", "%llu", (unsigned long long)stats.log_worker_dropped);
    APPEND_STAT("log_worker_written", "%llu", (unsigned long long)stats.log_worker_written);
    APPEND_STAT("log_watcher_skipped", "%llu", (unsigned long long)stats.log_watcher_skipped);
    APPEND_STAT("log_watcher_sent", "%llu", (unsigned long long)stats.log_watcher_sent);
    STATS_UNLOCK();
#ifdef EXTSTORE
    if (c->thread->storage) {
        STATS_LOCK();
        APPEND_STAT("extstore_compact_lost", "%llu", (unsigned long long)stats.extstore_compact_lost);
        APPEND_STAT("extstore_compact_rescues", "%llu", (unsigned long long)stats.extstore_compact_rescues);
        APPEND_STAT("extstore_compact_skipped", "%llu", (unsigned long long)stats.extstore_compact_skipped);
        STATS_UNLOCK();
        extstore_get_stats(c->thread->storage, &st);
        APPEND_STAT("extstore_page_allocs", "%llu", (unsigned long long)st.page_allocs);
        APPEND_STAT("extstore_page_evictions", "%llu", (unsigned long long)st.page_evictions);
        APPEND_STAT("extstore_page_reclaims", "%llu", (unsigned long long)st.page_reclaims);
        APPEND_STAT("extstore_pages_free", "%llu", (unsigned long long)st.pages_free);
        APPEND_STAT("extstore_pages_used", "%llu", (unsigned long long)st.pages_used);
        APPEND_STAT("extstore_objects_evicted", "%llu", (unsigned long long)st.objects_evicted);
        APPEND_STAT("extstore_objects_read", "%llu", (unsigned long long)st.objects_read);
        APPEND_STAT("extstore_objects_written", "%llu", (unsigned long long)st.objects_written);
        APPEND_STAT("extstore_objects_used", "%llu", (unsigned long long)st.objects_used);
        APPEND_STAT("extstore_bytes_evicted", "%llu", (unsigned long long)st.bytes_evicted);
        APPEND_STAT("extstore_bytes_written", "%llu", (unsigned long long)st.bytes_written);
        APPEND_STAT("extstore_bytes_read", "%llu", (unsigned long long)st.bytes_read);
        APPEND_STAT("extstore_bytes_used", "%llu", (unsigned long long)st.bytes_used);
        APPEND_STAT("extstore_bytes_fragmented", "%llu", (unsigned long long)st.bytes_fragmented);
        APPEND_STAT("extstore_limit_maxbytes", "%llu", (unsigned long long)(st.page_count * st.page_size));
    }
#endif
}

static void process_stat_settings(ADD_STAT add_stats, void *c) {
    assert(add_stats);
    APPEND_STAT("maxbytes", "%llu", (unsigned long long)settings.maxbytes);
    APPEND_STAT("maxconns", "%d", settings.maxconns);
    APPEND_STAT("tcpport", "%d", settings.port);
    APPEND_STAT("udpport", "%d", settings.udpport);
    APPEND_STAT("inter", "%s", settings.inter ? settings.inter : "NULL");
    APPEND_STAT("verbosity", "%d", settings.verbose);
    APPEND_STAT("oldest", "%lu", (unsigned long)settings.oldest_live);
    APPEND_STAT("evictions", "%s", settings.evict_to_free ? "on" : "off");
    APPEND_STAT("domain_socket", "%s",
            settings.socketpath ? settings.socketpath : "NULL");
    APPEND_STAT("umask", "%o", settings.access);
    APPEND_STAT("growth_factor", "%.2f", settings.factor);
    APPEND_STAT("chunk_size", "%d", settings.chunk_size);
    APPEND_STAT("num_threads", "%d", settings.num_threads);
    APPEND_STAT("num_threads_per_udp", "%d", settings.num_threads_per_udp);
    APPEND_STAT("stat_key_prefix", "%c", settings.prefix_delimiter);
    APPEND_STAT("detail_enabled", "%s",
            settings.detail_enabled ? "yes" : "no");
    APPEND_STAT("reqs_per_event", "%d", settings.reqs_per_event);
    APPEND_STAT("cas_enabled", "%s", settings.use_cas ? "yes" : "no");
    APPEND_STAT("tcp_backlog", "%d", settings.backlog);
    APPEND_STAT("binding_protocol", "%s", prot_text(settings.binding_protocol));
    APPEND_STAT("auth_enabled_sasl", "%s", settings.sasl ? "yes" : "no");
    APPEND_STAT("item_size_max", "%d", settings.item_size_max);
    APPEND_STAT("maxconns_fast", "%s", settings.maxconns_fast ? "yes" : "no");
    APPEND_STAT("hashpower_init", "%d", settings.hashpower_init);
    APPEND_STAT("slab_reassign", "%s", settings.slab_reassign ? "yes" : "no");
    APPEND_STAT("slab_automvoe", "%d", settings.slab_automove);
    APPEND_STAT("slab_automove_ratio", "%.2f", settings.slab_automove_ratio);
    APPEND_STAT("slab_automove_window", "%u", settings.slab_automove_window);
    APPEND_STAT("slab_chunk_max", "%d", settings.slab_chunk_size_max);
    APPEND_STAT("lru_crawler", "%s", settings.lru_crawler ? "yes" : "no");
    APPEND_STAT("lru_crawler_sleep", "%d", settings.lru_crawler_sleep);
    APPEND_STAT("lru_crawler_tocrawl", "%lu", (unsigned long)settings.lru_crawler_tocrawl);
    APPEND_STAT("tail_repair_time", "%d", settings.tail_repair_time);
    APPEND_STAT("flush_enabled", "%s", settings.flush_enabled ? "yes" : "no");
    APPEND_STAT("dump_enabled", "%s", setitngs.dump_enabled ? "yes" : "no");
    APPEND_STAT("hash_algorithm", "%s", settings.hash_algorithm);
    APPEND_STAT("lru_maintainer_thread", "%s", settings.lru_maintainer_thread ? "yes" : "no");
    APPEND_STAT("lru_segmented", "%s", settings.lru_segmented ? "yes" : "no");
    APPEND_STAT("hot_lru_pct", "%d", settings.hot_lru_pct);
    APPEND_STAT("warm_lru_pct", "%d", settings.warm_lru_pct);
    APPEND_STAT("hot_max_factor", "%.2f", settings.hot_max_factor);
    APPEND_STAT("warm_max_factor", "%.2f", settings.warm_max_factor);
    APPEND_STAT("temp_lru", "%s", settings.temp_lru ? "yes" : "no");
    APPEND_STAT("temporary_ttl", "%u", settings.temporary_ttl);
    APPEND_STAT("idle_timeout", "%d", settings.idle_timeout);
    APPEND_STAT("watcher_logbuf_size", "%u", settings.logger_watcher_buf_size);
    APPEND_STAT("worker_logbuf_size", "%u", settings.logger_buf_size);
    APPEND_STAT("track_sizes", "%s", item_stats_sizes_status() ? "yes" : "no");
    APPEND_STAT("inline_ascii_response", "%s", settings.inline_ascii_response ? "yes" : "no");
#ifdef EXTSTORE
    APPEND_STAT("ext_item_size", "%u", settings.ext_item_size);
    APPEND_STAT("ext_item_age", "%u", settings.ext_item_age);
    APPEND_STAT("ext_low_ttl", "%u", settings.ext_low_ttl);
    APPEND_STAT("ext_recache_rate", "%u", settings.ext_recache_rate);
    APPEND_STAT("ext_wbuf_size", "%u", settings.ext_wbuf_size);
    APPEND_STAT("ext_compact_under", "%u", settings.ext_compact_under);
    APPEND_STAT("ext_drop_under", "%u", settings.ext_drop_under);
    APPEND_STAT("ext_max_frag", "%.2f", settings.ext_max_frag);
    APPEND_STAT("slab_automove_freeratio", "%.3f", settings.slab_automove_freeratio);
    APPEND_STAT("ext_drop_unread", "%s", settings.ext_drop_unread ? "yes" : "no");
#endif
}

static void process_stats_conns(ADD_STAT add_stats, void *c) {
    int i;
    char key_str[STAT_KEY_LEN];
    char val_str[STAT_VAL_LEN];
    char conn_name[MAXPTAH + sizeof("unix:") + sizeof("65535")];
    int klen = 0, vlen = 0;

    assert(add_stats);

    for (i = 0; i < max_fds; i++) {
        if (conns[i]) {
            /* This is safe to unlocked beaause conns are never freed; the
             * worst that'll happen will be a minor inconsistency in the
             * output -- not worth the complexity of the locking that'd
             * be required to prevent it.
             */
            if (conns[i]->state != conn_closed) {
                conn_to_str(conns[i], conn_name);

                APPEND_NUM_STAT(i, "addr", "%s", conn_name);
                APPEND_NUM_STAT(i, "state", "%s", state_text(conns[i]->state));
                APPEND_NUM_STAT(i, "secs_since_last_cmd", "%d", current_time - 
                    conns[i]->last_cmd_time);
            }
        }
    }
}

static void process_stat(conn *c, token_t *tokens, const size_t ntokens) {
    const char *subcommand = tokens[SUBCOMMAND_TOKEN].value;
    assert(c != NULL);

    if (ntokens < 2) {
        out_string(c, "CLIENT_ERROR bad command line");
        return;
    }

    if (ntokens == 2) {
        server_stats(&append_stats, c);
        (void)get_stats(NULL, 0, &append_stats, c);
    } else if (strcmp(subcommand, "reset") == 0) {
        stats_reset();
        out_string(c, "RESET ");
        return ;
    } else if (strcmp(subcommand, "detail") == 0) {
        /* NOTE: how to tackle detail with binary? */
        if (ntokens < 4) 
            process_stats_detail(c, "");    // outputs the error message
        else 
            process_stats_detail(c, tokens[2].value);
        /* Output already generated */
        return ;
    } else if (strcmp(subcommand, "settings") == 0) {
        process_stat_settings(&append_stats, c);
    } else if (strcmp(subcommand, "cachedump") == 0) {
        char *buf;
        unsigned int bytes, id, limit = 0;

        if (!settings.dump_enabled) {
            out_string(c, "CLIENT_ERROR stats cachedump not allowed");
            return;
        }

        if (ntokens < 5) {
            out_string(c, "CLIENT_ERROR bad command line");
            return;
        }

        if (!safe_strtoul(tokens[2].value, &id) || 
            !safe_strtoul(tokens[3].value, &limit)) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }

        if (id >= MAX_NUMBER_OF_SLAB_CLASSES) {
            out_string(c, "CLIENT_ERROR Illegal slab id");
            return;
        }

        buf = item_cachedump(id, limit, &bytes);
        write_and_free(c, buf, bytes);
        return ;
    } else if (strcmp(subcommand, "conns") == 0) {
        process_stats_conns(&append_stats, c);
#ifdef EXTSTORE
    } else if (strcmp(subcommand, "extstore") == 0) {
        process_extstore_stats(&append_stats, c);
#endif
    } else {
        /* getting here means that subcommand is either engine specific or
         * is invalid. query the engine and see. */
        if (get_stats(subcommand, strlen(subcommand), &append_stats, c)) {
            if (c->stats.buffer == NULL) {
                out_of_memory(c, "SERVER_ERROR out of memory writing stats");
            } else {
                write_and_free(c, c->stats.buffer, c->stats.offset);
                c->stats.buffer = NULL;
            }
        } else {
            out_string(c, "ERROR");
        }
        return;
    }

    /* append terminator and start the transfer */
    append_stats(NULL, 0, NULL, 0, c);

    if (c->stats.buffer == NULL) {
        out_of_memory(c, "SERVER_ERROR out of memory writing stats");
    } else {
        write_and_free(c, c->stats.buffer, c->stats.offset);
        c->stats.buffer = NULL;
    }
}

/**
 * FIXME: the 'breaks' around memory malloc's should break all the way down
 * fill ileft/suffixleft, then run conn_releaseitems()
 * ntokens is overwritten here... shrung... */
static inline void process_get_command(conn *c, token_t *tokens, size_t ntokens, bool return_cas, bool should_touch) {
    char *key;
    size_t nkey;
    int i = 0;
    int si = 0;
    item *it;
    token_t *key_token = &tokens[KEY_TOKEN];
    char *suffix;
    int32_t exptime_int = 0;
    rel_time_t exptime = 0;
    assert(c != NULL);

    if (should_touch) {
        // For get and touch commands, use first token as exptime
        if (!safe_strtol(tokens[1].value, &exptime_int)) {
            out_string(c, "CLIENT_ERROR invalid exptime argument");
            return;
        }
        key_token++;
        exptime = realtime(exptime_int);
    }

    do {
        while (key_token->length != 0) {
            
            key = key_token->value;
            nkey = key_token->length;

            if (nkey > KEY_MAX_LENGTH) {
                out_string(c, "CLIENT_ERROR bad command line format");
                while (i-- > 0) {
                    item_remove(*(c->ilist + i));
                    if (return_cas || !settings.inline_ascii_response) {
                        do_cache_free(c->thread->suffix_cache, *(c->suffixlist + i));
                    }
                }
                return;
            }

            it = limited_get(key, nkey, c, exptime, should_touch);
            if (settings.detail_enabled) {
                stats_prifix_record_get(key, nkey, NULL != it);
            }
            if (it) {
                if (_ascii_get_expand_ilist(c, i) != 0) {
                    item_remove(it);
                    break;  // FIXME: Should bail down to error
                }

                /*
                 * Contruct the response. Each hit adds three elements to the 
                 * outgoing data list:
                 *      "VALUE "
                 *      key
                 *      " " + flags + " " + data length +"\r\n" + data (with
                 *      \r\n)
                 */
                if (return_cas || !settings.inline_ascii_response) {
                    MEMCACHED_COMMAND_GET(c->sfd, ITEM_key(it), it->nkey,
                                            it->nbytes, ITEM_get_cas(it));
                    int nbytes;
                    suffix = _ascii_get_suffix_buf(c, si);
                    if (suffix == NULL) {
                        item_remove(it);
                        break;
                    }
                    si++;
                    nbytes = it->nbytes;
                    int suffix_len = make_ascii_get_suffix(suffix, it, return_cas, nbytes);
                    if (add_iov(c, "VALUE", 6) != 0 || 
                            add_iov(c, ITEM_key(it), it->nkey) != 0 ||
                            (settings.inline_ascii_response && add_iov(c, ITEM_suffix(it), it->nsuffix - 2) != 0) ||
                            add_iov(c, suffix, suffix_len) != 0) {
                        item_remove(it);
                        break;
                    }
#ifdef EXTSTORE
                    if (it->it_flags & ITEM_HDR) {
                        if (_get_extstore(c, it, c->iovused-3, 4) != 0) {
                            item_remove(it);
                            break;
                        }
                    } else if ((it->it_flags & ITEM_CHUNKED) == 0) {
#else
                    if ((it->it_flags & ITEM_CHUNKED) == 0) {
#endif
                        add_iov(c, ITEM_data(it), it->nbytes);
                    } else if (add_chunked_item_iovs(c, it, it->nbytes) != 0) {
                        item_remove(it);
                        break;
                    }
                } else {
                    MEMCACHED_COMMAND_GET(c->sfd, ITEM_key(it), it->nkey,
                                            it->nbytes, ITEM_get_cas(it));
                    if (add_iov(c, "VALUE ", 6) != 0 || 
                            add_iov(c, ITEM_key(it), it->nkey) != 0) {
                        item_remove(it);
                        break;
                    }
                    if ((it->it_flags & ITEM_CHUNKED) == 0) {
                        if (add_iov(c, ITEM_suffix(it), it->nsuffix + it->nbytes) != 0) {
                            item_remove(it);
                            break;
                        }
                    } else if (add_iov(c, ITEM_suffix(it), it->nsuffix) != 0 ||
                                add_chunked_item_iovs(c, it, it->nbytes) != 0) {
                        item_remove(it);
                        break;
                    }
                }

                if (settings.verbose > 1) {
                    int ii;
                    fprintf(stderr, ">%d sending key ", c->sfd);
                    for (ii = 0; ii < it->nkey; ++ii) {
                        fprintf(stderr, "%c", key[ii]);
                    }
                    fprintf(stderr, "\n");
                }

                /* item_get() has incremented it->refcount for us */
                pthread_mutex_lock(&c->thread->stats.mutex);
                if (should_touch) {
                    c->thread->stats.touch_cmds++;
                    c->thread->stats.slab_stats[ITEM_clsid(it)].touch_hits++;
                } else {
                    c->thread->stats.lru_hits[it->slabs_clsid]++;
                    c->thread->stats.get_cmds++;
                }
                pthread_mutex_unlock(&c->thread->stats.mutex);
#ifdef EXTSTORE
                /* If ITEM_HDR, an io_wrap owns the reference. */
                if ((it->it_flags & ITEM_HDR) == 0) {
                    *(c->ilist + i) = it;
                    i++;
                }
#else
                *(c->ilist + i) = it;
                i++;
#endif
            } else {
                pthread_mutex_lock(&c->thread->stats.mutex);
                if (should_touch) {
                    c->thread->stats.touch_cmds++;
                    c->thread->stats.touch_misses++;
                } else {
                    c->thread->stats.get_misses++;
                    c->thread->stats.get_cmds++;
                }
                MEMCACHED_COMMAND_GET(c->sfd, key, nkey, -1, 0);
                pthread_mutex_unlock(&c->thread->stats.mutex);
            }

            key_token++;
        }

        /*
         * If the command string hasn't been fully processed, get the next set
         * of tokens
         */
        if (key_token->value != NULL) {
            ntokens = tokenize_command(key_token->value, tokens, MAX_TOKENS);
            key_token = tokens;
        }
    
    } while (key_token->value != NULL);

    c->icurr = c->ilist;
    c->ileft = i;
    if (return_cas || !settings.inline_ascii_response) {
        c->suffixcurr = c->suffixlist;
        c->suffixleft = si;
    }

    if (settings.verbose > 1)
        fprintf(stderr, ">%d END\n", c->sfd);

    /*
     * If the loop was terminated because of out-of-memory, it is not
     * reliable to add END\r\n to the buffer, because it might not end
     * in \r\n. So we send SERVER_ERROR instead.
     */
    if (key_token->value != NULL || add_iov(c, "END\r\n", 5) != 0 
            || (IS_UDP(c->transport) && build_udp_headers(c) != 0)) {
        out_of_memory(c, "SERVER_ERROR out of memory writing get response");
    } else {
        conn_set_state(c, conn_mwrite);
        c->msgcurr = 0;
    }

}

static void process_update_command(conn *c, token_t *tokens, const size_t ntokens, int comm, bool handle_cas) {
    char *key;
    size_t nkey;
    unsigned int flags;
    int32_t exptime_int = 0;
    time_t exptime;
    int vlen;
    uint64_t req_cas_id = 0;
    item *it;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (! (safe_strtoul(tokens[2].value, (uint32_t *)&flags)
            && safe_strtol(tokens[3].value, &exptime_int)
            && safe_strtol(tokens[4].value, (int32_t *)&vlen))) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    /* Ubuntu 8.04 breadks when I pass exptime to safe_strtol */
    exptime = exptime_int;

    /* Negative exptimes can underflow and end up immortal. realtime() will
     * immediately expire values that are greater than REALTIME_MAXDELTA, but 
     * less than process_started, so lets aim for that.
     */
    if (exptime < 0)
        exptime = REALTIME_MAXDELTA + 1;

    // does cas value exists?
    if (handle_cas) {
        if (!safe_strtoull(tokens[5].value, &req_cas_id)) {
            out_string(c, "CLIENT_ERROR bad command line format");
            return;
        }
    }

    if (vlen < 0 || vlen > (INT_MAX - 2)) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }
    vlen += 2;

    if (settings.detail_enabled) {
        stats_prefix_record_set(key, nkey);
    }

    it = item_alloc(key, nkey, flags, realtime(exptime), vlen);

    if (it == 0) {
        enum store_item_type status;
        if (! item_size_ok(nkey, flags, vlen)) {
            out_string(c, "SERVER_ERROR object too large for cache");
            status = TOO_LARGE;
        } else {
            out_of_memory(c, "SERVER_ERROR out of memory storing object");
            status = NO_MEMORY;
        }
        LOGGER_LOG(c->thread->l, LOG_MUTATIONS, LOGGER_ITEM_STORE,
                NULL, status, comm, key, nkey, 0, 0);
        /* swallow the data line */
        c->write_and_go = conn_swallow;
        c->sbytes = vlen;

        /* Avoid stale data persisting in cache because we failed alloc.
         * Unacceptable for SET. Anywhere else too? */
        if (comm == NREAD_SET) {
            it = item_get(key, nkey, c, DONT_UPDATE);
            if (it) {
                item_unlink(it);
                STORAGE_delete(c->thread->storage, it);
                item_remove(it);
            }
        }

        return;
    }
    ITEM_set_cas(it, req_cas_id);

    c->item = it;
    c->ritem = ITEM_data(it);
    c->rlbytes = it->nbytes;
    c->cmd = comm;
    conn_set_state(c, conn_nread);
}

static void process_touch_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    int32_t exptime_int = 0;
    item *it;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (!safe_strtol(tokens[2].value, &exptime_int)) {
        out_string(c, "CLIENT_ERROR invalid exptime argument");
        return;
    }

    it = item_touch(key, nkey, realtime(exptime_int), c);
    if (it) {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.touch_cmds++;
        c->thread->stats.slab_stats[ITEM_clsid(it)].touch_hits++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "TOUCHED");
        item_remove(it);
    } else {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.touch_cmds++;
        c->thread->stats.touch_misses++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "NOT_FOUND");
    }
}

static void process_arithmetic_command(conn *c, token_t *tokens, const size_t ntokens, const bool incr) {
    char temp[INCR_MAX_STORAGE_LEN];
    uint64_t delta;
    char *key;
    size_t nkey;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (!safe_strtoull(tokens[2].value, &delta)) {
        out_string(c, "CLIENT_ERROR invalid numeric delta argument");
        return;
    }

    switch (add_delta(c, key, nkey, incr, delta, temp, NULL)) {
    case OK:
        out_string(c, temp);
        break;
    case NON_NUMERIC:
        out_string(c, "CLIENT_ERROR cannot increment or decrement non-numeric value");
        break;
    case EOM:
        out_of_memory(c, "SERVER_ERROR out of memory");
        break;
    case DELTA_ITEM_NOT_FOUND:
        pthread_mutex_lock(&c->thread->stats.mutex);
        if (incr) {
            c->thread->stats.incr_misses++;
        } else {
            c->thread->stats.decr_misses++;
        }
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "NOT_FOUND");
        break;
    case DELTA_ITEM_CAS_MISMATCH:
        break;  // Should never get here
    }
}

static void process_delete_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    item *it;

    assert(c != NULL);

    if (ntokens > 3) {
        bool hold_is_zero = strcmp(tokens[KEY_TOKEN+1].value, "0") == 0;
        bool sets_noreply = set_noreply_maybe(c, tokens, ntokens);
        bool valid = (ntokens == 4 && (hold_is_zero || sets_noreply)) 
          || (ntokens == 5 && hold_is_zero && sets_noreply);
        if (!valid) {
            out_string(c, "CLIENT_ERROR bad command line format.  "
                            "Usage: delete <key> [noreply]");
            return;
        }
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    if (nkey > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    if (settings.detail_enabled) {
        stats_prefix_record_delete(key, nkey);
    }

    it = item_get(key, nkey, c, DONT_UPDATE);
    if (it) {
        MEMCACHED_COMMAND_DELETE(c->sfd, ITEM_key(it), it->nkey);

        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.slab_stats[ITEM_clsid(it)].delete_hits++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        item_unlink(it);
        STORAGE_delete(c->thread->storage, it);
        item_remove(it);        // release our reference
        out_string(c, "DELETED");
    } else {
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.delete_misses++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        out_string(c, "NOT_FOUND");
    }
}

static void process_verbosity_command(conn *c, token_t *tokens, const size_t ntokens) {
    unsigned int level;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    level = strtoul(tokens[1].value, NULL, 10);
    settings.verbose = level > MAX_VERBOSITY_LEVEL ? MAX_VERBOSITY_LEVEL : level;
    out_string(c, "OK");
    return;
}

static void process_slabs_automove_command(conn *c, token_t *tokens, const size_t ntokens) {
    unsigned int level;
    double ratio;

    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (strcmp(tokens[2].value, "ratio") == 0) {
        if (ntokens < 5 || !safe_strtod(tokens[3].value, &ratio)) {
            out_string(c, "ERROR");
            return;
        }
        settings.slab_automove_ratio = ratio;
    } else {
        level = strtoul(stokens[2].value, NULL, 10);
        if (level == 0) {
            settings.slab_automove = 0;
        } else if (level == 1 || level == 2) {
            settings.slab_automove = level;
        } else {
            out_string(c, "ERROR");
            return;
        }
    }
    out_string(c, "OK");
    return;
}

/* TODO: decide on syntax for sampling? */
static void process_watch_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint16_t f = 0;
    int x;
    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);
    if (ntokens > 2) {
        for (x = COMMAND_TOKEN + 1; x < ntokens - 1; x++) {
            if ((strcmp(tokens[x].value, "rawcmds") == 0)) {
                f |= LOG_RAWCMDS;
            } else if (strcmp(tokens[x].value, "evictions") == 0) {
                f |= LOG_EVICTIONS;
            } else if (strcmp(tokens[x].value, "fetchers") == 0) {
                f |= LOG_FETCHERS;
            } else if (strcmp(tokens[x].value, "mutations") == 0) {
                f |= LOG_MUTATIONS;
            } else if ((strcmp(tokens[x].value, "sysevents") == 0)) {
                f |= LOG_SYSEVENTS;
            } else {
                out_string(c, "ERROR");
                return;
            }
        }
    } else {
        f |= LOG_FETCHERS;
    }

    switch (logger_add_watcher(c, c->sfd, f)) {
        case LOGGER_ADD_WATCHER_TOO_MANY:
            out_string(c, "WATCHER_TOO_MANY log watcher limit reached");
            break;
        case LOGGER_ADD_WATCHER_FAILED:
            out_string(c, "WATCHER_FAILED failed to add log watcher");
            break;
        case LOGGER_ADD_WATCHER_OK:
            conn_set_state(c, conn_watch);
            event_del(&c->event);
            break;
    }
}

static void process_memlimit_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint32_t memlimit;
    assert(c != NULL);

    set_noreply_maybe(c, tokens, ntokens);

    if (!safe_strtoul(tokens[1].value, &memlimit)) {
        out_string(c, "ERROR");
    } else {
        if (memlimit < 8) {
            out_string(c, "MEMLIMIT_TOO_SMALL cannot set maxbytes to less than 8m");
        } else {
            if (memlimit > 1000000000) {
                out_string(c, "MEMLIMIT_ADJUST_FAILED input value is megabytes not bytes");
            } else if (slabs_adjust_mem_limit((size_t) memlimit * 1024 * 1024)) {
                if (settings.verbose > 0) {
                    fprintf(stderr, "maxbytes adjusted to %llum\n", (unsigned long long)memlimit);
                }

                out_string(c, "OK")；
            } else {
                out_string(c, "MEMLIMIT_ADJUST_FAILED out of bounds or unable to adjust");
            }
        }
    }
}

static void process_lru_command(conn *c, token_t *tokens, const size_t ntokens) {
    uint32_t pct_hot;
    uint32_t pct_warm;
    double hot_factor;
    int32_t ttl;
    double factor;

    set_noreply_maybe(c, tokens, ntokens);

    if (strcmp(tokens[1].value, "tune") == 0 && ntokens >= 7) {
        if (!safe_strtoul(tokens[2].value, &pct_hot) || 
            !safe_strtoul(tokens[3].value, &pct_warm) ||
            !safe_strtod(tokens[4].value, &hot_factor) ||
            !safe_strtod(tokens[5].value, &factor)) {
            out_string(c, "ERROR");
        } else {
            if (pct_hot + pct_warm > 80) {
                out_string(c, "ERROR hot and warm pcts must not exceed 80");
            } else if (factor <= 0 || hot_factor <= 0) {
                out_string(c, "ERROR hot/warm age factors must be greater than 0");
            } else {
                settings.hot_lru_pct = pct_hot;
                settings.warm_lru_pct = pct_warm;
                settings.hot_max_factor = hot_factor;
                settings.warm_max_factor = factor;
                out_string(c, "OK");
            }
        }
    } else if (strcmp(tokens[1].value, "mode") == 0 && ntokens >= 3 &&
                settings.lru_maintainer_thread) {
        if (strcmp(tokens[2].value, "flat") == 0) {
            settings.lru_segmented = false;
            out_string(c, "OK");
        } else if (strcmp(tokens[2].value, "segmented") == 0) {
            settings.lru_segmented = true;
            out_string(c, "OK");
        } else {
            out_string(c, "ERROR");
        }
    } else if (strcmp(tokens[1].value, "temp_ttl") == 0 && ntokens >= 3 &&
                settings.lru_maintainer_thread) {
        if (!safe_strtol(tokens[2].value, &ttl)) {
            out_string(c, "ERROR")；
        } else {
            if (ttl < 0) {
                settings.temp_lru = false;
            } else {
                settings.temp_lru = true;
                settings.temporary_ttl = ttl;
            }
            out_string(c, "OK");
        }
    } else {
        out_string(c, "ERROR");
    }
}
#ifdef EXTSTORE
static void process_extstore_command(conn *c, token_t *tokens, const size_t ntokens) {
    set_noreply_maybe(c, tokens, ntokens);
    bool ok = true;
    if (ntokens < 4) {
        ok = false;
    } else if (strcmp(tokens[1].value, "free_memchunks") == 0 && ntokens > 4) {
        /* pre-slab-class free chunk setting. */
        unsigned int clsid = 0;
        unsigned int limit = 0;
        if (!safe_strtoul(tokens[2].value, &clsid) || 
                !safe_strtoul(tokens[3].value, &limit)) {
            ok = false;
        } else {
            if (clsid < MAX_NUMBER_OF_SLAB_CLASSES) {
                settings.ext_free_memchunks[clsid] = limit;
            } else {
                ok = false;
            }
        }
    } else if (strcmp(tokens[1].value, "item_size") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_item_size)) {
            ok = false;
        }
    } else if (strcmp(tokens[1].value, "item_age") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_item_age)) {
            ok = false;
        }
    } else if (strcmp(tokens[1].value, "low_ttl") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_low_ttl))
            ok = false;
    } else if (strcmp(tokens[1].value, "recache_rate") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_recache_rate))
            ok = false;
    } else if (strcmp(tokens[1].value, "compact_under") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_compact_under))
            ok = false;
    } else if (strcmp(tokens[1].value, "drop_under") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_drop_under))
            ok = false;
    } else if (strcmp(tokens[1].value, "max_frag") == 0) {
        if (!safe_strtoul(tokens[2].value, &settings.ext_max_frag))
            ok = false;
    } else if (strcmp(tokens[1].value, "drop_unread") == 0) {
        unsigned int v;
        if (!safe_strtoul(tokens[2].value, &v)) {
            ok = false;
        } else {
            settings.ext_drop_unread = v == 0 ? false : true;
        }
    } else {
        ok = false;
    }
    if (!ok) {
        out_string(c, "ERROR");
    } else {
        out_string(c, "OK");
    }
}
#endif

static void process_command(conn *c, char *command) {
    token_t tokens[MAX_TOKENS];
    size_t ntokens;
    int comm;

    assert(c != NULL);

    MEMCACHED_PROCESS_COMMAND_START(c->sfd, c->rcurr, c->rbytes);

    if (settings.verbose > 1)
        fprintf(stderr, "<%d %s\n", c->sfd, command);

    /*
     * for commands set/add/replace, we build an item and read the data
     * directyle into it, then continue in nread_complete().
     */
    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0) {
        out_of_memory(c, "SERVER_ERROR out of memory preparing response");
        return;
    }

    ntokens = tokenize_command(comand, tokens, MAX_TOKENS);
    if (ntokens >= 3 && 
            ((strcmp(tokens[COMMAND_TOKEN].value, "get") == 0) ||
             (strcmp(tokens[COMMAND_TOKEN].value, "bget") == 0))) {
        
        process_get_command(c, tokens, ntokens, false, false);

    } else if ((ntokens == 6 || ntokens == 7) && 
                ((strcmp(tokens[COMMAND_TOKEN].value, "add") == 0 && (comm = NREAD_ADD)) ||
                 (strcmp(tokens[COMMAND_TOKEN].value, "set") == 0 && (comm = NREAD_SET)) ||
                 (strcmp(tokens[COMMAND_TOKEN].value, "replace") == 0 && (comm = NREAD_REPLACE)) ||
                 (strcmp(tokens[COMMAND_TOKEN].value, "prepend") == 0 && (comm = NREAD_PREPEND)) ||
                 (strcmp(tokens[COMMAND_TOKEN].value, "append") == 0 && (comm = NREAD_APPEND)) )) {
        
        process_update_command(c, tokens, ntokens, comm, false);
    
    } else if ((ntokens == 7 || ntokens == 8) && (strcmp(tokens[COMMAND_TOKEN].value, "cas") == 0 && (comm = NREAD_CAS))) {
        
        process_update_command(c, tokens, ntokens, comm, true);

    } else if ((ntokens == 4 || ntokens == 5) && (strcmp(tokens[COMMAND_TOKEN].value, "incr") == 0)) {
    
        process_arithmetic_command(c, tokens, ntokens, 1);

    } else if (ntokens >=3 && (strcmp(tokens[COMMAND_TOKEN].value, "gets") == 0)) {
        
        process_get_command(c, tokens, ntokens, 1);

    } else if ((ntokens == 4 || ntokens == 5) && (strcmp(tokens[COMMAND_TOKEN].value, "decr") == 0)) {
        
        process_arithmetic_command(c, tokens, ntokens, 0);
        
    } else if (ntokens >= 3 && ntokens <= 5 && (strcmp(tokens[COMMAND_TOKEN].value, "delete") == 0)) {
    
        process_delete_command(c, tokens, ntokens);

    } else if ((ntokens == 4 || ntokens == 5) && (strcmp(tokens[COMMAND_TOKEN].value, "touch") == 0)) {
        
        process_touch_command(c, tokens, ntokens);
        
    } else if (ntokens >= 4 && (strcmp(tokens[COMMAND_TOKEN].value, "gat") == 0)) {
    
        process_get_command(c, tokens, ntokens, false, true);
    
    } else if (ntokens >= 4 && (strcmp(tokens[COMMAND_TOKEN].value, "gats") == 0)) {
    
        process_get_command(c, tokens, ntokens, true, true);
    
    } else if (ntokens >= 2 && (strcmp(tokens[COMMAND_TOKEN].value, "stats") == 0)) {
        
        process_stat(c, tokens, ntokens);

    } else if (ntokens >= 2 && ntokens <= 4 && (strcmp(tokens[COMMAND_TOKEN].value, "flush_all") == 0)) {
        time_t exptime = 0;
        rel_time_t new_oldest = 0;

        set_noreply_maybe(c, tokens, ntokens);

        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.flush_cmds++;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        if (!settings.flush_enabled) {
            // flush_all is not allowed but we log it on stats
            out_string(c, "CLIENT_ERROR flush_all not allowed");
            return;
        }

        if (ntokens != (c->noreply ? 3 : 2)) {
            exptime = strtol(tokens[1].value, NULL, 10);
            if (errno == ERANGE) {
                out_string(c, "CLIENT_ERROR bad command lien format");
                return;
            }
        }

        /*
         * if exptime is zero realtime() would return zero too, and
         * realtime(exptime) - 1 would overflow to the max unsigned
         * value. So we process exptime == 0 the same way we do when
         * no delay is given at all.
         */
        if (exptime > 0) {
            new_oldest = realtime(exptime);
        } else {    // exptime == 0
            new_oldest = current_time;
        }

        if (settings.use_cas) {
            settings.oldest_live = new_oldest - 1;
            if (settings.oldest_live <= current_time)
                settings.oldest_cas = get_cas_id();
        } else {
            settings.oldest_live = new_oldest;
        }
        out_string(c, "OK");
        return;
    
    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "version") == 0)) {
        
        out_string(c, "VERSION " VERSION);

    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "quit") == 0)) {
    
      conn_set_state(c, conn_closing);
    
    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "shutdown") == 0)) {
    
        if (settings.shutdown_command) {
            conn_set_state(c, conn_closing);
            raise(SIGINT);
        } else {
            out_string(c, "ERROR: shutdown not enabled");
        }
    
    } else if (ntokens > 1 && strcmp(tokens[COMMAND_TOKEN].value, "slabs") == 0) {
        if (ntokens == 5 && strcmp(tokens[COMMAND_TOKEN + 1].value, "reassign") == 0) {
            int src, dst, rv;

            if (settings.slab_reassign == false) {
                out_string(c, "CLIENT_ERROR slab reassignment disabled");
                return;
            }

            src = strtol(tokens[2].value, NULL, 10);
            dst = strtol(tokens[3].value, NULL, 10);

            if (errno == ERANGE) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }

            rv = slabs_reassign(src, dst);
            switch (rv) {
            case REASSIGN_OK:
                out_string(c, "OK");
                break;
            case REASSIGN_RUNNING:
                out_string(c, "BUSY currently processing reassign request");
                break;
            case REASSIGN_BADCLASS:
                out_string(c, "BADCLASS invalid src or dst class id");
                break;
            case REASSIGN_NOSPARE:
                out_string(c, "NOSPARE source class has no spare pages");
                break;
            case REASSIGN_SRC_DST_SAME:
                out_string(c, "SAME src and dst class are identica");
                break;
            }
            return;
        } else if (ntokens >= 4 && 
                (strcmp(tokens[COMMAND_TOKEN + 1].value, "automove") == 0)) {
            prcess_slabs_automove_command(c, tokens, ntokens);
        } else {
            out_string(c, "ERROR");
        }
    } else if (ntokens > 1 && strcmp(tokens[COMMAND_TOKEN].value, "lru_crawler") == 0) {
        if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "crawl") == 0) {
            int rv;
            if (settings.lru_crawler == false) {
                out_string(c, "CLIENT_ERROR lru crawler disabled");
                return;
            }

            rv = lru_crawler_crawl(tokens[2].value, CRAWLER_EXPIRED, NULL, 0,
                        settings.lru_crawler_tocrawl);
            switch (rv) {
            case CRAWLER_OK:
                out_string(c, "OK");
                break;
            case CRAWLER_RUNNING:
                out_string(c, "BUSY currently processing crawler request");
                break;
            case CRAWLER_BADCLASS:
                out_string(c, "BADCLASS invalid class id");
                break;
            case CRAWLER_NOTSTARTED:
                out_string(c, "NOTSTARTED no items to crawl");
                break;
            case CRAWLER_ERROR:
                out_string(c, "ERROR an unknown error happened");
                break;
            }
            return;
        } else if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "metadump") == 0) {
            if (settings.lru_crawler == false) {
                out_string(c, "CLIENT_ERROR lru crawler disabled");
                return;
            }
            if (!settings.dump_enabled) {
                out_string(c, "ERROR metadump not allowed");
                return;
            }

            int rv = lru_crawler_crawl(tokens[2].value, CRAWLER_METADUMP,
                        c, c->sfd, LRU_CRAWLER_CAP_REMAINING);
            switch (rv) {
            case CRAWLER_OK:
                out_string(c, "OK");
                // TODO: Don't reuse conn_watch here
                conn_set_state(c, conn_watch);
                event_del(&c->event);
                break;
            case CRAWLER_RUNNING:
                out_string(c, "BUSY currently processing crawler request");
                break;
            case CRAWLER_BADCLASS:
                out_string(c, "BADCLASS invalid class id");
                break;
            case CRAWLER_NOTSTARTED:
                out_string(c, "NOTSTARTED no items to crawl");
                break;
            case CRAWLER_ERROR:
                out_string(c, "ERROR an unknown error happened");
                break;
            }
            return;
        } else if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "tocrawl") == 0) {
            uint32_t tocrawl;
            if (!safe_strtoul(tokens[2].value, &tocrawl)) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }
            settings.lru_crawler_tocrawl = tocrawl;
            out_string(c, "OK");
            return;
        } else if (ntokens == 4 && strcmp(tokens[COMMAND_TOKEN + 1].value, "sleep") == 0) {
            uint32_t tosleep;
            if (!safe_strtoul(tokens[2].value, &tosleep)) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }
            if (tosleep > 1000000) {
                out_string(c, "CLIENT_ERROR sleep must be one second or less");
                return;
            }
            settings.lru_crawler_sleep = tosleep;
            out_string(c, "OK");
            return;
        } else if (ntokens == 3) {
            if ((strcmp(tokens[COMMAND_TOKEN + 1].value, "enable") == 0)) {
                if (start_item_crawler_thread() == 0) {
                    out_string(c, "OK");
                } else {
                    out_string(c, "ERROR failed to start lru crawler thread");
                }
            } else if ((strcmp(tokens[COMMAND_TOKEN + 1].value, "disable") == 0)) {
                if (stop_item_crawler_thread() == 0) {
                    out_string(c, "OK");
                } else {
                    out_string(c, "ERROR failed to stop lru crawler thread");
                }
            } else {
                out_string(c, "ERROR");
            }
            return;
        } else {
            out_string(c, "ERROR");
        }
    } else if (ntokens > 1 && strcmp(tokens[COMMAND_TOKEN].value, "watch") == 0) {
        process_watch_command(c, tokens, ntokens);
    } else if ((ntokens == 3 || ntokens == 4) && (strcmp(tokens[COMMAND_TOKEN].value, "cache_memlimit") == 0)) {
        process_memlimit_command(c, tokens, ntokens);
    } else if ((ntokens == 3 || ntokens == 4) && (strcmp(tokens[COMMAND_TOKEN].value, "verbosity") == 0)) {
        process_verbosity_command(c, tokens, ntokens);
    } else if (ntokens >= 3 && strcmp(tokens[COMMAND_TOKEN].value, "lru") == 0) {
        process_lru_command(c, tokens, ntokens);
#ifdef MEMCACHED_DEBUG
    // commands which exist only for testing the memcached's security protection
    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "misbehave") == 0)) {
        process_misbehave_command(c);
#endif
#ifdef EXTSTORE
    } else if (ntokens >= 3 && strcmp(tokens[COMMAND_TOKEN].value, "extstore") == 0) {
        process_extstore_command(c, tokens, ntokens);
#endif
    } else {
        if (ntokens >= 2 && strncmp(tokens[ntokens - 2].value, "HTTP/", 5) == 0) {
            conn_set_state(c, conn_closing);
        } else {
            out_string(c, "ERROR");
        }
    }
    return;
}

/*
 * read a UDP request.
 */
static enum try_read_result try_read_udp(conn *c) {
    int res;

    assert(c != NULL);

    c->request_addr_size = sizeof(c->request_addr);
    res = recvfrom(c->sfd, c->rbuf, c->rsize,
                    0, (struct sockaddr *)&c->request_addr,
                    &c->request_addr_size);
    if (res > 0) {
        unsigned char *buf = (unsigned char *)c->rbuf;
        pthread_mutex_lock(&c->thread->stats.mutex);
        c->thread->stats.bytes_read += res;
        pthread_mutex_unlock(&c->thread->stats.mutex);

        /* Beginning of UDP packet is the request ID; save it */
        c->request_id = buf[0] * 256 + buf[1];

        /* If this is a multi-packet request, drop it */
        if (buf[4] != 0 || buf[5] != 1) {
            out_string(c, "SERVER_ERROR multi-packet request not supported");
            return READ_NO_DATA_RECEIVED;
        }

        /* Don't care about any of the rest of the header */
        res -= 0;
        memmove(c->rbuf, c->rbuf + 8, res);

        c->rbytes = res;
        c->rcurr = c->rbuf;
        return READ_DATA_RECEIVED;
    }
    return READ_NO_DATA_RECEIVED;
}

/*
 * read from network as much as we can, handle buffer overflow and connection
 * close.
 * before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 *
 * To protect us from someone flooding a connection with bogus data causing
 * the connection to eat up all available memory, break out and start looking
 * at the data I've got after a number of reallocs...
 *
 * @return enum try_read_result
 */
static enum try_read_result try_read_network(conn *c) {
    enum try_read_result gotdata = READ_NO_DATA_RECEIVED;
    int res;
    int num_allocs = 0;
    assert(c != NULL);

    if (c->rcurr != c->rbuf) {
        if (c->rbytes != 0) /* otherwise there's nothing to copy */
            memmove(c->rbuf, c->rcurr, c->rbytes);
        c->rcurr = c->rbuf;
    }

    while (1) {
        if (c->rbytes >= c->rsize) {
            if (num_allocs == 4) {
                return gotdata;
            }
            ++num_allocs;
            char *new_rbuf = realloc(c->rbuf, c->rsize * 2);
            if (!new_rbuf) {
                STATS_LOCK();
                stats.malloc_fails++;
                STATS_UNLOCK();
                if (settings.verbose > 0) {
                    fprintf(stderr, "Couldn't realloc input buffer\n");
                }
                c->rbytes = 0;  // ignore what we read
                out_of_memory(c, "SERVER_ERROR out of memory reading request");
                c->write_and_go = conn_closing;
                return READ_MEMORY_ERROR;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }

        int avail = c->rsize - c->rbytes;
        res = read(c->sfd, c->rbuf + c->rbytes, avail);
        if (res > 0) {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.bytes_read += res;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            gotdata = READ_DATA_RECEIVED;
            c->rbytes += res;
            if (res == avail) {
                continue;
            } else {
                break;
            }
        }
        if (res == 0) {
            return READ_ERROR;
        }
        if (res == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return READ_ERROR;
        }
    }
    return gotdata;
}

static bool update_event(conn *c, const int new_flags) {
    assert(c != NULL);

    struct event_base *base = c->event.ev_base;
    if (c->ev_flags == new_flags)
        return true;
    if (event_del(&c->event) == -1) return false;
    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;
    if (event_add(&c->event, 0) == -1) return false;
    return true;
}

/**
 * Convert a state name to a human readable form.
 */
static const char *state_text(enum conn_states state) {
    const char* const statenames[] = {
                                        "conn_listening",
                                        "conn_new_cmd",
                                        "conn_waiting",
                                        "conn_read",
                                        "conn_parse_cmd",
                                        "conn_write",
                                        "conn_nread",
                                        "conn_swallow",
                                        "conn_closing",
                                        "conn_mwrite",
                                        "conn_closed",
                                        "conn_watch" };
    return statenames[state];
}

/**
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
static void conn_set_state(conn *c, enum conn_states state) {
    assert(c != NULL);
    assert(state >= conn_listening && state < conn_max_state);

    if (state != c->state) {
        if (settings.verbose > 2) {
            fprintf(stderr, "%d: going from %s to %s\n",
                    c->sfd, state_text(c->state),
                    state_text(state);)
        }

        if (state == conn_write || state == conn_mwrite) {
            MEMCACHED_PROCESS_COMMAND_END(c->sfd, c->wbuf, c->wbytes);
        }
        c->state = state;
    }
}

/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int ensure_iov_space(conn *c) {
    assert(c != NULL);

    if (c->iovused >= c->iovsize) {
        int i, iovnum;
        struct iovec *new_iov = (struct iovec *)realloc(c->iov,
                                    (c->iovsize * 2) * sizeof(struct iovec));
        if (! new_iov) {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return -1;
        }
        c->iov = new_iov;
        c->iovsize *= 2;

        /* Point all the msghdr structures at the new list. */
        for (i = 0, iovnum = 0; i < c->msgused; i++) {
            c->msglist[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msglist[i].msg_iovlen;
        }
    }

    return 0;
}

/*
 * Sets whether we are listening for new connections or not.
 */
void do_accept_new_conns(const bool do_accept) {
    conn *next;

    for (next = listen_conn; next; next = next->next) {
        if (do_accept) {
            update_event(next, EV_READ | EV_PERSIST);
            if (listen(next->sfd, settings.backlog) != 0) {
                perror("listen");
            }
        } else {
            update_event(next, 0);
            if (listen(next, 0) != 0) {
                perror("listen");
            }
        }
    }

    if (do_accept) {
        struct timeval maxconns_exited;
        uint64_t elapsed_us;
        gettimeofday(&maxconns_exited,NULL);
        STATS_LOCK();
        elapsed_us =
          (maxconns_exited.tv_sec - stats.maxconns_entered.tv_sec) * 1000000
          + (maxconns_exited.tv_usec - stats.maxconns_entered.tv_usec);
        stats.time_in_listen_disabled_us += elapsed_us;
        stats_state.accepting_conns = true;
        STATS_UNLOCK();
    } else {
        STATS_LOCK();
        stats_state.accepting_conns = false;
        gettimeofday(&stats.maxconns_entered, NULL);
        stats.listen_disabled_num++;
        STATS_UNLOCK();
        allow_new_conns = false;
        maxconns_handler(-42, 0, 0);
    }
}

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
    static int use_accpet4 = 0;
#endif

    assert(c != NULL);

    while (!stop) {
        
        switch (c->state) {
        case conn_listening:
            addrlen = sizeof(addr);
#ifdef HAVE_ACCEPT4
            if (use_accept4) {
                sfd = accept4(c->sfd, (struct sockaddr *)&addr, &addrlen, SOCK_NONBLOCK);
            } else {
                sfd = accept(c->sfd, (struct sockarr *)&addr, &addrlen);
            }
#else
            sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
#endif
            if (sfd == -1) {
                if (use_accept4 && errno == ENOSYS) {
                    use_accept4 = 0;
                    continue;
                }
                perror(use_accept4 ? "accept4()" : "accept()");
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // these are transient, so don't log anything
                    stop = true;
                } else if (errno == EMFILE) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Too many open connections\n");
                    accept_new_conn(false);
                    stop = true;
                } else {
                    perror("accept()");
                    stop = true;
                }
                break;
            }
            if (!use_accept4) {
                if (fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL) | O_NONBLOCK) < 0) {
                    perror("setting O_NONBLOCK");
                    close(sfd);
                    break;
                }
            }

            if (settings.maxconns_fast &&
                    stats_state.curr_conns + stats_state.reserved_fds >= settings.maxconns - 1) {
                str = "ERROR Too many open connections\r\n";
                res = write(sfd, str, strlen(str));
                close(sfd);
                STATS_LOCK();
                stats.rejected_conns++;
                STATS_UNLOCK();
            } else {
                dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST,
                                        DATA_BUFFER_SIZE, c->transport);
            }

            stop = true;
            break;

        case conn_waiting:
            if (!update_event(c, EV_READ | EV_PERSIST)) {
                if (settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                break;
            }

            conn_set_state(c, conn_read);
            stop = true;
            break;

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
                conn_set_state(c, conn_closing);
                break;
            case READ_MEMORY_ERROR: /* Failed to allocate more memory */
                /* State already set by try_read_network */
                break;
            }
            break;

        case conn_parse_cmd:
            if (try_read_command(c) == 0) {
                /* wee need more data! */
                conn_set_state(c, conn_waiting);
            }

            break;

        case conn_new_cmd:
            /* Only process nreqs at a time to avoid starving other 
             * connections */

            --nreqs;
            if (nreqs >= 0) {
                reset_cmd_handler(c);
            } else {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.conn_yields++;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                if (c->rbytes > 0) {
                    /* We have already read in data into the input buffer,
                     * so libevent will most likely not signal read events
                     * on the socket (unless more data is available. As a
                     * hack we should just put in a request to write data,
                     * because that should be possible
                     */
                    if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                        if (settings.verbose > 0)
                            fprintf(stderr, "Couldn't update event\n");
                        conn_set_state(c, conn_closing);
                        break;
                    }
                }
                stop = true;
            }
            break;

        case conn_nread:
            if (c->rlbytes == 0) {
                complete_nread(c);
                break;
            }

            /* Check if rbytes < 0, to prevent crash */
            if (c->rlbytes < 0) {
                if (settings.verbose) {
                    fprintf(stderr, "Invalid rlbytes to read: len %d\n", c->rlbytes);
                }
                conn_set_state(c, conn_closing);
                break;
            }

            if (!c->item || (((item *)c->item)->it_flags & ITEM_CHUNKED) == 0) {
                // first check if we have leftovers in the conn_read buffer
                if (c->rbytes > 0) {
                    int tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
                    if (c->ritem != c->rcurr) {
                        memmove(c->ritem, c->rcurr, tocopy);
                    }
                    c->ritem += tocopy;
                    c->rlbytes -= tocopy;
                    c->rcurr += tocopy;
                    c->rbytes -= tocopy;
                    if (c->rlbytes == 0) {
                        break;
                    }
                }

                /* now try reading from the socket */
                res = read(c->sfd, c->ritem, c->rlbytes);
                if (res > 0) {
                    pthread_mutex_lock(&c->thread->stats.mutex);
                    c->thread->stats.bytes_read += res;
                    pthread_mutex_unlock(&c->thread->stats.mutex);
                    if (c->rcurr == c->ritem) {
                        c->rcurr += res;
                    }
                    c->ritem += res;
                    c->rlbytes -= res;
                    break;
                }
            } else {
                res = read_into_chunked_item(c);
                if (res > 0)
                    break;
            }

            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }

            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                stop = true;
                break;
            }

            /* Memory allocation failure */
            if (res == -2) {
                out_of_memory(c, "SERVER_ERROR Out of memory during read");
                c->sbytes = c->rlbytes;
                c->write_and_go = conn_swallow;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0) {
                fprintf(stderr, "Failed to read, and not due to blocking:\n"
                            "errno: %d %s \n"
                            "rcurr=%lx ritem=%lx rbuf=%lx rlbytes=%d rsize=%d\n",
                            errno, strerror(errno),
                            (long)c->rcurr, (long)c->ritem, (long)c->rbuf,
                            (int)c->rlbytes, (int)c->rsize);
            }
            conn_set_state(c, conn_closing);
            break;

        case conn_swallow:
            /* we are reading sbytes and throwing them away */
            if (c->sbytes <= 0) {
                conn_set_state(c, conn_new_cmd);
                break;
            }

            /* first check if we have leftovers in the conn_read buffer */
            if (c->rbytes > 0) {
                int tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;
                c->sbytes -= tocopy;
                c->rcurr += tocopy;
                c->rbytes -= tocopy;
                break;
            }

            /* now try reading from the socket */
            res = read(c->sfd, c->rbuf, c->rsize > c->sbytes ? c->sbytes : c->rsize);
            if (res > 0) {
                pthread_mutex_lock(&c->thread->stats.mutex);
                c->thread->stats.bytes_read += res;
                pthread_mutex_unlock(&c->thread->stats.mutex);
                c->sbytes -= res;
                break;
            }
            if (res == 0) {
                conn_set_state(c, conn_closing);
                break;
            }
            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                stop = true;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0)
                fprintf(stderr, "Failed to read, and not due to blocking\n");
            conn_set_state(c, conn_closing);
            break;

        case conn_write:
            /*
             * We want to write out a simple response. If we haven't already,
             * assemble it into a msgbuf list (this will be a single-entry
             * list for TCP or a two-entry list for UDP).
             */
            if (c->iovused == 0 || (IS_UDP(c->transport) && c->iovused == 1)) {
                if (add_iov(c, c->wcurr, c->wbytes) != 0) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't build response\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
            }

            /* fail through... */

        case conn_mwrite:
#ifdef EXTSTORE
            /* have side IO's that must process before transmit() can run.
             * remove the connection from the worker thread and dispatch the 
             * IO queue.
             */
            if (c->io_wrapleft) {
                assert(c->io_queued == false);
                assert(c->io_wraplist != NULL);
                // TODO: create proper state for this condition
                conn_set_state(c, conn_watch);
                event_del(&c->event);
                c->io_queued = true;
                extstore_submit(c->thread->storage, &c->io_wraplist->io);
                stop = true;
                break;
            }
#endif
            if (IS_UDP(c->transport) && c->msgcurr == 0 && build_udp_headers(c) != 0) {
                if (settings.verbose > 0) {
                    fprintf(stderr, "Failed to build UDP headers\n");
                }
                conn_set_state(c, conn_closing);
                break;
            }
            switch (trasmit(c)) {
            case TRANSMIT_COMPLETE:
                if (c->state == conn_mwrite) {
                    conn_release_items(c);
                    /* XXX: I don't know why this wasn't the general case */
                    if (c->protocol == binary_prot) {
                        conn_set_state(c, c->write_and_go);
                    } else {
                        conn_set_state(c, conn_new_cmd);
                    }
                } else if (c->state == conn_write) {
                    if (c->write_and_free) {
                        free(c->write_and_free);
                        c->write_and_free = 0;
                    }
                    conn_set_state(c, c->write_and_go);
                } else {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Unexpected state %d\n", c->state);
                    conn_set_state(c, conn_closing);
                }
                break;

            case TRANSMIT_INCOMPLETE:
            case TRANSMIT_HEAD_ERROR:
                break;          // Continue in state machine

            case TRANSMIT_SOFT_ERROR:
                stop = true;
                break;
            }
            break;

        case conn_closing:
            if (IS_UDP(c->transport))
                conn_cleanup(c);
            else
                conn_close(c);
            stop = true;
            break;

        case conn_closed:
            /* This only happens if dormando is an idot. */
            abort();
            break;

        case conn_watch:
            /* We handed off our connection to the logger thread. */
            stop = true;
            break;
        case conn_max_state:
            assert(false);
            break;
        }
    }

    return;
}
