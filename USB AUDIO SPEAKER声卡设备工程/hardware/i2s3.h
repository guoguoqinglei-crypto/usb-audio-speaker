/**
  ******************************************************************************
  * @file    i2s3.h
  * @brief   I2S3 录音接收（从机模式 + DMA 双缓冲）
  *
  * 【为什么需要单独一路 I2S】
  *   STM32F103 的每个 I2S 只能单工（参考手册 23.2.2：单工通信，仅发送或接收）。
  *   播放走 I2S2（PB12/PB13/PB15，主发送），录音只能再开一路 I2S3（从接收）。
  *
  * 【硬件前提（必须接线，否则收不到数据）】
  *   NAU8822 的 ADCOUT(pin9) → PB5 (I2S3_SD)
  *   Codec 的 BCLK (PB13 网络) → PB3 (I2S3_CK)
  *   Codec 的 LRCLK (PB12 网络) → PA15 (I2S3_WS)
  *   三根杜邦线全部接在底板排针上（IO12→IO16、IO10→IO18、IO9→IO19）。
  *
  *   ⚠ PB3/PA15 复位后默认是 JTAG 引脚，本模块初始化时会关掉 JTAG、
  *     只保留 SWD —— 否则这两个脚不受 GPIO 控制，I2S3 拿不到时钟。
  *     SWD 用的是 PA13/PA14（IO35/IO36），下载调试不受影响。
  *
  * 【时钟来源】
  *   I2S3 做从机，CK/WS 由 Codec 那一侧（实际上是 I2S2 主时钟）提供，
  *   所以录音采样率和播放同源：46.875 kHz / 16 bit / 立体声帧。
  ******************************************************************************
  */

#ifndef _I2S3_H
#define _I2S3_H

#include <stdint.h>

/* I2S3 采样率（与播放同源，由 I2S2 的分频决定） */
#define I2S3_SAMPLE_RATE   46875u

/* 初始化：关 JTAG、配 PA15/PB3/PB5、I2S3 从机接收、DMA2 通道1 双缓冲 */
void I2S3_Init(void);

/* 取左声道样本（单声道，I2S3_SAMPLE_RATE 速率）。
 * 返回实际取出的样本数；返回 0 表示当前没有新数据。
 * 缓冲区大小按 maxSamples 限制，调用者给 128 左右即可。 */
uint16_t I2S3_ReadLeft(int16_t *dst, uint16_t maxSamples);

/* 统计：因读得不够快而丢掉的半缓冲区个数 */
uint32_t I2S3_OverrunCount(void);

/* 累计收到的样本数。上电几秒后仍为 0，说明录音那三根线没接通。 */
uint32_t I2S3_TotalSamples(void);

#endif /* _I2S3_H */
