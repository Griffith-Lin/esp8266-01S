/* OTA 远程升级模块 —— 实现

   本文件手写了 OTA 的完整五步，而不是直接调 SDK 的 esp_https_ota()。
   原因是【必须的】，不是偏好：

     esp_https_ota() 内部最后一句是无条件的 esp_ota_set_boot_partition()
     （见 components/esp_https_ota/src/esp_https_ota.c:127），
     没有任何办法让它"只写不切"。而本项目第一次调试恰恰需要"只写不切"，
     所以只能自己走一遍它内部那五步：

       esp_ota_get_next_update_partition()   找另一块槽
       esp_ota_begin()                       擦除
       esp_ota_write()                       循环写入
       esp_ota_end()                         校验
       esp_ota_set_boot_partition()          切换（这一步被开关控制）

   附带好处：直接用 esp_http_client 也绕开了 CONFIG_OTA_ALLOW_HTTP 那道门 ——
   那个检查只写在 esp_https_ota.c 里，所以本项目【不需要】改 menuconfig
   就能用普通 http:// 升级。 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "spi_flash.h"

#include "ota.h"

/* spi_flash_get_id() 在 SDK 里是个【非 static】函数（spi_flash.c:169），
   但它没被写进任何公开头文件 —— spi_flash.h 里只导出了 spi_flash_get_chip_size()。
   所以想用它只能自己声明一句。链接得上的。 */
extern uint32_t spi_flash_get_id(void);

/* ======================= 诊断 =======================

   为什么要有这一块？

   第一次跑 OTA 时，串口在 "Content-Length" 之后直接吐乱码然后没了，
   什么错误信息都没留下。这种情况下继续读源码猜是没用的，
   必须让程序自己把数字打出来：

     heap        —— 还剩多少内存。ESP8266 一共才 ~80KB，OTA 又要开 socket
                    又要开 8KB 栈，很可能不够。
     stack_free  —— 这条任务的栈【历史最低剩余量】（high water mark）。
                    它一旦接近 0，就说明栈要爆了。
                    ⚠ 单位是"字"，但这个 port 把 StackType_t 定义成了 uint8_t，
                      所以数字就是【字节】。
                    ⚠ 它记的是【历史最低】，是往下走不回来的 —— 所以最后打的那次
                      就是全程峰值。 */

#define OTA_DIAG(stage)   do {                                              \
        printf("[ota][diag] %-14s heap=%-6u stack_low=%-5u\n",              \
               (stage),                                                     \
               (unsigned)esp_get_free_heap_size(),                          \
               (unsigned)uxTaskGetStackHighWaterMark(NULL));                \
    } while (0)

/* 把 flash 的真实芯片 ID 读出来。

   ⚠ 这一条是本次排查的重点，值得说清楚：

     spi_flash_get_chip_size() 返回的是 g_rom_flashchip.chip_size，
     而那个结构体在 spi_flash.c:67 是【静态初始化】的：

         esp_rom_spiflash_chip_t g_rom_flashchip = {
             0x1640ef,                 ← 芯片 ID，写死的
             CONFIG_SPI_FLASH_SIZE,    ← 容量，直接取自 sdkconfig
             ...
         };

     也就是说，它报的是【配置里写了多少】，不是【芯片实际是多少】。
     所以 print_chip_info() 里那句 "2MB flash" 无论插的是 1MB 还是 2MB 模块，
     都会打印 2MB —— 它证明不了任何事。

     而 spi_flash_erase_sector() 的越界检查（spi_flash.c:465）用的也正是
     这个配置值。真芯片要是比配置小，这个检查就是摆设：
     擦除命令会带着超出芯片范围的地址发下去，而 SPI flash 的地址是
     按芯片容量【回绕】的 —— 0x108000 在 1MB 芯片上会落到 0x008000，
     也就是【分区表】所在的位置。

     真芯片容量只能靠读 JEDEC ID 知道。 */
static void ota_print_flash_id(void)
{
    uint32_t id = spi_flash_get_id();

    printf("[ota] flash ID = 0x%06X, 配置容量 = %u 字节 (%u MB)\n",
           (unsigned)(id & 0xFFFFFF),
           (unsigned)spi_flash_get_chip_size(),
           (unsigned)(spi_flash_get_chip_size() / (1024 * 1024)));

    /* JEDEC 标准里，第三个字节是【容量的以 2 为底的对数】：
         0x14 = 2^20 = 1 MB      0x15 = 2^21 = 2 MB
         0x16 = 2^22 = 4 MB      0x17 = 2^23 = 8 MB
       只作参考，别当铁证 —— 各家编码并不完全统一，
       最权威的还是 esptool.py flash_id。 */
    {
        unsigned cap_code = (id >> 16) & 0xFF;
        if (cap_code >= 0x14 && cap_code <= 0x18) {
            unsigned mb = 1u << (cap_code - 0x14);
            printf("[ota] 按 JEDEC 解码，芯片实际容量约 %u MB（对照 esptool flash_id 确认）\n", mb);
            if (mb * 1024 * 1024 < spi_flash_get_chip_size()) {
                printf("[ota] ⚠⚠ 配置的容量比芯片实际的大！分区表很可能超出物理 flash，\n");
                printf("[ota]     擦除会回绕到低地址，把分区表甚至正在运行的程序擦掉。\n");
            }
        } else {
            printf("[ota] 容量字节 0x%02X 不在常见范围内，请用 esptool flash_id 核对\n", cap_code);
        }
    }
}

/* ======================= 配置 ======================= */

/* 固件下载地址。

   192.168.137.1 —— 电脑在"移动热点"网段上的地址，和 tcp_client.c 里写的一致。
   8000          —— PC 上那条 `python -m http.server 8000` 的端口。
   /esp-01S.bin  —— 必须是 build/esp-01S.bin，也就是【应用镜像】。
                    别指向 flash 时那种合并镜像，那个开头是 bootloader，不是 app。 */
#define OTA_URL   "http://192.168.137.1:8000/esp-01S.bin"

/* ⚠⚠⚠  安全开关  ⚠⚠⚠

   0 = 只把固件写进备用槽，【不】切换启动分区，也【不】重启   ← 默认，先跑通用这个
   1 = 写完且校验通过后，切换启动分区并重启，新固件真正生效

   第一次调试务必先用 0。因为本 SDK 没有回滚（见 ota.h），
   set_boot_partition 调下去就没有退路。
   确认串口打出"✓ 校验通过"之后，再改成 1，重新编译烧录一次，
   之后再触发 OTA 才会真的升级。 */
#define OTA_SWITCH_BOOT   0

/* 任务栈大小（单位：字节）。

   ⚠ 别照抄 tcp_client.c 里的 4096 —— 那个任务不碰 HTTP 协议栈。

   esp_http_client 要在【这条栈】上跑 URL 解析、拼请求头、读 HTTP 状态行，
   再加上 esp_ota_* 自己的调用，4KB 不够。SDK 自带的
   examples/system/ota/simple_ota_example 用的就是 8192，本模块保持一致。

   ── 顺带说一个容易搞混的点 ──────────────────────────────────
   FreeRTOS 的 xTaskCreate() 第 3 个参数，标准文档说的是"字的个数，不是字节数"
   （见 include/freertos/task.h:266），tasks.c 里也是这么算的：
       pvPortMalloc( usStackDepth * sizeof( StackType_t ) )

   但 ESP8266 这个 port 把 StackType_t 直接定义成了 uint8_t
   （见 port/esp8266/include/freertos/portmacro.h:67 的 portSTACK_TYPE），
   于是 sizeof(StackType_t) == 1 —— "字的个数" 正好等于 "字节数"。

   所以这里写 8192，就是实打实的 8192 字节。
   在 ESP32 的 ESP-IDF 上也是字节，但那边【不是】靠这个办法实现的，
   别把两边的理由记混了。 */
#define OTA_TASK_STACK    8192
#define OTA_TASK_PRIO     5

/* 每次从网络读多少字节再往 flash 写。

   sdkconfig 里 CONFIG_OTA_BUF_SIZE = 256 是 SDK 例程的保守值；
   这里用 1024，write 次数少 4 倍，能快不少。

   注意这只是"一次读多少"，跟 flash 写入的对齐【无关】——
   spi_flash_write 内部有 bounce buffer，地址/源指针/长度不对齐它自己处理
   （见 components/spi_flash/src/spi_flash.c 里 NOT_ALIGN 那一段判断）。
   所以不需要迁就 4 字节边界。 */
#define OTA_RECV_BUF      1024

/* ======================= 模块内部状态 ======================= */

static volatile bool          s_running        = false;
static ota_before_restart_t   s_before_restart = NULL;

void ota_set_before_restart(ota_before_restart_t cb)
{
    s_before_restart = cb;
}

/* ======================= 干活的任务 ======================= */

static void ota_task(void *arg)
{
    /* 所有变量都在开头声明：这个函数里到处都是 goto cleanup，
       在 C 里从声明前面跳过去虽然合法，但很容易读错作用域。 */
    esp_http_client_config_t cfg        = { .url = OTA_URL, .timeout_ms = 15000 };
    esp_http_client_handle_t client     = NULL;
    const esp_partition_t   *running    = NULL;
    const esp_partition_t   *part       = NULL;
    const esp_app_desc_t    *desc       = NULL;
    esp_ota_handle_t         ota        = 0;
    char                    *buf        = NULL;
    size_t                   begin_size = OTA_SIZE_UNKNOWN;
    uint32_t                 part_end   = 0;
    uint32_t                 chip_end   = 0;
    bool                     ota_opened = false;
    int                      content_len = 0;
    int                      written     = 0;
    int                      last_pct    = -1;
    int                      last_64k    = -1;
    esp_err_t                err;

    (void)arg;

    printf("\n=============== OTA 开始 ===============\n");

    ota_print_flash_id();
    OTA_DIAG("task start");

    /* ---- 第 0 步：先看清楚"从哪来、到哪去" ---- */

    running = esp_ota_get_running_partition();
    desc    = esp_ota_get_app_description();
    if (running != NULL) {
        printf("[ota] 当前运行 : %s @ 0x%06X\n",
               running->label, (unsigned)running->address);
    }
    if (desc != NULL) {
        /* 固件里烧死的【编译日期时间】。这是验证"OTA 到底有没有生效"
           最直接的证据：升级后重启，这里应该变成新固件编译时的时间。 */
        printf("[ota] 当前固件 : 编译于 %s %s\n", desc->date, desc->time);
    }

    part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        /* ⚠ 本项目【正常情况下就会走到这里】—— 这是预期行为，不是故障。

           本板是 1MB flash，分区表 partitions_1mb.csv 里只有一块
           "app, factory" 槽，没有 ota_0/ota_1。OTA 需要【另一块】空地
           来放新固件，而 1MB 里塞不下两份 606KB 的固件
           （Espressif 官方的 1MB 双槽表每槽只有 448KB，更小）。

           注意这里【不会】崩：esp_ota_get_next_update_partition() 找不到
           ota_* 子类型的分区时是干净地返回 NULL（见 esp_ota_ops.c:637
           的 return default_ota），不是断言失败。

           代码留着是为了以后换 2MB/4MB 模块时能直接接上 ——
           那时候把分区表换成带 ota_0/ota_1 的版本就行，本文件一行不用改。 */
        printf("[ota] ✗ 本固件没有备用 app 槽（1MB 单槽分区表），OTA 用不了\n");
        printf("[ota]   换固件请插串口线重新烧录。\n");
        printf("[ota]   换 WiFi 热点不用 OTA —— 发 TCP 命令 wifi <SSID>,<密码>，\n");
        printf("[ota]   或者等它连不上 60 秒自动进配网。\n");
        goto cleanup;
    }
    printf("[ota] 写入目标 : %s @ 0x%06X (%u 字节)\n",
           part->label, (unsigned)part->address, (unsigned)part->size);
    printf("[ota] 下载地址 : %s\n", OTA_URL);

    /* 目标分区的末尾 vs 系统声称的 flash 末尾。

       ⚠ 这道检查只能挡住"分区表超过了 sdkconfig 里写的容量"这一种情况。
         它挡不住"配置的容量超过芯片实际容量" —— 因为 spi_flash_get_chip_size()
         返回的就是配置值本身，自己跟自己比永远相等。
         后者只能靠上面读出来的 flash ID 判断。 */
    part_end = part->address + part->size;
    chip_end = (uint32_t)spi_flash_get_chip_size();
    if (part_end > chip_end) {
        printf("[ota] ✗ 分区 [%s] 结束于 0x%06X，超出 flash 容量 0x%06X\n",
               part->label, (unsigned)part_end, (unsigned)chip_end);
        printf("[ota]   分区表比 flash 还大，擦除会跑到不存在的地址上。先改分区表。\n");
        goto cleanup;
    }

    /* ---- 第 1 步：HTTP GET，把固件拉下来 ---- */

    client = esp_http_client_init(&cfg);
    if (client == NULL) {
        printf("[ota] ✗ esp_http_client_init 失败\n");
        goto cleanup;
    }

    /* 第二个参数是【请求体】长度：GET 没有请求体，传 0 */
    if (esp_http_client_open(client, 0) != ESP_OK) {
        printf("[ota] ✗ 连不上服务器\n");
        printf("[ota]   逐条检查：\n");
        printf("[ota]     PC 上 cd build 然后 python -m http.server 8000 起了吗\n");
        printf("[ota]     Windows 防火墙放行了吗（第一次运行会弹窗）\n");
        printf("[ota]     PC 的 IP 还是 192.168.137.1 吗\n");
        goto cleanup;
    }
    OTA_DIAG("http open");

    /* 这句才会真正读回 HTTP 状态行和响应头。
       ⚠ 必须【在它之后】查状态码，之前查拿到的是初值 -1。 */
    content_len = esp_http_client_fetch_headers(client);
    printf("[ota] HTTP 状态 %d, Content-Length %d\n",
           esp_http_client_get_status_code(client), content_len);
    OTA_DIAG("http headers");   /* ← 这一步通常是【栈用得最狠】的地方 */

    if (esp_http_client_get_status_code(client) != 200) {
        /* 这一步检查很值得加：不加的话 404 页面会被当成固件往下写，
           直到 esp_ota_write 发现首字节不是 0xE9 才报错，
           报出来的是"镜像无效"，跟真正的原因（文件名写错了）差着十万八千里。 */
        printf("[ota] ✗ 服务器没返回 200 —— 文件名写错了？还是 build 目录不对？\n");
        goto cleanup;
    }

    /* ---- 第 2 步：esp_ota_begin —— 擦除 ----

       begin 会把目标分区【擦掉】。擦多少，取决于第二个参数：

         传 OTA_SIZE_UNKNOWN  → 擦【整块】992KB
         传确切长度           → 只擦这么多

       我们现在已经从 Content-Length 知道确切大小了，所以只擦 449KB 左右，
       比全擦快一倍多。

       两种情况下退回 OTA_SIZE_UNKNOWN：
         · 服务器用 chunked 编码 —— Content-Length 读出来是 0
         · 长度比分区还大 —— 那本来也装不下，交给 SDK 去报错 */
    if (content_len > 0 && (uint32_t)content_len <= part->size) {
        begin_size = (size_t)content_len;
    }

    err = esp_ota_begin(part, begin_size, &ota);
    if (err != ESP_OK) {
        printf("[ota] ✗ esp_ota_begin 失败: %s\n", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_PARTITION_CONFLICT) {
            printf("[ota]   意思是【目标槽 == 正在运行的槽】，不能原地改自己\n");
        }
        goto cleanup;
    }
    ota_opened = true;
    printf("[ota] 正在擦除并写入，请稍候（几百 KB 要几十秒）...\n");
    OTA_DIAG("ota_begin");      /* ← 上一次就是在这句【之后】断的 */

    /* ---- 第 3 步：收一块、写一块 ---- */

    buf = malloc(OTA_RECV_BUF);
    if (buf == NULL) {
        printf("[ota] ✗ 内存不够，%d 字节都分配不出来\n", OTA_RECV_BUF);
        goto cleanup;
    }

    while (1) {
        /* 返回 0 表示服务器把连接关了，也就是数据发完了。
           它【不保证】一次读满 OTA_RECV_BUF，读到多少算多少。 */
        int n = esp_http_client_read(client, buf, OTA_RECV_BUF);

        if (n < 0) {
            printf("\n[ota] ✗ 读取中断，连接断了\n");
            goto cleanup;
        }
        if (n == 0) {
            break;
        }

        err = esp_ota_write(ota, buf, (size_t)n);
        if (err != ESP_OK) {
            printf("\n[ota] ✗ esp_ota_write 失败: %s\n", esp_err_to_name(err));
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                printf("[ota]   固件头几个字节不是 0xE9 —— 下到的不是镜像\n");
                printf("[ota]   多半是下成了网页（404 页 / 目录列表页）\n");
            }
            goto cleanup;
        }

        written += n;

        /* 进度。每 5% 打一行，不然几百行会把串口刷爆。 */
        if (content_len > 0) {
            int pct = (int)((long)written * 100 / content_len);
            if (pct / 5 != last_pct / 5) {
                last_pct = pct;
                printf("[ota] %3d%%  (%d / %d 字节)\n", pct, written, content_len);
            }
        } else {
            int blocks = written / (64 * 1024);
            if (blocks != last_64k) {
                last_64k = blocks;
                printf("[ota] 已写入 %d KB\n", written / 1024);
            }
        }
    }

    /* 长度对不上就直接放弃，别把这个半截固件当成成功。
       走到 esp_ota_end 才失败的话，报错信息没那么直白。 */
    if (content_len > 0 && written != content_len) {
        printf("[ota] ✗ 只收到 %d / %d 字节，下载不完整，放弃\n",
               written, content_len);
        goto cleanup;
    }
    printf("[ota] 下载完成，共 %d 字节\n", written);
    OTA_DIAG("write done");     /* ← 这是全程最后一次采样，stack_low 就是最终峰值 */

    /* ---- 第 4 步：esp_ota_end —— 校验 ----

       它会检查镜像头、段表、校验和。
       ⚠ 调用之后 handle 就失效了（不管成功还是失败），所以先清标志，
         免得 cleanup 里又调一次。 */
    err = esp_ota_end(ota);
    ota_opened = false;
    if (err != ESP_OK) {
        printf("[ota] ✗ 校验不通过: %s\n", esp_err_to_name(err));
        printf("[ota]   固件本身是坏的。启动分区【没有】被动过，重启还是跑原来的。\n");
        goto cleanup;
    }
    printf("[ota] ✓ 校验通过，%s 里已经躺着一份完整的固件\n", part->label);

    /* ---- 第 5 步：切换启动分区 ---- */

#if OTA_SWITCH_BOOT

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        printf("[ota] ✗ 切换启动分区失败: %s\n", esp_err_to_name(err));
        printf("[ota]   固件是好的，只是没生效。重启还是跑原来的。\n");
        goto cleanup;
    }
    printf("[ota] ✓ 启动分区已切到 %s\n", part->label);
    printf("\n=========== OTA 成功，即将重启 ===========\n");

    /* 重启前先把 GPIO0 拉高 —— 否则复位瞬间会被采样成低电平，
       芯片进 UART 下载模式，程序根本不跑。见 ota.h 的说明。 */
    if (s_before_restart != NULL) {
        s_before_restart();
    }
    vTaskDelay(pdMS_TO_TICKS(300));   /* 留点时间把串口日志吐完 */
    esp_restart();

#else

    printf("\n========== OTA 成功（安全模式）==========\n");
    printf("[ota] OTA_SWITCH_BOOT = 0：\n");
    printf("[ota]   固件已经写进 %s 了，但【没有】切换启动分区。\n", part->label);
    printf("[ota]   现在重启的话，跑的还是 %s，看不出任何变化。\n",
           (running != NULL) ? running->label : "原来那块");
    printf("[ota]\n");
    printf("[ota] 确认这一步没问题后，把 ota.c 顶部的 OTA_SWITCH_BOOT 改成 1，\n");
    printf("[ota] 重新编译 + 烧录一次，之后再触发 OTA 才会真的升级。\n");
    printf("[ota]\n");
    printf("[ota] ⚠ 改 1 之前先想清楚：本 SDK 没有回滚，\n");
    printf("[ota]   新固件起不来就是无限重启，只能串口线救。\n");

#endif

cleanup:
    OTA_DIAG("cleanup");

    /* 中途失败时 handle 还开着，得还给 SDK */
    if (ota_opened) {
        esp_ota_end(ota);
    }
    if (buf != NULL) {
        free(buf);
    }
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    s_running = false;
    vTaskDelete(NULL);
}

/* ======================= 对外接口 ======================= */

bool ota_start(void)
{
    if (s_running) {
        printf("[ota] 上一次升级还在跑，这次请求忽略\n");
        return false;
    }
    s_running = true;

    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK, NULL,
                    OTA_TASK_PRIO, NULL) != pdPASS) {
        s_running = false;
        printf("[ota] ✗ 创建 OTA 任务失败 —— 8KB 栈都分配不出来，内存太紧了\n");
        return false;
    }
    return true;
}
