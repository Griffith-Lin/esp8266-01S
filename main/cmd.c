/**
  * @file    cmd.c
  * @brief   命令解析 —— 缓冲区、关键字表、动作派发
  *
  * 对外接口和下位机契约见 cmd.h。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §1.2、§5.5、§6
  */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "wifi_sta.h"
#include "relay.h"
#include "link.h"
#include "cmd.h"

/**
  * @brief  命令缓冲区大小（字节）
  *
  * @par 坑 1：TCP 是【字节流】，不是消息队列
  *
  *   你点一次"发送"，这边 recv() 收到的可能是完整的 "开灯"、被拆开的
  *   "开" + "灯"、或者粘在一起的 "开灯关灯"。
  *
  *   所以【绝对不能用每次 recv 到的内容直接去比较】。正确做法：把收到的字节
  *   先攒进这个缓冲区，再在缓冲区里找关键字；找到就把那条命令抠掉，
  *   剩下的继续留着等下一批字节来拼。
  *
  * @note  坑 1 是【TCP 特有】的。换成 UDP 之后只剩"粘包"那一半：
  *        UDP 保留消息边界，不会被拆开，但发送方仍然可以在一包里塞两条命令。
  *        所以下面这套写法【照旧必须留着】。
  *
  * @see   学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §1.2、学习笔记/ESP8266开发流程.md §9.4
  *
  * @par 坑 2：中文"长什么样"，取决于【谁】把它变成字节
  *
  *   strstr() 比的是字节。同样是"开灯"，UTF-8 是 6 字节、GBK 是 4 字节。
  *   本文件存成 UTF-8，而中文 Windows 上的网络调试助手大多发 GBK ——
  *   两边对不上就永远找不到，现象是：
  *
  *     【串口明明打印"收到 N 字节"，但继电器不动，也没有回复。】
  *
  *   所以下面那张表把两种编码都收进来，另外再给一组纯 ASCII 的 on/off。
  *
  * @see   学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §6.2（怎么看出来的）、学习笔记/ESP8266开发流程.md §9.5
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
  * @note   用枚举而不是 bool：再来第三种（比如"查询状态"）时，
  *         只是往下面那张表里加一行、往 switch 里加一个 case。
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
  *
  * @note    往这张表里加关键字时，len 必须数【字节】：
  *          "on" = 2、"off" = 3、"net tcp" = 7，中文按上面的转义串数。
  *          数错了不会报错，只会把后面相邻的字节一起吃掉。
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
  *        用空格当分隔符会把 "My Home WiFi" 劈成两半。
  */
#define KEY_WIFI       "wifi "

/** @brief KEY_WIFI 的字节数（不含结尾 '\0'） */
#define KEY_WIFI_LEN   5

/** @brief 802.11 协议规定的 SSID 上限（字节） */
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
  * @warning  返回值跟别的命令【不一样】，调用方要留意。
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
       而这里还没换，正好符合要求，所以不要再补一句 link_send()。
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
       被新链路的字节"补全"，拼出一条谁都没发过的命令：
       旧链路上来了个 "开"，切换后又来了个 "灯"，缓冲区里就凑成了 "开灯"。

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
           不转的话 0xBF 会被符号扩展成 FFFFFFBF，打印出 8 位。 */
        printf(" %02X", (unsigned char)data[i]);
    }
    if (len > n) {
        printf(" ...");
    }
    printf("\n");
}

/**
  * @brief    每收到一批字节就被调用一次
  *
  * @param[in] data  裸字节，可能只是一条命令的一部分
  * @param[in] len   字节数
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
  * @note     为什么需要这一层：两个传输模块的 stop() 都只是置个标志，
  *           任务要等下一次 recv/recvfrom 超时（最多 5 秒）才真正断开。
  *           这段窗口里【旧链路还在收】—— 不管它的话，切走之后发过来的
  *           命令照样会被执行，而回执走的是新链路。
  *
  *           加上这一层，切换就是【干脆】的：链路一改，旧链路的包
  *           立刻失效，不用等它自己慢慢关。
  */
static void cmd_on_rx_from(link_mode_t src, const char *data, int len)
{
    if (src != link_current()) {
        printf("[cmd] 忽略 %d 字节：来自已停用的链路\n", len);
        return;
    }
    cmd_on_rx(data, len);
}

void cmd_on_tcp_rx(const char *data, int len)
{
    cmd_on_rx_from(LINK_TCP, data, len);
}

void cmd_on_udp_rx(const char *data, int len)
{
    cmd_on_rx_from(LINK_UDP, data, len);
}
