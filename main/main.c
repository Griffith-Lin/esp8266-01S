/**
  * @file    main.c
  * @brief   把各模块串起来并启动
  *
  * 这个文件【只做粘合剂和启动器】，不含任何业务逻辑 —— 继电器怎么动、
  * 命令怎么解析、走 TCP 还是 UDP，全在下面这些模块里：
  *
  *   relay.c        继电器（GPIO0、上电为关、strapping 脚）
  *   link.c         当前走哪条链路 + 所有回执的出口
  *   cmd.c          收字节 → 拼命令 → 调 relay / link / wifi_sta
  *   wifi_sta.c     连热点、凭据存 NVS、断线重连
  *   tcp_client.c   连服务端、收发字节（纯传输，不知道"开灯"是什么）
  *   udp_client.c   不建连接、直接丢数据报（同样纯传输）
  *
  * @par 要改什么，去哪儿
  *
  *   - 服务端 IP / 端口   → tcp_client.c 和 udp_client.c 的顶部【两个文件都要改】
  *   - 热点名 / 密码      → 优先发命令 wifi \<SSID\>,\<密码\>；改代码只能改默认值
  *   - 能识别的命令        → cmd.c 的 s_cmds 表
  *   - 继电器接哪个脚      → relay.c 顶部的 RELAY_GPIO
  *   - 开机默认走哪条链路  → 本文件 app_main() 里的 link_init()
  *
  * @warning 节点一旦连不上，就【没有】远程通道了 —— 只能拆下来重烧。
  *          所以改主节点凭据的顺序必须是：
  *              ① 先发命令改从节点（它此时还连得上）
  *              ② 再改主节点
  *          反过来做，这块板子就只能插串口线了。
  *
  * @warning "改配置"和"换固件"是两件事：改 WiFi 密码靠上面那条命令，
  *          换固件只能插串口线（本板 1MB，两个 app 槽放不下，所以没有 OTA）。
  *
  * @see     学习笔记/ESP8266开发流程.md §9.1（模块分工）、§9.7（连不上怎么查）、§9.9（为什么没有 OTA）、
  *          学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §5（整体架构）
  *
  * This example code is in the Public Domain (or CC0 licensed, at your option.)
  */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_spi_flash.h"

#include "wifi_sta.h"
#include "tcp_client.h"
#include "udp_client.h"
#include "relay.h"
#include "link.h"
#include "cmd.h"

/**
  * @brief 启动时把芯片信息打到串口上
  *
  * @note  spi_flash_get_chip_size() 读的是【编译时写进 sdkconfig 的值】，
  *        不是探测出来的 —— 所以这行打印不能用来判断板上真实有多少 flash。
  *        要确认容量，用 esptool.py flash_id（见 学习笔记/ESP8266开发流程.md）。
  */
static void print_chip_info(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is ESP8266 chip with %d CPU cores, WiFi, ", chip_info.cores);
    printf("silicon revision %d, ", chip_info.revision);
    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}

/**
  * @brief 程序入口
  *
  * @note  下面四步的【顺序不能动】，每一步的原因都写在那一行上面。
  */
void app_main(void)
{
    print_chip_info();

    /* ① 硬件先就位：把 GPIO0 配成输出并置成"关"。 */
    relay_init();

    /* ② 联网：连热点、断线重连、打印 IP。异步的，返回时还没连上。 */
    wifi_sta_init();

    /* ③ 注册回调 —— 必须在 link_init() 之前：后者会立刻把链路启动起来，
          注册晚了第一段到达的数据会因为回调还是 NULL 而被悄悄丢掉。

          两个都注册：当前只有一条在收，但切换之后另一条需要自己的入口。 */
    tcp_client_set_rx_handler(cmd_on_tcp_rx);
    udp_client_set_rx_handler(cmd_on_udp_rx);

    /* ④ 定初始链路并启动它。开机默认走 TCP —— 保持和"加 UDP 之前"一致，
          加一个模式不该顺手改变已经跑通的那条路。
          想改成开机走 UDP，把这里换成 LINK_UDP 即可，别处不用动。

          它会自己等 WiFi 拿到 IP，所以紧跟在 wifi_sta_init() 后面调用即可。 */
    link_init(LINK_TCP);

    /* ⑤ 主任务保持存活。真正的活儿都在上面那几条任务里异步跑着。

          ⚠ 这里本身【不要】返回、也不要主动重启：一旦复位，GPIO0 会被重新
            采样成启动模式选择脚，此时若正好处在低电平，芯片就会进下载模式。
            本工程现在没有任何地方会重启，所以这条约束是"别引入"而不是"要处理"——
            将来真要加重启，记得先 relay_on()。 */
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
