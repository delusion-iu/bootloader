# 仓库目录说明

本文档用于快速说明 `BootLoader` 仓库的目录结构、关键文件和阅读顺序。

## 顶层目录

```text
BootLoader/
|-- BL/
|-- App/
|-- tools/
|-- DIRECTORY.md
|-- README.md
|-- .gitignore
`-- bootloader.zip
```

## 目录说明

### `BL/`

Bootloader MCU 工程目录。

主要内容：

- `BL.ioc`
  - STM32CubeMX 工程文件
- `Core/`
  - CubeMX 生成的启动入口、GPIO、USART、中断等基础代码
- `BSP/bootloader.h`
  - Bootloader 常量、命令字、错误码和外部接口声明
- `BSP/bootloader.c`
  - 串口协议解析、Flash 写入、CRC 校验、应用有效性判断和跳转主逻辑
- `MDK-ARM/BL.uvprojx`
  - Keil 工程文件
- `MDK-ARM/BL/BL.sct`
  - Bootloader 链接脚本，当前放置在 `0x08000000`

### `App/`

示例应用工程目录。

主要内容：

- `App.ioc`
  - STM32CubeMX 工程文件
- `Core/`
  - 示例应用源码，当前逻辑较简单，主要用于验证跳转和运行
- `MDK-ARM/App.uvprojx`
  - Keil 工程文件
- `MDK-ARM/App/App.sct`
  - App 链接脚本，当前放置在 `0x08004000`

### `tools/`

PC 侧工具目录。

主要内容：

- `OTA/`
  - Qt 图形化升级工具源码
- `OTA/OTA.pro`
  - Qt qmake 工程文件
- `OTA/mainwindow.cpp`
  - 串口交互、固件加载、协议发送、日志和界面逻辑的主要实现
- `serial_flasher.py`
  - Python 命令行串口烧录脚本
- `README.md`
  - Python 脚本的独立说明文档

### `bootloader.zip`

仓库现有的归档文件。它不参与当前源码构建流程，保留时更适合作为历史打包产物或备份文件看待。

## 关键关系

可以按下面的顺序理解整个仓库：

1. 先看 `README.md`，了解项目目标和升级链路
2. 再看 `DIRECTORY.md`，定位关键目录和文件
3. 然后重点阅读：
   - `BL/BSP/bootloader.c`
   - `BL/BSP/bootloader.h`
   - `BL/MDK-ARM/BL/BL.sct`
   - `App/MDK-ARM/App/App.sct`
   - `tools/OTA/mainwindow.cpp`
4. 如果需要命令行升级方式，再看 `tools/serial_flasher.py` 和 `tools/README.md`

## 地址与职责对应

- `0x08000000`
  - Bootloader 入口地址
- `0x08004000`
  - 当前示例 App 链接地址
- `BL`
  - 负责接收、校验、写入和跳转
- `App`
  - 负责作为最终运行固件
- `tools/OTA`
  - 负责图形化升级操作
- `tools/serial_flasher.py`
  - 负责命令行升级与协议调试

## 当前阅读时需要特别注意的点

- 虽然 Bootloader 协议支持目标地址字段，但当前示例 App 不是任意地址可运行的通用镜像。
- App 下载地址、链接地址和 Bootloader 校验范围需要保持一致。
- 仓库里同时存在 Qt 工具和 Python 工具，两者用途不同，不应混为同一交付形态。
