/**
  * @file    wifi_sta.c
  * @brief   WiFi STA（客户端）模块 —— 凭据读写、断线重连、状态上报
  *
  * 这个文件管两件事：
  *
  *   ① 把热点名/密码存进 NVS，开机先读它（而不是焊死在代码里）
  *   ② 收到 TCP 命令就换热点，立刻重连
  *
  * 以前连不上会自己进 SmartConfig，让手机 App 把凭据编成一串广播包发出来。
  * 那条路已经整个拆掉，来龙去脉记在 README §9.7。
  *
  * @warning 如果主节点的 SSID/密码改了，而你没先告诉这个节点，
  *          它就再也回不来，只能拆下来重烧。所以改主节点凭据的顺序必须是：
  *
  *              ① 先发 TCP 命令  wifi \<新SSID\>,\<新密码\>   （节点还连得上时）
  *              ② 再改主节点
  *
  *          反过来做就得重烧固件。（或者是增加ap配网的功能，手机连esp8266热点，通过网页来改密码）
  *
  * @see     README §9.6（换热点的操作纪律）、§9.7（连不上了怎么查）
  */

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

/* ======================= 出厂默认凭据 ======================= */

/**
  * @brief    出厂默认热点名
  *
  * @note     定位：这【不再】是唯一的凭据来源，而是"三个来源里最低的那一层"。
  *           优先级：NVS > 这里 > 内置默认。
  *
  *           第一次上电时 NVS 是空的，就用这里，并顺手写进 NVS；
  *           之后不管用什么方式改过密码，都以 NVS 为准，这几行就不再生效了。
  *
  *           所以改热点最省事的办法不是改这里重烧固件，而是：
  *             - 节点还连得上   → 发一条 TCP 命令 wifi \<SSID\>,\<密码\>
  *             - 节点已经连不上 → 只能改这里重烧（没有别的路了，见文件头）
  *
  * @warning  SSID 区分大小写，而且【空格也算一个字符】—— 写错不会报错，
  *           只会一直连不上（串口上会看到 reason=201）。
  *
  * @warning  ESP8266 只支持 2.4GHz。热点开在 5GHz 上芯片根本扫不到，
  *           表现和"SSID 写错"一模一样。
  *
  * @note     这个值必须和 ESP32 主节点上那个 SoftAP 【逐字一致】。两边是
  *           各自独立编译的，编译器不会帮你核对（见 README §9.2 的四值表）。
  */
#define WIFI_SSID_DEFAULT       "ESP32-S3-host"

/**
  * @brief    出厂默认密码
  *
  * @warning  不能少于 8 字节 —— WPA2 的下限，短了 ESP32 那边拒绝开 AP。
  *
  * @note     主节点 ESP32 的 ap.authmode 必须选 WPA2_PSK。本文件里
  *           threshold.authmode 设的就是 WPA2_PSK（见 apply_credentials()），
  *           主节点开成【开放式】热点反而连不上。
  */
#define WIFI_PASSWORD_DEFAULT   "88888888"

/* ======================= NVS ======================= */

/**
  * @brief    NVS 命名空间
  *
  * @note     NVS = Non-Volatile Storage，芯片 flash 里划出来的一小块键值存储区
  *           （本项目分区表里的 nvs 分区，24KB）。
  *
  *           它跟"文件系统"不是一回事：没有目录、没有文件，
  *           只有一个 (命名空间, 键) → 值 的映射。用起来像个小字典。
  *
  * @note     用 "app_cfg" 是为了避开 WiFi 驱动自己用的那些
  *           （驱动把 PHY 校准数据存在自己的命名空间里，两者互不干扰）。
  */
#define NVS_NAMESPACE   "app_cfg"

/** @brief  NVS 里存热点名的键 */
#define NVS_KEY_SSID    "ssid"

/** @brief  NVS 里存密码的键 */
#define NVS_KEY_PASS    "pass"

/** @brief 802.11 协议规定的 SSID 上限（字节，不含结尾 '\0'） */
#define SSID_MAX_LEN    32

/** @brief 802.11 协议规定的密码上限（字节，不含结尾 '\0'） */
#define PASS_MAX_LEN    64

/* ======================= 重连参数 ======================= */

/**
  * @brief    没连上时，每隔多久在串口上吭一声（毫秒）
  *
  * @note     重连本身【不靠这个值】—— wifi_event_handler() 收到"断开"事件就直接
  *           esp_wifi_connect() 了，不需要定时器。这个值只控制报平安的频率：
  *           不然连不上时日志会一片安静，看起来像死机。
  *
  * @note     30 秒 ≈ Wi-Fi 扫一遍全部 13 个信道再加认证超时，大概就是这个量级。
  */
#define WIFI_DOWN_REPORT_MS   30000

/* ======================= 模块内部状态 ======================= */

/** @brief WiFi 事件组。用来在"事件回调"和"等待的任务"之间传递状态。 */
static EventGroupHandle_t s_wifi_event_group;

/**
  * @brief 事件位：拿到 IP 了（真的能通信了）
  */
#define WIFI_GOT_IP_BIT   BIT0

/**
  * @brief 事件位：掉线过一次（用来把管理任务从"已连上"的睡眠里叫醒）
  */
#define WIFI_DOWN_BIT     BIT1

/**
  * @brief 当前生效的热点名
  *
  * @note  放在这里而不是每次去读 NVS，是因为事件回调里要打印它，
  *        而 NVS 读操作不该在回调里频繁做。
  *
  * @note  比协议上限多一个字节，留给结尾的 '\0'。
  */
static char s_ssid[SSID_MAX_LEN + 1];

/** @brief 当前生效的密码。容量说明同 s_ssid。 */
static char s_pass[PASS_MAX_LEN + 1];

/**
  * @brief 连接重试计数
  *
  * @note  除了在串口上区分"第几次失败"，管理任务还会把它打出来 ——
  *        数字一直涨就是真的在原地打转，该去查 SSID / 密码 / 频段了。
  */
static int s_retry = 0;

/* ======================= 小工具 ======================= */

/**
  * @brief    把 src 拷进定长缓冲区，超长就截断，并保证以 '\0' 结尾。
  *
  * @param[out] dst       目标缓冲区
  * @param[in]  dst_size  目标缓冲区的总容量（含留给 '\0' 的那一个字节）
  * @param[in]  src       源字符串
  *
  * @note     为什么不用 strncpy()？
  *           strncpy 在源串长度 >= 目标缓冲区时【不补 '\0'】，
  *           结果是一个没有结尾的字符串 —— 后面 strlen/printf 会一路读下去越界。
  *           这是 C 里最经典的坑之一，干脆自己写一个语义明确的。
  */
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

/**
  * @brief    从 NVS 读凭据到 s_ssid / s_pass
  *
  * @retval   true   读到了（两个键都在，而且 SSID 不是空串）
  * @retval   false  没读到，或者读到的 SSID 是空串
  *
  * @note     读出来的值【直接写进模块自己的】s_ssid / s_pass，不从参数返回 ——
  *           这两个缓冲区是 wifi_sta.c 的私事，外面只经 wifi_sta.h 那几个函数看它。
  *
  * @note     【失败时不会清空缓冲区】。nvs 的规矩是出错就不碰 out_value
  *           （nvs.h:401：In case of any error, out_value is not modified），
  *           所以读到一半失败时，s_ssid 里可能还留着上一次留给它的内容。
  *           这正是调用方 wifi_sta_init() 必须在 false 分支里把【两个】缓冲区
  *           都改成默认值的原因 —— 否则会拿"SSID 是新的、密码是旧的"这种
  *           半新半旧的凭据去连，然后在串口上对着一个正确的 SSID 怀疑人生。
  */
static bool nvs_load_credentials(void)
{
    nvs_handle_t h;
    size_t       len;
    esp_err_t    err;

    /* 只读方式打开 —— 这一步顺便就是"这个节点配过没有"的判据。

       命名空间不存在时，NVS_READONLY 直接返回 ESP_ERR_NVS_NOT_FOUND：
       nvs_partition_manager.cpp:200 传下去的 canCreate 写的是
       "open_mode == NVS_READWRITE"，只读就是 false；nvs_storage.cpp:407-409
       查不到这个名字、canCreate 又是 false，当场返回 NOT_FOUND。
       第一次上电正是这种情况 —— 所以这里 return false 去走默认值那条路，
       这不是错误，串口上什么都不用打。

       为什么不用 NVS_READWRITE？那样 open 会【顺手把命名空间建出来并写进
       flash】（nvs_storage.cpp:427 的 writeItem(Page::NS_INDEX, ...)）。
       一个纯读的函数不该有写副作用，而且那会把"命名空间在不在"这个信息抹掉 ——
       它现在正好是"配过 / 没配过"最干脆的区别。

       ⚠ 别把这里的 false 一律读成"没配过"：忘了 nvs_flash_init() 时
         open 会返回 ESP_ERR_NVS_NOT_INITIALIZED，也走同一条 return false，
         表现和"NVS 是空的"一模一样。查"明明存过却读不出来"时，
         光看返回值不够，得把 err 打出来。 */
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    /* nvs_get_str 的 length 参数是【传入缓冲区大小，传出实际长度】，三种走法
       都在 nvs_api.cpp:493-504：
           进来时     = 我这边能放多少字节
           成功时     = 这个值实际占了多少字节（含结尾的 '\0'，见 nvs.h:409-410）
           装不下时   = 它需要多少字节，同时返回 ESP_ERR_NVS_INVALID_LENGTH

       两个后果：
         ① 每次调用前都要重新赋 sizeof(...)，不能把上一次写回来的值再用一遍
            —— 它是实际大小，不是缓冲区大小，第二次传进去就是"我只有这么点地方"；
         ② 缓冲区给大了没关系，多出来的字节不会被动。
       ① 是本函数自己必须守的纪律；② 由上游保证：写 NVS 之前
       wifi_sta_set_credentials() 已经把长度卡在 SSID_MAX_LEN / PASS_MAX_LEN
       以内（它开头那两段长度检查），而这两个缓冲区是它们 +1（多出来的那一个
       留给 '\0'），所以一定装得下。 */
    len = sizeof(s_ssid);
    err = nvs_get_str(h, NVS_KEY_SSID, s_ssid, &len);

    if (err == ESP_OK) {
        /* 三次调用共用一个 err，所以末尾那个 err 是【最后执行到的那一步】的结果。
           SSID 这一步没成功的话，密码那一步根本不会执行 ——
           函数直接带着 SSID 的错误码返回 false（两个键要么都读，要么都不算数）。 */
        len = sizeof(s_pass);
        err = nvs_get_str(h, NVS_KEY_PASS, s_pass, &len);
    }

    /* 外面那个提前 return 时句柄还没开出来，没东西要关；走到这里就已经开出来了，
       所以无论成败都得关。nvs_close() 干的事是把句柄从 NVS 的句柄表里摘掉
       （nvs_partition_manager.cpp:216-224），不关的话每读一次凭据就漏一个。 */
    nvs_close(h);

    /* 两个键都读到才算数：err == ESP_OK。

       s_ssid[0] != '\0' 是第二道，防御性的 —— 正常路径到不了这儿，因为两个写入口
       （main.c 的 cmd_do_wifi() 和本文件的 wifi_sta_set_credentials()）都已经
       拒收空 SSID 了。但如果 NVS 里的值不是这版固件写的（老固件、别的程序），
       空串就会一路走到 esp_wifi_set_config()，然后拿去连 —— 结果是永远连不上，
       而且 reason 还是 201，跟"SSID 拼错"长得一模一样。与其这样，不如当场
       当成"没配过"，退回出厂默认值。 */
    return (err == ESP_OK && s_ssid[0] != '\0');
}

/**
  * @brief    把凭据写进 NVS
  *
  * @param[in] ssid      热点名
  * @param[in] password  密码
  *
  * @retval   true   写入成功（已落盘）
  * @retval   false  打开 NVS 或写入失败，串口上会打原因
  *
  * @note     这里【只写 NVS】，不碰当前连接 —— 所以 wifi_sta_init() 里
  *           写默认值的时候也能安全调用。改连接是 wifi_sta_set_credentials() 的事。
  */
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
        /* ⚠ 这一句在【本 SDK（v3.4）上是空操作】—— 原注释说"set 只写进内存缓存、
           这一句才真正落到 flash"，那句话在别的 SDK 版本上成立，在这里不成立：

             nvs_set_str()  → NVSHandleSimple::set_string()
                            → mStoragePtr->writeItem(...)   ← 当场落盘，没有中间缓存
             nvs_commit()   → NVSHandleSimple::commit()
                            → if (!valid) return ERR; return ESP_OK;  ← 只检查句柄

           而且 SDK 自己在 nvs_commit() 里留了注释（nvs_api.cpp:390）：
             // no-op for now, to be used when intermediate cache is added

           那为什么还留着这一句？
             ① 它是 API 契约的一部分：ESP-IDF v4+ 的 NVS 真的加了缓存，
                到那边漏掉 commit 就是"掉电丢失，而且当场看不出任何异常"；
             ② 代码是要移植的，习惯要在没有代价的时候就养对。
           来龙去脉见 学习笔记/ESP8266-NVS.md §3.3。 */
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

/**
  * @brief    把 s_ssid / s_pass 灌进 WiFi 驱动
  *
  * @note     只改配置，不负责连接。真正发起连接的是 esp_wifi_connect()。
  */
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

/**
  * @brief    WiFi / IP 事件回调
  *
  * @param[in] arg         注册时传进来的参数，本模块没用（注册时传的 NULL）
  * @param[in] event_base  事件大类：WIFI_EVENT 或 IP_EVENT
  * @param[in] event_id    具体是哪个事件
  * @param[in] event_data  事件携带的数据，类型由 event_id 决定
  *
  * @note     它由 esp_event 的默认事件循环任务调用，【不在调用者的任务里跑】。
  *           也正因为如此，这里绝对不能做阻塞操作（比如 vTaskDelay），
  *           否则整个 WiFi 状态机都会停摆。
  *
  * @note     事件分两大类，靠 event_base 区分：
  *             WIFI_EVENT —— WiFi 状态变化（启动、关联上 AP、断开……）
  *             IP_EVENT   —— 网络层事件（拿到 IP……）
  *           event_data 的类型由事件本身决定，所以要自己按 event_id
  *           把它转成对应的结构体指针。
  */
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

/* ======================= 管理任务 ======================= */

/**
  * @brief    状态上报任务
  *
  * @param[in] arg  任务参数，本模块没用
  *
  * @note     这条任务只干一件事：没连上的时候，每隔 WIFI_DOWN_REPORT_MS
  *           在串口上吭一声，从不退出。
  *
  * @note     为什么要单独一条任务？因为"等一段时间"本质上是阻塞的，
  *           而 wifi_event_handler() 跑在系统的事件循环任务上 ——
  *           在那里阻塞，整个 WiFi 状态机就停摆了。
  *
  * @warning  重连本身【不在这里】：wifi_event_handler() 收到"断开"事件就直接
  *           esp_wifi_connect() 了。所以这条任务挂掉不影响重连，只会让日志变哑。
  *
  * @note     （以前它还负责"连不上超过一段时间就进 SmartConfig"，
  *           那条路整个拆掉了，见文件头。）
  */
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

/**
  * @brief    初始化 WiFi，读凭据，连上热点
  *
  * @note     接口说明见 wifi_sta.h。这里记的是实现顺序上的讲究。
  *
  * @warning  本函数【不阻塞】：esp_wifi_start() 是异步的，它返回时还没连上，
  *           连接结果由事件循环在后台的 WiFi 任务里通过 wifi_event_handler()
  *           打印出来。要等"真的拿到 IP"，请调 wifi_sta_wait_ip()。
  */
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

/**
  * @brief    等 WiFi 拿到 IP
  *
  * @param[in] timeout_ms  超时毫秒数；传 UINT32_MAX 表示一直等
  *
  * @retval   true   拿到 IP 了
  * @retval   false  超时了
  *
  * @note     多个任务可以同时在这里等，本函数【不清除】事件位，
  *           所以不影响别人。接口说明见 wifi_sta.h。
  */
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

/**
  * @brief    换凭据：落盘 + 立刻切到新热点
  *
  * @param[in] ssid      新的热点名，长度 1~SSID_MAX_LEN
  * @param[in] password  新的密码，长度 <= PASS_MAX_LEN
  *
  * @retval   true   凭据已存进 NVS 并开始切换
  * @retval   false  参数不合法或写 NVS 失败，【当前连接不受影响】
  *
  * @note     被 main.c 里那条 wifi 命令调用。接口说明见 wifi_sta.h。
  *
  * @warning  长度必须在【存进 NVS 之前】检查。超长的 SSID 被截断后照样能写进
  *           NVS，但永远连不上 —— 那种"命令说成功了、就是连不上"的毛病最难查。
  */
bool wifi_sta_set_credentials(const char *ssid, const char *password)
{
    size_t n;

    if (ssid == NULL || password == NULL) {
        return false;
    }

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

/**
  * @brief    读出当前生效的热点名
  *
  * @return   指向模块内部静态缓冲区的指针
  *
  * @warning  不要 free()，也不要长期保存 —— 它会在换凭据时被就地改写。
  *           接口说明见 wifi_sta.h。
  */
const char *wifi_sta_get_ssid(void)
{
    return s_ssid;
}
