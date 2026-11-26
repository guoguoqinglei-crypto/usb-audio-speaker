# STM32 USB Audio Speaker 固件

## 工程信息

- 主控：STM32F103RCT6
- Codec：NAU8822 / NAU88C22 兼容器件
- USB：TinyUSB，UAC 1.0
- 播放：48 kHz / 16 bit / 立体声
- 录音：16 kHz / 16 bit / 单声道
- Keil 工程：`Project.uvprojx`
- 可烧录文件：`Objects/Project.hex`

交付配置已在 `hardware/TinyUSB/tusb_config.h` 中设置：

```c
#define ENABLE_USB_MIC 1
```

## 编译

在 Keil µVision 中打开工程并 Rebuild。当前完整构建结果为：

```text
Program Size: Code=45144 RO-data=1148 RW-data=36 ZI-data=10404
0 Error(s), 470 Warning(s)
FromELF: creating hex file...
```

告警主要来自 STM32 标准外设库与 TinyUSB 的严格编译检查。完整日志为工程根目录的 `build_firmware.log`。

## 上板前检查

1. PB6/PB7 的软件 I²C 有合适上拉，并能读到正确 Codec ID。
2. I²S2 的 PB12、PB13、PB15、PC6 与 Codec 对应引脚连接。
3. 录音额外连接 ADCOUT→PB5、BCLK→PB3、LRCLK→PA15，并确保共地。
4. PA15/PB3 的 JTAG 功能已由 `i2s3.c` 关闭，SWD 下载调试仍保留。
5. 调试串口 USART1 使用 921600 8N1。

完整设计、流程和验收步骤见 `../docs/USB_Audio_Speaker设计文档.md`。
