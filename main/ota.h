/* OTA 远程升级模块

   通过网络给模块换固件，不用再插串口线。

   ── 它到底在干什么 ──────────────────────────────────────────────

   三步，注意【不是原地覆盖】：

     ① 把新固件写进【另一块】app 槽 —— 正在跑的那块一个字节都不动
     ② 改 otadata，告诉 bootloader：下次启动换到新的那块
     ③ 重启

   分区表里有 ota_0 / ota_1 两块，就是为了让第 ① 步"有地方可写"：
   你跑在 ota_0 就写 ota_1，跑在 ota_1 就写 ota_0。
   esp_ota_get_next_update_partition() 就是替你做这个判断的。

   为什么非得"另一块"？因为不能一边跑一边改自己 ——
   那等于边开车边换发动机。

   ── ⚠ 这个 SDK 【没有回滚】 ────────────────────────────────────

   ESP8266_RTOS_SDK v3.4 里：

     · esp_ota_select_entry_t 只有 {ota_seq, seq_label, crc}，【没有状态位】
     · 没有 esp_ota_mark_app_valid_cancel_rollback()（那是 ESP32 才有的）

   意思是：set_boot_partition 一旦调下去，bootloader 下次就认准那块。
   新固件要是起不来（WiFi 密码写错、看门狗喂不上、GPIO0 卡在低电平……），
   就是无限重启，只能插串口线重新烧 —— OTA 自己救不回来。

   所以 ota.c 顶部有个 OTA_SWITCH_BOOT 开关，默认 0（只写不切）。
   第一次调务必先用 0 跑通。

   下载地址同样在 ota.c 顶部。 */

#pragma once

#include <stdbool.h>

/* 触发一次 OTA。

   立即返回；真正干活的是它内部起的一条独立任务，
   所以【不会】阻塞调用它的地方（也就是接收 TCP 命令的那条任务）。

   返回 false 表示上一次 OTA 还没跑完，这次直接忽略。 */
bool ota_start(void);

/* 重启前的钩子：OTA 成功、且在 esp_restart() 之前会被调用一次。

   ⚠ 在 main.c 里注册这个钩子是【必须的】，不是可选装饰：

     esp_restart() 会让芯片重新采样 GPIO0 —— 复位瞬间 GPIO0 若是低电平，
     芯片直接进 UART 下载模式，程序根本不跑，板子看起来就是"变砖"。
     而本项目的继电器【关灯】状态下 GPIO0 正好是低的，
     所以重启前必须先把它拉高。具体见 main.c 里的 ota_before_restart()。 */
typedef void (*ota_before_restart_t)(void);

void ota_set_before_restart(ota_before_restart_t cb);
