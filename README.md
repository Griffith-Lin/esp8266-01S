# ESP-01S (ESP8266) 开发环境笔记

在 Windows 下使用 ESP8266_RTOS_SDK 开发 ESP-01S 的完整搭建、编译、烧录记录。

## 目录

- [1. 工具链说明](#1-工具链说明)
- [2. 环境变量配置](#2-环境变量配置)
- [3. Python 依赖安装](#3-python-依赖安装)
- [4. 创建示例工程](#4-创建示例工程)
- [5. 配置与构建](#5-配置与构建)
- [6. 波特率说明](#6-波特率说明)
- [7. 启动日志分析](#7-启动日志分析)
- [8. 下一步建议](#8-下一步建议)

---

## 1. 工具链说明

使用 `mingw32.exe` 进入终端。

> **注意**：`esp8266_toochain` 不同步至代码仓库。

ESP8266_RTOS_SDK 位于：

```
G:\github\esp-01S\esp8266_toochain\msys32\home\Administrator\esp\ESP8266_RTOS_SDK
```

> **路径限制**：ESP8266_RTOS_SDK 构建系统**不支持**在 SDK 或项目路径中使用空格。

---

## 2. 环境变量配置

`mingw32` **无法读取 Windows 系统环境变量**，因此每次进入终端都需要手动设置。虽然 `IDF_PATH` 会和系统的 esp-idf 冲突，但冲突本身无所谓——根本原因是读不到。

```bash
# SDK 路径
export IDF_PATH=~/esp/ESP8266_RTOS_SDK

# Python（须确认是 Python 3，且为 32 位）
export PATH="/c/Users/Administrator/AppData/Local/Programs/Python/Python313-32:$PATH"

# 交叉编译工具链
export PATH="$PATH:/g/github/esp-01S/esp8266_toochain/xtensa-lx106-elf/bin"
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
cd ~/esp
cp -r $IDF_PATH/examples/get-started/hello_world .
```

---

## 5. 配置与构建

### 5.1 项目配置

```bash
cd ~/esp/hello_world
make menuconfig
```

在 menuconfig 中将串口配置为 `//./COM8`（Windows 下的写法），根据具体COM口作修改。

### 5.2 编译并烧录

> 烧录前需先让 ESP-01S 进入**下载模式**（将io0接地）。

```bash
make flash
```

### 5.3 打开串口监视器 （先进入运行模式：断开 GPIO0 与 GND 的连线（ESP-01S 板载 10k 上拉，浮空即为高电平），然后复位。）
```bash
make monitor
```

退出监视器：<kbd>Ctrl</kbd> + <kbd>]</kbd>

### 5.4 加速编译

```bash
make -j4
```

ESP-01S 项目建议最多 4 线程，实测 10 线程也可行。

### 5.5 构建失败时

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


