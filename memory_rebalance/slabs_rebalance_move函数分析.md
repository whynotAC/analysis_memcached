slabs\_rebalance\_move函数
================================
1 函数的流程
-------------------------------
函数的整个流程如下:

* 获取`item`的`it_flags`标记，从而设置`status`。
* 根据`status`的不同从而采取不同的策略，来转移`item`。
* 根据`slab_rebal`的`slab_pos`来判断是否清空了整个`slab`。
* 如果`slab`中存在未完成的`item`，则从`slab`的头部重新进行扫描。
* 如果整个`slab`清空，设置`slab_rebal`的`done`字段，然后函数退出。

2 函数的使用的全局变量
--------------------------------

| 变量名 	| 定义 		| 设置			| 备注 	  |
| ---- 	| ----- 	| ----- 		| ----   |
| `slab_bulk_check` | `int slab_bulk_check = DEFAULT_BULK_CHECK` | 此变量可以通过设置环境变量`MEMCACHED_SLAB_BULK_CHECK`来设置其大小 | `DEFAULT_BULK_CHECK`值为1，默认每次只转移`slab`中的一个`item` |

3 函数的定义
---------------------------------

	

