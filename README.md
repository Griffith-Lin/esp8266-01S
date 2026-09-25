

# ESP-01S (ESP8266) 开发笔记

在 Windows 下使用 ESP8266_RTOS_SDK 开发 ESP-01S 流程。（建议先学会sdk裸开发作为保底手段，后面再使用vscode插件ESP8266-IDF简化开发操作）

## 目录

- [1. 工具链与 SDK 准备](#1-工具链与-sdk-准备)
- [2. 环境变量配置](#2-环境变量配置)
- [3. Python 依赖安装](#3-python-依赖安装)
- [4. 创建示例工程](#4-创建示例工程)
- [5. 配置与构建](#5-配置与构建)
- [6. 波特率说明](#6-波特率说明)
- [7. 启动日志分析](#7-启动日志分析)
- [8. 自定义分区表](#8-自定义分区表)
- [9. 联网控制：TCP 命令与 WiFi 配网](#9-联网控制tcp-命令与-wifi-配网)

---

## 1. 工具链与 SDK 准备

### 1.1 下载工具链压缩包

从乐鑫官方下载地址获取以下两个压缩包（Windows 32 位）：

| 压缩包 | 下载地址 |
| --- | --- |
| `xtensa-lx106-elf-gcc8_4_0-esp-2020r3-win32.zip` | <https://dl.espressif.com/dl/xtensa-lx106-elf-gcc8_4_0-esp-2020r3-win32.zip> |
| `esp32_win32_msys2_environment_and_toolchain-20181001.zip` | <https://dl.espressif.com/dl/esp32_win32_msys2_environment_and_toolchain-20181001.zip> |

分别解压到 `esp8266_toochain/` 目录下：

| 压缩包 | 解压后目录 | 作用 |
| --- | --- | --- |
| `xtensa-lx106-elf-gcc8_4_0-esp-2020r3-win32.zip` | `xtensa-lx106-elf/` | Xtensa 交叉编译器（`gcc`、`ld`、`objcopy` 等） |
| `esp32_win32_msys2_environment_and_toolchain-20181001.zip` | `msys32/` | MSYS2 环境，提供 `mingw32.exe` 终端 |

两个都是**解压即用**的绿色包，不需要安装，也不需要写注册表，删掉文件夹就等于卸载。

#### 这两个分别是什么？

**① `xtensa-lx106-elf` —— 干活的：交叉编译器**

ESP8266 芯片里的 CPU 是 **Xtensa** 架构，跟你的电脑（Intel/AMD 的 x86）**不是同一种语言**。你写的 C 代码，用电脑上普通的编译器编译出来的程序，ESP8266 根本跑不了。

所以需要一个"翻译官"：**它运行在你的 Windows 电脑上，产出的却是 ESP8266 能执行的机器码**。这种"在 A 平台上生成 B 平台代码"的编译器，就叫**交叉编译器**（cross compiler）。

它里面装着：

| 工具 | 用途 |
| --- | --- |
| `xtensa-lx106-elf-gcc` | 编译器，把 `.c` 翻译成 ESP8266 的机器码 |
| `xtensa-lx106-elf-ld` | 链接器，把编译出的一堆碎片拼成完整固件 |
| `xtensa-lx106-elf-objcopy` | 格式转换，生成能烧进 Flash 的 `.bin` 文件 |
| `xtensa-lx106-elf-objdump` | 反汇编，调试时用来查看机器码 |

名字是拼出来的，拆开读就懂了：

```text
xtensa - lx106 - elf - gcc8_4_0 - esp-2020r3 - win32
   ↑       ↑      ↑       ↑            ↑          ↑
 CPU架构  核心型号 文件格式 GCC版本   乐鑫发布版本  运行在Windows
```

ESP8266EX 的内核正是 Xtensa **L106**，所以叫 `lx106`。ESP32 用的是 L6，工具链名字是 `xtensa-esp32-elf`——看一眼名字就知道是给哪块芯片用的。

**② `msys32` —— 提供场地的：Linux 风格的终端环境**

ESP8266_RTOS_SDK 的构建系统是**照着 Linux 写的**：靠 `make` 驱动一堆 `.mk` 文件，脚本里到处是 `ls`、`cp`、`rm`、`sed` 这类 Linux 命令。Windows 自带的 cmd 和 PowerShell 都不认识它们，直接跑会寸步难行。

`msys32`（Minimal SYStem 2）相当于在 Windows 上开了个**"假装是 Linux"的小房间**，里面备齐了 bash 和那些 Linux 命令。`mingw32.exe` 就是这个房间的门——双击它，你就进到了一个长得像 Linux 的终端里。

> **名字里为什么带 `esp32`？** 这个 MSYS2 环境本身和芯片无关，只负责提供 shell 和基础命令。乐鑫当年把它打包给 ESP32 用，后来 ESP8266_RTOS_SDK 的 Windows 教程直接沿用了同一个包，所以名字没改。

**一句话总结分工**：`msys32` 负责把环境搭起来（让你能敲命令），`xtensa-lx106-elf` 负责把代码变成固件（让芯片能跑）。

完成后目录结构如下：

```text
esp8266_toochain/
├── msys32/                      ← esp32_win32_msys2_environment_and_toolchain-20181001.zip
│   └── home/Administrator/
│       └── esp/
│           ├── ESP8266_RTOS_SDK/
│           └── hello_world/
└── xtensa-lx106-elf/            ← xtensa-lx106-elf-gcc8_4_0-esp-2020r3-win32.zip
    └── bin/
```

> **版本配对**：`gcc8_4_0-esp-2020r3` 是 ESP8266_RTOS_SDK v3.4 对应的编译器版本，不要与其他版本的 SDK 混用。

### 1.2 获取 ESP8266_RTOS_SDK

使用 `esp8266_toochain/msys32/mingw32.exe` 打开终端，然后克隆 SDK（这里克隆不了就用cmd克隆，然后再用mingw32.exe进行其它操作）：

```bash
mkdir -p ~/esp
cd ~/esp
git clone --recursive https://github.com/espressif/ESP8266_RTOS_SDK.git
```

`--recursive` **不能省略**：SDK 依赖多个子模块（mbedtls、lwip 等），漏掉会导致后续编译缺文件。如果已经克隆但忘了加，可以补跑：

```bash
cd ~/esp/ESP8266_RTOS_SDK
git submodule update --init --recursive
```

mingw32 中的 `~` 对应 Windows 下的 `esp8266_toochain\msys32\home\Administrator`，因此 SDK 最终位于：

```text
..\esp8266_toochain\msys32\home\Administrator\esp\ESP8266_RTOS_SDK
```

> **不同步至代码仓库**：`esp8266_toochain` 整个目录已在 `.gitignore` 中排除——工具链体积大，且 SDK 本身有自己的 Git 仓库。

### 1.3 路径限制

ESP8266_RTOS_SDK 构建系统**不支持**在 SDK 或项目路径中使用空格，安装与克隆时请全程避开带空格的目录。（路径别带有中文）

### 1.4 安装 Python（32 位）

SDK 的构建脚本依赖 Python 3，且需要 **32 位**版本。

下载地址：<https://www.python.org/downloads/windows/>

在页面的 **Stable Releases** 表格中找到目标版本（如 Python 3.13.x），下载 **Windows installer (32-bit)** 一项——注意不要点成 64-bit，也不要下载 embeddable package（那是给嵌入用的，没有 pip）。

安装时保持默认的 **Install Now** 即可，安装路径一般是：

```text
C:\Users\Administrator\AppData\Local\Programs\Python\Python313-32
```

对应到 mingw32 中的写法（第 2 节要用的）：

```bash
/c/Users/Administrator/AppData/Local/Programs/Python/Python313-32
```

> **目录名怎么看位数**：32 位装的目录名结尾带 `-32`（`Python313-32`），64 位则不带后缀（`Python313`）。两者可以同时装在同一台机器上，互不干扰。
>
> **路径不一样的情况**：如果安装时选了 "Install for all users"，路径会变成 `C:\Program Files (x86)\Python313-32`，第 2 节的 `export PATH` 要跟着改。安装路径可以在安装向导第一屏的 "Customize installation" 里确认，装完后也能在终端用 `where python`（cmd）查看。

`AppData` 是隐藏目录，资源管理器里看不到属于正常现象，直接在地址栏粘贴上面的路径即可打开。

安装完成后验证：

```bash
which python      # 应指向 Python313-32 下的 python.exe
python -V         # 应输出 Python 3.13.x
```

---

## 2. 环境变量配置

`mingw32` **无法读取 Windows 系统环境变量**，因此每次进入mingw32.exe都需要手动设置。虽然 `IDF_PATH` 会和系统的 esp-idf 冲突，但冲突本身无所谓——根本原因是读不到。

```bash
# 添加环境变量：SDK 
export IDF_PATH=/home/Administrator/esp/ESP8266_RTOS_SDK

# 添加环境变量：Python（须确认路径下是 Python 3，且为 32 位，安装见 1.4 节）。
# :$PATH 不能省，$PATH 的意思就是"把path原来的内容抄过来"，因为path里面有很多其它的路径，不能丢掉。把$PATH放到末尾意味着，新添加的路径放到所有其它路径的最前面。 
export PATH="/c/Users/Administrator/AppData/Local/Programs/Python/Python313-32:$PATH"

# 添加环境变量：交叉编译工具链
export PATH="$PATH:/g/github/esp8266_toochain/xtensa-lx106-elf/bin"
```

---

## 3. Python 依赖安装

### 3.1 安装 SDK 依赖

SDK 所需的 Python 包列在 `requirements.txt` 中：

```bash
python -m pip install --user -r $IDF_PATH/requirements.txt
```

ESP8266 SDK 专属依赖：

```bash
python -m pip install click pyelftools
```

### 3.2 处理 `pkg_resources` 缺失问题

从 **Python 3.12** 开始，官方彻底移除了内置的 `ensurepip` 和 `setuptools`。这意味着新装的 Python 3.13 是一个"纯净版"，连最基础的包管理工具都没有。而 ESP8266 RTOS SDK 的构建脚本依赖 `pkg_resources`（`setuptools` 的一部分）来检查环境，找不到就会直接罢工。

```bash
# 下载并运行 get-pip.py（需要联网）
curl https://bootstrap.pypa.io/get-pip.py -o /tmp/get-pip.py
/c/Users/Administrator/AppData/Local/Programs/Python/Python313-32/python.exe /tmp/get-pip.py
```

### 3.3 安装基础工具链（有坑）

最新版 `setuptools` (v71+) 已彻底删除 `pkg_resources`，而 ESP8266 SDK 的 Makefile 依赖这个旧模块。**装最新版等于白装。**

```bash
# ❌ 错误：会装上没有 pkg_resources 的新版
python -m pip install setuptools wheel

# ✅ 正确：锁定在 71 以下
python -m pip install "setuptools<71" wheel
```

---

## 4. 创建示例工程

将 `get-started/hello_world` 复制到工作目录：

```bash
cd ~/project
cp -r $IDF_PATH/examples/get-started/hello_world .
```

---

## 5. 配置与构建

### 5.1 项目配置

```bash
cd ~/project/hello_world
make menuconfig
```

在 menuconfig 中将串口配置为 `//./COM8`（Windows 下的写法），根据具体COM口作修改。

### 5.2 编译并烧录

> 烧录前需先让 ESP-01S 进入**下载模式**（将io0接地）。

```bash
make -j10 #构建，ESP-01S 项目建议调用 4 线程，实测 10 线程也可行。
make ESPPORT=COM8 ESPBAUD=921600 flash #烧录成功率跟中间全链路有关，排除问题的优先级为，用手插紧线（尤其是杜邦线！接触不良时，高阻抗、时变噪声）->降波特率（本质上是增大时序容错窗口， 降低高频寄生参数的影响）->...   （先查连接，再测阻抗，最后才怀疑芯片）
```



[^]: 杜邦线接触不良被称作“万恶之源”，因为它会伪装成软件 Bug。按危害分级：🔴 **直接失效（易诊断）**——VCC/GND 接触电阻 >1Ω 时，WiFi 发射瞬间 300mA+ 的电流就会造成 >0.3V 压降、芯片欠压复位（“跑着跑着突然重启”“一连 WiFi 就死机”）；GPIO0 无法被拉低 → 卡在 `Connecting..._____`；TX/RX 松动 → 日志乱码或全无输出。🟠 **间歇性故障（极难诊断）**——SPI/I2C 偶发误码，让你花几天怀疑驱动代码、时序甚至芯片；ADC 读数跳变、PID 震荡；EN/RST 受干扰引入**无理由随机复位**，无法用代码逻辑复现。🟡 **隐性劣化**——信号完整性下降（高波特率烧录失败、WiFi 吞吐降低）；接触点发热；松动连接点成为“微型天线”恶化 EMC。⚫ **不可逆损伤**——电弧碳化排针与母头弹片；触摸松动线材引入 ESD 潜伤，芯片当时没坏但寿命大幅缩短。💡 **核心认知：接触不良是在系统中引入一个“时变、非线性、不可预测”的故障源。** 代码 Bug 是确定性的（可复现、可定位、可修复），接触不良是概率性的（受温度、湿度、振动、氧化影响）——无法被单元测试覆盖，无法被静态分析发现，只能靠物理层可靠性设计消除。



[^]: 接触不良时降波特率能提高烧录成功率，并非“修好”了连接，而是**降低了系统对物理层缺陷的敏感度**。四个机制：① **时序容错窗口变大**——UART 为异步通信，无时钟线同步。921600 下单个比特仅 **1.08μs**，一个 0.5μs 的瞬态开路/短路就占了半个比特周期；74880 下单比特 **13.3μs**，同样干扰占比不足 4%，接收端滤波器有时间忽略毛刺、在稳定区采样。② **高频寄生参数影响减弱**——松动接触点等效为不稳定的 RLC 网络，高频时寄生 LC 形成低通效应，边沿变缓、过冲振铃加剧直至逼近判决阈值；低频时能量集中在低端，波形接近理想方波，眼图张开度更大。③ **接触点非线性效应减小**——金属接触存在**收缩电阻**与**隧道效应**，阻值随电流大小和变化速率而变；高速翻转诱发微电弧与热电势噪声，低速时接触点有更长的“静止时间”，热平衡更稳定。④ **打断 esptool 的重传正反馈**——高波特率下误码 → 不断重传 → 总线持续高频活动 → 接触点发热氧化 → 误码率再升，形成**正反馈崩溃**；低波特率下单次传输长、空闲间隔占比大，接触点有机会“冷却”恢复。**本质：用时间冗余换取抗干扰能力。**



### 5.3 打开串口监视器 （先进入运行模式：断开 GPIO0 与 GND 的连线（ESP-01S 板载 10k 上拉，浮空即为高电平），然后复位。）

```bash
make monitor
```

退出监视器：<kbd>Ctrl</kbd> + <kbd>]</kbd>

### 5.4 构建失败时，尝试清除构建文件

```bash
make clean
```

---

## 6. 波特率说明

| 配置项 | 默认值 | 用途 |
| --- | --- | --- |
| `Default baud rate` | 115200 baud | 烧录 / 下载速度 |
| `'make monitor' baud rate` | 74880 bps | 串口调试 / 日志输出速度 |

### 为什么监视器波特率是 74880？

74880 是一个**非常特殊**的数字——它是 ESP8266 在 26MHz 晶振下的原生波特率（26000000 / 345 ≈ 75362，接近 74880）。

ESP8266 在刚上电、Bootloader 阶段打印的底层启动信息（Flash 电压、RF 校准、Reset 原因）**固定使用 74880 波特率**。

如果把这个值改成 115200，你将**看不到开机时的底层诊断信息，只能看到乱码**，直到应用程序初始化并重新配置 UART 为 115200 后，日志才会恢复正常。

> 只有当你确认不需要看底层启动日志，且希望应用层日志与电脑端终端默认波特率一致时，才考虑改为 115200。

---

## 7. 启动日志分析

一次成功的启动输出：

```text
 ets Jan  8 2013,rst cause:2, boot mode:(3,4)

load 0x40100000, len 7544, room 16
tail 8
chksum 0xb9
load 0x3ffe8408, len 24, room 0
tail 8
chksum 0x30
load 0x3ffe8420, len 3476, room 0
tail 4
chksum 0x7f
csum 0x7f
I (84) boot: ESP-IDF v3.4-115-g858c7c2e-dirty 2nd stage bootloader
I (84) boot: compile time 15:04:29
I (88) qio_mode: Enabling default flash chip QIO
I (102) boot: SPI Speed      : 40MHz
I (115) boot: SPI Mode       : QIO
I (128) boot: SPI Flash Size : 1MB
I (140) boot: Partition Table:
I (152) boot: ## Label            Usage          Type ST Offset   Length
I (174) boot:  0 nvs              WiFi data        01 02 00009000 00006000
I (198) boot:  1 phy_init         RF data          01 01 0000f000 00001000
I (221) boot:  2 factory          factory app      00 00 00010000 000f0000
I (244) boot: End of partition table
I (257) esp_image: segment 0: paddr=0x00010010 vaddr=0x40210010 size=0x1cc5c (117852) map
0x40210010: _stext at ??:?

I (340) esp_image: segment 1: paddr=0x0002cc74 vaddr=0x4022cc6c size=0x07008 ( 28680) map
I (354) esp_image: segment 2: paddr=0x00033c84 vaddr=0x3ffe8000 size=0x00544 (  1348) load
I (363) esp_image: segment 3: paddr=0x000341d0 vaddr=0x40100000 size=0x00080 (   128) load
I (390) esp_image: segment 4: paddr=0x00034258 vaddr=0x40100080 size=0x0512c ( 20780) load
I (426) boot: Loaded app from partition at offset 0x10000
Hello world!
```

这段日志是 ESP8266 的**"出生证明"和"体检报告"**，表明开发环境、硬件连接、烧录过程以及代码逻辑**全部完美通过**。

### 7.1 核心结论：系统启动成功

- **`Hello world!`**：最直接的信号。说明 `app_main()` 已被正确加载并执行，业务代码跑通了。
- **`rst cause:2`**：复位原因是 **2 (External System Reset)**。通常对应按下开发板 RST 按钮，或 USB-TTL 下载器在烧录完成后自动触发的复位。属于正常启动流程。
- **`boot mode:(3,4)`**：
  - `3`：GPIO0 为高电平（运行模式），GPIO2 为高电平 —— 芯片没有进入下载模式，而是正常启动。
  - `4`：SPI Flash 电压为 3.3V。

### 7.2 硬件与存储状态确认

- **`SPI Flash Size : 1MB`**：~~验证模块确实是 1MB 版本~~ —— **这一行什么都不验证。**

  > ⚠️ **本节原来写的是**：
  >
  > > 验证模块确实是 **2MB (16Mbit)** 版本。ESP-01S 有 1MB 和 2MB 两种常见版本。
  > > 若 menuconfig 里选错大小（比如选了 1MB），这里就会报错或启动失败。
  > > 当前日志说明**分区表配置与实际硬件完全匹配**。
  >
  > **这段话每一个字都是错的**，而且它直接导致了一次把芯片擦成砖的事故——
  > 过程见 [§8.6](#86-怎么确认-flash-到底多大)。留着它是为了记住这个坑长什么样。

  这一行是 **sdkconfig 绕了一圈之后打印回来的自己**，跟芯片真实容量毫无关系：

  ```text
  sdkconfig: CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
      ↓ 构建系统生成
  build/flasher_args.json:  "flash_size": "2MB"
      ↓ make flash 把它作为参数交给 esptool
  esptool --flash_size 2MB
      ↓ esptool 把这个值写进 .bin 的镜像头
  esp_image_header_t.spi_size = ESP_IMAGE_FLASH_SIZE_2MB
      ↓ bootloader 从 flash 里把镜像头读出来
  bootloader_init.c:281   ESP_LOGI(TAG, "SPI Flash Size : %s", str)
  ```

  `spi_size` 只是镜像头里一个 **4 bit 的字段**（`esp_image_format.h:87`），
  它记录的是**烧录时告诉 esptool 的那个数**，不是芯片回答的任何东西。

  > **怎么正确判断？** 见 [§8.6](#86-怎么确认-flash-到底多大)——问芯片本身。

- **`SPI Speed : 40MHz` / `QIO Mode`**：Flash 工作在 40MHz 四线 IO 模式，是 ESP8266 的标准性能配置。若显示 26MHz 或 DIO 模式，可能意味着晶振频率不同或 Flash 芯片较老，但 **40MHz QIO 是最理想的状态**。

### 7.3 内存布局分析（进阶视角）

日志中的 `esp_image: segment` 揭示了固件在 Flash 和 RAM 中的分布：

| Segment | 类型 | 大小 | 说明 |
| --- | --- | --- | --- |
| 0 | `map` | ~117 KB | **应用程序代码**，存放在 Flash 中，CPU 通过映射直接执行（XIP） |
| 2 & 4 | `load` | ~1.3 KB + 20 KB | **静态数据、全局变量和中断向量表**，启动时从 Flash 复制到内部 SRAM（`0x3FFE8000` / `0x40100000`）才能被 CPU 快速访问 |

**分区表：**

| Label | 大小 | 用途 |
| --- | --- | --- |
| `nvs` | 24 KB | 键值存储。WiFi 驱动存射频数据，应用用命名空间 `app_cfg` 存 SSID/密码 |
| `phy_init` | 4 KB | 存储射频校准参数 |
| `factory` | 960 KB | 主程序分区 |

### 7.4 为什么波特率是 74880？

注意日志开头的 `ets Jan 8 2013...`：

- 这段信息由 ESP8266 的 **ROM Bootloader**（固化在芯片内部的只读存储器）打印。
- ROM Bootloader **固定使用 74880 波特率**输出，无法更改。
- 这就是 menuconfig 中把 monitor 波特率设为 74880 的原因：**只有这样才能看到完整的启动链路**。若设为 115200，这几行关键的硬件自检信息就会变成乱码。

---

## 8. 自定义分区表

默认用的是 SDK 里写死的分区表，想自己分配 Flash 空间，就要切到自定义模式。

### 8.1 开启方式

在 menuconfig 中：

```text
Partition Table  --->
    Partition Table (Custom partition table CSV)  --->
    (partitions_1mb.csv) Custom partition CSV file
```

- 第一项选 **Custom partition table CSV**（另外两个选项是 `Single factory app, no OTA` 和 `Factory app, two OTA definitions`）。
- 第二项填 csv 的文件名，这就是你自己的分区表。
- **这个文件名是相对于项目根目录解析的**，所以 csv 必须放在项目根目录下（Kconfig 原文：`This path is evaluated relative to the project root directory.`）。

### 8.2 两条铁律

这个 csv 的容错度极低，**违反任意一条，整个工程直接编译不了**：

| # | 规则 | 违反后果 |
| --- | --- | --- |
| 1 | 注释必须**单独开行**，以 `#` 开头 | 行末注释会被当成 Flags 列 → `unknown flag` |
| 2 | 文件必须**纯英文**（纯 ASCII），注释里也不能有中文 | 读取时解码失败 → `UnicodeDecodeError` |

### 8.3 为什么必须纯英文？

分区表是由 Python 脚本 `gen_esp32part.py` 解析的，而它打开文件的方式是：

```python
# parttool.py:100
with open(partition_table_file, "r") as f:
    partition_table = gen.PartitionTable.from_csv(f.read())
```

注意是 `open(..., "r")` —— **没有指定 `encoding`**，于是 Python 会用**系统默认编码**（中文 Windows 上是 GBK），而不是 UTF-8：

```text
UnicodeDecodeError: 'gbk' codec can't decode byte 0xaf in position 53: illegal multibyte sequence
```

csv 一旦存成 **UTF-8 且含中文**，这些 UTF-8 字节按 GBK 去解就会失败。某些环境下默认编码还会退化成 ASCII，报的是另一种：

```text
UnicodeDecodeError: 'ascii' codec can't decode byte 0xe8 in position 50: ordinal not in range(128)
```

> **写在注释里也救不了**：解码发生在 `f.read()` —— **读取整个文件的那一刻**，早于任何逐行处理，更早于 `#` 的判断。所以"我把中文放在注释行，应该会被跳过吧"是行不通的，实测一样报错。

### 8.4 为什么注释必须单独开行？

csv 按逗号切分后，每一列的含义是固定的，第 6 列（`fields[5]`）是 **Flags**：

```text
# Name,   Type, SubType, Offset,   Size, Flags
    ↑       ↑      ↑        ↑        ↑      ↑
 fields[0]  [1]    [2]      [3]      [4]    [5]
```

而 Flags 列只认一个值 `encrypted`：

```python
flags = fields[5].split(":")
for flag in flags:
    if flag in cls.FLAGS:          # FLAGS = {"encrypted": 0}
        setattr(res, flag, True)
    elif len(flag) > 0:
        raise InputError("CSV flag column contains unknown flag '%s'" % flag)
```

所以在字段行末尾写 `..., 0x1E0000,  <-- 注释` ，这个注释会被当作 Flags 列的内容：

```text
Error at line 4: CSV flag column contains unknown flag '<-- main app'
```

**这一条和中文无关** —— 就算注释是纯英文，只要写在字段行末尾就一样报错。

### 8.5 为什么这个坑特别难查

分区表是在 CMake 的 **configure 阶段**就被读取解析的，根本轮不到编译。所以 csv 里一个字节的错误，表现出来是**整个工程全线报错**：

```text
-- Configuring incomplete, errors occurred!
FAILED: build.ninja
ninja: error: rebuilding 'build.ninja': subcommand failed
```

这时候 build、clean、menuconfig、size **每一个** target 都会失败 —— 因为每个 target 都要先过 configure 这一关。遇到这种"什么都不能用了"的场面，**先回头检查分区表 csv**，不要急着怀疑工具链、环境变量或者代码。

### 8.6 怎么确认 flash 到底多大

**结论先说：别看启动日志，去问芯片本身。**

```bash
python $IDF_PATH/components/esptool_py/esptool/esptool.py --port COM8 flash_id
```

```text
Chip is ESP8266EX
MAC: 8c:aa:b5:0b:dd:62
Manufacturer: 68
Device: 4014
Detected flash size: 1MB        ← 芯片当场回答的那个数
```

`Device` 的后两位就是容量，查表：

| Device 后两位 | 容量 |
| --- | --- |
| `14` | 1 MB |
| `15` | 2 MB |
| `16` | 4 MB |
| `17` | 8 MB |
| `18` | 16 MB |

> 这是 esptool 发 **JEDEC RDID (`0x9F`)** 命令、芯片**实时答复**的。
> 下面整条链路里，**只有这一步真正问过芯片**。

#### 为什么启动日志靠不住

因为 SDK **从来没有探测过 flash 大小**。同一个 `CONFIG_SPI_FLASH_SIZE` 有两个去处：

```c
/* spi_flash.c:67-74 —— 结构体初始化，chip_size 在第 69 行 */
esp_rom_spiflash_chip_t g_rom_flashchip = {
    0x1640ef,
    CONFIG_SPI_FLASH_SIZE,   /* ← chip_size：直接把配置抄过来 */
    64 * 1024,
    4 * 1024,
    256,
    0xffff
};
```

```c
/* spi_flash.c:790-793 —— 名字叫"获取芯片大小" */
size_t spi_flash_get_chip_size()
{
    return g_rom_flashchip.chip_size;   /* ← 还是那个配置值 */
}
```

**函数名骗人。** `get_chip_size()` 不 get 任何东西，它只是把配置读出来。
（`port/port.c:77` 里还有一份同样的赋值。）

#### 后果：擦除能绕过芯片的物理边界

```c
/* spi_flash.c:465 —— 擦除前的边界检查 */
if (sec >= (g_rom_flashchip.chip_size / g_rom_flashchip.sector_size)) {
    return ESP_ERR_FLASH_OP_FAIL;
}
```

**边界和被检查的对象是同一个值。** 配置写错了，"检查"就变成**拿错误去证明错误合法**。

于是那次事故是这样发生的：

```text
配置写 2MB，实际是 1MB
ota_1 排在 0x108000，长 0xF8000
              ↓
1MB 芯片只解码 A0–A19，第 20 位被直接丢弃
              ↓
0x108000 & 0xFFFFF = 0x008000   ← 地址"回绕"，不是报错
              ↓
擦 149 个扇区，从 0x008000 一路往上擦
              ↓
分区表(0x8000)、nvs(0x9000)、otadata(0xD000)、phy_init(0xF000)
以及正在运行的固件的 95.2% —— 全部归零
```

> **为什么连一句报错都没有？**
> 因为负责报错的那段代码，自己也在被擦的范围内。
> 芯片先把打印日志的函数擦掉，再继续调用它——于是串口安静地死掉。
> 这不是"日志看不懂"，是**根本没有机会打印**。

#### 一张表记住

| 想知道什么 | ❌ 别问 | ✅ 该问 |
| --- | --- | --- |
| flash 多大 | 启动日志的 `SPI Flash Size` | `esptool.py flash_id` |
| 分区表对不对 | 启动日志的分区列表 | `gen_esp32part.py --verify`（用法见 [分区表笔记 §12](学习笔记/ESP8266分区表.md#12-怎么验证)） |
| 固件多大 | 猜 | `ls -l build/esp-01S.bin` |

> **一句话**：凡是"SDK 打印出来告诉你"的硬件参数，先想一想它是**问来的**还是**抄配置抄来的**。

### 8.7 本项目当前使用的分区表

1MB 的 ESP-01S，**单 app 槽，没有 OTA**。见根目录的 [`partitions_1mb.csv`](partitions_1mb.csv)：

```text
# Name,   Type, SubType, Offset,   Size, Flags
nvs,       data, nvs,     0x9000,   0x6000,
phy_init,  data, phy,     0xf000,   0x1000,
app,       app,  factory, 0x10000,  0xF0000,
```

首尾相接自检：

```text
0x9000  + 0x6000  = 0xF000     ✓
0xF000  + 0x1000  = 0x10000    ✓
0x10000 + 0xF0000 = 0x100000   ✓  正好 1MB
```

> **巧的是，这张表和 Espressif 自己给 1MB 芯片准备的默认表一字不差。**
> 打开 `components/partition_table/partitions_singleapp.csv`，就是这三行。
> 绕了一大圈，回到的正是官方默认布局。

#### 为什么没有 OTA

**两个槽放不下。** Espressif 自己给 1MB 芯片准备的双槽表
`partitions_two_ota.1MB.csv`，每槽只有 `0x70000` = **448 KB**，
而本项目固件实测 **606544 字节**（≈593 KiB），**超出 144 KiB**。

> **SDK 其实早就知道这件事** —— `partition_table/Kconfig.projbuild:40-41`：
>
> ```text
> default partitions_two_ota.csv      if PARTITION_TABLE_TWO_OTA && ... && !ESPTOOLPY_FLASHSIZE_1MB
> default partitions_two_ota.1MB.csv  if PARTITION_TABLE_TWO_OTA && ... &&  ESPTOOLPY_FLASHSIZE_1MB
> ```
>
> 只要 `FLASHSIZE` 选的是 1MB，再选双 OTA，SDK 会**自动换成小槽表**。
> 出事的那份配置是 **Custom partition table CSV** —— 手写的表绕过了这条保护。

> **代码层面已经把 OTA 摘干净了**：`main/ota.c` / `main/ota.h` 和 `升级` 命令
> 都从工程里移走了，详见 [§9.9](#99-为什么这个工程没有-ota)。

#### 没有 otadata 也能启动吗

能，而且更简单。没有 otadata 分区时，bootloader 直接选 `factory`：

```c
/* bootloader_utility.c:117-118 —— 按 SubType 认，不看名字 */
case PART_SUBTYPE_FACTORY: /* factory binary */
    bs->factory = partition->pos;
```

所以 CSV 里这一行 `app, app, factory, ...` 一定启动得起来，
**不需要任何 OTA 选择逻辑参与**。

#### 换 WiFi 热点要不要改分区表

不要。热点名/密码存在 `nvs` 里，走网络就能改（`main/main.c` 里的 `wifi <SSID>,<密码>` 命令）；
实在连不上了，节点自己会进 SmartConfig 配网。

> **顺带澄清一个常见误解**：OTA 是"换固件"，改密码是"改配置"，两件不相干的事。
> 而且 OTA 本身要走网络——密码改错、连不上了，OTA 也够不着它。
> 真正能救"已经失联"的节点的，是**不依赖路由器**的那条路（SmartConfig）。

烧录后，启动日志里会打印实际生效的分区表（格式见第 7 节），可以核对是否改对了 ——
注意**核对的是三行的 Offset/Size，不是 `SPI Flash Size` 那一行**（理由见 §8.6）。

---

## 9. 联网控制：TCP 命令与 WiFi 配网

前面几节把"能编译、能烧进去、能启动"这条链路走通了。这一节讲的是
它长成**一个能被远程控制的灯开关**之后的样子。

### 9.1 三个模块，各管一段

`main/` 里不是一个越来越大的 `main.c`，而是按**职责**切开的：

| 文件 | 管什么 | 不管什么 |
| --- | --- | --- |
| `main.c` | 继电器怎么动、收到什么命令做什么 | 数据是怎么来的 |
| `wifi_sta.c` | 连热点、存凭据、断线重连、配网兜底 | 连上以后干什么 |
| `tcp_client.c` | 建连接、收发字节 | 这些字节**是什么意思** |

数据的流向：

```text
   手机 / 电脑
   网络调试助手
        │  ① 发 "开灯"
        ▼
   ┌──────────────┐
   │ tcp_client.c │  ② recv() 拿到【裸字节】，原样往上抛
   └──────┬───────┘     （它不知道"开灯"是什么东西）
          │  回调 cmd_on_rx()
          ▼
   ┌──────────────┐
   │    main.c    │  ③ 攒进缓冲区 → 找关键字 → 决定干什么
   └──────┬───────┘
          │  gpio_set_level()
          ▼
       GPIO0 ──→ 继电器
```

**为什么要拆开？** 因为 `tcp_client.c` 里没有一行代码知道"灯"这个东西存在。
将来把继电器换成温度传感器，`tcp_client.c` 一行都不用改。

### 9.2 连上服务端

服务端地址写在 [`main/tcp_client.c`](main/tcp_client.c) 顶部：

```c
#define TCP_SERVER_IP    "192.168.137.1"
#define TCP_SERVER_PORT  8086
```

**`192.168.137.1` 不是随便编的** —— 那是 Windows 开移动热点时虚拟网卡拿到的地址，
也正是 ESP 的**网关**。开机日志里会打印出来：

```text
========== WiFi 连接成功 ==========
  SSID     : DESKTOP-HTLNPUV 4127
  IP       : 192.168.137.100
  子网掩码 : 255.255.255.0
  网关     : 192.168.137.1        ← 必须和上面那个宏一致
===================================
```

> 上面的 IP 只是个例子（DHCP 每台设备分的都不一样，`.100` 没有特殊含义）。
> **真正要对上的是「网关」那一行** —— 它必须等于 `TCP_SERVER_IP`。

> **对不上就连不上。** 换台电脑、或者热点关掉重开，这个地址都可能变。
> 现象是 WiFi 明明连上了，串口却一直刷
> `[tcp] 连接 192.168.137.1:8086 失败 errno=...`。
> **"网通了但连不上服务端"是最容易查错方向的一类问题** ——
> 先去看网关，别急着怀疑 WiFi 驱动。

连上之后 ESP 会先主动发一句 `hello from ESP-01S`，
用来一眼确认这条链路是**双向**通的。

### 9.3 命令表

| 发送 | 动作 | 收到的回复 |
| --- | --- | --- |
| `开灯` / `on` | GPIO0 拉高，继电器吸合 | `LED ON  (GPIO0 = HIGH)` |
| `关灯` / `off` | GPIO0 拉低，继电器释放 | `LED OFF (GPIO0 = LOW)` |
| `wifi <SSID>,<密码>` | 换热点并存进 NVS | `WIFI OK (reconnecting)` |
| `配网` / `config` | 手动进 SmartConfig | `CONFIG MODE (see serial log)` |

回复一律用**纯 ASCII** —— 这样不管调试助手设成 UTF-8 还是 GBK，都不会显示成乱码。

> ⚠ **`wifi` 命令必须勾上"发送新行"。**
> 因为它要等到**整行**才执行：TCP 是字节流，一次 `recv()` 可能只收到
> `wifi DESKTOP`，后面的密码还没来。硬猜的话会把半截 SSID 存进 NVS。
> 所以规矩是**以换行结尾**。其它命令没有这个要求。

### 9.4 坑一：TCP 是字节流，不是消息队列

你在网络调试助手里点一次"发送"，ESP 这边 `recv()` 收到的**可能是**：

```text
① 一次收到完整的 "开灯"          ← 最理想
② 分两次收到 "开" 和 "灯"        ← 被网络拆包了
③ 一次收到 "开灯关灯"            ← 你连点了两次，粘在一起了
```

**所以绝对不能用每次 `recv` 到的内容直接去比较。**
那样 ②③ 两种情况都会失败，而且现象是"**偶尔灵偶尔不灵**"——最难查的那一类。

正确做法是**攒**：收到的字节先堆进一个缓冲区，再在缓冲区里找关键字，
找到就把那一条抠掉，剩下的继续留着等下一批字节来拼。

```c
static char s_cmd_buf[CMD_BUF_SIZE + 1];   /* 64 字节，+1 留给结尾的 '\0' */
static int  s_cmd_len = 0;
```

> 代码里那个 `while (cmd_try_one()) { }` 空循环是有意的：
> **一次收到的字节里可能拼出好几条命令**（就是上面的 ③），
> 得一条一条处理干净，不能只处理一条就走。

### 9.5 坑二：中文命令的编码

`strstr()` 比的是**字节**，不是"字"。同样是"开灯"两个字：

| 命令 | UTF-8 | GBK |
| --- | --- | --- |
| 开灯 | `E5 BC 80 E7 81 AF`（6 字节） | `BF AA B5 C6`（4 字节） |
| 关灯 | `E5 85 B3 E7 81 AF` | `B9 D8 B5 C6` |
| 配网 | `E9 85 8D E7 BD 91` | `C5 E4 CD F8` |

**谁说了算？**

- `.c` 文件里写的 `"开灯"` 是什么字节 → 由**这个文件存成什么编码**决定（本文件是 UTF-8，6 字节）
- 调试助手发出来的是什么字节 → 由**助手的编码设置**决定（中文 Windows 上大多默认 GBK，4 字节）

两边对不上，`strstr` 就永远找不到。现象是：

> **串口明明打印"收到 N 字节"，但继电器不动，也没有回复。**

解法有三层：

1. **命令表里两种编码都收** —— UTF-8 和 GBK 各一行
2. **关键字写成转义字节**，不让它跟着文件编码走：

   ```c
   #define KEY_KAI_UTF8   "\xE5\xBC\x80\xE7\x81\xAF"   /* 开灯 */
   #define KEY_KAI_GBK    "\xBF\xAA\xB5\xC6"           /* 开灯 */
   ```

   这样无论这个文件将来被谁存成什么编码，匹配到的都是同一串字节。
3. **能不用中文就别用** —— `on` / `off` 这些 ASCII 别名没有编码歧义，是最保险的路

串口还会把收到的字节按十六进制打出来，一眼就能对上是哪种编码：

```text
[cmd] 收到 4 字节: BF AA B5 C6         ← 4 字节 = GBK
[cmd] 收到 6 字节: E5 BC 80 E7 81 AF   ← 6 字节 = UTF-8
```

> **一个容易忽略的坑**：`"config"` 里面**含**着 `"on"`（c-**on**-fig）。
> 如果"找到一条就执行"，那收到 `config` 会先去**开灯**。
> 所以代码扫完整张表，挑**位置最靠前**的那条：
> `config` 从下标 0 开始，`on` 从下标 1 开始 —— config 胜出。
> 这也是为什么那张表要**扫完再决定**，而不是找到第一条就返回。

### 9.6 换热点：三层凭据

**热点名和密码不该焊死在固件里。** 焊死的后果是：主节点换个密码，
这个节点就连不上了，只能把板子拆下来插串口线重烧。

所以凭据按优先级存在三个地方：

```text
   ① NVS（命名空间 app_cfg）        ← 运行时改过就用这个，掉电不丢
        ↓ 没有
   ② main/wifi_sta.c 顶部的默认值    ← 编译进固件的"出厂默认值"
        ↓ 没有
   ③ 用 ②，并顺手写进 ①
```

第一次上电时 NVS 是空的，走 ③；之后不管用什么方式改过密码，
都以 NVS 为准，`wifi_sta.c` 顶部那两行就不再生效了。

> **NVS = Non-Volatile Storage**，芯片 flash 里划出来的一小块**键值存储**区
> （就是 §8.7 分区表里的 `nvs` 分区，24 KB）。
> 它跟文件系统不是一回事：没有目录、没有文件，只有 `(命名空间, 键) → 值` 的映射，
> 用起来像个小字典。用 `nvs_open` / `nvs_get_str` / `nvs_set_str` / `nvs_commit` 操作。

**改热点的三种办法，按"节点还连不连得上"选：**

| 节点状态 | 怎么做 |
| --- | --- |
| 还连得上 | 发 TCP 命令 `wifi <SSID>,<密码>` |
| 已经连不上 | **等 60 秒**，它会自己进 SmartConfig（见 §9.7） |
| 想提前配好 | 发 TCP 命令 `config` |

> **为什么用逗号分隔，不用空格？** 因为 **SSID 里可以有空格** ——
> 本项目默认那个 `DESKTOP-HTLNPUV 4127` 就带一个。
> 用空格当分隔符的话，它会被劈成两半。

长度限制来自 802.11 协议：**SSID ≤ 32 字节，密码 ≤ 64 字节**。
超长会在写进 NVS **之前**就被拒绝 —— 因为截断后照样能存进去，但永远连不上，
那种"**命令说成功了、就是连不上**"的毛病最难查。

### 9.7 已经连不上了：SmartConfig 兜底

这是**唯一一条不需要节点认识任何路由器**的路，也是它存在的全部理由。

```text
   节点启动
      │
      ├─ 拿到 IP ─────────────→ 正常工作
      │
      └─ 60 秒还没拿到 IP
             │
             ▼
      进入 SmartConfig，听 90 秒
             │
             │   手机：① 连上【要连的那个热点】
             │        ② 打开 ESP-TOUCH App
             │        ③ 输入该热点密码 → 确认
             │   （App 把 SSID+密码编码成一串特制的 802.11 广播包）
             ▼
      节点在空中收齐、解出来
             │
             ├─ 收到 → 存进 NVS → 用新凭据重连
             └─ 90 秒没听到 → 退出来，恢复自动重连
```

**为什么 90 秒后一定要退出来？**
如果一直卡在配网状态，那热点恢复了、或者只是当时信号不好，
这个节点就**永远回不来了** —— 而它本来只需要当个灯开关。

配网期间还会**暂停自动重连**：一边疯狂重连一边嗅探空中的广播包，是收不到的。

> **整个过程不需要 ESP 认识任何路由器。** 这正是它能救
> "密码改了、节点已经连不上"的原因 —— 那种情况网络已经断了，
> 任何走网络的方案都够不着它。

密码**不会**被打印到串口，日志里只写字节数：

```text
[wifi] 配网：收到凭据
[wifi]   SSID   : NewRouter
[wifi]   密码   : (11 字节，不打印)
[wifi] ✓ 凭据已存入 NVS，掉电不丢
```

### 9.8 顺带两个硬件坑

**① 开灯 = GPIO0 拉高，而 GPIO0 是启动模式选择脚**

```text
复位那一刻芯片重新采样 GPIO0：
   高  →  从 Flash 启动，程序正常运行
   低  →  进 UART 下载模式，【程序根本不跑】
```

而继电器"关灯"时，GPIO0 正好是**低**的。所以将来任何要加重启的地方，
都必须**先把 GPIO0 拉高**（`gpio_set_level(RELAY_GPIO, RELAY_ON)`）——
否则重启后模块是死的，串口一片安静，
**看起来像把板子刷坏了，其实只是进错模式了**。

> 本工程现在没有任何地方会调 `esp_restart()`，所以这条是"**别引入**"的提醒，
> 而不是"要处理"的问题。原来唯一会重启的是 OTA（见 §9.9），它已经移出工程了。

> 注意复位采样那一刻，GPIO0 是**输入 / 高阻态**，芯片只是在"读"这根线，
> 它此刻输出什么无关紧要。让它被读成高的，是**上拉电阻** ——
> 而上拉电阻不是驱动器，只能提供几十 µA。
> 一旦外接电路（比如继电器模块的输入端）把这根线拉低，弱上拉就扛不住，
> 芯片每次复位都会掉进下载模式。
>
> **真正决定命运的是挂在这根线上的外部电路**，不是程序里写 0 还是 1。

**② 供电不够会伪装成"程序跑飞"**

WiFi 发射瞬间电流会冲到 **170~300 mA**，叠加上继电器线圈，
如果还是拿 USB 转串口板那个 3.3V 脚供电，很容易掉电复位（brownout）。

> **模块开始反复重启、串口刷乱码时，先怀疑供电。**
> 在 VCC-GND 之间并 **100~470µF 电解电容 + 0.1µF 瓷片电容**。
> 这类故障和"代码有 bug"的现象几乎一模一样，但**改代码永远改不好**。

### 9.9 为什么这个工程没有 OTA

**OTA（远程升级）在这块板子上做不了，所以代码已经从工程里移走了**，
连同它的命令一起。剩下的是这条推理 —— 它是 §8 分区表那笔账的直接结论。

> 那两个文件没有删，挪到了本工程**外面**保存：
> `../ota-for-larger-flash/`（跟 `esp-01S/` 同级，同一个 `project/` 目录下）。
> 将来换 2MB 的模组，把它们移回 `main/`、换一张双槽分区表就能用。
> 那边的 `README.md` 写了来龙去脉。

要 OTA，分区表里就得有**两块 app 槽**：跑在 A 槽，把新固件写进 B 槽，
写完改 otadata 让 bootloader 下次从 B 启动。关键在"写进**另一块**" ——
不能一边跑一边改自己，那等于边开车边换发动机。

而两块槽意味着**每块都要放得下整个固件**：

```text
1MB flash 去掉 bootloader / 分区表 / nvs / phy_init 之后，正好剩 960 KB（0xF0000）给 app
   │
   ├─ 单槽：960 KB 全给一块              ✅ 现固件约 593 KB，放得下
   │
   └─ 双槽：两块对半分 = 每块 480 KB     ❌ 593 KB > 480 KB，差约 144 KiB
                                         （SDK 自带那张 1MB 双槽表更紧：每槽 448 KB）
```

差多少，[§8.6](#86-怎么确认-flash-到底多大) 里那段算术算过了：**约 144 KiB**。

> **SDK 自己也知道这件事。** `components/partition_table/Kconfig.projbuild:40-41`：
>
> ```text
> default partitions_two_ota.csv      if PARTITION_TABLE_TWO_OTA && ... && !ESPTOOLPY_FLASHSIZE_1MB
> default partitions_two_ota.1MB.csv  if PARTITION_TABLE_TWO_OTA && ... &&  ESPTOOLPY_FLASHSIZE_1MB
> ```
>
> 这边只要 `FLASHSIZE` 选 1MB、又开了双 OTA，SDK 会自动换成小槽表
> （每槽 448 KB）—— 但 448 KB 同样放不下。换成自定义 CSV 只是**绕过了这层保护**，
> 并没有解决问题。

**为什么不是"把固件改小点"？** 这个固件已经很小了 —— 真正占地方的是
SDK 自带的 WiFi 协议栈和 TCP/IP 栈，砍不掉。想留 OTA，起点是 2MB 的模组。

> **顺带说说那个容易搞混的地方**：OTA 是"换固件"，改密码是"改配置"，两件不相干的事。
> 而且 OTA 本身要走网络 —— 密码改错、连不上了，OTA 也够不着它。
> 真正能救"已经失联"的节点的，是 §9.7 那条**不依赖路由器**的路。
> 这也说明：**少了 OTA，这个工程的应急能力一点没少** ——
> 因为在 1MB 的板子上它本来就用不了，能救场的从来都是 §9.7 的 SmartConfig。


