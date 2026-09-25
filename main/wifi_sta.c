/* WiFi STA（客户端）连接模块 —— 实现见 wifi_sta.h

   这个文件管两件事：

     ① 把热点名/密码存进 NVS，开机先读它（而不是焊死在代码里）
     ② 收到 TCP 命令就换热点，立刻重连

   ⚠ 这里【没有配网兜底】了。

   以前连不上会自己进 SmartConfig，让手机 App 把凭据编成一串广播包发出来。
   那条路已经整个拆掉，来龙去脉记在 README §9.7。一句话版本：

     SmartConfig 的整条链路是「手机 WiFi 驱动的广播行为 → 路由器 →
     ESP 的混杂模式」，任何一环不配合就死，而且【没有反馈】——
     串口上什么都看不出来，手机上只会说"配网失败"，根本没法查。
     我们实测过：60 秒里收到 17090 帧，其中组播/广播数据帧是 0。
     那是不可能的（ARP/DHCP/mDNS/IPv6 邻居发现全是组播），
     说明连"手机到底发没发"这个前提都无法判定。

   代价说清楚：如果主节点的 SSID/密码改了，而你没先告诉这个节点，
   它就再也回不来，只能拆下来重烧。所以改主节点凭据的顺序必须是：

       ① 先发 TCP 命令  wifi <新SSID>,<新密码>    （节点还连得上时）
       ② 再改主节点

   反过来做就得重烧固件。

   将来从节点和主节点之间走哪条链路，另定 —— 见 README §9.7 末尾。 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "wifi_sta.h"

/* ======================= 出厂默认凭据 =======================

   注意定位：这【不再】是唯一的凭据来源，而是"三个来源里最低的那一层"。
   优先级：NVS > 这里 > 内置默认。

   第一次上电时 NVS 是空的，就用这里，并顺手写进 NVS；
   之后不管用什么方式改过密码，都以 NVS 为准，这几行就不再生效了。

   所以改热点最省事的办法不是改这里重烧固件，而是：
     · 节点还连得上   → 发一条 TCP 命令 wifi <SSID>,<密码>
     · 节点已经连不上 → 只能改这里重烧（没有别的路了，见文件头）

   ⚠ SSID 区分大小写，而且【空格也算一个字符】—— 写错不会报错，
     只会一直连不上（串口上会看到 reason=201）。
   ⚠ ESP8266 只支持 2.4GHz。热点开在 5GHz 上芯片根本扫不到，
     表现和"SSID 写错"一模一样。

   ── 这两个值必须和 ESP32 主节点上那个 SoftAP 【逐字一致】 ──

   链路方案（已定）：主节点 ESP32 开 SoftAP，从节点就是本模块，
   连上去之后走 TCP 连 ESP32 上的服务器。路由器不参与。

   所以这里的 SSID / 密码，要和 ESP32 那边
   esp_wifi_set_config(ESP_IF_WIFI_AP, ...) 里的 ap.ssid / ap.password
   一模一样。两边是【各自独立编译】的，编译器不会帮你核对。

   ⚠ 密码不能少于 8 字节 —— WPA2 的下限，短了 ESP32 那边拒绝开 AP。
   ⚠ ESP32 的 ap.authmode 必须选 WPA2_PSK。本文件里
     threshold.authmode 设的就是 WPA2_PSK（见 apply_credentials()），
     主节点开成【开放式】热点反而连不上。 */
#define WIFI_SSID_DEFAULT       "ESP32-S3-host"
#define WIFI_PASSWORD_DEFAULT   "88888888"

/* ======================= NVS =======================

   NVS = Non-Volatile Storage，芯片 flash 里划出来的一小块键值存储区
   （本项目分区表里的 nvs 分区，24KB）。

   它跟"文件系统"不是一回事：没有目录、没有文件，
   只有一个 (命名空间, 键) → 值 的映射。用起来像个小字典。

   命名空间用 "app_cfg"，是为了避开 WiFi 驱动自己用的那些
   （驱动把 PHY 校准数据存在自己的命名空间里，两者互不干扰）。 */
#define NVS_NAMESPACE   "app_cfg"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"

/* 802.11 协议规定的上限：SSID 32 字节，密码 64 字节（不含结尾的 '\0'）。
   缓冲区各留一个字节给 '\0'。 */
#define SSID_MAX_LEN    32
#define PASS_MAX_LEN    64

/* ======================= 重连参数 =======================

   没连上时，每隔多久在串口上吭一声。

   重连本身【不靠这个值】—— wifi_event_handler 收到"断开"事件就直接
   esp_wifi_connect() 了，不需要定时器。这个值只控制报平安的频率：
   不然连不上时日志会一片安静，看起来像死机。

   30 秒 = Wi-Fi 扫一遍全部 13 个信道再加认证超时，大概就是这个量级。 */
#define WIFI_DOWN_REPORT_MS   30000

/* ======================= 模块内部状态 ======================= */

static EventGroupHandle_t s_wifi_event_group;

/* 事件位：
     GOT_IP  —— 拿到 IP 了（真的能通信了）
     DOWN    —— 掉线过一次（用来把管理任务从"已连上"的睡眠里叫醒） */
#define WIFI_GOT_IP_BIT   BIT0
#define WIFI_DOWN_BIT     BIT1

/* 当前生效的凭据。放在这里而不是每次去读 NVS，
   是因为事件回调里要打印它，而 NVS 读操作不该在回调里频繁做。

   ⚠ 比协议上限各多一个字节，留给结尾的 '\0'。 */
static char s_ssid[SSID_MAX_LEN + 1];
static char s_pass[PASS_MAX_LEN + 1];

/* 重试计数。除了在串口上区分"第几次失败"，管理任务还会把它打出来 ——
   数字一直涨就是真的在原地打转，该去查 SSID / 密码 / 频段了。 */
static int s_retry = 0;

/* ======================= 小工具 ======================= */

/* 把 src 拷进定长缓冲区，超长就截断，并保证以 '\0' 结尾。

   ⚠ 为什么不用 strncpy()？
     strncpy 在源串长度 >= 目标缓冲区时【不补 '\0'】，
     结果是一个没有结尾的字符串 —— 后面 strlen/printf 会一路读下去越界。
     这是 C 里最经典的坑之一，干脆自己写一个语义明确的。 */
static void copy_str(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n > dst_size - 1) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ======================= NVS 读写 ======================= */

/* 从 NVS 读凭据。读到返回 true，没读到（或读到的 SSID 是空串）返回 false。 */
static bonvs_load_credentials(void)
{
    nvs_handle_t h;
    size_t       len;
    esp_err_t    err;

    /* 只读方式打开。命名空间还不存在时这一步就会失败 ——
       第一次上电正是这种情况，所以直接返回 false 走默认值那条路，不算错误。 */
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    /* nvs_get_str 的 length 参数是【传入缓冲区大小，传出实际长度】。
       所以每次用之前都要重新赋一遍，不能复用上一个值。 */
    len = sizeof(s_ssid);
    err = nvs_get_str(h, NVS_KEY_SSID, s_ssid, &len);

    if (err == ESP_OK) {
        len = sizeof(s_pass);
        err = nvs_get_str(h, NVS_KEY_PASS, s_pass, &len);
    }

    nvs_close(h);

    return (err == ESP_OK && s_ssid[0] != '\0');
}

/* 把凭据写进 NVS。成功返回 true。

   注意这里【只写 NVS】，不碰当前连接 —— 所以 wifi_sta_init() 里
   写默认值的时候也能安全调用。改连接是 wifi_sta_set_credentials() 的事。 */
static bool nvs_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t    err;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        printf("[wifi] ✗ NVS 打不开（分区表里有 nvs 分区吗？）\n");
        return false;
    }

    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, password);
    }
    if (err == ESP_OK) {
        /* ⚠ 前面两个 set 只是写进内存缓存，这一句才真正落到 flash。
           漏掉 commit 的话，掉电就没了 —— 而且当场看不出任何异常。 */
        err = nvs_commit(h);
    }

    nvs_close(h);

    if (err != ESP_OK) {
        printf("[wifi] ✗ NVS 写入失败: %d\n", err);
        return false;
    }
    return true;
}

/* ======================= 应用凭据 ======================= */

/* 把 s_ssid / s_pass 灌进 WiFi 驱动。只改配置，不负责连接。 */
static void apply_credentials(void)
{
    wifi_config_t cfg;

    /* 整块清零。ESP8266 的 wifi_config_t 里还有很多字段
       （信道、BSSID、各种阈值），不清零的话它们就是栈上的随机值。 */
    memset(&cfg, 0, sizeof(cfg));

    /* ⚠ 这里为什么不能像以前那样写 .ssid = WIFI_SSID？
       因为那是【初始化器】，C 标准专门允许用字符串字面量初始化字符数组。
       而现在是运行期的赋值，左右两边类型对不上（uint8_t[32] 和 char*），
       必须老老实实拷贝。 */
    copy_str((char *)cfg.sta.ssid,     sizeof(cfg.sta.ssid),     s_ssid);
    copy_str((char *)cfg.sta.password, sizeof(cfg.sta.password), s_pass);

    /* threshold.authmode = 最弱的、可以接受的加密方式。
       设成 WPA2_PSK 就是"比 WPA2 弱的（WEP、WPA-TKIP）我都不连"。
       Windows 热点默认是 WPA2-Personal，对得上。
       万一你的热点是 WPA/WPA2 混合模式或者 WPA3，连不上时把这一行
       去掉再试 —— 不设的话等于接受任何加密方式。

       ⚠ 这条对主节点自建 SoftAP 的场景同样适用：ESP32 的 SoftAP 默认
         加密方式也要选 WPA2-PSK，否则这里会把它挡在外面。 */
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);
}

/* ======================= 事件回调 ======================= */

/* WiFi / IP 事件的回调。
   它由 esp_event 的默认事件循环任务调用，不在调用者的任务里跑。

   事件分两大类，靠 event_base 区分：
     WIFI_EVENT —— WiFi 状态变化（启动、关联上 AP、断开……）
     IP_EVENT   —— 网络层事件（拿到 IP……）
   event_id 是具体哪个事件；event_data 是事件携带的数据，类型由事件本身决定，
   所以要自己按 event_id 把它转成对应的结构体指针。 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* esp_wifi_start() 只是把驱动拉起来，并不会去连 AP。
           真正的连接动作要在这里发起。 */
        printf("[wifi] STA 已启动，开始连接 \"%s\" ...\n", s_ssid);
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 连不上、被踢掉、掉线，都走这里。
           reason 是 802.11 的断开原因码，最常撞见的两个：
             201 —— 找不到这个 SSID（名字写错，或者热点开在 5GHz 上）
             15 / 205 —— 密码不对，或者四次握手超时 */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;

        xEventGroupClearBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_DOWN_BIT);

        printf("[wifi] 第 %d 次失败，reason=%d\n", ++s_retry, d->reason);

        /* 直接重连、不在这里 sleep：esp_wifi_connect() 内部要先扫一遍信道、
           再等认证超时，本身就要好几秒，不会把事件循环转成死循环。
           反过来，在事件回调里 vTaskDelay 会把整个默认事件循环卡住，得不偿失。 */
        esp_wifi_connect();

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* 这才是"真的连上了"的标志：DHCP 拿到了 IP 地址。
           WIFI_EVENT_STA_CONNECTED 只是关联上了 AP，那时还没有 IP。 */
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        s_retry = 0;

        /* 注意 ip4addr_ntoa() 返回的是指向【静态缓冲区】的指针，
           连着调用三次会互相覆盖 —— 所以下面一行只放一次调用。 */
        printf("\n========== WiFi 连接成功 ==========\n");
        printf("  SSID     : %s\n", s_ssid);
        printf("  IP       : %s\n", ip4addr_ntoa(&e->ip_info.ip));
        printf("  子网掩码 : %s\n", ip4addr_ntoa(&e->ip_info.netmask));
        printf("  网关     : %s\n", ip4addr_ntoa(&e->ip_info.gw));
        printf("===================================\n\n");

        /* 叫醒还在 wifi_sta_wait_ip() 或管理任务里等着的任务 */
        xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
    }
}

/* ======================= 管理任务 =======================

   这条任务只干一件事：没连上的时候，每隔 WIFI_DOWN_REPORT_MS 在串口上
   吭一声。

   为什么要单独一条任务？因为"等一段时间"本质上是阻塞的，
   而 wifi_event_handler 跑在系统的事件循环任务上 —— 在那里阻塞，
   整个 WiFi 状态机就停摆了。

   ⚠ 重连本身【不在这里】：wifi_event_handler 收到"断开"事件就直接
     esp_wifi_connect() 了。所以这条任务挂掉不影响重连，只会让日志变哑。

   （以前它还负责"连不上超过一段时间就进 SmartConfig"，
     那条路整个拆掉了，见文件头。） */
static void wifi_mgr_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* 已经连上 —— 睡到掉线为止。
           portMAX_DELAY = 无限等，不占 CPU。 */
        if (xEventGroupGetBits(s_wifi_event_group) & WIFI_GOT_IP_BIT) {
            xEventGroupWaitBits(s_wifi_event_group, WIFI_DOWN_BIT,
                                pdTRUE,    /* 等到了就清掉，下次还能再等 */
                                pdTRUE,    /* 指定的位全到齐 */
                                portMAX_DELAY);
            continue;   /* 掉线了，回到循环开头重新判断 */
        }

        /* 没连上 —— 等一段。注意这里 pdFALSE：不能清除 GOT_IP 位，
           否则 wifi_sta_wait_ip() 那边永远等不到。 */
        if (xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT,
                                pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(WIFI_DOWN_REPORT_MS))
            & WIFI_GOT_IP_BIT) {
            continue;   /* 连上了 */
        }

        /* 还是没连上。打一行就走，下一轮接着等 —— 重连是回调那边做的。 */
        printf("[wifi] 还没连上 \"%s\"：已失败 %d 次，仍在重试\n", s_ssid, s_retry);
        printf("[wifi]   查这三样：SSID 拼写（空格和大小写都算）、密码、\n");
        printf("[wifi]   热点是不是只在 5GHz 上（ESP8266 只认 2.4GHz）\n");
    }
}

/* ======================= 对外接口 ======================= */

void wifi_sta_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* WiFi 驱动要把 PHY 校准数据之类的存进 NVS，不初始化会直接启动失败。
       第一次上电时 NVS 分区是空的，驱动会自己把需要的页初始化好。
       如果分区表动过、或者上一版固件用的是另一种 NVS 格式，这两个错误会冒出来，
       标准做法是擦掉整个 NVS 分区再重来一次。

       ⚠ 注意 nvs_flash_erase() 会把【我们的凭据也一起擦掉】——
         所以这条路径走完，节点会退回代码里的默认凭据。
         这其实正好是一个"恢复出厂设置"的办法。 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        printf("[wifi] NVS 需要重建 (%d)，正在擦除...\n", ret);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ---- 凭据：NVS 优先，没有就用编译进去的默认值 ---- */

    if (nvs_load_credentials()) {
        printf("[wifi] 凭据来自 NVS\n");
    } else {
        copy_str(s_ssid, sizeof(s_ssid), WIFI_SSID_DEFAULT);
        copy_str(s_pass, sizeof(s_pass), WIFI_PASSWORD_DEFAULT);
        printf("[wifi] NVS 里没有凭据，用出厂默认值，并写入 NVS\n");
        nvs_save_credentials(s_ssid, s_pass);
    }

    /* 这两行是所有网络功能的地基：
         tcpip_adapter_init()             —— 把 lwIP 协议栈拉起来
         esp_event_loop_create_default()  —— 起默认事件循环，"连上了 / 断开了"
                                             就是靠它把事件派发给上面的回调 */
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 初始化 WiFi 驱动本身 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 显式关掉省电模式。

       ⚠ 先说清楚：【在本 SDK 上这一句是冗余的】，它不是任何问题的修复。
         esp_wifi.h:413 的原文：

             @attention Default power save type is WIFI_PS_NONE.

         默认本来就是不省电。（当年这一句是从官方 examples/wifi/smart_config
         抄过来的，那个例子已经和本项目无关了 —— 见 README §9.7。）

         留着它是为了"把前提写死"，而且这个前提在将来会真的有用：
         SDK 自带的 ESP-NOW 示例（examples/wifi/espnow/README.md）里
         明确写了一句 ——

             如果接收方是 station 模式且连着一个 AP，
             必须关掉 modem sleep。

         也就是说，哪天从节点改用 ESP-NOW 走点对点，省电模式开着
         就会收不到包。现在写死，等于提前把那个坑守住。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* 注册回调。IP 那一侧只关心"拿到 IP"这一个事件，其它不用管。 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_credentials();
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 起管理任务。栈 4KB：它只做 printf 和等事件，不干重活。 */
    if (xTaskCreate(wifi_mgr_task, "wifi_mgr", 4096, NULL, 4, NULL) != pdPASS) {
        printf("[wifi] ✗ 管理任务创建失败（只影响「还没连上」那几行提示）\n");
    }

    /* esp_wifi_start() 是异步的：它返回时还没连上，连接结果由事件循环
       在后台的 WiFi 任务里通过 wifi_event_handler() 打印出来。 */
}

bool wifi_sta_wait_ip(uint32_t timeout_ms)
{
    /* pdMS_TO_TICKS(UINT32_MAX) 会被除成一个有限的 tick 数，
       所以"无限等待"要单独映射到 portMAX_DELAY。 */
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY
                                                  : pdMS_TO_TICKS(timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_GOT_IP_BIT,
                                           pdFALSE,   /* 不清除这个 bit */
                                           pdTRUE,    /* 要等到全部指定的 bit */
                                           ticks);
    return (bits & WIFI_GOT_IP_BIT) != 0;
}

bool wifi_sta_set_credentials(const char *ssid, const char *password)
{
    size_t n;

    if (ssid == NULL || password == NULL) {
        return false;
    }

    /* 长度必须在【存进 NVS 之前】检查。
       超长的 SSID 被截断后照样能写进 NVS，但永远连不上 ——
       那种"命令说成功了、就是连不上"的毛病最难查。 */
    n = strlen(ssid);
    if (n == 0 || n > SSID_MAX_LEN) {
        printf("[wifi] ✗ SSID 长度 %u 不合法（1~%d 字节）\n",
               (unsigned)n, SSID_MAX_LEN);
        return false;
    }
    if (strlen(password) > PASS_MAX_LEN) {
        printf("[wifi] ✗ 密码超过 %d 字节\n", PASS_MAX_LEN);
        return false;
    }

    /* 先落盘。写失败就整个放弃，不动当前连接 ——
       否则会出现"内存里换了、重启又变回去"这种鬼打墙。 */
    if (!nvs_save_credentials(ssid, password)) {
        return false;
    }

    copy_str(s_ssid, sizeof(s_ssid), ssid);
    copy_str(s_pass, sizeof(s_pass), password);

    printf("[wifi] 凭据已更新，正在切到 \"%s\" ...\n", s_ssid);

    apply_credentials();     /* 必须先改配置，再断开重连 */
    s_retry = 0;

    /* esp_wifi_disconnect() 会触发 DISCONNECTED 事件，
       而 wifi_event_handler 里会自己 esp_wifi_connect() ——
       所以正常情况下不用在这里再连一次。

       但如果当时本来就没连上，disconnect 会返回错误、也不会有事件，
       那就得手动补一脚。 */
    if (esp_wifi_disconnect() != ESP_OK) {
        esp_wifi_connect();
    }

    return true;
}

const char *wifi_sta_get_ssid(void)
{
    return s_ssid;
}
