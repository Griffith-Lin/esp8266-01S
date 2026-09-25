/* WiFi STA（客户端）连接模块

   把"连上一个热点"这件事整个封在这里。外面只看得见这几个函数：

     wifi_sta_init()                 初始化 + 开始连接（异步，不阻塞）
     wifi_sta_wait_ip(ms)            阻塞等拿到 IP —— 这才是"真的连上了"
     wifi_sta_set_credentials(s,p)   换一个热点，存 NVS 并立刻重连
     wifi_sta_get_ssid()             当前用的热点名
     wifi_sta_start_smartconfig()    手动进配网模式

   ── 热点名和密码从哪来 ──────────────────────────────────────────

   三层，从上往下找：

     ① NVS（命名空间 "app_cfg"）—— 运行时改过就用这个，掉电不丢
     ② 本文件顶部的 WIFI_SSID_DEFAULT / WIFI_PASSWORD_DEFAULT
        —— 编译进固件里的"出厂默认值"
     ③ 都没有 → 用 ②，并顺手写进 ①

   为什么要这么麻烦？因为热点名/密码是【会变的环境参数】，
   不该焊死在固件里。焊死的后果是：主节点换个密码，
   这个节点就连不上了，只能把板子拆下来插串口线重烧。

   ⚠ 顺带说清楚一件事：这件事【跟 OTA 无关】。
     OTA 是"换固件"，改密码是"改配置"，两件不同的事。
     而且 OTA 走网络 —— 密码改了、连不上了，OTA 也够不着它。
     真正能救"已经连不上"的节点的，是下面的 SmartConfig。 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* 初始化并开始连接。函数返回时连接还在后台进行，不会阻塞。
   连接过程与结果通过串口日志输出。

   内部会再起一条管理任务：连不上超过 60 秒就自动进 SmartConfig 配网。 */
void wifi_sta_init(void);

/* 阻塞等待，直到拿到 IP（DHCP 分配成功，也就是真的能通信了）。

   timeout_ms 传 UINT32_MAX 表示无限等待。
   返回 true = 已拿到 IP；false = 等超时了。 */
bool wifi_sta_wait_ip(uint32_t timeout_ms);

/* 换一个热点：写进 NVS（掉电不丢）+ 立刻用新凭据重连。

   ssid 最长 32 字节，password 最长 64 字节（802.11 协议的规定），
   密码可以为空串（开放热点）。
   返回 false 表示参数不合法或 NVS 写入失败 —— 此时【不会】动当前连接。 */
bool wifi_sta_set_credentials(const char *ssid, const char *password);

/* 当前正在用的热点名。返回的是模块内部的缓冲区，别 free，别改。 */
const char *wifi_sta_get_ssid(void);

/* 手动进一次 SmartConfig 配网（平时用不着，自动那条路在 wifi_sta_init 里）。
   非阻塞：启动完就返回，结果通过串口日志和后续连接体现。 */
void wifi_sta_start_smartconfig(void);
