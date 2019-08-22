
int main (int argc, char **argv) {
    // 其他代码
    
#ifdef EXTSTORE
    void *storage = NULL;
    struct extstore_conf_file *storage_file = NULL;
    struct extstore_conf ext_cf;
#endif

    enum {
        // 其他相关代码
#ifdef EXTSTORE
        EXT_PAGE_SIZE,
        EXT_WBUF_SIZE,
        EXT_THREADS,
        EXT_IO_DEPTH,
        EXT_PATH,
        EXT_ITEM_SIZE,
        EXT_ITEM_AGE,
        EXT_LOW_TTL,
        EXT_RECACHE_RATE,
        EXT_COMPACT_UNDER,
        EXT_DROP_UNDER,
        EXT_MAX_FRAG,
        EXT_DROP_UNREAD,
        SLAB_AUTOMOVE_FREERATIO,
#endif
    };

    char *const subopts_tokens[] = {
        // 其他代码
#ifdef EXTSTORE
        [EXT_PAGE_SIZE] = "ext_page_size",
        [EXT_WBUF_SIZE] = "ext_wbuf_size",
        [EXT_THREADS] = "ext_threads",
        [EXT_IO_DEPTH] = "ext_io_depth",
        [EXT_PATH] = "ext_path",
        [EXT_ITEM_SIZE] = "ext_item_size",
        [EXT_ITEM_AGE] = "ext_item_age",
        [EXT_LOW_TTL] = "ext_low_ttl",
        [EXT_RECACHE_RATE] = "ext_recache_rate",
        [EXT_COMPACT_UNDER] = "ext_compact_under",
        [EXT_DROP_UNDER] = "ext_drop_under",
        [EXT_MAX_FRAG] = "ext_max_frag",
        [EXT_DROP_UNREAD] = "ext_drop_unread",
        [SLAB_AUTOMOVE_FREERATIO] = "slab_automove_freeratio",
#endif
        NULL
    };

    // 其他代码
#ifdef EXTSTORE
    settings.ext_item_size = 512;
    settings.ext_item_age = UINT_MAX;
    settings.ext_low_ttl = 0;
    settings.ext_recache_rate = 2000;
    settings.ext_max_frag = 0.8;
    settings.ext_drop_unread = false;
    settings.ext_wbuf_size = 1024 * 1024 * 4;
    settings.ext_compact_under = 0;
    settings.ext_drop_under = 0;
    settings.slab_automove_freeratio = 0.01;
    ext_cf.page_size = 1024 * 1024 * 64;
    ext_cf.wbuf_size = settings.ext_wbuf_size;
    ext_cf.io_threadcount = 1;
    ext_cf.io_depth = 1;
    ext_cf.page_buckets = 4;
    ext_cf.wbuf_count = ext_cf.page_buckets;
#endif
    
    // 其他代码
    char *shortopts =
        "a:"    // access mask for unix socket
        "A"     // enable admin shutdown command
        "Z"     // enable SSL
        "p:"    // TCP port number to listen on
        "s:"    // unix socket path to listen on
        "U:"    // UDP port number to listen on
        "m:"    // max memory to use for items in megabytes
        "M"     // return error on memory exhausted
        "c:"    // max simultaneous connections
        "k"     // lock down all paged memory
        "hiV"   // help, licence info, version
        "r"     // maximize core file limit
        "v"     // verbose
        "d"     // daemon mode
        "l:"    // interface to listen on
        "u:"    // user identity to run as
        "P:"    // save PID in file
        "f:"    // factor?
        "n:"    // minimum space allocated for key+value+flags
        "t:"    // threads
        "D:"    // prefix delimiter?
        "L"     // Large memory pages
        "R:"    // max requests per event
        "C"     // Disable use of CAS
        "b:"    // backlog queue limit
        "B:"    // Binding protocol
        "I:"    // Max item size
        "S"     // Sasl ON
        "F"     // Disable flush_all
        "X"     // Disable dump commands
        "Y:"    // Enable token auth
        "o:"    // Extended generic options
        ;

    // process arguments
#ifdef HAVE_GETOPT_LONG
    const struct option longopts[] = {
        {"unix-mask", required_argument, 0, 'a'},
        {"enable-shutdown", no_argument, 0, 'A'},
        {"enable-ssl", no_argument, 0, 'Z'},
        {"port", required_argument, 0, 'p'},
        {"unix-socket", required_argument, 0, 's'},
        {"udp-port", required_argument, 0, 'U'},
        {"memory-limit", required_argument, 0, 'm'},
        {"disable-evictions", required_argument, 0, 'M'},
        {"conn-limit", required_argument, 0, 'c'},
        {"lock-memory", no_argument, 0, 'k'},
        {"help", no_argument, 0, 'h'},
        {"license", no_argument, 0, 'i'},
        {"version", no_argument, 0, 'V'},
        {"enable-coredumps", no_argument, 0, 'r'},
        {"verbose", optional_argument, 0, 'v'},
        {"daemon", no_argument, 0, 'd'},
        {"listen", required_argument, 0, 'l'},
        {"user", required_argument, 0, 'u'},
        {"pidfile", required_argument, 0, 'P'},
        {"slab-growth-factor", required_argument, 0, 'f'},
        {"slab-min-size", required_argument, 0, 'n'},
        {"threads", required_argument, 0, 't'},
        {"enable-largepages", no_argument, 0, 'L'},
        {"max-reqs-per-event", required_argument, 0, 'R'},
        {"disable-cas", no_argument, 0, 'C'},
        {"listen-backlog", required_argument, 0, 'b'},
        {"protocol", required_argument, 0, 'B'},
        {"max-item-size", required_argument, 0, 'I'},
        {"enable-sasl", no_argument, 0, 'S'},
        {"disable-flush-all", no_argument, 0, 'F'},
        {"disable-dumping", no_argument, 0, 'X'},
        {"auth-file", required_argument, 0, 'Y'},
        {"extended", required_argument, 0, 'o'},
        {0, 0, 0, 0}
    };
    int optindex;
    while (-1 != (c = getopt_long(argc, argv, shortopts,
                        longopts, &optindex))) {
#else
    while (-1 != (c = getopt(argc, argv, shortopts))) {
#endif
        switch (c) {
        // 其他代码
            case 'o': // It's sub-opts time!
                subopts_orig = subopts = strdup(optarg); // getsubopt() changes the original args

                while (*subopts != '\0') {
                
                switch (getsubopt(&subopts, subopts_tokens, &subopts_value)) {
                // 其他代码
                
#ifdef EXTSTORE
                case EXT_PAGE_SIZE:
                    if (storage_file) {
                        fprintf(stderr, "Must specify ext_page_size before any ext_path arguments\n");
                        return 1;
                    }
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_page_size argument\n");
                        return 1;
                    }
                    if (!safe_strtoul(subopts_value, &ext_cf.page_size)) {
                        fprintf(stderr, "could not parse argument to ext_page_size\n");
                        return 1;
                    }
                    ext_cf.page_size *= 1024 * 1024;    // megabytes
                    break;
                case EXT_WBUF_SIZE:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_wbuf_size argument\n");
                        return 1;
                    }
                    if (!safe_strtoul(subopts_value, &ext_cf.wbuf_size)) {
                        fprintf(stderr, "could not parse argument to ext_wbuf_size\n");
                        return 1;
                    }
                    ext_cf.wbuf_size *= 1024 * 1024;    // emgabytes
                    settings.ext_wbuf_size = ext_cf.wbuf_size;
                    break;
                case EXT_THREADS:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_threads argument\n");
                        return 1;
                    }
                    if (!safe_strtoul(subopts_value, &ext_cf.io_threadcount)) {
                        fprintf(stderr, "could not parse argument to ext_threads\n");
                        return 1;
                    }
                    break;
                case EXT_IO_DEPTH:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_io_depth arguemnt\n");
                        return 1;
                    }
                    if (!safe_strtoul(subopts_value, &ext_cf.io_depth)) {
                        fprintf(stderr, "could not parse argument to ext_io_depth\n");
                        return 1;
                    }
                    break;
                case EXT_ITEM_SIZE:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_item_size argument\n");
                        return 1;
                    }
                    if (!safe_strtoul(subopts_value, &settings.ext_item_size)) {
                        fprintf(stderr, "could not parse argument to ext_item_size\n");
                        return 1;
                    }
                    break;
                case EXT_ITEM_AGE:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_item_age argument\n");
                        return 1;
                    }
                    if (!safe_strtoul(subopts_value, &settings.ext_item_age)) {
                        fprintf(stderr, "could not parse argument to ext_item_age\n");
                        return 1;
                    }
                    break;
                case EXT_LOW_TTL:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_low_ttl argument\n");
                        return 1;
                    }
                    if (!safe_strtoul(subopts_value, &settings.ext_low_ttl)) {
                        fprintf(stderr, "could not parse argument to ext_low_ttl\n");
                        return 1;
                    }
                    break;
                case EXT_RECACHE_RATE:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_recache_rate argument\n");
                        return 1;
                    }
                    if (!safe_strtoul(subopts_value, &settings.ext_recache_rate)) {
                        fprintf(stderr, "could not parse argument to ext_recache_rate\n");
                        return 1;
                    }
                    break;
                case EXT_COMPACT_UNDER:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_compact_under argument\n");
                        return 1;
                    }
                    if (!safe_strtoul(subopts_value, &settings.ext_compact_under)) {
                        fprintf(stderr, "could not parse argument to ext_compact_under\n");
                        return 1;
                    }
                    break;
                case EXT_DROP_UNDER:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_drop_under argument\n");
                        return 1;
                    }
                    if (!safe_strtoul(subopts_value, &settings.ext_drop_under)) {
                        fprintf(stderr, "could not parse argument to ext_drop_under\n");
                        return 1;
                    }
                    break;
                case EXT_MAX_FRAG:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing ext_max_frag argument\n");
                        return 1;
                    }
                    if (!safe_strtod(subopts_value, &settings.ext_max_frag)) {
                        fprintf(stderr, "could not parse argument to ext_max_frag\n");
                        return 1;
                    }
                    break;
                case SLAB_AUTOMOVE_FREERATIO:
                    if (subopts_value == NULL) {
                        fprintf(stderr, "Missing slab_automove_freeratio argument\n");
                        return 1;
                    }
                    if (!safe_strtod(subopts_value, &settings.slab_automove_freeratio)) {
                        fprintf(stderr, "could not parse argument to slab_automove_freeratio\n");
                        return 1;
                    }
                    break;
                case EXT_DROP_UNREAD:
                    settings.ext_drop_unread = true;
                    break;
                case EXT_PATH:
                    if (subopts_value) {
                        struct extstore_conf_file *tmp = storage_conf_parse(subopts_value, ext_cf.page_size);
                        if (tmp == NULL) {
                            fprintf(stderr, "failed to parse ext_path argument\n");
                            return 1;
                        }
                        if (storage_file != NULL) {
                            tmp->next = storage_file;
                        }
                        storage_file = tmp;
                    } else {
                        fprintf(stderr, "missing argument to ext_path, ie: ext_path=/d/file:5G\n");
                        return 1;
                    }
                    break;
#endif
                }

                }
        }

    // 其他代码
#ifdef EXTSTORE
    if (storage_file) {
        if (settings.item_size_max > ext_cf.wbuf_size) {
            fprintf(stderr, "-I (item_size_max: %d) cannot be larger than ext_wbuf_size: %d\n",
                settings.item_size_max, ext_cf.wbuf_size);
            exit(EX_USAGE);
        }

        if (settings.udpport) {
            fprintf(stderr, "Cannot use UDP with extstore enabled (-U 0 to disable)\n");
            exit(EX_USAGE);
        }
    }
#endif

    // 其他代码
#ifdef EXTSTORE
    if (storage_file) {
        enum extstore_res eres;
        if (settings.ext_compact_under == 0) {
            settings.ext_compact_under = storage_file->page_count / 4;
            /* Only rescues non-COLD items if below this threshold */
            settings.ext_drop_under = storage_file->page_count / 4;
        }
        crc32_init();
        /* Init free chunks to zero. */
        for (int x = 0; x < MAX_NUMBER_OF_SLAB_CLASSES; x++) {
            settings.ext_free_memchunks[x] = 0;
        }
        storage = extstore_init(storage_file, &ext_cf, &eres);
        if (storage == NULL) {
            fprintf(stderr, "Failed to initialize external storage: %s\n",
                    extstore_err(eres));
            if (eres == EXTSTORE_INIT_OPEN_FAIL) {
                perror("extstore open");
            }
            exit(EXIT_FAILURE);
        }
        ext_storage = storage;
        // page mover algorithm for extstore needs memory prefilled
        slabs_prefill_global();
    }
#endif

    // 其他代码
#ifdef EXTSTORE
    slabs_set_storage(storage);
    memcached_thread_init(settings.num_threads, storage);
    init_lru_crawler(storage);
#else
    memcached_thread_init(settings.num_threads, NULL);
    init_lru_crawler(NULL);
#endif

    // 其他代码
    
#ifdef EXTSTORE
    if (storage && start_storage_compact_thread(storage) != 0) {
        fprintf(stderr, "Failed to start storage compaction thread\n");
        exit(EXIT_FAILURE);
    }
    if (storage && start_storage_write_thread(storage) != 0) {
        fprintf(stderr, "Failed to start storage writer thread\n");
        exit(EXIT_FAILURE);
    }

    if (start_lru_maintainer && start_lru_maintainer_thread(storage) != 0) {
#else
    if (start_lru_maintainer && start_lru_maintainer_thread(NULL) != 0) {
#endif
        fprintf(stderr, "Failed to enable LRU maintainer thread\n");
        return 1;
    }

    // 其他代码
}
