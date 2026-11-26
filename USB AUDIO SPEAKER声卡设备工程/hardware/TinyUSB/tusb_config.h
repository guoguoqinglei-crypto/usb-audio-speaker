#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * TinyUSB 配置：STM32F103RC + UAC 1.0 扬声器/麦克风
 * ----------------------------------------------------------------------------
 * 正式版本只枚举音频功能：48kHz/16bit/立体声播放，16kHz/16bit/单声道录音。
 * CDC 调试接口关闭；新增 IN-only 厂商 HID 回传 A/B 与板载音量状态。
 * 状态接口使用 EP3，不占用麦克风 EP2，也不影响标准 UAC 驱动。
 * ========================================================================== */

/* 目标 MCU 与 USB FS Device 控制器 */
#define CFG_TUSB_MCU                OPT_MCU_STM32F1
#ifndef STM32F103xE
#define STM32F103xE
#endif

/* TinyUSB 不启用内置 RTOS 抽象，由 FreeRTOS 的 USB 任务周期推进协议栈 */
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUSB_DEBUG              0

/* 单 USB 控制器，全速设备模式 */
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

/* CDC 调试接口：0 = 关闭；正式声卡固件不使用虚拟串口 */
#define ENABLE_USB_CDC              0
#define CFG_TUD_CDC                 ENABLE_USB_CDC
#define CFG_TUD_CDC_EP_BUFSIZE      64
#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      256

/* 麦克风开关：1 = 扬声器 + 麦克风，0 = 仅扬声器 */
#define ENABLE_USB_MIC              1

/* 声卡状态 HID：1 = 启用，只有 8 字节输入报告，无输出端点 */
#define ENABLE_AUDIO_STATUS_HID     1
#define CFG_TUD_HID                 ENABLE_AUDIO_STATUS_HID
#define CFG_TUD_HID_EP_BUFSIZE      64

/* 一路 UAC 1.0 音频功能，播放方向始终开启，录音方向由麦克风开关决定 */
#define CFG_TUD_AUDIO               1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1
#define CFG_TUD_AUDIO_ENABLE_EP_IN  ENABLE_USB_MIC

/* 播放：48kHz × 2 声道 × 16bit，每 1ms 一包，最大 192 字节 */
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX      192
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ   768

/* 录音：16kHz × 1 声道 × 16bit，每 1ms 一包，最大 32 字节 */
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX       32
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ    256

/* 固定包长录音，不启用 UAC2 流控或异步反馈端点 */
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL        0
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP        0

/* STM32F1 USB 寄存器与 PMA 基地址 */
#define FSDEV_REG_BASE              0x40005C00UL
#define FSDEV_PMA_BASE              0x40006000UL

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
