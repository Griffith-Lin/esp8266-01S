/* WiFi STA（客户端）连接模块 —— 实现见 wifi_sta.h */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "wifi_sta.h"

/* 要连接的 AP。
   这两个宏会被逐字节拷进 wifi_config_t 里的 uint8_t ssid[32] / password[64]，
   所以必须是【字符串字面量】，不能写成 const char * 变量。
   注意 SSID 区分大小写，而且【空格也算一个字符】—— 写错不会报错，
   只会一直连不上（串口上会看到 reason=201）。

   ⚠ ESP8266 只支持 2.4GHz。如果热点开在 5GHz 上，芯片根本扫不到，
     表现和"SSID 写错"一模一样。 */
#define WIFI_SSID       "DESKTOP-HTLNPUV 4127"
#define WIFI_PASSWORD   "88888888"

/* 事件组：只用来往外传"拿到 IP 了"这一个信号。
   之所以不用全局 bool，是因为事件组自带阻塞等待，
   wifi_sta_wait_ip() 不用自己去轮询。 */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_GOT_IP_BIT  BIT0

/* 重试计数，只为了在串口上区分"第几次失败"，方便看出是不是在原地打转 */
static int s_retry = 0;

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
        printf("[wifi] STA 已启动，开始连接 \"%s\" ...\n", WIFI_SSID);
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 连不上、被踢掉、掉线，都走这里。
           reason 是 802.11 的断开原因码，最常撞见的两个：
             201 —— 找不到这个 SSID（名字写错，或者热点开在 5GHz 上）
             15 / 205 —— 密码不对，或者四次握手超时 */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
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
        printf("  SSID     : %s\n", WIFI_SSID);
        printf("  IP       : %s\n", ip4addr_ntoa(&e->ip_info.ip));
        printf("  子网掩码 : %s\n", ip4addr_ntoa(&e->ip_info.netmask));
        printf("  网关     : %s\n", ip4addr_ntoa(&e->ip_info.gw));
        printf("===================================\n\n");

        /* 叫醒还在 wifi_sta_wait_ip() 里等着的任务 */
        xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
    }
}

void wifi_sta_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* WiFi 驱动要把 PHY 校准数据之类的存进 NVS，不初始化会直接启动失败。
       第一次上电时 NVS 分区是空的，驱动会自己把需要的页初始化好。
       如果分区表动过、或者上一版固件用的是另一种 NVS 格式，这两个错误会冒出来，
       标准做法是擦掉整个 NVS 分区再重来一次。 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        printf("[wifi] NVS 需要重建 (%d)，正在擦除...\n", ret);
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 这两行是所有网络功能的地基：
         tcpip_adapter_init()             —— 把 lwIP 协议栈拉起来
         esp_event_loop_create_default()  —— 起默认事件循环，"连上了 / 断开了"
                                             就是靠它把事件派发给上面的回调 */
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 初始化 WiFi 驱动本身 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 注册回调。IP 那一侧只关心"拿到 IP"这一个事件，其它不用管。 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    /* STA（客户端）配置。这里用的是"指定初始化器"，只写关心的字段，
       剩下的由编译器自动补 0 —— 所以漏填的字段不会变成随机值。 */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            /* threshold.authmode = 最弱的、可以接受的加密方式。
               设成 WPA2_PSK 就是"比 WPA2 弱的（WEP、WPA-TKIP）我都不连"。
               Windows 热点默认是 WPA2-Personal，对得上。
               万一你的热点是 WPA/WPA2 混合模式或者 WPA3，连不上时把这一行
               注释掉再试 —— 不设的话等于接受任何加密方式。 */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

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
