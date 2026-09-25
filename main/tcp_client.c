/* TCP 客户端模块 —— 接口说明见 tcp_client.h */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* BSD socket 风格的那套 API（socket / connect / recv / send ...） */
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi_sta.h"
#include "tcp_client.h"

/* 服务端（跑在 ESP32 主节点上的 TCP Server）的地址和端口。

   192.168.4.1 是 ESP32 开 SoftAP 时的【固定地址】：
   它不是 DHCP 分的，是 tcpip_adapter_init() 里写死的常量
   （components/tcpip_adapter/tcpip_adapter_lwip.c:207-209，
    ip / gw 都赋成 192.168.4.1，掩码 255.255.255.0）。

   所以这一行可以放心硬编码，而且和"网关"永远是同一个地址 ——
   串口启动日志里「网关」那一行应该打印出 192.168.4.1。

   ⚠ 这正是当初选 SoftAP 方案、而不是"两个都连路由器"的理由：
     走路由器的话主节点的地址是 DHCP 分的，租约一换这一行就失效，
     还得回头去补静态 IP 或 mDNS。

   ⚠ 端口 8086 必须和 ESP32 那边 TCP Server 监听的端口一致。 */
#define TCP_SERVER_IP    "192.168.4.1"
#define TCP_SERVER_PORT  8086

#define TCP_RX_BUF_SIZE     128
#define TCP_RECV_TIMEOUT_S  5

/* 当前连接的 socket。-1 表示没连上。
   存成全局的是为了让 tcp_client_send() 能在任务外面被调用。 */
static int s_sock = -1;

/* 收到数据时往哪儿送。由 tcp_client_set_rx_handler() 填。 */
static tcp_rx_handler_t s_rx_handler = NULL;

void tcp_client_set_rx_handler(tcp_rx_handler_t handler)
{
    s_rx_handler = handler;
}

int tcp_client_send(const char *data, int len)
{
    if (s_sock < 0) {
        printf("[tcp] 还没连上服务端，%d 字节被丢弃\n", len ? len : (int)strlen(data));
        return -1;
    }
    if (len == 0) {
        len = strlen(data);
    }
    return send(s_sock, data, len, 0);
}

static void tcp_client_task(void *pvParameters)
{
    char rxbuf[TCP_RX_BUF_SIZE];

    /* 先等 WiFi 真的拿到 IP。没有 IP 的时候 connect() 必然失败，
       与其盲目重试，不如在这里阻塞等待，拿到 IP 再开工。
       UINT32_MAX = 一直等。 */
    if (!wifi_sta_wait_ip(UINT32_MAX)) {
        printf("[tcp] 等 IP 超时，放弃\n");
        vTaskDelete(NULL);
        return;
    }
    printf("[tcp] 已获得 IP，准备连接 %s:%d\n", TCP_SERVER_IP, TCP_SERVER_PORT);

    while (1) {
        struct sockaddr_in dest;

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            printf("[tcp] 创建 socket 失败: errno=%d\n", errno);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        /* 给 recv() 设一个接收超时。没有它的话，连接半死不活的时候 recv()
           会永远阻塞，这个任务就再也醒不过来了。超时不算错误，
           下面靠 errno == EWOULDBLOCK 把它和真错误区分开。 */
        struct timeval tv = { .tv_sec = TCP_RECV_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* sockaddr_in 里的东西都是"网络字节序"，所以端口要用 htons() 转。
           sin_addr 用 inet_addr() 把点分十进制字符串直接转成 32 位整数。 */
        memset(&dest, 0, sizeof(dest));
        dest.sin_family      = AF_INET;
        dest.sin_port        = htons(TCP_SERVER_PORT);
        dest.sin_addr.s_addr = inet_addr(TCP_SERVER_IP);

        if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
            printf("[tcp] 连接 %s:%d 失败 errno=%d，2 秒后重试\n",
                   TCP_SERVER_IP, TCP_SERVER_PORT, errno);
            close(sock);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        printf("[tcp] 已连接上 %s:%d\n", TCP_SERVER_IP, TCP_SERVER_PORT);

        /* 对上层公开，tcp_client_send() 从这里拿到 fd */
        s_sock = sock;

        /* 先打个招呼：让调试助手那边立刻看到东西，
           一眼就能确认这条链路是双向通的。 */
        tcp_client_send("hello from ESP-01S\r\n", 0);

        /* 收数据循环 */
        while (1) {
            int len = recv(sock, rxbuf, sizeof(rxbuf) - 1, 0);

            if (len > 0) {
                /* recv 交出来的是裸字节流，不是 C 字符串。
                   这里【不做任何解释】，原样交给业务层的回调，
                   由它决定这一堆字节是什么意思。 */
                if (s_rx_handler != NULL) {
                    s_rx_handler(rxbuf, len);
                }

            } else if (len == 0) {
                /* recv 返回 0 只有一个含义：对端主动关掉了连接 */
                printf("[tcp] 对端关闭了连接\n");
                break;

            } else {
                /* len < 0：可能只是超时（这 5 秒没数据），也可能是真出错 */
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    continue;
                }
                printf("[tcp] 接收出错 errno=%d\n", errno);
                break;
            }
        }

        s_sock = -1;
        close(sock);
        printf("[tcp] 连接结束，1 秒后重连\n");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void tcp_client_start(void)
{
    /* 参数：入口函数、任务名、栈大小(字节)、传参、优先级、句柄。
       任务一进去就卡在 wifi_sta_wait_ip() 上等 IP，拿到 IP 之前不占 CPU。 */
    xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
}
