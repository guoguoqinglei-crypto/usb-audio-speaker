# USB Audio Speaker 声卡设备

基于 STM32F103RCT6、NAU8822 兼容 Codec 和 TinyUSB 的 USB Audio Class 1.0 声卡，包含 Windows Qt 上位机。

## 主要功能

- 48 kHz／16 bit／立体声 USB 播放，I²S＋DMA 输出到 Codec
- 16 kHz／16 bit／单声道 USB 录音与上位机回放
- 支持电脑音量、静音控制和上位机播放测试
- A（PC4）／B（PC5）独立状态机控制板载 Codec 音量 ±1；20 ms 消抖、长按不连发、0～100 边界饱和
- 新增厂商 HID 状态接口，上位机分别显示 Windows 软件音量、板载 Codec 音量及 A／B 实时状态
- Qt 上位机支持录音、回放及离线文字转语音

## 仓库内容

- `USB AUDIO SPEAKER声卡设备工程/`：STM32 Keil 固件工程与源码。
- `USB AUDIO SPEAKER声卡设备上位机/`：Windows Qt 上位机可执行程序及运行库。
- `firmware/Project.hex`：可直接烧录的固件（源码中已提供时）。
- 根目录设计文档：项目原理、协议、流程、调试方法与测试结果。

## 构建与烧录

1. 使用 Keil µVision 打开 `USB AUDIO SPEAKER声卡设备工程/Project.uvprojx`。
2. 执行 Build／Rebuild，生成固件。
3. 也可直接烧录 `firmware/Project.hex`（若仓库包含该文件）。
4. 主控为 STM32F103RCT6；具体引脚、供电和接线以设计文档为准。

## 上位机

进入 `USB AUDIO SPEAKER声卡设备上位机/`，运行 `appUSBAUDIOSPEAKER.exe`。请保留整个目录，不要只复制 EXE，以免缺少 Qt 插件或运行库。

## 设计文档

- `USB Audio Speaker声卡设备设计文档.docx`
- `USB Audio Speaker声卡设备设计文档.pdf`

## 开源许可

项目自有代码与文档采用 [MIT License](LICENSE)。第三方组件遵循各自原始许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 可选离线语音模型

为遵守 GitHub 单文件大小限制，仓库不包含 `stt/ggml-base.bin`（约 141 MB）。只有使用相关离线语音识别功能时才需要自行准备该模型，并放到上位机 `stt/` 目录；普通播放、录音、音量控制和文字转语音不依赖此文件。
