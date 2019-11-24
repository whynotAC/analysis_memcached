# memcached网络通信模型

通过[网络简介](/network/网络简介)这篇文章，您已经知道了大多数网络模型的设计架构。本文将在前文的基础上，描述memcached使用的网络通信模型及其使用的`Libevent`框架的相关部分。

## 1. 网络通信框架模型
在网络层的概述中描述过memcached使用的多线程Reactor网络模型，本文通过更具体的图示来描述memcached的网络通信架构。

![网络通信架构模型](/images/memcached网络通信模型.png)

通过上图可以看出，`memcached`网络模型中有多个`Reactor`管理器，每个网络通信子线程负责自己`Reactor`管理器上的文件描述符以及其对应的事件处理，主线程负责维护自己的`Reactor`管理器，将新来的`connection`链接分配给网络通信子线程去处理。

## 2. Libevent相关

memcached中使用`Libevent`实现网络事件(链接请求、读和写)的处理和定时器。其使用Libevent中的函数如下表所示:

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
