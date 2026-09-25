/* WiFi STA（客户端）连接模块

   把"连上一个热点"这件事整个封在这里。外面只看得见这几个函数：

     wifi_sta_init()                 初始化 + 开始连接（异步，不阻塞）
     wifi_sta_wait_ip(ms)            阻塞等拿到 IP —— 这才是"真的连上了"
     wifi_sta_set_credentials(s,p)   换一个热点，存 NVS 并立刻重连
     wifi_sta_get_ssid()             当前用的热点名

   ── 热点名和密码从哪来 ──────────────────────────────────────────

   三层，从上往下找：

     ① NVS（命名空间 "app_cfg"）—— 运行时改过就用这个，掉电不丢
     ② 本文件顶部的 WIFI_SSID_DEFAULT / WIFI_PASSWORD_DEFAULT
        —— 编译进固件里的"出厂默认值"
     ③ 都没有 → 用 ②，并顺手写进 ①

   为什么要这么麻烦？因为热点名/密码是【会变的环境参数】，
   不该焊死在固件里。焊死的后果是：主节点换个密码，
   这个节点就连不上了，只能把板子拆下来插串口线重烧。

   ⚠ 上面这句话是真的，不是吓唬人：本模块【没有】任何配网兜底。
     以前还有一条 SmartConfig 的路（手机 App 把密码编成广播包发出来，
     节点在空口上收），已经整个拆掉，理由和实测数据见 README §9.7 ——
     它的链路太长（手机 WiFi 驱动的广播行为 → 路由器 → ESP 的混杂模式），
     任何一环不配合就死，而且【没有反馈】，串口上什么都看不出来。

     所以这里的"分层"解决的是"不用重烧就能改"，
     但【前提是节点还连得上】。一旦失联，就只剩重烧一条路。
     改主节点凭据的顺序必须是：① 先发 TCP 命令 ② 再改主节点。

   ⚠ 顺带说清楚一件事：这件事【跟 OTA 无关】。
     OTA 是"换固件"，改密码是"改配置"，两件不同的事。 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* 初始化并开始连接。函数返回时连接还在后台进行，不会阻塞。
   连接过程与结果通过串口日志输出。

   内部会再起一条管理任务：没连上时每隔 30 秒在串口上报一次状态，
   提示该去查 SSID 拼写、密码、以及热点是不是只在 5GHz 上。
   它【只报不修】—— 重连本身靠 wifi_event_handler()，那是一条事件驱动的
   快路径，不需要定时器。 */
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
