#ifndef _I2S_H
#define _I2S_H

// 包含标准整数类型定义
#include <stdint.h>
// 包含 STM32F10x 标准库头文件
#include <stm32f10x.h>

// I2S2 硬件连接说明（STM32F103RCT6）
// PB12 - I2S2_WS  (字选 / LRCK)
// PB13 - I2S2_CK  (串行时钟 / BCLK)
// PB15 - I2S2_SD  (串行数据 / DATA)
// PC6  - I2S2_MCK (主时钟输出，固定为 256 × Fs)

// 一个"半区"包含的半字个数：5 ms @ 48 kHz
// 240 帧 × 每帧 2 个半字（左、右）= 480 个半字 = 960 字节
// 为什么从 1 ms 加深到 5 ms：
//   1 ms 的半区意味着 DMA 每 1 ms 就要数据，而 USB 也是每 1 ms 来一包，
//   两边只要有一点点相位差就会欠载（听感上就是"突然停一下"）。
//   5 ms 的半区等于给回填留了 5 倍的余量，USB 侧抖动一两毫秒也顶得住。
#define AUDIO_HALF_HALFWORDS   480u

// 双缓冲区总长：10 ms = 960 个半字 = 1920 字节（RAM 够用）
#define AUDIO_BUF_HALFWORDS    (AUDIO_HALF_HALFWORDS * 2u)

// I2S2 初始化
// 配置为主发送模式、Philips 标准、16 位数据、16 位声道、MCK 输出
// 在 SYSCLK = 72MHz 时，采样率约为 46.875kHz（目标 48kHz，误差 2.34%）
// 注意：受 72MHz 整数分频限制，无法精确得到 48kHz（需 73.728MHz，8MHz 晶振不可达）
void I2S2_Init(void);

// 启动 I2S2 DMA 循环播放（单缓冲，仅用于固定正弦波自测）
// buffer: 16 位 PCM 数据缓冲区
// len:    缓冲区半字个数（即采样点个数）
void I2S2_Play(uint16_t *buffer, uint32_t len);

// 启动 I2S2 DMA 双缓冲流式播放（用于音效文件）
// src:    单声道 16 位 PCM 样本数组（通常放在 Flash）
// samples: 样本个数（不是字节数，也不是半字数）
// 说明：内部维护 384 字节双缓冲区，半传输/传输完成中断自动回填，
//       播完自动从头循环；同一采样会同时写进左右两个声道。
void I2S2_PlayEffect(const int16_t *src, uint32_t samples);

// 只启动 I2S2 双缓冲播放，输出静音（不播任何本地音效）
// 用途：DMA 必须跑起来主机才有声音，但不需要开机自检音
void I2S2_StartSilence(void);

/* 句间软静音服务：由低频任务周期调用（约 2ms 一次）。
 * 主机送来的数据连续无声超过约 50ms 时，把 DAC 数字音量分步拉低到静音；
 * 一旦重新出现有效样本再分步恢复。目的是消除"每句话开口时哒一下"的问题。 */
void I2S2_SilenceMuteService(void);

// 停止 I2S2 DMA 播放
void I2S2_Stop(void);

#endif
