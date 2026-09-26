/**
  * @file    link.c
  * @brief   链路选择 —— 当前走 TCP 还是 UDP
  *
  * 对外接口和所有"不能这么改"的告警见 link.h。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §5.5
  */

#include "tcp_client.h"
#include "udp_client.h"

#include "link.h"

/**
  * @brief 当前链路
  *
  * @note  全工程只有这一个"现在走哪条路"的答案。想改它只有两条路：
  *        link_init() 定初值，link_switch() 运行期换 —— 别的地方不要碰。
  */
static link_mode_t s_link = LINK_TCP;

void link_init(link_mode_t initial)
{
    s_link = initial;

    /* 顺手把对应的那条启动起来，这样"开机默认走哪条"就只有这一处。
       两个 start() 都是幂等的（模块内部有任务已创建标志），重复调用无副作用。 */
    if (initial == LINK_UDP) {
        udp_client_start();
    } else {
        tcp_client_start();
    }
}

link_mode_t link_current(void)
{
    return s_link;
}

int link_send(const char *data, int len)
{
    return (s_link == LINK_UDP) ? udp_client_send(data, len)
                                : tcp_client_send(data, len);
}

void link_switch(link_mode_t mode)
{
    if (s_link == mode) {
        link_send(mode == LINK_UDP ? "NET already UDP\r\n"
                                   : "NET already TCP\r\n", 0);
        return;
    }

    /* ① 先回执。此刻 s_link 还是旧值，所以走的是旧链路 —— 正是我们要的。 */
    link_send(mode == LINK_UDP ? "NET -> UDP\r\n" : "NET -> TCP\r\n", 0);

    /* ② 再换向，然后停一条、起另一条。 */
    s_link = mode;

    if (mode == LINK_UDP) {
        tcp_client_stop();
        udp_client_start();
    } else {
        udp_client_stop();
        tcp_client_start();
    }
}
