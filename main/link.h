/**
  * @file    link.h
  * @brief   链路选择 —— 当前走 TCP 还是 UDP
  *
  * tcp_client 和 udp_client 是【二选一】的，同一时刻只该有一个在收。
  * 这个模块就是那个"现在走哪条"的唯一答案，外加切换动作。
  *
  * 所有回执都要经过 link_send()，它按当前链路选路 —— 业务层不需要知道
  * 自己是在哪条链路上被调起来的。
  *
  * @warning 同一时刻【只有一条】链路在收。两条都开着的话，同一条命令会从
  *          两条路各到一次，"开灯"被执行两次、"关灯"也是。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §5.5（链路和命令之间的三道闸，
  *          这里是第一道）
  */

#pragma once

/**
  * @brief 走哪条链路
  */
typedef enum {
    LINK_TCP,   ///< 走 tcp_client.c
    LINK_UDP,   ///< 走 udp_client.c
} link_mode_t;

/**
  * @brief 设定初始链路，并把对应的传输模块启动起来
  *
  * @warning 必须在两个模块的 set_rx_handler() 【之后】调用 ——
  *          它会立刻 start，而注册晚了第一段数据会因为回调还是 NULL
  *          而被悄悄丢掉。
  *
  * @note  这是"开机默认走哪条"的【唯一】一处。想换成 UDP，
  *        只改调用方传进来的这个参数即可，传输模块那边不用动。
  */
void link_init(link_mode_t initial);

/**
  * @brief 当前走的是哪条链路
  *
  * @return 当前链路
  *
  * @note   给接收回调判断"这包数据是不是从已经停用的链路来的"用的 ——
  *         两个模块的 stop() 都只置个标志，任务要等下一次超时（最多 5 秒）
  *         才真正断开，这段窗口里旧链路还在收。
  */
link_mode_t link_current(void);

/**
  * @brief    按当前链路发一段字节
  *
  * @param[in] data  要发的数据
  * @param[in] len   字节数；传 0 表示"data 是 C 字符串，自己算长度"
  *
  * @return   实际发出的字节数；失败返回 -1
  *
  * @warning  所有回执一律调它，【不要直接调 tcp_client_send()】。
  *           漏掉哪一处，那条回执在 UDP 模式下就会往 TCP 发 ——
  *           命令从 UDP 进来了，回复却跑去了另一条路，
  *           发送方会觉得"命令生效了但没回音"。
  */
int link_send(const char *data, int len);

/**
  * @brief    切换链路，并替调用方发掉那条回执
  *
  * @param[in] mode  要切到哪条
  *
  * @note     回执由本函数【自己】发，调用方不要再补一句 link_send() ——
  *           补了会发两遍。
  *
  * @note     【顺序是有意的】：先用【旧】链路把回执发出去，发完才切。
  *           反过来的话，这条回执会走新链路，而发命令的人正在旧链路上
  *           等着看结果 —— 永远等不到。现象就是"发了 net udp 之后就
  *           再没动静了"，很容易误判成板子死了。
  *
  * @warning  切换【不是瞬时】的：新链路那条任务要等它自己那一轮空转醒来
  *           （最多 500ms）才会建好 socket 并报到。所以切到 UDP 之后，
  *           主节点最快也要过半秒左右才发得进来。
  *
  * @warning  会调用两个传输模块的 stop()。那两个 stop() 都只置标志就返回，
  *           【不能】改成"等对方任务退出"：本函数是顺着接收回调被调用的，
  *           也就是跑在传输任务【自己】的栈上，等它退出就是在等自己，直接死锁。
  */
void link_switch(link_mode_t mode);
