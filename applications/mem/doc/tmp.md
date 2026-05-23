# varmem.c 不定长分配流程图示（拆解版）

> 对应源码：`applications/mem/varmem.c`
> 重点函数：`varmem_alloc_block()`

## 1. 分配主流程（决策图）

```mermaid
flowchart TD
    A[调用 varmem_alloc_block(size)] --> B{size==0 或 未 init?}
    B -- 是 --> R0[返回 RT_NULL]
    B -- 否 --> C[size 按 RT_ALIGN 对齐]
    C --> D{size < VARMEM_MIN_SIZE?}
    D -- 是 --> E[size = VARMEM_MIN_SIZE]
    D -- 否 --> F
    E --> F{size > mem_size?}
    F -- 是 --> R1[返回 RT_NULL]
    F -- 否 --> G[从 offset=0 开始扫描链表]

    G --> H[取 item 和 block_size]
    H --> I{item->used 或 block_size < size?}
    I -- 是 --> J[offset = item->next]
    J --> K{offset <= mem_size-size?}
    K -- 是 --> G
    K -- 否 --> R2[返回 RT_NULL]

    I -- 否 --> L{block_size >= size + ITEM_SIZE + MIN_SIZE?}
    L -- 是 --> M[切分: 生成 split 空闲块]
    M --> N[used_size += size + ITEM_SIZE]
    L -- 否 --> O[整块分配]
    O --> P[used_size += item->next-offset]
    N --> Q[item->used = 1]
    P --> Q
    Q --> S[更新 max_used_size]
    S --> T[返回 item + ITEM_SIZE]
```

## 2. first-fit 扫描路径（无 lfree）

`varmem.c` 的扫描起点固定是 `heap_ptr`（`offset=0`），不是最低空闲块。

```text
heap 头
  |
  v
[blk0] -> [blk1] -> [blk2] -> ... -> [heap_end]
  ^        ^        ^
  |        |        |
检查 used? / size?，找到“第一个可用块”就停止扫描
```

扫描推进语句（源码同款）：

```c
offset = ((varmem_item_t *)&g_varmem.heap_ptr[offset])->next;
```

## 3. 命中块后的两条分支

### 3.1 可切分分支

触发条件：

```c
block_size >= size + VARMEM_ITEM_SIZE + VARMEM_MIN_SIZE
```

含义：当前空闲块在满足本次请求后，剩余空间还能形成“合法新块（头+最小用户区）”。

```text
分配前
[ item(FREE, block_size) ]

分配后
[ item(USED, size) ][ split(FREE, remain) ]
```

链表更新点：

- `split->next = item->next`
- `split->prev = offset`
- `item->next = split_offset`
- `varmem_next_item(split)->prev = split_offset`

统计更新：

```text
used_size += size + VARMEM_ITEM_SIZE
```

### 3.2 不可切分分支（整块吃掉）

条件不满足时，剩余空间太小，不能形成合法新块。

```text
分配前
[ item(FREE, block_size) ]

分配后
[ item(USED, block_size) ]
```

统计更新：

```text
used_size += item->next - offset
```

## 4. 块大小与地址关系图

```text
item 地址 = heap_ptr + offset
用户地址 = item + VARMEM_ITEM_SIZE

item_user_size = item->next - offset - VARMEM_ITEM_SIZE
```

布局：

```text
+------------------------+----------------------+
| varmem_item_t header   | user payload         |
+------------------------+----------------------+
^                        ^
item                     返回给用户的指针
```

## 5. 初始化后的初始可分配形态

`varmem_init_heap()` 建立如下结构：

```text
[first header(FREE)] [free user area: mem_size] [heap_end header(USED)]
```

也就是说：第一次分配一定从 `first` 这一个大 FREE 块开始切分。

## 6. 一组直观状态演进（对应 demo）

`varmem demo` 里是：`alloc 16 -> alloc 32 -> alloc 64 -> alloc 16 -> free p2 -> free p4 -> free p3`

可抽象成：

```text
init:
[FREE big] [END]

alloc 16:
[USED16][FREE...][END]

alloc 32:
[USED16][USED32][FREE...][END]

alloc 64:
[USED16][USED32][USED64][FREE...][END]

free p2:
[USED16][FREE32][USED64][FREE...][END]   <- 中间洞，暂不能合并

free p4:
[USED16][FREE32][USED64][FREE...merged][END]

free p3:
[USED16][FREE32+64+tail merged][END]
```
