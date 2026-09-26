/**
  * @file    tcp_client.c
  * @brief   TCP 客户端模块 —— 连服务端、收发字节
  *
  * @note    这个模块是【纯传输层】：它只负责把字节搬过去、搬回来，
  *          完全不知道"开灯"是什么。业务在 main.c 里，通过
  *          tcp_client_set_rx_handler() 注册的回调接进去。
  *
  * @note    接口说明见 tcp_client.h。
  */

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

/**
  * @brief 服务端（跑在主节点上的 TCP Server）的地址
  *
  * @note  【两套值，用哪套就放开哪套】。切这一行的时候，必须连 wifi_sta.c
  *        顶部的 SSID / 密码【一起切】—— 那边管"连哪个网"，这边管"连网里的谁"。
  *        只切一半的现象：WiFi 连上了（串口打印出 GOT_IP），TCP 却连不上，
  *        一直刷 errno=113。那【不是】"对方服务器没开"：113 是 EHOSTUNREACH，
  *        包在路由那一步就没出去（lwip/errno.h:162 的注释就是 No route to host）。
  *
  * @note  两个地址【都是编译期常量】，各自有各自的理由：
  *
  *          192.168.4.1   —— ESP32 开 SoftAP 时它自己的地址，不是 DHCP 分的，
  *                           tcpip_adapter_init() 里无条件赋值
  *                           （components/tcpip_adapter/tcpip_adapter_lwip.c:207-209，
  *                           ip / gw 都赋成 192.168.4.1，掩码 255.255.255.0）。
  *                           所以它和"网关"永远是同一个地址 —— 串口启动日志里
  *                           「网关」那一行应该打印出 192.168.4.1。
  *
  *          192.168.137.1 —— Windows 移动热点（ICS）给【这台电脑自己】的地址，
  *                           同样是固定的，本机 ipconfig 实测：
  *                             "WLAN 3" 适配器  IPv4 192.168.137.1 / 255.255.255.0
  *                           它没有默认网关 —— 因为它自己就是网关。
  *                           所以在"主节点地址是死的"这一点上，拿电脑当主节点
  *                           和拿 ESP32 当主节点是同一回事，这行照样能硬编码。
  *
  * @warning 这正是当初选 SoftAP 方案、而不是"两个都连路由器"的理由：
  *          走路由器的话主节点的地址是 DHCP 分的，租约一换这一行就失效，
  *          还得回头去补静态 IP 或 mDNS。
  *
  * @warning 端口也要一起对：用电脑当主节点时，网络调试助手要监听 8086。
  *
  * @see     README §9.2（拓扑与四值表）、§9.2 最后一节「台面调试」
  */

/* 台面调试：电脑开移动热点 + 网络调试助手当服务器（当前生效） */
#define TCP_SERVER_IP    "192.168.137.1"

/* 正式：ESP32 主节点开 SoftAP */
/* #define TCP_SERVER_IP    "192.168.4.1" */

/**
  * @brief 服务端监听的端口
  *
  * @warning 必须和 ESP32 那边 TCP Server 监听的端口一致。
  */
#define TCP_SERVER_PORT  8086

/** @brief 接收缓冲区大小（字节）。一次 recv 最多收这么多。 */
#define TCP_RX_BUF_SIZE     128

/**
  * @brief recv() 的接收超时（秒）
  *
  * @note  没有它的话，连接半死不活的时候 recv() 会永远阻塞，
  *        这个任务就再也醒不过来了。
  */
#define TCP_RECV_TIMEOUT_S  5

/**
  * @brief    当前连接的 socket，-1 表示没连上
  *
  * @note     存成全局的是为了让 tcp_client_send() 能在任务外面被调用。
  */
static int s_sock = -1;

/**
  * @brief 收到数据时往哪儿送。由 tcp_client_set_rx_handler() 填。
  */
static tcp_rx_handler_t s_rx_handler = NULL;

/**
  * @brief    本模块当前是不是"在用"
  *
  * @note     main.c 切链路时把它置成 false，任务在下一轮发现后自行关闭
  *           socket —— 关 socket 的动作【只由任务自己做】，别的任务不碰。
  *
  * @warning  为什么不让 stop() 直接替任务关？见 tcp_client.h 里的说明：
  *           调用 stop() 的那个函数正跑在传输任务自己的栈上，去等任务
  *           退出就是等自己。
  */
static volatile bool s_running = false;

/**
  * @brief 任务是否已经创建过
  *
  * @note  这个标志让 start() 变成幂等的：切回 TCP 时直接调用即可，
  *        不会重复创建任务（重复创建的后果是两条任务共用一个 s_sock，
  *        一条在 recv、另一条把它 close 掉，谁都说不清接下来会怎样）。
  */
static bool s_task_started = false;

/**
  * @brief    注册"收到数据"的回调
  *
  * @param[in] handler  上层提供的处理函数；传 NULL 表示不处理
  *
  * @note     必须在 tcp_client_start() 【之前】调用，否则第一段收到的数据
  *           会因为回调还是 NULL 而被悄悄丢掉。
  */
void tcp_client_set_rx_handler(tcp_rx_handler_t handler)
{
    s_rx_handler = handler;
}

/**
  * @brief    往服务端发一段字节
  *
  * @param[in] data  要发的数据
  * @param[in] len   字节数；传 0 表示"data 是 C 字符串，自己算长度"
  *
  * @return   实际发出的字节数；失败返回 -1
  *
  * @note     没连上时不会阻塞等待，而是【直接丢弃】并打一行日志 ——
  *           调用方（命令解析）不该因为网络没通就卡住。
  */
int tcp_client_send(const char *data, int len)
{
    if (!s_running || s_sock < 0) {
        printf("[tcp] 还没连上服务端，%d 字节被丢弃\n", len ? len : (int)strlen(data));
        return -1;
    }
    if (len == 0) {
        len = strlen(data);
    }
    return send(s_sock, data, len, 0);
}

/**
  * @brief    传输任务：连服务端 → 收发 → 断了就重连，永不退出
  *
  * @param[in] pvParameters  任务参数，本模块没用
  *
  * @note     整个流程是一个死循环：连上 → 收数据 → 断了 → 等 1 秒 → 再连。
  *
  * @note     这个任务【不会退出】，s_running 为 false 时它只是空转。
  *           这样切模式就不用反复创建/删除任务，也不会出现两条任务
  *           抢同一个 socket 的局面。
  */
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
        int sock;

        /* 没被启用就空转。500ms 一轮：够快，切换手感不迟钝；
           够慢，空转时基本不占 CPU。

           ⚠ 这一句同时也兜住了下面那些 vTaskDelay(2000) 的重试等待 ——
             切走的时候最多多等一轮重试（2 秒），之后就会走到这里停下。 */
        if (!s_running) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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

        /* 收数据循环。条件是 s_running 而不是 1：被切走之后，
           最多等 recv() 那一轮超时（5 秒）就会退出这个循环去关连接。
           这段窗口里到达的包由 main.c 那边挡掉，不会被执行。 */
        while (s_running) {
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
        /* 这句要说准：切走的时候【不会】再连了，日志里必须能看出来，
           否则查问题时会被"1 秒后重连"这句误导成"它应该自己回来了"。 */
        if (s_running) {
            printf("[tcp] 连接结束，1 秒后重连\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else {
            printf("[tcp] 已停用\n");
        }
    }
}

/**
  * @brief 启用 TCP
  *
  * @note  任务只创建一次；之后每次调用只是把 s_running 置回 true。
  *
  * @note  参数：入口函数、任务名、栈大小(字节)、传参、优先级、句柄。
  *        任务一进去就卡在 wifi_sta_wait_ip() 上等 IP，拿到 IP 之前不占 CPU。
  */
void tcp_client_start(void)
{
    if (!s_task_started) {
        s_task_started = true;
        xTaskCreate(tcp_client_task, "tcp_client", 4096, NULL, 5, NULL);
    }
    s_running = true;
}

/**
  * @brief 停用 TCP
  *
  * @note  只置标志，立刻返回 —— 断开连接的活儿由任务自己收尾。
  *        理由见 tcp_client.h，一句话：调用方跑在这个任务自己的栈上。
  */
void tcp_client_stop(void)
{
    s_running = false;
}
