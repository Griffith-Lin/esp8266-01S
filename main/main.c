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

   This example code is in the Public Domain (or CC0 licensed, at your option.)
*/

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "driver/gpio.h"

//波特率74880
//io0控制继电器，高电平吸合
//esp-01S进入运行模式，io0必须为高电平，io2必须为高电平
//那为什么，芯片复位时，io0并没有输出高电平？运行模式靠 ROM 在复位瞬间读到的电平决定。这个电平是谁给的都行——内部上拉、板上 10k、或者外部驱动器，ROM 不关心。上拉只是"让悬空脚有个确定值"的手段
#define RELAY_GPIO   GPIO_NUM_0
#define RELAY_ON     0
#define RELAY_OFF    1

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

    printf("GPIO0 toggle: LOW 1s / HIGH 1s\n");

    /* 拉低 1 秒（继电器吸合）→ 拉高 1 秒（继电器释放）→ 无限循环。
       主任务绝不能让程序返回或调用 esp_restart()：一旦复位，GPIO0 会被重新
       采样成启动模式选择脚，此时若正好处在低电平相位，芯片就会进下载模式。 */
    while (1) {
        // gpio_set_level(RELAY_GPIO, RELAY_ON);       // 拉低
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        // gpio_set_level(RELAY_GPIO, RELAY_OFF);      // 拉高
        // vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
