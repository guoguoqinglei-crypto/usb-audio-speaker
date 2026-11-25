#ifndef USB_DESCRIPTORS_H_
#define USB_DESCRIPTORS_H_

#include <stdint.h>

#include <tusb_config.h>    /* 需要 ENABLE_HID_DEMO 决定接口数量 */

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
 * 打开 ENABLE_HID_DEMO 时再多一个 Interface 2（HID 调试接口），
 * 用来在网页上给板子发报告、板子原样回传，验证协议栈通了。
 * ========================================================================== */

enum
{
    ITF_NUM_AUDIO_CONTROL = 0,      /* 音频控制接口 */
    ITF_NUM_AUDIO_STREAMING,        /* 播放流接口（有 alt 0 / alt 1） */
#if ENABLE_USB_MIC
    ITF_NUM_AUDIO_MIC,              /* 麦克风流接口（有 alt 0 / alt 1） */
#endif
#if ENABLE_HID_DEMO
    ITF_NUM_HID,                    /* HID 调试接口（仅验证用） */
#endif
    ITF_NUM_TOTAL
};

/* 音频数据 OUT 端点地址：0x01 = 端点 1，方向 OUT（主机 -> 设备）
 * 注意 OUT 端点的地址最高位是 0，不要写成 0x81（那是 IN 方向的写法） */
#define EPNUM_AUDIO_OUT     0x01
#define EPNUM_AUDIO_IN      0x82   /* 麦克风：等时 IN，端点 2 */

/* HID 中断端点：IN 0x82、OUT 0x02。
 * 为什么两个方向都要有：
 *   Windows 的 HID 栈发输出报告时走的是"中断 OUT 端点"（WriteFile），
 *   只声明 IN 端点的话，网页点 Send 在 Windows 上会直接失败。
 *   两个端点各 64 字节，正好把 PMA 用满（64+128+192+64+64 = 512）。
 * 端点号用 2，不与音频的 EP1 冲突。 */
/* ⚠ HID 调试接口和麦克风都用端点 2/接口 2，不能同时打开 */
#if ENABLE_HID_DEMO && ENABLE_USB_MIC
#error "ENABLE_HID_DEMO 与 ENABLE_USB_MIC 不能同时为 1（接口号与端点号冲突）"
#endif

#define EPNUM_HID_IN        0x82
#define EPNUM_HID_OUT       0x02

#endif /* USB_DESCRIPTORS_H_ */
