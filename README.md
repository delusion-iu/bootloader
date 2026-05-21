# STM32 Bootloader + Qt OTA 工具

这是一个面向 `STM32F103C8T6` 的串口 Bootloader 示例仓库，包含三部分内容：

- `BL`：运行在 `0x08000000` 的 Bootloader 工程
- `App`：运行在 `0x08004000` 的示例应用工程
- `tools`：用于串口升级的 Qt 上位机与 Python 脚本

仓库当前以公开阅读和二次整理为目标，重点展示 Bootloader、应用程序和 PC 侧升级工具之间的配合关系，而不是完整产品化方案。

## 项目简介

Bootloader 负责在上电后短时间等待上位机握手，通过串口接收新的应用镜像、写入 Flash、完成 CRC 校验，并在条件满足时跳转到 App。

示例 App 是一个最小化的 STM32F103 工程，当前主要用于演示与 Bootloader 配套的下载地址和启动方式。

PC 侧同时提供两类工具：

- `tools/OTA`：Qt 图形化升级工具，适合演示和人工操作
- `tools/serial_flasher.py`：Python 串口烧录脚本，适合快速调试和命令行验证

## Bootloader + Qt OTA 工具结构说明

这一套结构可以理解为一条完整的固件升级链路：

1. `App` 工程生成目标固件，当前链接地址固定为 `0x08004000`
2. `tools/OTA` 负责选择串口、加载 `.bin` 或 `.hex` 固件、执行握手和分包传输
3. `BL` 在串口侧解析协议帧，擦写 Flash，校验完整镜像
4. 烧录完成后 Bootloader 保存当前应用地址，并在收到 `JUMP` 或启动超时后跳转到应用

换句话说：

- Bootloader 负责“接收、校验、写入、跳转”
- App 负责“作为最终运行的业务固件”
- Qt OTA 工具负责“把固件按当前协议稳定送到 Bootloader”

## 目录结构

仓库主目录如下：

```text
.
|-- BL/                 Bootloader 工程
|-- App/                示例应用工程
|-- tools/              PC 侧升级与调试工具
|-- DIRECTORY.md        目录说明文档
|-- README.md           仓库总览
|-- .gitignore
`-- bootloader.zip      现有归档文件
```

更细的目录说明见 [DIRECTORY.md](./DIRECTORY.md)。

## 各部分分别做什么

### Bootloader

- 使用 `USART1` 作为升级串口，默认参数为 `115200 8N1`
- 位于 Flash 起始地址 `0x08000000`
- 当前 Bootloader 区大小为 `16 KB`
- 接收协议帧并执行 `HELLO / START / DATA / END / JUMP`
- 对接收数据执行 `CRC16/IBM` 校验
- 检查应用向量表是否合法，再决定是否跳转

### App

- 是一个最小可运行的 STM32 应用示例
- 当前链接地址固定为 `0x08004000`
- 主要作用是与 Bootloader 的下载地址、向量表和跳转逻辑配套

### Qt 上位机

- 提供串口选择、固件文件选择、目标地址输入、升级日志和进度展示
- 当前实现走串口分包下载，发送 `HELLO -> START -> DATA -> END -> JUMP`
- 支持加载 `.bin` 与 Intel HEX 文件
- 会在发送前校验目标地址与镜像范围

## 当前 OTA 协议关键流程

仓库中 MCU 与上位机当前对齐的是一套简化串口协议，核心流程如下：

1. 上位机打开串口，向设备发送 `HELLO`
2. Bootloader 回复 `ACK/NACK`，并进入可接收状态
3. 上位机发送 `START`
   - 当前载荷为 8 字节
   - 前 4 字节为镜像长度
   - 后 4 字节为目标 App 地址
4. 上位机按块发送 `DATA`
   - Qt 工具当前默认分包大小为 `128` 字节
   - Bootloader 按顺序写入目标 Flash 区域
5. 上位机发送 `END`
   - 载荷为镜像整体 `CRC16`
   - Bootloader 完成尾字节写入、整体校验和应用有效性检查
6. 上位机发送 `JUMP`
   - Bootloader 在确认应用有效后跳转执行

协议实现还包含这些约束：

- 帧头固定为 `0x55 0xAA`
- 采用顺序序号和逐帧 `ACK/NACK`
- Bootloader 启动后会等待握手，当前超时约为 `9` 秒
- 若已有有效 App 且等待超时，Bootloader 会转去启动 App

## 开发环境与构建方式

### MCU 工程

- 芯片目标：`STM32F103C8T6`
- 工程类型：STM32CubeMX 生成基础工程，Keil MDK-ARM 工程用于构建
- Bootloader 工程：`BL/MDK-ARM/BL.uvprojx`
- App 工程：`App/MDK-ARM/App.uvprojx`

建议的基本构建方式：

1. 使用 Keil 打开对应的 `.uvprojx`
2. 根据需要先在 CubeMX 打开 `.ioc` 检查外设配置
3. 分别编译 `BL` 和 `App`
4. 将 `App` 产物交给 Qt OTA 工具或 Python 脚本下载

### Qt OTA 工具

- 工程文件：`tools/OTA/OTA.pro`
- 依赖模块：`Qt Widgets`、`Qt SerialPort`
- 代码中使用了 `QStringConverter`，因此更适合使用 Qt 6 环境构建

### Python 脚本

- 脚本文件：`tools/serial_flasher.py`
- 依赖：`Python 3.9+`、`pyserial`

安装依赖：

```bash
pip install pyserial
```

## 已知限制

- 当前 `App` 示例工程的链接地址固定为 `0x08004000`，因此正常使用时，下载目标地址也应保持与之相同。
- Bootloader 协议层支持在合法范围内指定目标 App 地址，但如果目标地址与 App 的实际链接地址不一致，应用可能无法正常启动。
- Qt 工具允许输入页面对齐的目标地址，但这不等于任意地址都能直接运行当前示例 App。
- Python 脚本当前把 `0x08004000` 作为保护性默认地址，不适合作为多地址下载器使用。
- 当前仓库中可见实现主要覆盖串口分包、CRC 校验与跳转流程，未体现加密、签名、断点续传或回滚保护等更完整的 OTA 能力。

## 相关文档

- [DIRECTORY.md](./DIRECTORY.md)：目录与关键文件说明
- [tools/README.md](./tools/README.md)：Python 串口烧录脚本说明
