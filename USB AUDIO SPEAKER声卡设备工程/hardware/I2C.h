#ifndef _I2C_H
#define _I2C_H

#include <stdint.h>

// 软件模拟 I2C 驱动
// 使用 PB6(SCL) / PB7(SDA)，连接 NAU8822 控制接口
// SCL/SDA 必须外接 4.7kΩ 上拉电阻到 3.3V

// NAU8822 I2C 从机固定地址（7bit = 0b0011010 = 0x1A）
// 写地址：0x34（R/W = 0），读地址：0x35（R/W = 1）
#define NAU8822_I2C_WR    0x34
#define NAU8822_I2C_RD    0x35

// NAU8822 设备 ID 寄存器和固定返回值
#define NAU8822_REG_DEVICE_ID  0x3F
#define NAU8822_DEVICE_ID      0x01A

// I2C1 主模式初始化（软件模拟）
// 配置 PB6/PB7 为开漏输出，SCL/SDA 由 GPIO 模拟时序
void I2C1_Master_Init(void);

// I2C1 向指定从机地址写入多个字节（轮询方式，带超时）
// 参数 slaveAddr：8 位从机地址（已包含 R/W 位，如 0x34/0x35）
// 参数 data：待发送数据缓冲区
// 参数 len：待发送字节数
// 返回值：0 成功，1 失败
uint8_t I2C1_WriteData(uint8_t slaveAddr, uint8_t *data, uint8_t len);

// 从指定从机地址读取多个字节，最后一个字节返回 NACK
uint8_t I2C1_ReadData(uint8_t slaveAddr, uint8_t *data, uint8_t len);

// 向 NAU8822 指定寄存器写入 9bit 数据
// 参数 reg：寄存器地址 0~0x7F
// 参数 val：9bit 寄存器值（0~0x1FF）
// 返回值：0 成功，1 失败
uint8_t NAU8822_WriteReg(uint8_t reg, uint16_t val);

// 从 NAU8822 指定寄存器读取 9bit 数据
// 参数 reg：寄存器地址 0~0x7F
// 参数 val：读取到的寄存器值保存指针
// 返回值：0 成功，1 失败
uint8_t NAU8822_ReadReg(uint8_t reg, uint16_t *val);

// 检查 NAU8822 I2C 通信是否正常
// 读取设备 ID 寄存器并校验固定 ID 0x01A
// 参数 chipId：保存读取到的设备 ID，允许传入 0
// 返回值：0 通信和 ID 均正常，1 通信失败或 ID 错误
uint8_t NAU8822_CheckCommunication(uint16_t *chipId);

// NAU8822 上电初始化（消除爆音、配置时钟和音频通路）
void NAU8822_Init(void);

// 上电麦克风录音通路（MICBIAS + PGA + ADC），必须在 NAU8822_Init() 之后调用
void NAU8822_EnableMic(void);

// 设置 NAU8822 耳机输出音量（左右声道同时更新）
// 参数 volume: 0~100，0=静音，100=最大音量(+6dB)
// 实际映射：0 -> 0x00(-57dB 以下静音), 100 -> 0x3F(+6dB)
void NAU8822_SetVolume(uint8_t volume);

/* 耳机增益直接设置（0x00 = -57dB … 0x3F = +6dB），不带静音位。
 * 用于"句间无声段软静音"：分步改变增益让底噪一起降下去，
 * 又不会像静音位/自动静音那样硬切换而在下一句开口时产生"哒"声。 */
void NAU8822_SetHpGainRaw(uint8_t gain);

/* 取用户设定的耳机增益（0x00~0x3F），软静音结束后恢复到该值 */
uint8_t NAU8822_GetHpGainUser(void);

#endif
