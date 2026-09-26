/**
  * @file    udp_client.c
  * @brief   UDP 传输模块 —— 不建连接，直接往主节点丢数据报
  *
  * @note    和 tcp_client.c 一样是【纯传输层】：只负责把字节搬过去、搬回来，
  *          完全不知道"开灯"是什么。业务在 main.c 里，通过
  *          udp_client_set_rx_handler() 注册的回调接进去。
  *
  * @note    接口说明（含和 TCP 的差别）见 udp_client.h。
  *
  * @warning 这个模块和 tcp_client 是【二选一】的，同一时刻只该有一个在收。
  *          两个一起开的话，同一条命令会从两条路各到一次 —— "开灯"会被
  *          执行两次，"关灯"也是。切换逻辑在 main.c 的 link_switch()。
  */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 和 tcp_client.c 同一套 BSD socket API，只是多了 bind / sendto / recvfrom。
   INADDR_ANY 来自 lwip/inet.h，它已经被 lwip/sockets.h 转手带进来了。 */
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi_sta.h"
#include "udp_client.h"

/**
  * @brief 主节点（UDP 对端）的地址
  *
  * @note  和 tcp_client.c 里的 TCP_SERVER_IP 是【同一个地址、同一套开关】：
  *        两套值，用哪套就放开哪套，而且必须和 wifi_sta.c 顶部的 SSID / 密码
  *        一起切。完整理由（为什么这两个地址都能硬编码、只切一半会看到什么）
  *        写在 tcp_client.c 那段注释里，这里不重复。
  *
  * @warning 改 TCP_SERVER_IP 的时候别忘了这里 —— 这两个 define 在
  *          【不同文件里各存了一份】，编译器不会帮你核对。
  *          漏改的表现：net tcp 已经通了，一敲 net udp 又开始刷 errno=113。
  */

/* 台面调试：电脑开移动热点（当前生效） */
#define UDP_SERVER_IP   "192.168.137.1"

/* 正式：ESP32 主节点开 SoftAP */
/* #define UDP_SERVER_IP "192.168.4.1" */

/**
  * @brief 端口号
  *
  * @note  本端 bind 的端口和目的端口用的是【同一个值】，8087。
  *        这样定是为了让主节点的回包规则简单到没有歧义：收到包的源地址是
  *        从节点IP:8087，回包就发给这个地址，不需要额外协商，也不需要
  *        维护一张"谁在哪"的表。
  *
  * @warning 必须和 ESP32 那边一致，而且【不】要设成 8086 ——
  *          8086 是 TCP 那边的端口。UDP 端口和 TCP 端口是两套互不干扰的
  *          编号空间，理论上重号也没事，但重号之后看日志、抓包、配防火墙
  *          都得先在脑子里换算一次是哪个协议，没必要给自己挖这个坑。
  */
#define UDP_PORT        8087

/** @brief 接收缓冲区大小（字节）。一个数据报超过它，多出来的部分会被丢掉。 */
#define UDP_RX_BUF_SIZE     128

/**
  * @brief recvfrom() 的接收超时（秒）
  *
  * @note  这个超时在这里是【双重身份】：
  *          ① 让任务有机会看到 s_running 被置成 false，否则会永远阻塞；
  *          ② 顺便充当下面那个"重新报到"的定时心跳。
  *        所以它不再是纯粹的超时保护，改成别的值之前先想清楚这两件事。
  */
#define UDP_RECV_TIMEOUT_S  5

/**
  * @brief 每隔这么久，主动往主节点再报一次到（毫秒）
  *
  * @warning 这条心跳【不是】冗余设计，是 UDP 模式下必须有的东西。
  *
  *          TCP 里主节点靠 accept() 就知道谁连上来了；UDP 没有连接，
  *          主节点对我们的存在【一无所知】，只能靠我们主动发一包过去，
  *          它才能从源地址学到"该往哪儿回"。
  *
  *          那为什么不能只发一次？因为主节点一重启，它记的地址就没了 ——
  *          而它重启时我们这个从节点是【收不到任何通知】的（UDP 无连接，
  *          也无从感知对端重启）。没有周期心跳的话，从节点会一直以为
  *          链路好好的，主节点却再也找不到它，直到从节点自己也重启一次。
  *
  *          30 秒是权衡：够短，主节点重启后半分钟内链路自动恢复；
  *          够长，不至于在空口上刷屏。
  */
#define UDP_ANNOUNCE_MS     30000

/**
  * @brief    当前 socket，-1 表示没在收
  *
  * @note     存成全局的是为了让 udp_client_send() 能在任务外面被调用。
  */
static int s_sock = -1;

/**
  * @brief 收到数据时往哪儿送。由 udp_client_set_rx_handler() 填。
  */
static udp_rx_handler_t s_rx_handler = NULL;

/**
  * @brief    这个模块当前是不是"在用"
  *
  * @note     main.c 切链路时把它置成 false，任务在下一轮发现后自行关闭
  *           socket —— 关 socket 的动作【只由任务自己做】，别的任务不碰。
  *
  * @warning  为什么不让 stop() 直接替任务关？见 udp_client.h 里的说明：
  *           调用 stop() 的那个函数正跑在传输任务自己的栈上，去等任务
  *           退出就是等自己。
  */
static volatile bool s_running = false;

/**
  * @brief 任务是否已经创建过
  *
  * @note  这个标志让 start() 变成幂等的：切回 UDP 时直接调用即可，
  *        不会重复创建任务（重复创建的后果是两条任务抢同一个 s_sock）。
  */
static bool s_task_started = false;

/**
  * @brief 注册"收到数据"的回调
  *
  * @param[in] handler  上层提供的处理函数；传 NULL 表示不处理
  *
  * @note  必须在 udp_client_start() 【之前】调用。
  */
void udp_client_set_rx_handler(udp_rx_handler_t handler)
{
    s_rx_handler = handler;
}

/**
  * @brief    往主节点发一个数据报
  *
  * @param[in] data  要发的数据
  * @param[in] len   字节数；传 0 表示"data 是 C 字符串，自己算长度"
  *
  * @return   实际发出的字节数；失败或被丢弃返回 -1
  *
  * @note     和 tcp_client_send() 一样，没在用的时候【直接丢弃】并打日志，
  *           不阻塞等待 —— 调用方（命令解析）不该因为网络没通就卡住。
  *
  * @note     目的地址每次现算，而不是建 socket 时存一份全局的。
  *           反正就十几个字节的填表，换来的是不必操心"上次存的地址
  *           会不会是过期的"。
  *
  * @warning  返回成功只说明"交给 lwIP 了"，【不代表对方收到】。
  *           UDP 没有确认、没有重传、没有连接状态，对端是死是活本地
  *           完全看不出来。要确认对端还在，得靠上层自己约心跳。
  */
int udp_client_send(const char *data, int len)
{
    struct sockaddr_in dest;

    if (!s_running || s_sock < 0) {
        printf("[udp] 当前没在用，%d 字节被丢弃\n", len ? len : (int)strlen(data));
        return -1;
    }
    if (len == 0) {
        len = strlen(data);
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(UDP_PORT);
    dest.sin_addr.s_addr = inet_addr(UDP_SERVER_IP);

    return sendto(s_sock, data, len, 0,
                  (struct sockaddr *)&dest, sizeof(dest));
}

/**
  * @brief 主动往主节点报一次到
  *
  * @note  开头那包和周期心跳用的是同一条消息，所以抽成一个函数。
  *        ASCII 内容，原因同 tcp_client 那边：不挑编码，不会显示成乱码。
  */
static void udp_announce(void)
{
    udp_client_send("hello from ESP-01S\r\n", 0);
}

/**
  * @brief    传输任务：bind 本机端口 → 收发 → 没在用了就空转，永不退出
  *
  * @param[in] pvParameters  任务参数，本模块没用
  *
  * @note     注意这个任务【不会退出】，s_running 为 false 时它只是空转。
  *           这样切模式就不用反复创建/删除任务，也不会出现两条任务
  *           抢同一个 socket 的局面。
  */
static void udp_client_task(void *pvParameters)
{
    char rxbuf[UDP_RX_BUF_SIZE];

    /* 先等 WiFi 真的拿到 IP。没有 IP 的时候 bind / sendto 都没意义，
       与其盲目重试，不如在这里阻塞等待。UINT32_MAX = 一直等。 */
    if (!wifi_sta_wait_ip(UINT32_MAX)) {
        printf("[udp] 等 IP 超时，放弃\n");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        struct sockaddr_in local;
        struct timeval     tv;
        uint32_t           last_announce;
        int                sock;

        /* 没被启用就空转。500ms 一轮：够快，切换手感不迟钝；
           够慢，空转时基本不占 CPU。 */
        if (!s_running) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            printf("[udp] 创建 socket 失败: errno=%d\n", errno);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        /* 接收超时。没有它的话 recvfrom() 会永远阻塞，这个任务既看不到
           s_running 变成 false，也没机会发心跳。超时不算错误，
           下面靠 errno == EWOULDBLOCK 把它和真错误区分开。 */
        tv.tv_sec  = UDP_RECV_TIMEOUT_S;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* bind：把本机端口固定下来。
           不 bind 也行（lwIP 会在第一次 sendto 时随便挑一个临时端口），
           但那样主节点就只能记"从哪个端口来的"，端口一变回包就发错地方。
           固定住之后，主节点的回包规则是死的：回 源IP:UDP_PORT。
           地址填 INADDR_ANY，意思是"收本机所有网卡上的这个端口"。 */
        memset(&local, 0, sizeof(local));
        local.sin_family      = AF_INET;
        local.sin_port        = htons(UDP_PORT);
        local.sin_addr.s_addr = INADDR_ANY;

        if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            printf("[udp] bind 端口 %d 失败 errno=%d，2 秒后重试\n", UDP_PORT, errno);
            close(sock);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        s_sock = sock;
        printf("[udp] 已就绪，本机端口 %d，发往 %s:%d\n",
               UDP_PORT, UDP_SERVER_IP, UDP_PORT);

        /* 先喊一声。UDP 没有 connect，主节点不会"看到"我们上线，
           只能靠我们主动发一包，它才从源地址学到该往哪儿回。 */
        udp_announce();
        last_announce = xTaskGetTickCount();

        while (s_running) {
            struct sockaddr_in from;
            socklen_t          fromlen = sizeof(from);

            /* from 拿不拿都行 —— 本模块【故意不】用源地址去更新主节点的
               地址，理由见 udp_client.h（免得热点里谁丢一包就冒充成主节点）。
               留着这个变量只是因为 recvfrom 的参数不能传 NULL。 */
            int len = recvfrom(sock, rxbuf, sizeof(rxbuf), 0,
                               (struct sockaddr *)&from, &fromlen);

            if (len > 0) {
                /* 和 TCP 那边一样，recvfrom 交出来的是裸字节，也不保证是
                   C 字符串（UDP 不会替你补 '\0'），这里【不做任何解释】，
                   原样交给业务层的回调。 */
                if (s_rx_handler != NULL) {
                    s_rx_handler(rxbuf, len);
                }

            } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
                /* 只是这 5 秒没数据。这里也是本任务【唯一】的定时心跳点：
                   既然已经醒了，顺手看看该不该重新报一次到。 */
                if ((xTaskGetTickCount() - last_announce)
                        >= (UDP_ANNOUNCE_MS / portTICK_PERIOD_MS)) {
                    udp_announce();
                    last_announce = xTaskGetTickCount();
                }

            } else {
                printf("[udp] 接收出错 errno=%d\n", errno);
                break;
            }
        }

        s_sock = -1;
        close(sock);
        printf("[udp] 已停用\n");
    }
}

/**
  * @brief 启用 UDP
  *
  * @note  任务只创建一次；之后每次调用只是把 s_running 置回 true。
  *
  * @note  参数：入口函数、任务名、栈大小(字节)、传参、优先级、句柄。
  *        优先级和 tcp_client 一样是 5 —— 反正同一时刻只有一条在干活。
  */
void udp_client_start(void)
{
    if (!s_task_started) {
        s_task_started = true;
        xTaskCreate(udp_client_task, "udp_client", 4096, NULL, 5, NULL);
    }
    s_running = true;
}

/**
  * @brief 停用 UDP
  *
  * @note  只置标志，立刻返回 —— 关 socket 的活儿由任务自己收尾。
  *        理由见 udp_client.h，一句话：调用方跑在这个任务自己的栈上。
  */
void udp_client_stop(void)
{
    s_running = false;
}
