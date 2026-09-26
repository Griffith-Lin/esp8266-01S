/**
  * @file    main.c
  * @brief   ESP-01S 继电器控制 —— 主业务
  *
  * 这个文件只放"业务"：继电器怎么用、收到什么命令该做什么、程序怎么把模块串起来。
  * 联网的细节都被挡在两个模块后面了：
  *
  *   - wifi_sta.c     连热点、把凭据存 NVS、断线重连
  *   - tcp_client.c   连服务端、收发字节（纯传输，不知道"开灯"是什么）
  *   - udp_client.c   不建连接、直接丢数据报（同样纯传输）
  *
  * 两个传输模块是【二选一】的，同一时刻只有一条在收。切哪条由下面
  * 「链路切换」那一节决定，命令是 net tcp / net udp。
  *
  * 曾经还有一个 ota.c 负责远程升级，已经移出本工程 —— 本板 1MB 单槽，
  * 两个 app 槽放不下这个固件。见 ../ota-for-larger-flash/。
  *
  * @par 换 WiFi 热点
  *
  *   节点还连得上   → 发 TCP 命令：wifi \<SSID\>,\<密码\>
  *
  *   wifi_sta.c 顶部那两个宏只是【出厂默认值】，NVS 里有凭据就以 NVS 为准。
  *
  * @warning 节点一旦连不上，就【没有】远程通道了 —— 只能拆下来重烧。
  *
  *   以前这里有一条 SmartConfig 兜底（手机 App 把密码编成广播包发出来，
  *   节点在空口上收），那条路已经整个拆掉。理由和实测数据见 README §9.7：
  *   它的链路太长（手机 WiFi 驱动的广播行为 → 路由器 → ESP 的混杂模式），
  *   任何一环不配合就死，而且【没有反馈】，串口上什么都看不出来。
  *
  *   所以改主节点凭据的顺序必须是：
  *     ① 先发 TCP 命令 wifi \<新SSID\>,\<新密码\>
  *     ② 再改主节点
  *   反过来做，这块板子就只能重烧了。
  *
  * @warning "改配置"和"换固件"是两件事，别混。
  *   改 WiFi 密码靠上面那条 TCP 命令；换固件只能插串口线重烧。
  *
  * @note  要改服务端 IP/端口  → tcp_client.c 顶部
  * @note  要改能识别的命令    → 本文件下面"命令解析"那一节
  *
  * @par 硬件相关
  *
  *   GPIO0 驱动继电器。
  *     开灯 = GPIO0 拉高 = RELAY_ON
  *     关灯 = GPIO0 拉低 = RELAY_OFF
  *
  * @warning GPIO0 是 ESP8266 的启动模式选择脚（strapping pin）：
  *   复位时必须为高电平，芯片才会从 Flash 启动；
  *   复位时为低电平，芯片会进入 UART 下载模式，程序根本不会运行。
  *
  *   还要注意：复位采样那一刻，GPIO0 是【输入 / 高阻态】——芯片只是"读"这根线，
  *   并不会"输出"高电平。让它被读成高的，是上拉电阻，而上拉电阻不是驱动器：
  *   它只能提供几十 µA，一旦外接电路（比如继电器模块的输入端）把线拉低，
  *   弱上拉就扛不住，芯片每次复位都会掉进下载模式。
  *   所以真正决定命运的是【挂在这根线上的外部电路】，不是程序里写 0 还是 1。
  *
  *   ESP-01S 上引出来的脚只有 GPIO0 / GPIO1 / GPIO2 / GPIO3，其中 GPIO1 和
  *   GPIO3 是串口的 TX/RX，GPIO2 在启动时同样要求高电平 —— 这块板子上没有
  *   更"干净"的脚可以换。用 GPIO0 做输出，就必须接受这个约束。
  *
  * @warning 供电：WiFi 发射瞬间电流会冲到 170~300mA，叠加上继电器线圈，
  *   如果还是拿 USB 转串口板那个 3.3V 脚供电，很容易掉电复位（brownout）。
  *   如果模块开始反复重启、串口刷乱码，先怀疑供电，
  *   在 VCC-GND 之间并 100~470µF 电解电容 + 0.1µF 瓷片电容。
  *
  * This example code is in the Public Domain (or CC0 licensed, at your option.)
  */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_spi_flash.h"
#include "driver/gpio.h"

#include "wifi_sta.h"
#include "tcp_client.h"
#include "udp_client.h"

/**
  * @brief 继电器接在 GPIO0 上
  *
  * @note  串口监视器波特率是 74880，不是 115200 —— 那是 ESP8266 ROM
  *        bootloader 的固定输出速率，写成别的会把启动头部变成乱码。
  *
  * @note  运行模式靠 ROM 在复位瞬间读到的电平决定，而**【这个电平是谁给的都行】**：
  *        内部上拉、板上 10k、或者外部驱动器，ROM 不关心。
  *        上拉只是"让悬空脚有个确定值"的手段，它本身不产生驱动能力。
  */
#define RELAY_GPIO   GPIO_NUM_0

/** @brief 开灯：GPIO0 拉高 */
#define RELAY_ON     1

/** @brief 关灯：GPIO0 拉低 */
#define RELAY_OFF    0

/**
  * @brief  把 GPIO0 配成继电器输出，并立刻置成"关灯"状态（上电默认不动作）
  *
  * @note   gpio_config() 只是"使能输出驱动器"，它并不写电平；但驱动器一开，
  *         引脚立刻开始输出【输出寄存器里已有的值】，而该寄存器复位后是 0。
  *         所以 gpio_config() 返回的那一刻 GPIO0 是【低】的 —— 也就是继电器
  *         处于"关"的状态。这正好是我们要的上电默认，所以下面那句
  *         gpio_set_level() 是把这个状态【明确写出来】，而不是在纠正什么。
  */
static void relay_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1 << RELAY_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(RELAY_GPIO, RELAY_OFF);
}

/** @brief 继电器吸合（开灯） */
static void relay_on(void)
{
    gpio_set_level(RELAY_GPIO, RELAY_ON);
    printf("[relay] 开灯 —— GPIO0 拉高\n");
}

/** @brief 继电器释放（关灯） */
static void relay_off(void)
{
    gpio_set_level(RELAY_GPIO, RELAY_OFF);
    printf("[relay] 关灯 —— GPIO0 拉低\n");
}

/* ======================= 链路切换 ======================= */

/**
  * @brief  当前用哪条链路收发
  *
  * @note   全局只有这一个"现在走哪条路"的答案，命令回执也按它选路。
  */
typedef enum {
    LINK_TCP,   ///< 走 tcp_client.c
    LINK_UDP,   ///< 走 udp_client.c
} link_mode_t;

/**
  * @brief 开机默认走 TCP
  *
  * @note  特意保持和"加 UDP 之前"的行为一致 —— 加一个模式不该顺手改变
  *        已经跑通的那条路。想换默认值就改这一行。
  */
static link_mode_t s_link = LINK_TCP;

/**
  * @brief    按当前链路发一段字节
  *
  * @param[in] data  要发的数据
  * @param[in] len   字节数；传 0 表示"data 是 C 字符串，自己算长度"
  *
  * @return   实际发出的字节数；失败返回 -1
  *
  * @note     本文件里所有回执一律调它，【不再直接调 tcp_client_send()】。
  *           漏掉哪一处，那条回执在 UDP 模式下就会往 TCP 发 ——
  *           命令从 UDP 进来了，回复却跑去了另一条路，
  *           发送方会觉得"命令生效了但没回音"。
  */
static int link_send(const char *data, int len)
{
    return (s_link == LINK_UDP) ? udp_client_send(data, len)
                                : tcp_client_send(data, len);
}

/**
  * @brief    切换链路
  *
  * @param[in] mode  要切到哪条
  *
  * @note     【顺序是有意的】：先用【旧】链路把回执发出去，发完才切。
  *           反过来的话，这条回执会走新链路，而发命令的人正在旧链路上
  *           等着看结果 —— 永远等不到。现象就是"发了 net udp 之后就
  *           再没动静了"，很容易误判成板子死了。
  *
  * @warning  这里的 stop() / start() 【不能】改成"等对方任务退出"。
  *           原因：本函数是通过接收回调被调用的，也就是跑在传输任务
  *           【自己】的栈上 —— 去等它退出就是在等自己，直接死锁。
  *           两个 stop() 都只置一个标志就返回，正是为了绕开这一点。
  *
  * @warning  同一时刻【只有一条】链路在收。两条都开着的话，同一条命令
  *           会从两条路各到一次，"开灯"被执行两次、"关灯"也是。
  *           （UDP 那条的心跳包不会造成这个问题：它是我们【发出去】的，
  *             不会绕回自己的接收口。）
  */
static void link_switch(link_mode_t mode)
{
    if (s_link == mode) {
        link_send(mode == LINK_UDP ? "NET already UDP\r\n"
                                   : "NET already TCP\r\n", 0);
        return;
    }

    /* ① 先回执。此刻 s_link 还是旧值，所以走的是旧链路 —— 正是我们要的。 */
    link_send(mode == LINK_UDP ? "NET -> UDP\r\n" : "NET -> TCP\r\n", 0);

    /* ② 再换向，然后停一条、起另一条。
          两个 start() 都是幂等的（模块内部有任务已创建标志），
          所以来回切多少次都不会多出任务来。

          ⚠ 切换【不是瞬时】的：新链路那条任务要等它自己那一轮空转醒来
          （最多 500ms）才会建好 socket 并报到。所以切到 UDP 之后，
          主节点最快也要过半秒左右才发得进来 —— 在那之前它不知道我们在
          这个网段的哪个地址上（UDP 没有连接可建，它只能等我们的第一包心跳）。
          这是 UDP 的固有代价，不是这里偷懒：TCP 没这个问题，因为 TCP 是
          【我们】拨出去，主节点 accept 就知道对端是谁。 */
    s_link = mode;

    if (mode == LINK_UDP) {
        tcp_client_stop();
        udp_client_start();
    } else {
        udp_client_stop();
        tcp_client_start();
    }
}

/* ======================= 命令解析 ======================= */

/**
  * @brief  命令缓冲区大小（字节）
  *
  * @par 坑 1：TCP 是【字节流】，不是消息队列
  *
  *   你在网络调试助手里点一次"发送"，ESP 这边 recv() 收到的可能是：
  *     ① 一次收到完整的 "开灯"            —— 最理想的情况
  *     ② 分两次收到 "开" 和 "灯"          —— 被网络拆包了
  *     ③ 一次收到 "开灯关灯"              —— 你连点了两次，粘在一起了
  *
  *   所以【绝对不能用每次 recv 到的内容直接去比较】，
  *   否则 ②③ 两种情况都会失败，而且现象是"偶尔灵偶尔不灵"，特别难查。
  *
  *   正确做法：把收到的字节先攒进一个缓冲区，再在缓冲区里找关键字。
  *   找到就把那条命令抠掉，剩下的继续留着等下一批字节来拼。
  *
  * @note  坑 1 是【TCP 特有】的，换成 UDP 之后只剩 ③ 这一半：
  *        UDP 保留消息边界，你发一次它收一次，② 那种被拆开的情况不会发生。
  *        但发送方完全可以在一个数据报里塞两条命令（③ 仍然成立），
  *        所以下面这套"攒起来再找关键字"的写法【照旧必须留着】。
  *        UDP 只是帮你省掉了一半的坑，不是全部。
  *
  * @par 坑 2：中文"长什么样"，取决于【谁】把它变成字节
  *
  *   strstr() 比的是【字节】，不是"字"。同样是"开灯"两个字，
  *   在不同编码下的字节完全不同：
  *
  * @code
  *             UTF-8                GBK
  *   开灯      E5 BC 80 E7 81 AF    BF AA B5 C6     ← 6 字节 vs 4 字节
  *   关灯      E5 85 B3 E7 81 AF    B9 D8 B5 C6
  *   配网      E9 85 8D E7 BD 91    C5 E4 CD F8
  * @endcode
  *
  *   谁说了算？
  *     - 本文件里写的 "开灯" 是什么字节 —— 由【这个 .c 文件存成什么编码】决定。
  *       本文件是 UTF-8，所以直接用字面量的话是 6 字节。
  *     - 网络调试助手发出来的是什么字节 —— 由【助手的编码设置】决定。
  *       中文 Windows 上的调试助手大多默认 GBK，也就是 4 字节。
  *
  *   两边对不上，strstr 就永远找不到，现象是：
  *     【串口明明打印"收到 N 字节"，但继电器不动，也没有回复。】
  *
  *   所以下面那张表把两种编码都收进来，另外再给一组纯 ASCII 的 on/off ——
  *   ASCII 没有编码歧义，是最保险的那条路。
  */
#define CMD_BUF_SIZE  64

/** @brief 收到的字节攒在这里，等拼出完整命令 */
static char s_cmd_buf[CMD_BUF_SIZE + 1];   /* +1 留给结尾的 '\0' */

/** @brief s_cmd_buf 里当前有效字节数 */
static int  s_cmd_len = 0;

/**
  * @brief "开灯" 的 UTF-8 字节
  *
  * @note  关键字特意写成【转义字节】，而不是让字面量跟着文件编码走 ——
  *        这样无论本文件存成 UTF-8 还是 GBK，匹配到的都是同一串字节。
  */
#define KEY_KAI_UTF8   "\xE5\xBC\x80\xE7\x81\xAF"

/** @brief "开灯" 的 GBK 字节 */
#define KEY_KAI_GBK    "\xBF\xAA\xB5\xC6"

/** @brief "关灯" 的 UTF-8 字节 */
#define KEY_GUAN_UTF8  "\xE5\x85\xB3\xE7\x81\xAF"

/** @brief "关灯" 的 GBK 字节 */
#define KEY_GUAN_GBK   "\xB9\xD8\xB5\xC6"

/**
  * @brief  一条命令对应一个"动作"
  *
  * @note   原先这里是个 bool turn_on，只装得下"开/关"两种；换成枚举之后，
  *         再来第三种（比如"查询状态"）也只是往下面那张表里加一行、
  *         往 switch 里加一个 case。
  */
typedef enum {
    ACT_ON,       ///< 继电器吸合
    ACT_OFF,      ///< 继电器释放
    ACT_NET_TCP,  ///< 切到 TCP 链路
    ACT_NET_UDP,  ///< 切到 UDP 链路
} cmd_action_t;

/**
  * @brief  命令表里的一行：关键字 → 动作
  */
typedef struct {
    const char   *key;    ///< 关键字（按字节比较）
    int           len;    ///< 关键字的【字节数】—— 中文不是 1 个字 1 个字节
    cmd_action_t  action; ///< 命中后执行哪个动作
} cmd_t;

/**
  * @brief  命令表
  *
  * @warning 这张表是【扫完再决定】的，不是"找到第一条就返回" —— 别改成那样。
  *          规则：在所有命中的关键字里，挑【在缓冲区里位置最靠前】的那一个。
  *
  *          现在表里已经没有互相包含的关键字了，规则仍然留着：
  *          它是正确的通用规则，而下一个加进来的关键字随时可能再踩同一个坑。
  */
static const cmd_t s_cmds[] = {
    { KEY_KAI_UTF8,  6, ACT_ON  },
    { KEY_KAI_GBK,   4, ACT_ON  },
    { KEY_GUAN_UTF8, 6, ACT_OFF },
    { KEY_GUAN_GBK,  4, ACT_OFF },
    { "on",          2, ACT_ON  },   /* ASCII 别名，小写；不区分大小写的版本没做 */
    { "off",         3, ACT_OFF },

    /* 链路切换。故意带上 "net " 前缀，而不是直接叫 "udp" / "tcp" ——
       裸的 "tcp" 太容易在别的句子里撞上，而这两个词是要改设备行为的，
       撞一次就够难受了。
       ⚠ 和 "wifi " 不同，它们【不需要换行结尾】，匹配到就立刻执行，
         这一点和 on / off 一样。 */
    { "net tcp",     7, ACT_NET_TCP },
    { "net udp",     7, ACT_NET_UDP },
};

/**
  * @brief 带参数的命令头：wifi \<SSID\>,\<密码\>
  *
  * @note  它没法放进上面那张表 —— 表里每条都是【定长关键字】，匹配到就能按
  *        固定长度抠掉；而这条后面挂着两个长度不定的参数。
  *        所以单独处理，但它的【先后顺序】仍然要和表里那些一起排
  *        （见 cmd_try_one()），否则 "开灯wifi A,B\n" 的执行顺序就乱了。
  *
  * @note  为什么用逗号而不是空格分隔？因为 SSID 里【可以有空格】——
  *        比如热点叫 "My Home WiFi"，用空格当分隔符就会被劈成两半。
  *
  * @warning 这条和当前默认值无关。本项目现在连的是 ESP32 的 SoftAP
  *          "ESP32-S3-host"，没有空格 —— 但别因为"现在的名字没空格"
  *          就把分隔符改成空格，那等于把这条命令悄悄写窄了。
  */
#define KEY_WIFI       "wifi "

/** @brief KEY_WIFI 的字节数（不含结尾 '\0'） */
#define KEY_WIFI_LEN   5

/**
  * @brief 802.11 协议规定的 SSID 上限（字节）
  *
  * @note  wifi_sta.c 里也有一份，这里重写一遍是因为
  *        那是模块内部的事，不值得为它去动头文件。
  */
#define WIFI_SSID_MAX  32

/** @brief 802.11 协议规定的密码上限（字节） */
#define WIFI_PASS_MAX  64

/** @brief 命令表里有多少条 */
#define CMD_COUNT  (sizeof(s_cmds) / sizeof(s_cmds[0]))

/**
  * @brief    处理一条 wifi 命令
  *
  * @param[in] p  指向缓冲区里找到的 "wifi "
  *
  * @retval   true   这一行处理完了（不管成功失败），已经从缓冲区里抠掉，调用方继续
  * @retval   false  【行还没收全】，什么都没做，等下一批字节
  *
  * @note     返回值跟别的命令不一样，要留意。
  *
  * @note     为什么必须等到整行？因为 TCP 是字节流，一次 recv 可能只到
  *           "wifi DESKTOP" 就断了，这时候后面的密码还没来。
  *           硬猜的话会把半截 SSID 当成真的存进 NVS。
  *
  * @note     所以规矩是：以换行结尾。网络调试助手发的时候记得勾"发送新行"。
  */
static bool cmd_do_wifi(char *p)
{
    char   *nl;
    char   *comma;
    char   *next;
    char    ssid[WIFI_SSID_MAX + 1];
    char    pass[WIFI_PASS_MAX + 1];
    size_t  n;
    bool    valid = true;

    nl = strpbrk(p, "\r\n");
    if (nl == NULL) {
        return false;              /* 行还没到齐 */
    }

    p += KEY_WIFI_LEN;             /* 跳过 "wifi " 本身 */

    comma = memchr(p, ',', (size_t)(nl - p));
    if (comma == NULL) {
        printf("[cmd] ✗ wifi 命令里没有逗号。格式：wifi <SSID>,<密码>\n");
        valid = false;
    } else {
        n = (size_t)(comma - p);
        if (n == 0 || n > WIFI_SSID_MAX) {
            printf("[cmd] ✗ SSID 长度 %u 不合法（1~%d 字节）\n",
                   (unsigned)n, WIFI_SSID_MAX);
            valid = false;
        } else {
            memcpy(ssid, p, n);
            ssid[n] = '\0';

            /* 密码从逗号后一直到行尾。允许是空串（开放热点），
               所以这里不像 SSID 那样检查 n == 0。 */
            p = comma + 1;
            n = (size_t)(nl - p);
            if (n > WIFI_PASS_MAX) {
                printf("[cmd] ✗ 密码超过 %d 字节\n", WIFI_PASS_MAX);
                valid = false;
            } else {
                memcpy(pass, p, n);
                pass[n] = '\0';
            }
        }
    }

    /* 参数没问题才真去改。改失败时 wifi_sta_set_credentials() 内部会打原因，
       而且【不会】动当前连接 —— 所以这条命令是安全的，
       打错字不会把节点弄成砖。 */
    if (valid) {
        printf("[cmd] 换热点 → \"%s\"\n", ssid);
        link_send(wifi_sta_set_credentials(ssid, pass)
                  ? "WIFI OK (reconnecting)\r\n"
                  : "WIFI FAIL (see serial log)\r\n", 0);
    } else {
        link_send("WIFI FAIL (bad format)\r\n", 0);
    }

    /* 不管成功失败，这一整行都要从缓冲区里抠掉。
       漏掉的话它会永远卡在最前面，把后面所有命令都堵死。 */
    next = nl;
    while (*next == '\r' || *next == '\n') {
        next++;
    }
    s_cmd_len -= (int)(next - s_cmd_buf);
    memmove(s_cmd_buf, next, s_cmd_len);
    s_cmd_buf[s_cmd_len] = '\0';

    return true;
}

/**
  * @brief    在缓冲区里找一条命令并执行
  *
  * @retval   true   找到并处理了一条（调用方要接着再试，可能还有第二条）
  * @retval   false  没找到完整命令，等下一批字节
  */
static bool cmd_try_one(void)
{
    const cmd_t *found = NULL;
    char        *hit   = NULL;
    int          i;
    bool         switched = false;   /* 这一轮是不是换了链路 */

    /* 扫整张表，挑【位置最靠前】的那条 —— 这样收到 "开灯关灯" 时，
       会先执行开灯、再执行关灯，顺序和对方发送的顺序一致。 */
    for (i = 0; i < (int)CMD_COUNT; i++) {
        char *p = strstr(s_cmd_buf, s_cmds[i].key);
        if (p != NULL && (hit == NULL || p < hit)) {
            hit   = p;
            found = &s_cmds[i];
        }
    }

    /* 把带参数的 "wifi " 也拉进来一起比位置。

       ⚠ 这里有个规矩容易忽略：如果 "wifi " 靠前、但【整行还没到齐】，
         cmd_do_wifi() 会返回 false，我们就必须原样返回 false 继续等，
         而【不能】退回去执行表里那条 —— 那等于把后面的命令提前执行了。

       "wifi " 末尾那个空格是有意的：它保证匹配到的是命令头，
       而不是某个 SSID 里恰好出现的 "wifi"。 */
    {
        char *w = strstr(s_cmd_buf, KEY_WIFI);
        if (w != NULL && (hit == NULL || w < hit)) {
            return cmd_do_wifi(w);
        }
    }

    if (found == NULL) {
        return false;      /* 还没拼出完整的命令，继续等 */
    }

    switch (found->action) {
    case ACT_ON:
        relay_on();
        /* 回复用纯 ASCII：无论调试助手设成 UTF-8 还是 GBK，都不会显示成乱码 */
        link_send("LED ON  (GPIO0 = HIGH)\r\n", 0);
        break;

    case ACT_OFF:
        relay_off();
        link_send("LED OFF (GPIO0 = LOW)\r\n", 0);
        break;

    /* 这两条的回执由 link_switch() 自己发 —— 因为回执必须走【旧】链路，
       而这里 s_link 还没换，正好符合要求，所以不要再补一句 link_send()。
       补了的话会发两遍。 */
    case ACT_NET_TCP:
        link_switch(LINK_TCP);
        switched = true;
        break;

    case ACT_NET_UDP:
        link_switch(LINK_UDP);
        switched = true;
        break;
    }

    /* 把这条命令从缓冲区里抠掉：
       把 hit 之后的内容整体搬到开头，长度相应减少。
       注意减掉的是找到的那条命令【自己的字节数】，不是写死的 2。 */
    char *next = hit + found->len;
    s_cmd_len -= (int)(next - s_cmd_buf);
    memmove(s_cmd_buf, next, s_cmd_len);
    s_cmd_buf[s_cmd_len] = '\0';

    /* 刚换了链路 → 缓冲区整个清空。
       不清的话，旧链路在切换前发来的半条命令会留在缓冲区里，
       被新链路的字节"补全"，拼出一条谁都没发过的命令。
       举个具体的：旧链路上来了个 "开"，切换后又来了个 "灯"，
       缓冲区里就凑成了 "开灯" —— 而没有任何一方发过这两个字。
       清空之后，换链路就等于把解析器也复位了，
       和上面 cmd_on_rx_from() 挡住旧链路的包是同一个道理。

       ⚠ 必须放在【抠掉命令之后】。放前面的话 s_cmd_len 先归零，
         上面那句相减会变成负数，memmove 的长度会大得离谱，直接崩。 */
    if (switched) {
        s_cmd_len = 0;
        s_cmd_buf[0] = '\0';
    }

    return true;
}

/**
  * @brief    把收到的字节按十六进制打出来
  *
  * @param[in] data  要打印的字节
  * @param[in] len   字节数
  *
  * @note     加这个是因为"编码不对"这类毛病，光看【字节数】是看不出来的：
  *           把实际字节打出来，一眼就能对上自己是 UTF-8 还是 GBK。
  *           最多打 16 个，免得刷屏。
  */
static void dump_hex(const char *data, int len)
{
    int n = (len > 16) ? 16 : len;
    int i;

    for (i = 0; i < n; i++) {
        /* 必须转成 unsigned char：char 在这里是有符号的，
           不转的话 0xBF 会被 sizeof 成 FFFFFFBF，打印出 8 位。 */
        printf(" %02X", (unsigned char)data[i]);
    }
    if (len > n) {
        printf(" ...");
    }
    printf("\n");
}

/**
  * @brief    这是注册给 tcp_client 的回调：每收到一批字节就被调用一次
  *
  * @param[in] data  裸字节，可能只是一条命令的一部分
  * @param[in] len   字节数
  *
  * @note     注册动作在 app_main() 里，通过 tcp_client_set_rx_handler()。
  */
static void cmd_on_rx(const char *data, int len)
{
    printf("[cmd] 收到 %d 字节:", len);
    dump_hex(data, len);

    /* 缓冲区放不下就整个丢掉重来。
       走到这一步说明前面攒的字节一直没拼出任何命令（比如对方发的是乱码）。 */
    if (s_cmd_len + len > CMD_BUF_SIZE) {
        printf("[cmd] 缓冲区放不下，丢掉前面攒的 %d 字节\n", s_cmd_len);
        s_cmd_len = 0;
    }

    /* 极端情况：一次就送来超过缓冲区大小的数据，只留最后一段 */
    if (len > CMD_BUF_SIZE) {
        data += (len - CMD_BUF_SIZE);
        len = CMD_BUF_SIZE;
    }

    memcpy(s_cmd_buf + s_cmd_len, data, len);
    s_cmd_len += len;
    s_cmd_buf[s_cmd_len] = '\0';

    /* 一次可能匹配出好几条命令，循环处理干净 */
    while (cmd_try_one()) {
        /* 空循环体，活儿都在 cmd_try_one() 里干完了 */
    }
}

/**
  * @brief    带【来源】的入口：不是当前活动链路来的数据，一律丢掉
  *
  * @param[in] src   这包数据是从哪条链路来的
  * @param[in] data  裸字节，可能只是一条命令的一部分
  * @param[in] len   字节数
  *
  * @note     为什么需要这一层：两个模块的 stop() 都只是置个标志，
  *           任务要等下一次 recv/recvfrom 超时（最多 5 秒）才真正断开。
  *           这段窗口里【旧链路还在收】—— 不管它的话，切走之后发过来的
  *           命令照样会被执行，而回执走的是新链路。发送方会觉得
  *           "我明明切走了，怎么还被遥控"，而且串口和网络两边对不上账。
  *
  *           加上这一层，切换就是【干脆】的：s_link 一改，旧链路的包
  *           立刻失效，不用等它自己慢慢关。
  */
static void cmd_on_rx_from(link_mode_t src, const char *data, int len)
{
    if (src != s_link) {
        printf("[cmd] 忽略 %d 字节：来自已停用的链路\n", len);
        return;
    }
    cmd_on_rx(data, len);
}

/**
  * @brief 注册给 tcp_client 的回调
  *
  * @note  只是个转发，把"我从 TCP 来"这件事告诉 cmd_on_rx_from()。
  */
static void cmd_on_tcp_rx(const char *data, int len)
{
    cmd_on_rx_from(LINK_TCP, data, len);
}

/**
  * @brief 注册给 udp_client 的回调
  *
  * @note  同上。两个模块的回调签名一模一样，区别只在来源。
  */
static void cmd_on_udp_rx(const char *data, int len)
{
    cmd_on_rx_from(LINK_UDP, data, len);
}

/**
  * @brief 启动时把芯片信息打到串口上
  *
  * @note  spi_flash_get_chip_size() 读的是【编译时写进 sdkconfig 的值】，
  *        不是探测出来的 —— 所以这行打印不能用来判断板上真实有多少 flash。
  *        要确认容量，用 esptool.py flash_id（见 README）。
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
  * @note  真正的活儿都在下面起的那几条任务里异步跑着，这里只负责把它们串起来。
  */
void app_main(void)
{
    print_chip_info();

    /* 1. 硬件：继电器 */
    relay_init();

    /* 2. 联网：连热点、重连、打印 IP —— 细节都在 wifi_sta.c 里。
          这个函数是异步的，返回时连接还没建立，连上了会自己打日志。 */
    wifi_sta_init();

    /* 3. 告诉两个传输模块："收到数据就调各自的入口"。必须先注册再启动 ——
          注册晚了，第一段到达的数据会因为回调还是 NULL 而被悄悄丢掉。

          两个回调都注册上，但【只有当前链路那个会被真正触发】：
          另一条的任务此时在空转，压根没在收（见 cmd_on_rx_from）。 */
    tcp_client_set_rx_handler(cmd_on_tcp_rx);
    udp_client_set_rx_handler(cmd_on_udp_rx);

    /* 4. 网络通信：默认走 TCP —— 细节都在 tcp_client.c 里。
          它会自己等 WiFi 拿到 IP，所以紧跟在 wifi_sta_init() 后面调用即可。

          ⚠ 这里【不】调 udp_client_start()。UDP 那条任务要等到收到
          net udp 才会被创建，在那之前一条任务都不多占。
          想改成开机就走 UDP，把下面这行换成 udp_client_start()、
          并把 s_link 初始化成 LINK_UDP 即可，两处都要改。 */
    tcp_client_start();

    /* 5. 主任务保持存活。
          真正的活儿都在上面那几条任务里异步跑着，这里什么都不用做。

          ⚠ 这里本身【不要】返回、也不要主动重启：
            一旦复位，GPIO0 会被重新采样成启动模式选择脚，
            此时若正好处在低电平，芯片就会进下载模式。
            本工程现在没有任何地方会重启，所以这条约束是"别引入"而不是"要处理"——
            将来真要加重启，记得先 gpio_set_level(RELAY_GPIO, RELAY_ON)。 */
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
