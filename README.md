

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
I (128) boot: SPI Flash Size : 2MB
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

- **`SPI Flash Size : 2MB`**：验证模块确实是 **2MB (16Mbit)** 版本。
  > **风险提示**：ESP-01S 有 1MB 和 2MB 两种常见版本。若 menuconfig 里选错大小（比如选了 1MB），这里就会报错或启动失败。当前日志说明**分区表配置与实际硬件完全匹配**。
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
| `nvs` | 24 KB | 存储 WiFi 凭证等键值对数据 |
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
    (partitions_2mb.csv) Custom partition CSV file
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

### 8.6 本项目当前使用的分区表

2MB 版本 ESP-01S 的布局，见根目录的 [`partitions_2mb.csv`](partitions_2mb.csv)：

```text
# Name,   Type, SubType, Offset,   Size, Flags
#
# Custom 2MB layout for ESP-01S (2MB flash variant).
# NOTE: this file MUST stay pure ASCII - the SDK parser (gen_esp32part.py)
# decodes it as ASCII and aborts on any non-ASCII byte, comments included.
# NOTE: do NOT put trailing comments after the last comma - that column is
# parsed as flags, and any unknown flag is a hard error.
#
nvs,       data, nvs,     0x9000,   0x4000,
otadata,   data, ota,     0xd000,   0x2000,
phy_init,  data, phy,     0xf000,   0x1000,
# main app - extends up to the end of 2MB flash
factory,   app,  factory, 0x10000,  0x1E0000,
# reserved OTA slot - delete and give back to factory if unused
ota_0,     app,  ota_0,   0x1F0000, 0x10000,
```

烧录后，启动日志里会打印实际生效的分区表（格式见第 7 节），可以用来核对是否改对了。


