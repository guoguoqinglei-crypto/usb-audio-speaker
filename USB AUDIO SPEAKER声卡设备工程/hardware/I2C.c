// 软件模拟 I2C 驱动代码（用于 NAU8822）
// 使用 PB6(SCL) / PB7(SDA)
// SCL/SDA 必须外接 4.7kΩ 上拉电阻到 3.3V

#include <I2C.h>
#include <stm32f10x.h>
#include <delay.h>
#include <uart.h>

// I2C 引脚定义
#define SCL_PIN     GPIO_Pin_6      // PB6 作为 SCL
#define SDA_PIN     GPIO_Pin_7      // PB7 作为 SDA
#define I2C_GPIO    GPIOB

// 软件 I2C 延时，5us 约对应 100kHz
#define I2C_DELAY() Delay_us(5)

// 超时计数，防止总线死锁
#define I2C_TIMEOUT 1000

// -----------------------------------------------------------------------------
// 底层 GPIO 控制函数
// -----------------------------------------------------------------------------

// SCL 置高
static void SCL_H(void)
{
    GPIO_SetBits(I2C_GPIO, SCL_PIN);
}

// SCL 置低
static void SCL_L(void)
{
    GPIO_ResetBits(I2C_GPIO, SCL_PIN);
}

// SDA 置高（释放总线，由上拉电阻拉高）
static void SDA_H(void)
{
    GPIO_SetBits(I2C_GPIO, SDA_PIN);
}

// SDA 置低（拉低总线）
static void SDA_L(void)
{
    GPIO_ResetBits(I2C_GPIO, SDA_PIN);
}

// 读取 SDA 电平
// 返回值：0 表示低电平，1 表示高电平
static uint8_t SDA_Read(void)
{
    return GPIO_ReadInputDataBit(I2C_GPIO, SDA_PIN);
}

// I2C 起始信号：SCL 高电平时，SDA 从高变低
static void I2C_Start(void)
{
    SDA_H();
    SCL_H();
    I2C_DELAY();
    SDA_L();
    I2C_DELAY();
    SCL_L();
    I2C_DELAY();
}

// I2C 重复起始信号：在总线已被占用时，SCL 高电平期间 SDA 从高变低
// 用于读写转换时，不释放总线直接发起新一轮传输
static void I2C_ReStart(void)
{
    SDA_H();            // 先释放 SDA，确保为高电平
    I2C_DELAY();
    SCL_H();            // 拉高 SCL
    I2C_DELAY();
    SDA_L();            // SCL 高电平时 SDA 从高变低，产生 ReSTART
    I2C_DELAY();
    SCL_L();            // 拉低 SCL，准备传输数据
    I2C_DELAY();
}

// I2C 停止信号：SCL 高电平时，SDA 从低变高
static void I2C_Stop(void)
{
    SDA_L();
    SCL_L();
    I2C_DELAY();
    SCL_H();
    I2C_DELAY();
    SDA_H();
    I2C_DELAY();
}

// 发送应答信号：SCL 高电平时 SDA 为低
static void I2C_Ack(void)
{
    SDA_L();
    I2C_DELAY();
    SCL_H();
    I2C_DELAY();
    SCL_L();
    I2C_DELAY();
}

// 发送非应答信号：SCL 高电平时 SDA 为高
static void I2C_Nack(void)
{
    SDA_H();
    I2C_DELAY();
    SCL_H();
    I2C_DELAY();
    SCL_L();
    I2C_DELAY();
}

// 等待从机应答
// 返回值：0 收到 ACK，1 未收到 ACK（NACK 或超时）
static uint8_t I2C_WaitAck(void)
{
    uint16_t timeout = I2C_TIMEOUT;

    SDA_H();            // 释放 SDA，等待从机拉低
    I2C_DELAY();
    SCL_H();            // 拉高 SCL，从机在此时输出 ACK
    I2C_DELAY();

    // 等待从机拉低 SDA（ACK），或超时
    while (SDA_Read())
    {
        if (--timeout == 0)
        {
            SCL_L();
            return 1;   // 超时，无 ACK
        }
    }

    SCL_L();
    return 0;           // 收到 ACK
}

// 发送一个字节，高位先发
static void I2C_SendByte(uint8_t byte)
{
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        if (byte & 0x80)
        {
            SDA_H();
        }
        else
        {
            SDA_L();
        }
        byte <<= 1;

        I2C_DELAY();
        SCL_H();
        I2C_DELAY();
        SCL_L();
        I2C_DELAY();
    }
}

// 读取一个字节
// 参数 ack：1 发送 ACK，0 发送 NACK
// 返回值：读取到的字节
static uint8_t I2C_ReadByte(uint8_t ack)
{
    uint8_t i;
    uint8_t byte = 0;

    SDA_H();            // 释放 SDA，准备读取

    for (i = 0; i < 8; i++)
    {
        byte <<= 1;
        SCL_H();
        I2C_DELAY();

        if (SDA_Read())
        {
            byte |= 0x01;
        }

        SCL_L();
        I2C_DELAY();
    }

    if (ack)
    {
        I2C_Ack();
    }
    else
    {
        I2C_Nack();
    }

    return byte;
}

// -----------------------------------------------------------------------------
// I2C1 主模式初始化（软件模拟）
// -----------------------------------------------------------------------------

// 初始化 PB6/PB7 为开漏输出，并置高释放总线
void I2C1_Master_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    // 开启 GPIOB 时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    // 配置 PB6(SCL) 和 PB7(SDA) 为开漏输出
    GPIO_InitStructure.GPIO_Pin = SCL_PIN | SDA_PIN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_Init(I2C_GPIO, &GPIO_InitStructure);

    // 初始状态：SCL 和 SDA 都释放为高
    SCL_H();
    SDA_H();

    // 上电后延时一段时间，等待 NAU8822 完成内部复位
    Delay_ms(10);
}

// -----------------------------------------------------------------------------
// 通用 I2C 发送函数
// -----------------------------------------------------------------------------

// 向指定从机地址写入多个字节
// 参数 slaveAddr：8 位从机地址（含 R/W 位，如 0x34/0x35）
// 参数 data：待发送数据缓冲区
// 参数 len：待发送字节数
// 返回值：0 成功，1 失败
uint8_t I2C1_WriteData(uint8_t slaveAddr, uint8_t *data, uint8_t len)
{
    uint8_t i;

    I2C_Start();

    I2C_SendByte(slaveAddr);
    if (I2C_WaitAck())
    {
        I2C_Stop();
        return 1;
    }

    for (i = 0; i < len; i++)
    {
        I2C_SendByte(data[i]);
        if (I2C_WaitAck())
        {
            I2C_Stop();
            return 1;
        }
    }

    I2C_Stop();
    return 0;
}

// 从指定从机读取多个字节
uint8_t I2C1_ReadData(uint8_t slaveAddr, uint8_t *data, uint8_t len)
{
    uint8_t i;

    // 第 1 步：产生起始条件并发送读地址
    I2C_Start();
    I2C_SendByte(slaveAddr);
    if (I2C_WaitAck())
    {
        I2C_Stop();
        return 1;
    }

    // 第 2 步：连续读取数据，前 len-1 个字节返回 ACK
    for (i = 0; i < len; i++)
    {
        data[i] = I2C_ReadByte((i + 1U < len) ? 1U : 0U);
    }

    // 第 3 步：产生停止条件
    I2C_Stop();
    return 0;
}

// -----------------------------------------------------------------------------
// NAU8822 寄存器读写函数
// -----------------------------------------------------------------------------

// 向 NAU8822 指定寄存器写入 9bit 数据
// 参数 reg：寄存器地址 0~0x7F
// 参数 val：9bit 寄存器值（0~0x1FF）
// 返回值：0 成功，1 失败
uint8_t NAU8822_WriteReg(uint8_t reg, uint16_t val)
{
    uint8_t buf[2];

    // NAU8822 的 2-wire 写操作在设备地址后发送：
    // [A6:A0, D8], [D7:D0]
    buf[0] = (uint8_t)((reg << 1) | ((val >> 8) & 0x01));
    buf[1] = (uint8_t)(val & 0xFF);

    return I2C1_WriteData(NAU8822_I2C_WR, buf, 2);
}

// 从 NAU8822 指定寄存器读取 9bit 数据
// 流程：START + 0x34 + (reg << 1) + REP_START + 0x35 + D8 + ACK + D7~D0 + NACK + STOP
// 参数 reg：寄存器地址 0~0x7F
// 参数 val：读取到的寄存器值保存指针
// 返回值：0 成功，1 失败
uint8_t NAU8822_ReadReg(uint8_t reg, uint16_t *val)
{
    uint8_t rxBuf[2];

    // 第一步：发送起始条件
    I2C_Start();

    // 第二步：发送设备写地址 0x34
    I2C_SendByte(NAU8822_I2C_WR);
    if (I2C_WaitAck())
    {
        UART1_SendString("NAU8822 Read: No ACK after 0x34\r\n");
        I2C_Stop();
        return 1;
    }

    // 第三步：发送寄存器地址
    // 读操作中的控制地址低 1 位固定为 0
    I2C_SendByte((uint8_t)(reg << 1));
    if (I2C_WaitAck())
    {
        UART1_SendString("NAU8822 Read: No ACK after reg addr\r\n");
        I2C_Stop();
        return 1;
    }

    // 第四步：发送重复起始条件
    I2C_ReStart();

    // 第五步：发送设备读地址 0x35
    I2C_SendByte(NAU8822_I2C_RD);
    if (I2C_WaitAck())
    {
        UART1_SendString("NAU8822 Read: No ACK after 0x35\r\n");
        I2C_Stop();
        return 1;
    }

    // 第六步：读取数据高字节（bit8）
    rxBuf[0] = I2C_ReadByte(1);     // 发送 ACK，继续读取

    // 第七步：读取数据低字节（bit7~bit0）
    rxBuf[1] = I2C_ReadByte(0);     // 发送 NACK，读取结束

    // 第八步：发送停止条件
    I2C_Stop();

    // 组合为 9bit 寄存器值
    // 高字节 (rxBuf[0]) 只有 bit0 有效，对应 D8
    // 低字节 (rxBuf[1]) 8 位全有效，对应 D7~D0
    *val = (uint16_t)((((uint16_t)rxBuf[0] & 0x01) << 8) | rxBuf[1]);

    return 0;
}

// 检查 NAU8822 I2C 通信是否正常
// 读取设备 ID 寄存器并校验固定 ID 0x01A
uint8_t NAU8822_CheckCommunication(uint16_t *chipId)
{
    uint16_t value;

    if (NAU8822_ReadReg(NAU8822_REG_DEVICE_ID, &value) != 0)
    {
        return 1;
    }

    if (chipId != 0)
    {
        *chipId = value;
    }

    if ((value & 0x01FF) != NAU8822_DEVICE_ID)
    {
        return 1;
    }

    return 0;
}

// -----------------------------------------------------------------------------
// NAU8822 上电初始化
// -----------------------------------------------------------------------------

/* ---------------------------------------------------------------------------
 * R10 (0x0A) DAC Control 的取值 —— 寄存器手册 Rev 0.6 第 77 页位定义：
 *   bit8~bit0 = 0, 0, SOFTMT, 0, 0, DACOS, AUTOMT, RDACPL, LDACPL
 *
 *   0x008 = 只开 128x 过采样（当前使用）
 *   0x00C = 128x 过采样 + 自动静音（已停用，见下）
 *
 * 手册 5.2 节说明 AUTOMT 的行为：
 *   连续 1024 个采样点为 0（46.875 kHz 下约 21.8 ms）后，DAC 模拟输出**直接**静音；
 *   只要出现任何一个非零样本，立刻解除静音，计数清零。
 *   注意是"直接"切换，没有斜坡 —— 所以它能让无声段彻底安静，
 *   但每句话重新开口的瞬间会有一声"啪"。
 *
 * 【为什么改回 0x008】
 *   语音合成（TTS）每句话之间有 280~870 ms 的长停顿，远超 21.8 ms 的阈值，
 *   导致 DAC 反复"硬静音 → 解静音"，听起来就是"每讲一句话哒一下"。
 *   实测合成的 WAV 波形在句间边界跳变只有 0.0002（满幅 1.0），
 *   说明声音本身是干净的，问题就出在这个 AUTOMT 硬切换上。
 *   关掉自动静音后句间会有 Codec 本身的轻微底噪，但不会再有这声"哒"。
 * ------------------------------------------------------------------------- */
#define NAU8822_R10_VALUE   0x008

// NAU8822 播放初始化（消除爆音并配置 I2S 音频通路）
// 严格按照数据手册 Rev 3.3 第 11.2.1 节 Power Up 流程配置：
// 1. 软件复位 -> 2. 输出 boost 关闭 -> 3. 使能 DC/IO 缓冲 -> 4. 使能模拟偏置并等待 VREF 建立
// 5. 配置 I2S 接口 -> 6. 配置时钟 -> 7. 使能 DAC -> 8. DAC 音量
// 9. 配置输出混音器 -> 10. 使能耳机放大器 -> 11. 解除静音
void NAU8822_Init(void)
{
    // 第1步：软件复位，向 R0 写入任意值，触发内部复位
    NAU8822_WriteReg(0x00, 0x000);
    Delay_ms(10);

    // 第2步：VDDSPK <= 3.6V 时，关闭 speaker/aux 输出的 boost 模式
    // R49 = 0x000
    // Bit 4: AUX1BST = 0
    // Bit 3: AUX2BST = 0
    // Bit 2: SPKBST  = 0
    NAU8822_WriteReg(0x31, 0x000);

    // 第3步：使能内部 DC / IO tie-off 缓冲（消除上电爆音）
    // R1 = 0x104
    // Bit 8: DCBUFEN = 1  (DC 缓冲使能，用于非 boost 模式)
    // Bit 2: IOBUFEN = 1  (IO 缓冲使能)
    NAU8822_WriteReg(0x01, 0x104);

    // 第4步：使能内部模拟偏置缓冲，并将参考电阻设置为 80kΩ，让电容缓慢充电
    // R1 = 0x10D
    // Bit 8: DCBUFEN = 1
    // Bit 2: IOBUFEN = 1
    // Bit 1~0: REFIMP = 01 (80kΩ 内部参考电阻)
    // Bit 4: ABIASEN = 1 (模拟偏置缓冲使能)
    NAU8822_WriteReg(0x01, 0x10D);

    // 关键延时：等待 VREF 建立，消除上电爆音
    Delay_ms(250);

    // 第5步：配置音频接口格式（I2S / 16bit / 从机模式）
    // R4 = 0x010
    // Bit 6~5: WLEN = 00，16bit 数据
    // Bit 4~3: AIFMT = 10，标准 I2S 格式 (Philips)
    // Bit 7:   BCLKP = 0，正常相位
    // Bit 5:   LRP   = 0，正常相位
    NAU8822_WriteReg(0x04, 0x010);

    // R5 = 0x000：关闭 μ-law / A-law 压扩，关闭 ADC 到 DAC 直通
    // Bit 3: DACMUTE，bit 2: DEEMPH，默认均为 0，按普通线性 PCM 处理
    // 显式写出以防芯片复位不彻底导致残留压扩设置
    NAU8822_WriteReg(0x05, 0x000);

    // R7 = 0x000：48 kHz 对应的内部数字滤波器系数
    // 注意：该寄存器不决定实际采样率，实际 Fs 由 I2S2 分频（46.875 kHz）决定
    // 32 kHz 用 0x002，16 kHz 用 0x006，8 kHz 用 0x00A
    NAU8822_WriteReg(0x07, 0x000);

    // 第6步：配置时钟（外部 MCLK 输入，从机模式，MCLK 不分频）
    // R6 = 0x000
    // Bit 8:   CLKM = 0，选择 MCLK 作为内部主时钟
    // Bit 7~5: MCLKSEL = 000，MCLK 不分频 (256fs)
    // Bit 4~2: BCLKSEL = 000，BCLK 输出不分频（从机模式下无效）
    // Bit 0:   CLKIOEN = 0，FS/BCLK 为输入（从机模式）
    NAU8822_WriteReg(0x06, 0x000);

    // 第7步：使能 DAC 电源
    // R3 = 0x00F
    // Bit 3: RMIXEN = 1 (右声道混音器使能)
    // Bit 2: LMIXEN = 1 (左声道混音器使能)
    // Bit 1: RDACEN = 1 (右声道 DAC 使能)
    // Bit 0: LDACEN = 1 (左声道 DAC 使能)
    NAU8822_WriteReg(0x03, 0x00F);

    // 第7.1步：配置 DAC 控制寄存器 —— 消除底噪的关键
    // 取值见文件上方 NAU8822_R10_VALUE 的说明（当前 0x008：关自动静音，避免句间哒声）
    NAU8822_WriteReg(0x0A, NAU8822_R10_VALUE);

    // 第8步：设置 DAC 音量（0dB，不静音，同步更新左右声道）
    // R11 = 0x1FF (bit8=LDACVU 更新位, bit7~0=0xFF=0dB)
    // R12 = 0x1FF (bit8=RDACVU 更新位, bit7~0=0xFF=0dB)
    NAU8822_WriteReg(0x11, 0x1FF);
    NAU8822_WriteReg(0x12, 0x1FF);

    // 第9步：配置输出混音器（将 DAC 输出路由到耳机）
    // R50 = 0x001 (左声道混音器：LDACLMX = 1，左 DAC 接入)
    // R51 = 0x001 (右声道混音器：RDACRMX = 1，右 DAC 接入)
    NAU8822_WriteReg(0x32, 0x001);
    NAU8822_WriteReg(0x33, 0x001);

    // 第10步：先让左右耳机输出保持静音，再使能输出放大器
    NAU8822_WriteReg(0x34, 0x040);  // R52：LHPMUTE=1，不带更新位
    NAU8822_WriteReg(0x35, 0x140);  // R53：RHPMUTE=1，带更新位同步更新

    // 使能耳机输出放大器
    // R2 = 0x180
    // Bit 8: RHPEN = 1 (右耳机使能)
    // Bit 7: LHPEN = 1 (左耳机使能)
    // Bit 6: SLEEP = 0 (正常工作模式)
    NAU8822_WriteReg(0x02, 0x180);
    Delay_ms(250);  // 等待输出稳定

    // 第11步：解除输出静音，设置耳机音量为 0dB
    // R52/R53 的 bit8 是"更新位"：写 0 只把新值存进待生效寄存器，
    // 写 1 才把本级和"另一级待生效的值"一起激活。
    // 因此必须先写左（bit8=0 暂存），再写右（bit8=1 触发），两声道才同时生效。
    // 旧写法两边都写 0x039、都不带更新位，实际一行都没落地，
    // 真正解除静音靠的是后面 SetVolume()；这里改对，避免"看似有其实没有"的死代码。
    NAU8822_WriteReg(0x34, 0x039);  // R52：LHPMUTE=0，增益 0dB，暂存
    NAU8822_WriteReg(0x35, 0x139);  // R53：RHPGAIN=0x39 + 更新位，左右同时生效

    // 第12步：静音板载扬声器输出（本板 R20/R21 未贴，此处仅为求稳）
    // R54 = 0x140 (bit6 = SPKL_MUTE, bit8 = 更新位)
    // R55 = 0x140 (bit6 = SPKR_MUTE, bit8 = 更新位)
    NAU8822_WriteReg(0x36, 0x140);
    NAU8822_WriteReg(0x37, 0x140);
}

// -----------------------------------------------------------------------------
// NAU8822 音量控制
// -----------------------------------------------------------------------------

// 设置 NAU8822 耳机输出音量（左右声道同步更新）
// 参数 volume: 0~100，0=静音，100=最大音量(+6dB)
// 音量映射：0 -> 0x00(静音), 50 -> 0x1E(-3dB 左右), 100 -> 0x3F(+6dB)
// 用户设定的耳机增益（6 位：0x00 = -57dB, 0x3F = +6dB）
// 句间软静音会在这个值和 0x00 之间分步淡变，必须记住用户的原始设定
static uint8_t s_hpGainUser = 0x38;

// 直接设置耳机增益（0~0x3F），不带静音位。
// 用于句间软静音的渐进淡出/淡入：分步改增益，不会像静音位那样硬切换产生爆音。
// 写法与 NAU8822_SetVolume 一致：先写 R52（不带更新位），再写 R53（带更新位）同步左右。
void NAU8822_SetHpGainRaw(uint8_t gain)
{
    uint16_t g = (uint16_t)(gain & 0x3Fu);

    NAU8822_WriteReg(0x34, g);
    NAU8822_WriteReg(0x35, (uint16_t)(0x100u | g));
}

// 取用户设定的耳机增益（软静音恢复时用）
uint8_t NAU8822_GetHpGainUser(void)
{
    return s_hpGainUser;
}

void NAU8822_SetVolume(uint8_t volume)
{
    uint16_t gain;

    if (volume >= 100)
    {
        gain = 0x13F;  // bit8=更新位, bit5~0=0x3F (+6dB), 不静音
    }
    else if (volume == 0)
    {
        gain = 0x140;  // bit8=更新位, bit6=1 (静音)
    }
    else
    {
        // volume 1~99 映射到 0x01~0x3E
        gain = 0x100 | ((volume * 63 / 100) & 0x3F);
    }

    // 记住用户设定的增益，句间软静音淡入时要恢复到该值
    s_hpGainUser = (uint8_t)(gain & 0x3Fu);

    // 先写入左声道，再写入右声道并带更新位，实现同步
    NAU8822_WriteReg(0x34, gain & ~0x100);  // R52，不带更新位
    NAU8822_WriteReg(0x35, gain | 0x100);   // R53，带更新位，同步左右
}

// -----------------------------------------------------------------------------
// NAU8822 录音通路（麦克风 -> PGA -> ADC -> ADCOUT）
// -----------------------------------------------------------------------------

// 上电并配置麦克风录音通路。
// 必须在 NAU8822_Init()（DAC/耳机通路已配好）之后调用。
//
// 板级说明：3.5mm 耳机口的 MIC+ 经 C23/C24 耦合到 LMICP/LMICN（pin1/2），
// MICBIAS 从 pin32 供出。录音数据从 ADCOUT（pin9）以 I2S 输出，
// 采样率跟随 BCLK/LRCK（当前 46.875 kHz，与播放同源）。
void NAU8822_EnableMic(void)
{
    // 第1步：打开 MICBIAS 缓冲。
    // R1 bit4 = MICBIASEN，给驻极体麦克风提供偏置电压。
    // 其余位保持 NAU8822_Init() 里的设置（DCBUFEN/ABIASEN/REFIMP=80k）。
    NAU8822_WriteReg(0x01, 0x11D);
    Delay_ms(20);                       // 等 MICBIAS 稳定

    // 第2步：给 ADC 通路各级上电。
    // R2 位定义：
    //   bit0 LADCEN  = 1   左 ADC 上电
    //   bit1 RADCEN  = 1   右 ADC 上电
    //   bit2 LPGAEN  = 1   左输入 PGA 上电
    //   bit3 RPGAEN  = 1   右输入 PGA 上电
    //   bit4 LBSTEN  = 1   左 ADC 混音/提升级上电
    //   bit5 RBSTEN  = 1   右 ADC 混音/提升级上电
    //   bit7 LHPEN   = 1   左耳机输出（保持播放）
    //   bit8 RHPEN   = 1   右耳机输出（保持播放）
    NAU8822_WriteReg(0x02, 0x1BF);
    Delay_ms(50);                       // 等 ADC 建立

    // 第3步：ADC 输出的高通滤波。
    // R14：bit0 HPFEN=1 打开高通（去掉 MICBIAS 带来的直流分量），
    //      bit1 HPFAM=0 普通音频模式（3.7 Hz 一阶高通），过采样率保持默认 64x。
    // 不开高通的话录音波形会整体偏置，表现为"有直流台阶"。
    NAU8822_WriteReg(0x0E, 0x001);

    // 第4步：ADC 数字音量设成 0 dB（不做额外衰减）。
    // R15/R16：bit8 是同步更新位，低 8 位是衰减值，0xFF = 0 dB。
    // 先写左（暂存），再写右并带更新位，两声道同时生效。
    NAU8822_WriteReg(0x0F, 0x0FF);      // 左：0 dB，暂存
    NAU8822_WriteReg(0x10, 0x1FF);      // 右：0 dB + 更新位

    // 第5步：输入 PGA 增益设成 +18 dB。
    // R45/R46：bit8 更新位，bit7 过零检测，bit6 静音，bit5~0 增益。
    // 增益表：0x10 = 0 dB，每 1 步 0.75 dB，所以 +18 dB = 0x10 + 24 = 0x28。
    // 耳机口的麦克风输出只有几十 mV，不加这一级录音会几乎没有波形。
    NAU8822_WriteReg(0x2D, 0x028);      // 左：+18 dB，暂存
    NAU8822_WriteReg(0x2E, 0x128);      // 右：+18 dB + 更新位
}


