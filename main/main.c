/* ESP-01S 继电器控制 —— 主业务

   这个文件只放"业务"：继电器怎么用、收到什么命令该做什么、程序怎么把模块串起来。
   联网的细节都被挡在两个模块后面了：

     wifi_sta.c     连热点、把凭据存 NVS、断线重连
     tcp_client.c   连服务端、收发字节（纯传输，不知道"开灯"是什么）

   曾经还有一个 ota.c 负责远程升级，已经移出本工程 —— 本板 1MB 单槽，
   两个 app 槽放不下这个固件。见 ../ota-for-larger-flash/。

   ── 换 WiFi 热点 ──────────────────────────────────────────────

     节点还连得上   → 发 TCP 命令：wifi <SSID>,<密码>

     wifi_sta.c 顶部那两个宏只是【出厂默认值】，NVS 里有凭据就以 NVS 为准。

   ⚠ 节点一旦连不上，就【没有】远程通道了 —— 只能拆下来重烧。

     以前这里有一条 SmartConfig 兜底（手机 App 把密码编成广播包发出来，
     节点在空口上收），那条路已经整个拆掉。理由和实测数据见 README §9.7：
     它的链路太长（手机 WiFi 驱动的广播行为 → 路由器 → ESP 的混杂模式），
     任何一环不配合就死，而且【没有反馈】，串口上什么都看不出来。

     所以改主节点凭据的顺序必须是：
       ① 先发 TCP 命令 wifi <新SSID>,<新密码>
       ② 再改主节点
     反过来做，这块板子就只能重烧了。

   ⚠ "改配置"和"换固件"是两件事，别混。
     改 WiFi 密码靠上面那条 TCP 命令；换固件只能插串口线重烧。

   要改服务端 IP/端口  → tcp_client.c 顶部
   要改能识别的命令    → 本文件下面"命令解析"那一节

   ── 硬件相关 ─────────────────────────────────────────────────

   GPIO0 驱动继电器。
     开灯 = GPIO0 拉高 = RELAY_ON
     关灯 = GPIO0 拉低 = RELAY_OFF

   ⚠ GPIO0 是 ESP8266 的启动模式选择脚（strapping pin）：
     复位时必须为高电平，芯片才会从 Flash 启动；
     复位时为低电平，芯片会进入 UART 下载模式，程序根本不会运行。

     还要注意：复位采样那一刻，GPIO0 是【输入 / 高阻态】——芯片只是"读"这根线，
     并不会"输出"高电平。让它被读成高的，是上拉电阻，而上拉电阻不是驱动器：
     它只能提供几十 µA，一旦外接电路（比如继电器模块的输入端）把线拉低，
     弱上拉就扛不住，芯片每次复位都会掉进下载模式。
     所以真正决定命运的是【挂在这根线上的外部电路】，不是程序里写 0 还是 1。

     ESP-01S 上引出来的脚只有 GPIO0 / GPIO1 / GPIO2 / GPIO3，其中 GPIO1 和
     GPIO3 是串口的 TX/RX，GPIO2 在启动时同样要求高电平 —— 这块板子上没有
     更"干净"的脚可以换。用 GPIO0 做输出，就必须接受这个约束。

   ⚠ 供电：WiFi 发射瞬间电流会冲到 170~300mA，叠加上继电器线圈，
     如果还是拿 USB 转串口板那个 3.3V 脚供电，很容易掉电复位（brownout）。
     如果模块开始反复重启、串口刷乱码，先怀疑供电，
     在 VCC-GND 之间并 100~470µF 电解电容 + 0.1µF 瓷片电容。

   This example code is in the Public Domain (or CC0 licensed, at your option.)
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

//波特率74880
//io0控制继电器，高电平吸合
//esp-01S进入运行模式，io0必须为高电平，io2必须为高电平，那为什么，芯片复位时，io0并没有输出高电平？运行模式靠 ROM 在复位瞬间读到的电平决定。这个电平是谁给的都行——内部上拉、板上 10k、或者外部驱动器，ROM 不关心。上拉只是"让悬空脚有个确定值"的手段
#define RELAY_GPIO   GPIO_NUM_0
#define RELAY_ON     1      /* 开灯：GPIO0 拉高 */
#define RELAY_OFF    0      /* 关灯：GPIO0 拉低 */

/* 把 GPIO0 配成继电器输出，并立刻置成"关灯"状态（上电默认不动作） */
static void relay_init(void)
{
    /* 注意：gpio_config() 只是"使能输出驱动器"，它并不写电平；但驱动器一开，
       引脚立刻开始输出【输出寄存器里已有的值】，而该寄存器复位后是 0。
       所以 gpio_config() 返回的那一刻 GPIO0 是【低】的，
       紧接着下面那句 set_level 才把它拉高 —— 这才是真正的"开灯"。
       中间那个极短的低电平窗口只有几微秒，继电器来不及动作，可以忽略。 */
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

static void relay_on(void)
{
    gpio_set_level(RELAY_GPIO, RELAY_ON);
    printf("[relay] 开灯 —— GPIO0 拉高\n");
}

static void relay_off(void)
{
    gpio_set_level(RELAY_GPIO, RELAY_OFF);
    printf("[relay] 关灯 —— GPIO0 拉低\n");
}

/* ======================= 命令解析 =======================

   这里有两个坑，都很隐蔽。

   ── 坑 1：TCP 是【字节流】，不是消息队列 ─────────────────────

   你在网络调试助手里点一次"发送"，ESP 这边 recv() 收到的可能是：
     ① 一次收到完整的 "开灯"            —— 最理想的情况
     ② 分两次收到 "开" 和 "灯"          —— 被网络拆包了
     ③ 一次收到 "开灯关灯"              —— 你连点了两次，粘在一起了

   所以【绝对不能用每次 recv 到的内容直接去比较】，
   否则 ②③ 两种情况都会失败，而且现象是"偶尔灵偶尔不灵"，特别难查。

   正确做法：把收到的字节先攒进一个缓冲区，再在缓冲区里找关键字。
   找到就把那条命令抠掉，剩下的继续留着等下一批字节来拼。

   ── 坑 2：中文"长什么样"，取决于【谁】把它变成字节 ──────────

   strstr() 比的是【字节】，不是"字"。同样是"开灯"两个字，
   在不同编码下的字节完全不同：

               UTF-8                GBK
     开灯      E5 BC 80 E7 81 AF    BF AA B5 C6     ← 6 字节 vs 4 字节
     关灯      E5 85 B3 E7 81 AF    B9 D8 B5 C6
     配网      E9 85 8D E7 BD 91    C5 E4 CD F8

   谁说了算？
     · 本文件里写的 "开灯" 是什么字节 —— 由【这个 .c 文件存成什么编码】决定。
       本文件是 UTF-8，所以直接用字面量的话是 6 字节。
     · 网络调试助手发出来的是什么字节 —— 由【助手的编码设置】决定。
       中文 Windows 上的调试助手大多默认 GBK，也就是 4 字节。

   两边对不上，strstr 就永远找不到，现象是：
       【串口明明打印"收到 N 字节"，但继电器不动，也没有回复。】

   所以下面这张表把两种编码都收进来，另外再给一组纯 ASCII 的 on/off ——
   ASCII 没有编码歧义，是最保险的那条路。                              */

#define CMD_BUF_SIZE  64

static char s_cmd_buf[CMD_BUF_SIZE + 1];   /* +1 留给结尾的 '\0' */
static int  s_cmd_len = 0;

/* 关键字特意写成【转义字节】，而不是让字面量跟着文件编码走 ——
   这样无论本文件存成 UTF-8 还是 GBK，匹配到的都是同一串字节。 */
#define KEY_KAI_UTF8   "\xE5\xBC\x80\xE7\x81\xAF"   /* 开灯 */
#define KEY_KAI_GBK    "\xBF\xAA\xB5\xC6"           /* 开灯 */
#define KEY_GUAN_UTF8  "\xE5\x85\xB3\xE7\x81\xAF"   /* 关灯 */
#define KEY_GUAN_GBK   "\xB9\xD8\xB5\xC6"           /* 关灯 */

/* 一条命令对应一个"动作"。原先这里是个 bool turn_on，只装得下"开/关"
   两种；换成枚举之后，再来第三种（比如"查询状态"）也只是往下面那张表里
   加一行、往 switch 里加一个 case。 */
typedef enum {
    ACT_ON,       /* 继电器吸合 */
    ACT_OFF,      /* 继电器释放 */
} cmd_action_t;

typedef struct {
    const char   *key;    /* 关键字（按字节比较） */
    int           len;    /* 关键字的【字节数】—— 中文不是 1 个字 1 个字节 */
    cmd_action_t  action;
} cmd_t;

static const cmd_t s_cmds[] = {
    { KEY_KAI_UTF8,  6, ACT_ON  },
    { KEY_KAI_GBK,   4, ACT_ON  },
    { KEY_GUAN_UTF8, 6, ACT_OFF },
    { KEY_GUAN_GBK,  4, ACT_OFF },
    { "on",          2, ACT_ON  },   /* ASCII 别名，小写；不区分大小写的版本没做 */
    { "off",         3, ACT_OFF },
};

/* ⚠ 这张表是【扫完再决定】的，不是"找到第一条就返回" —— 别改成那样。
   规则：在所有命中的关键字里，挑【在缓冲区里位置最靠前】的那一个。

   为什么需要这条规则：关键字可能互相【包含】。历史上这里有过一条
   "config"（进 SmartConfig 配网，已经拆掉），它里面就含着 "on"
   （c-on-fig）—— 找到第一条就返回的话，收到 "config" 会先开灯。
   "config" 从下标 0 开始、"on" 从下标 1 开始，按"最靠前"才选得对。

   现在表里已经没有互相包含的关键字了，规则仍然留着：
   它是正确的通用规则，而下一个加进来的关键字随时可能再踩同一个坑。 */

/* 带参数的命令：wifi <SSID>,<密码>

   它没法放进上面那张表 —— 表里每条都是【定长关键字】，匹配到就能按
   固定长度抠掉；而这条后面挂着两个长度不定的参数。
   所以单独处理，但它的【先后顺序】仍然要和表里那些一起排
   （见 cmd_try_one()），否则 "开灯wifi A,B\n" 的执行顺序就乱了。

   为什么用逗号而不是空格分隔？因为 SSID 里【可以有空格】——
   比如热点叫 "My Home WiFi"，用空格当分隔符就会被劈成两半。

   ⚠ 这条和当前默认值无关。本项目现在连的是 ESP32 的 SoftAP
     "ESP32-S3-host"，没有空格 —— 但别因为"现在的名字没空格"
     就把分隔符改成空格，那等于把这条命令悄悄写窄了。 */
#define KEY_WIFI       "wifi "
#define KEY_WIFI_LEN   5

/* 802.11 协议的上限。wifi_sta.c 里也有一份，这里重写一遍是因为
   那是模块内部的事，不值得为它去动头文件。 */
#define WIFI_SSID_MAX  32
#define WIFI_PASS_MAX  64
#define CMD_COUNT  (sizeof(s_cmds) / sizeof(s_cmds[0]))

/* 处理一条 wifi 命令。p 指向缓冲区里找到的 "wifi "。

   返回值跟别的命令不一样，要留意：
     true  —— 这一行处理完了（不管成功失败），已经从缓冲区里抠掉，调用方继续
     false —— 【行还没收全】，什么都没做，等下一批字节

   为什么必须等到整行？因为 TCP 是字节流，一次 recv 可能只到
   "wifi DESKTOP" 就断了，这时候后面的密码还没来。
   硬猜的话会把半截 SSID 当成真的存进 NVS。

   所以规矩是：以换行结尾。网络调试助手发的时候记得勾"发送新行"。 */
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
        tcp_client_send(wifi_sta_set_credentials(ssid, pass)
                        ? "WIFI OK (reconnecting)\r\n"
                        : "WIFI FAIL (see serial log)\r\n", 0);
    } else {
        tcp_client_send("WIFI FAIL (bad format)\r\n", 0);
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

/* 在缓冲区里找一条命令并执行。
   返回 true 表示找到并处理了一条（调用方要接着再试，可能还有第二条）。 */
static bool cmd_try_one(void)
{
    const cmd_t *found = NULL;
    char        *hit   = NULL;
    int          i;

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
        tcp_client_send("LED ON  (GPIO0 = HIGH)\r\n", 0);
        break;

    case ACT_OFF:
        relay_off();
        tcp_client_send("LED OFF (GPIO0 = LOW)\r\n", 0);
        break;
    }

    /* 把这条命令从缓冲区里抠掉：
       把 hit 之后的内容整体搬到开头，长度相应减少。
       注意减掉的是找到的那条命令【自己的字节数】，不是写死的 2。 */
    char *next = hit + found->len;
    s_cmd_len -= (int)(next - s_cmd_buf);
    memmove(s_cmd_buf, next, s_cmd_len);
    s_cmd_buf[s_cmd_len] = '\0';

    return true;
}

/* 把收到的字节按十六进制打出来。
   加这个是因为"编码不对"这类毛病，光看【字节数】是看不出来的：
   把实际字节打出来，一眼就能对上是 UTF-8 还是 GBK。 */
static void dump_hex(const char *data, int len)
{
    int n = (len > 16) ? 16 : len;    /* 最多打 16 个，免得刷屏 */
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

/* 这是注册给 tcp_client 的回调：每收到一批字节就被调用一次。
   data 是裸字节，可能只是一条命令的一部分。 */
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

static void print_chip_info(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is ESP8266 chip with %d CPU cores, WiFi, ", chip_info.cores);
    printf("silicon revision %d, ", chip_info.revision);
    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
}

void app_main(void)
{
    print_chip_info();

    /* 1. 硬件：继电器 */
    relay_init();

    /* 2. 联网：连热点、重连、打印 IP —— 细节都在 wifi_sta.c 里。
          这个函数是异步的，返回时连接还没建立，连上了会自己打日志。 */
    wifi_sta_init();

    /* 3. 告诉 TCP 模块："收到数据就调 cmd_on_rx"。必须先注册再启动。 */
    tcp_client_set_rx_handler(cmd_on_rx);

    /* 4. 网络通信：连服务端、收数据 —— 细节都在 tcp_client.c 里。
          它会自己等 WiFi 拿到 IP，所以紧跟在 wifi_sta_init() 后面调用即可。 */
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
