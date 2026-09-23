/* ESP-01S 继电器控制

   GPIO0 驱动继电器，低电平激活。

   ⚠ GPIO0 是 ESP8266 的启动模式选择脚（strapping pin）：
     复位时必须为高电平，芯片才会从 Flash 启动；
     复位时为低电平，芯片会进入 UART 下载模式，程序根本不会运行。

     而下面这个"拉低 1 秒 / 拉高 1 秒"的循环，意味着这个脚有一半时间是低的。
     所以任何一次意外复位（掉电、看门狗、按 RST）都有大约 50% 的概率正好撞在
     低电平相位上 —— 那一刻芯片会停在 bootloader 里等下载，程序不会启动，
     要再复位一次（撞在高电平相位）才能恢复。

     这是用 GPIO0 做输出的固有代价，不是 bug。ESP-01S 上引出来的脚只有
     GPIO0 / GPIO1 / GPIO2 / GPIO3，其中 GPIO1 和 GPIO3 是串口的 TX/RX，
     GPIO2 在启动时同样要求高电平 —— 这块板子上没有更"干净"的脚可以换。

   ⚠ 供电：WiFi 发射瞬间电流会冲到 170~300mA，叠加上继电器线圈，
     如果还是拿 USB 转串口板那个 3.3V 脚供电，很容易掉电复位（brownout）。
     接上 WiFi 之后如果模块开始反复重启、串口刷乱码，先怀疑供电，
     在 VCC-GND 之间并 100~470µF 电解电容 + 0.1µF 瓷片电容。

   ⚠ 频段：ESP8266 只支持 2.4GHz。Windows 热点的"网络频段"如果设成了 5GHz，
     芯片根本看不到这个热点（表现为一直 reason=201 连不上）。

   This example code is in the Public Domain (or CC0 licensed, at your option.)
*/

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_spi_flash.h"
#include "driver/gpio.h"

#include "esp_event.h"
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "nvs_flash.h"
#include "nvs.h"

/* 要连接的 AP。
   这两个宏会被逐字节拷进 wifi_config_t 里的 uint8_t ssid[32] / password[64]，
   所以必须是【字符串字面量】，不能写成 const char * 变量。
   注意 SSID 区分大小写，而且【空格也算一个字符】—— 下面这个名字里
   DESKTOP-HTLNPUV 和 4127 之间就有一个空格。写错不会报错，只会一直连不上。 */
#define WIFI_SSID       "DESKTOP-HTLNPUV 4127"
#define WIFI_PASSWORD   "88888888"

//波特率74880
//io0控制继电器，高电平吸合
//esp-01S进入运行模式，io0必须为高电平，io2必须为高电平，那为什么，芯片复位时，io0并没有输出高电平？运行模式靠 ROM 在复位瞬间读到的电平决定。这个电平是谁给的都行——内部上拉、板上 10k、或者外部驱动器，ROM 不关心。上拉只是"让悬空脚有个确定值"的手段
#define RELAY_GPIO   GPIO_NUM_0
#define RELAY_ON     0
#define RELAY_OFF    1

/* 重试计数，只用来在串口上区分"第几次失败"，方便判断是不是一直在原地打转 */
static int s_retry = 0;

/* WiFi / IP 事件的回调。
   它由 esp_event 的默认事件循环任务调用，不在 app_main 里跑。

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
    }
}

static void wifi_init_sta(void)
{
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
}

void app_main(void)
{
    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is ESP8266 chip with %d CPU cores, WiFi, ", chip_info.cores);
    printf("silicon revision %d, ", chip_info.revision);
    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    /* 把 GPIO0 配成输出。
       注意：gpio_config() 只是"使能输出驱动器"，它并不写电平；但驱动器一开，
       引脚立刻开始输出【输出寄存器里已有的值】，而该寄存器复位后是 0。
       所以实际上在 gpio_config() 返回的那一刻，GPIO0 就已经被拉低了，
       下面的 set_level 写的是同一个 0 —— 它的价值是让"拉低"显式化、可维护，
       而不是"第一次拉低"。 */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1 << RELAY_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    printf("[wifi] 初始化 WiFi ...\n");
    wifi_init_sta();
    printf("[wifi] 初始化完成，连接结果会异步打印在上面。\n");

    /* 主任务保持存活。
       esp_wifi_start() 是异步的：它返回时还没连上，连接结果由事件循环在后台的
       WiFi 任务里通过 wifi_event_handler() 打印，所以这里不需要等。

       主任务绝不能让程序返回或调用 esp_restart()：一旦复位，GPIO0 会被重新
       采样成启动模式选择脚，此时若正好处在低电平相位，芯片就会进下载模式。 */
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
