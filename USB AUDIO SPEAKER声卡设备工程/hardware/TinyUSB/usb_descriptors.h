#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

#include <stdint.h>

#include <tusb_config.h>    /* 需要 ENABLE_AUDIO_STATUS_HID 决定接口数量 */

/* ============================================================================
 * USB 音频扬声器 —— 接口与端点编号
 * ----------------------------------------------------------------------------
 * UAC 1.0 的扬声器由两个接口组成：
 *
 *   Interface 0  AudioControl（AC）  —— 只放“音量/静音”这类控制信息，没有端点
 *   Interface 1  AudioStreaming（AS）—— 真正搬音频数据，alt=1 时才开端点
 *
 * 为什么 AS 要两个 alt setting：
 *   alt 0：空闲状态，0 个端点，主机没放音乐时停在这里，不占带宽
 *   alt 1：开流状态，1 个等时 OUT 端点，主机开始每 1ms 送 192 字节
 * 这是 USB 音频类的标准做法，Windows 也是靠切换 alt 来启停播放的。
 *
 * 打开 ENABLE_AUDIO_STATUS_HID 时增加厂商 HID 状态接口：
 * 无麦克风时是 Interface 2；启用麦克风时是 Interface 3。
 * 它只回传 A/B 按键、板载音量、序号和 UAC 静音，不代替标准音频控制。
 * ========================================================================== */

enum
{
    ITF_NUM_AUDIO_CONTROL = 0,      /* 音频控制接口 */
    ITF_NUM_AUDIO_STREAMING,        /* 播放流接口（有 alt 0 / alt 1） */
#if ENABLE_USB_MIC
    ITF_NUM_AUDIO_MIC,              /* 麦克风流接口（有 alt 0 / alt 1） */
#endif
#if ENABLE_AUDIO_STATUS_HID
    ITF_NUM_HID,                    /* 声卡状态 HID：A/B + 板载音量 */
#endif
    ITF_NUM_TOTAL
};

/* 音频数据 OUT 端点地址：0x01 = 端点 1，方向 OUT（主机 -> 设备）
 * 注意 OUT 端点的地址最高位是 0，不要写成 0x81（那是 IN 方向的写法） */
#define EPNUM_AUDIO_OUT     0x01
#define EPNUM_AUDIO_IN      0x82   /* 麦克风：等时 IN，端点 2 */

/* HID 仅使用 EP3 IN；EP2 已分配给麦克风，不能再复用。
 * 报告无 Report ID，固定 8 字节报告体，主机只读不写。 */
#if ENABLE_AUDIO_STATUS_HID
#define EPNUM_HID_IN                0x83
#define AUDIO_STATUS_REPORT_LEN     8U
#define AUDIO_STATUS_REPORT_DESC_LEN 21U
#endif

#endif /* USB_DESCRIPTORS_H_ */
