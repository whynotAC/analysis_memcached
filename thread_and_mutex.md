memcached中的线程和锁
=====================================

线程
---------------------------------------
| 文件名	|	线程函数|	作用	| 备注		|
| 	----	|	----	|	----	|	-----	|
| cralwer.c	|	item\_crawler\_thread|	| |
| slabs.c	| slab\_rebalance\_thread | | |
| thread.c | worker\_libevent | 创建工作线程 | |
| items.c | lru\_maintainer\_thread | 创建lru管理线程 | |
| storage.c | storage\_compact\_thread |  | |
| logger.c | logger\_thread | | |
| extstore.c | extstore\_io\_thread | |
| extstore.c | extstore\_maint\_thread | |
| memcached.c | conn\_timeout\_thread | |
| assoc.c | assoc\_maintenance\_thread | hash表自动扩展线程 | |

锁
----------------------------------------
| 文件名 | 变量名 | 作用  | 备注  |
| ---	| ---	| --- | --- |
| assoc.c | maintenance\_lock | 用于hash表扩展线程 | |
| crawler.c | lru\_crawler\_lock | | |
| crawler.h | lock	|	| 在结构体crawler\_expired\_data中 |
| extstore.c | mutex |  | 在结构体\_store\_page |
| extsotre.c | mutex |  | 在结构体store\_io\_thread |
| extstore.c | mutex |  | 在结构体store\_maint\_thread |
| extstore.c | mutex |  | 在结构体store\_engine |
| extstore.c | stats\_mutex | | 在结构体store\_engine |
| items.c | lru\_maintainer\_lock |  | |
| items.c | cas\_id\_lock | | |
| items.c | stats\_sizes\_lock | | |
| items.c | mutex | | 在结构体lru\_bump\_buf |
| items.c | bump\_buf\_lock |  |  |
| logger.c | logger\_stack\_lock |  |  |
| logger.c | logger\_atomics\_mutex |  |  |
| logger.h | mutex | | 在结构体logger |
| memcached.h | mutex | | 在结构体thread\_stats |
| slabs.c | slabs\_lock | 用于slabclass访问锁 | |
| slabs.c | slabs\_rebalance\_lock |  | |
| storage.c | storage\_compact\_plock |  | |
| storage.c | lock |  | 在结构体storage\_compact\_wrap |
| thread.c | lock |  | 在结构体conn\_queue |
| thread.c | lru\_locks | 用于lru队列 |  |
| thread.c | conn\_lock |  |  |
| thread.c | atomics\_mutex | | |
| thread.c | stats\_lock | | |
| thread.c | worker\_hang\_lock |  |  |
| thread.c | cqi\_freelist\_lock |  |  |
| thread.c | item\_locks | 用于hash表锁 |  |
| thread.c | init\_lock |  |  |

通过线程和锁角度来解析一个完整的项目,后续会继续更新这里的表中内容。