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
