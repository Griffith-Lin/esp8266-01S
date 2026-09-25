/* WiFi STA（客户端）连接模块

   把"连上一个热点"这件事整个封在这里。外面只看得见两个函数：

     wifi_sta_init()          初始化 + 开始连接（异步，不阻塞）
     wifi_sta_wait_ip(ms)     阻塞等拿到 IP —— 这才是"真的连上了"

   热点名和密码在本文件顶部。连接失败后的重试、掉线重连、串口日志，
   全部在模块内部处理，调用方不需要知道 WiFi 的任何细节。 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 初始化并开始连接。函数返回时连接还在后台进行，不会阻塞。
   连接过程与结果通过串口日志输出。 */
void wifi_sta_init(void);

/* 阻塞等待，直到拿到 IP（DHCP 分配成功，也就是真的能通信了）。

   timeout_ms 传 UINT32_MAX 表示无限等待。
   返回 true = 已拿到 IP；false = 等超时了。 */
bool wifi_sta_wait_ip(uint32_t timeout_ms);
