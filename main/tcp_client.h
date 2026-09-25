/* TCP 客户端模块（纯传输层）

   为什么必须是"客户端"？
   TCP 和 UDP 在这里不一样：UDP 没有连接的概念，谁都能随时往谁那儿丢数据；
   而 TCP 必须先建立连接，并且【只能由客户端发起 connect()】。
   服务端只能被动等（listen → accept），在有人连上来之前，
   它连"发送对象"都不存在 —— 网络调试助手会显示"当前没有连接对象"。

   这个模块只管"把字节收进来 / 发出去"，不知道"开灯"是什么意思。
   收到数据后交给注册进来的回调去解释。服务端地址和端口在 tcp_client.c 顶部。 */

#pragma once

/* 收到数据的回调。

   ⚠ 这里的 data 是【裸字节流】，不保证是一条完整消息 ——
     TCP 是字节流不是消息队列，你点一次"发送"，这边 recv() 可能：
       一次收到完整的 "开灯"
       分两次收到 "开" 和 "灯"
       一次收到 "开灯关灯"（连点了两次）
     所以回调里不能拿 data 直接去 strcmp，必须先攒起来再找关键字。
     具体做法见 main.c 里的 cmd_on_rx()。 */
typedef void (*tcp_rx_handler_t)(const char *data, int len);

/* 注册收到数据的回调。必须在 tcp_client_start() 之前调用。 */
void tcp_client_set_rx_handler(tcp_rx_handler_t handler);

/* 启动 TCP 客户端任务。
   它内部会先等 WiFi 拿到 IP，再去连服务端，所以可以放心地在
   wifi_sta_init() 之后立刻调用。 */
void tcp_client_start(void);

/* 往服务端发数据。len 传 0 表示自动按字符串长度算。
   返回实际发出的字节数；返回 -1 表示当前没连上（数据被丢弃）。 */
int tcp_client_send(const char *data, int len);
