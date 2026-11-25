#ifndef USB_AUDIO_H_
#define USB_AUDIO_H_

#include <stdint.h>

/* ============================================================================
 * USB 音频设备层与音频缓冲区接口
 * ----------------------------------------------------------------------------
 * UsbAudio_Init()：配置 USB 时钟、引脚、1ms 时基并启动 TinyUSB。
 * UsbAudio_Task()：由 FreeRTOS 的 USB 任务每 1ms 调用，推进协议栈与数据泵。
 * 播放数据经环形缓冲交给 I2S2 DMA；录音数据经 I2S3 DMA 送入 USB IN 端点。
 * ========================================================================== */

/* TIM4 维护的上电毫秒时基，供 TinyUSB 与音频任务使用 */
uint32_t hal_time_ms(void);

/* 上电早期拉低 D+（PA12），待 Codec 与 DMA 就绪后再允许主机枚举 */
void UsbAudio_PreInit(void);

/* 启动 TIM4 1ms 时基；函数幂等，可重复调用 */
void UsbAudio_TimeBaseInit(void);

/* 初始化 USB 时钟、引脚、时基与 TinyUSB 协议栈 */
void UsbAudio_Init(void);

/* 由 FreeRTOS 的 USB 任务周期调用，不能长时间阻塞 */
void UsbAudio_Task(void);

/* 返回主机是否已打开播放流；供 DMA 中断选择 USB 数据或静音 */
uint8_t UsbAudio_IsStreaming(void);

/* 返回是否允许本地音效；正式 USB 声卡模式下返回 0 */
uint8_t UsbAudio_AllowLocalEffect(void);

/* 从播放环形缓冲读取立体声帧；不足时由调用方补静音 */
uint32_t UsbAudio_ReadFrames(int16_t *dst, uint32_t frames);

/* 读取并完成 48kHz -> 46.875kHz 线性插值重采样 */
uint32_t UsbAudio_ReadResampled(int16_t *dst, uint32_t frames);

/* 当前播放缓冲区中的帧数 */
uint16_t UsbAudio_BufferedFrames(void);

/* 播放缓冲溢出与欠载统计 */
uint32_t UsbAudio_OverflowCount(void);
uint32_t UsbAudio_UnderrunCount(void);

#endif /* USB_AUDIO_H_ */
