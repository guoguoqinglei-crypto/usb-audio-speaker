// 包含 I2S 头文件
#include <i2s.h>
// 包含 USB 音频数据接口（USB 开流时音频源换成电脑发来的数据）
#include <usb_audio.h>
#include <I2C.h>

// I2S2 初始化
// 根据 STM32F103 参考手册 23.4.3 节时钟公式配置：
// 带 MCK 输出时：Fs = I2SxCLK / [(16*2) * ((2*I2SDIV)+ODD) * 8]
// SYSCLK=72MHz, I2SDIV=3, ODD=0 时：
// Fs = 72MHz / (32 * 6 * 8) = 72MHz / 1536 = 46875Hz
// MCK = 256 * Fs = 12MHz
// 说明：72MHz 时钟 + 整数分频无法精确得到 48kHz（需 73.728MHz，而 8MHz 晶振
// 无法整数倍频得到），46.875kHz 为最接近取值，误差约 2.34%
void I2S2_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    // 第1步：使能 GPIOB、GPIOC 和 SPI2 的时钟
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);

    // 第2步：配置 I2S2 引脚为复用推挽输出
    // PB12 - I2S2_WS, PB13 - I2S2_CK, PB15 - I2S2_SD
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    // PC6 - I2S2_MCK
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    // 第3步：复位 SPI2/I2S2 到默认状态
    SPI_I2S_DeInit(SPI2);

    // 第4步：配置 SPI_I2SCFGR 寄存器
    // 参考手册 23.5.8 节寄存器位定义：
    // I2SMOD[11]=1   : 选择 I2S 模式
    // I2SE[10]       : 初始为 0，配置完成后再使能
    // I2SCFG[9:8]=10 : 主设备发送模式
    // PCMSYNC[7]=0   : 短帧同步（仅在 PCM 标准有效）
    // I2SSTD[5:4]=00 : Philips 标准
    // CKPOL[3]=0     : 空闲时时钟为低电平
    // DATLEN[2:1]=00 : 16 位数据长度
    // CHLEN[0]=0     : 16 位声道长度
    SPI2->I2SCFGR = (1 << 11) |    // I2SMOD = 1
                    (2 << 8)  |    // I2SCFG = 10 (主发送)
                    (0 << 4)  |    // I2SSTD = 00 (Philips)
                    (0 << 3)  |    // CKPOL = 0
                    (0 << 1)  |    // DATLEN = 00 (16 位数据)
                    (0 << 0);      // CHLEN = 0 (16 位声道)

    // 第5步：配置 SPI_I2SPR 寄存器
    // MCKOE[9]=1     : 使能主时钟输出
    // ODD[8]=0       : 实际分频系数 = I2SDIV * 2
    // I2SDIV[7:0]=3  : 线性分频值
    SPI2->I2SPR = (1 << 9) |       // MCKOE = 1
                  (0 << 8) |       // ODD = 0
                  (3 << 0);        // I2SDIV = 3

    // 第6步：使能 I2S2
    SPI2->I2SCFGR |= (1 << 10);    // I2SE = 1
}

// 使用 DMA1 通道 5 启动 I2S2 循环播放
// DMA 请求映射参考手册表 59：SPI/I2S2_TX -> DMA1_Channel5
void I2S2_Play(uint16_t *buffer, uint32_t len)
{
    DMA_InitTypeDef DMA_InitStructure;

    // 使能 DMA1 时钟
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    // 确保 DMA1 通道 5 处于禁止状态，再重新初始化
    DMA_Cmd(DMA1_Channel5, DISABLE);

    // 配置 DMA1_Channel5
    // 方向：内存 -> 外设 (SPI2->DR)
    // 外设地址不递增，内存地址递增
    // 数据宽度：半字（16 位）
    // 模式：循环模式，适合连续播放音频
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DR;
    DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)buffer;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
    DMA_InitStructure.DMA_BufferSize = len;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
    DMA_InitStructure.DMA_Priority = DMA_Priority_High;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel5, &DMA_InitStructure);

    // 使能 SPI2 发送 DMA 请求
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, ENABLE);

    // 启动 DMA 通道 5
    DMA_Cmd(DMA1_Channel5, ENABLE);
}

// 停止 I2S2 DMA 播放
void I2S2_Stop(void)
{
    // 关闭 DMA1 通道 5
    DMA_Cmd(DMA1_Channel5, DISABLE);

    // 关闭 SPI2 发送 DMA 请求
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, DISABLE);
}

/* ============================================================================
 * 双缓冲流式播放
 * ----------------------------------------------------------------------------
 * 缓冲区结构（10 ms，共 960 个半字）：
 *   ┌────────────────────────┬────────────────────────┐
 *   │ 前半区 480 半字（5 ms）│ 后半区 480 半字（5 ms）│
 *   └────────────────────────┴────────────────────────┘
 *            ▲ HT 中断后填这里        ▲ TC 中断后填这里
 *
 * 时序：DMA 发前半区时，后半区正在被填充；发后半区时，前半区被填充。
 *       任何时候都只写 DMA 当前不读的那一半，避免读写同一块内存。
 *
 * 数据排布：源数据为单声道，每个样本要同时写进左、右两个半字，
 *           即 [L0][R0][L1][R1]... 交错排列，这样双耳听到同一个声音。
 * ========================================================================== */

// 双缓冲区：必须是全局变量，DMA 和中断都在用
static uint16_t audioBuf[AUDIO_BUF_HALFWORDS];

/* ============================================================================
 * 句间软静音（消除"每句话开口时哒一下"）
 * ----------------------------------------------------------------------------
 * Codec 的 AUTOMT 自动静音是【硬切换】：连续 1024 个零样本后直接把 DAC 模拟
 * 输出切断，下一句出现非零样本时又瞬间接通 —— 模拟通路的开关瞬间会"哒"一声。
 * 语音合成每句话之间有 280~870ms 的停顿，所以每句都能听到这一声。
 *
 * 这里改成固件自己【分步改变耳机增益】来做渐变：
 *   · 数据连续无声超过约 50ms  -> 把耳机增益分步降到 -57dB
 *     （数字音量拉低只能静掉数字信号，DAC 和耳机放大器的模拟底噪还在，
 *       听起来就是"电流声"，所以必须连模拟增益一起降下来）
 *   · 一旦重新出现有效样本      -> 分步恢复到用户设定的增益
 * 因为增益是逐步变化的，且切换发生在信号幅度接近 0 的时刻，所以听不到切换声。
 * ========================================================================== */
/* |样本| 低于此值算无声（约 -72dBFS）。
 * 不能取太大：语音合成每句话开头的幅度只有 -80dBFS 量级（实测 0.0001），
 * 门限过高会导致开口后要等几十毫秒才解除静音，第一个字被"吞"掉。 */
#define I2S_SILENT_THRESHOLD     8u
#define I2S_SILENCE_MUTE_FRAMES  2344u    /* 46.875kHz 下约 50ms */
#define I2S_HPGAIN_STEP          6u       /* 每步约 6dB（0x00=-57dB … 0x3F=+6dB） */
#define I2S_HPGAIN_UNINIT        0xFFu    /* 未初始化标记 */

/* 连续无声的样本数（由 DMA 中断里的 Audio_FillHalf 更新） */
static volatile uint32_t s_silentFrames = 0u;
/* 当前耳机增益（0x00~0x3F），0xFF 表示还没跟用户设定同步 */
static uint8_t s_hpGain = I2S_HPGAIN_UNINIT;

// 当前正在播放的音效数据（单声道）
static const int16_t *effectSrc = 0;
// 音效样本总数
static volatile uint32_t effectLen = 0;
// 下一次要取的样本下标
static volatile uint32_t effectPos = 0;

// 往指定半区填充 count 个半字（count 必须为偶数）
// 数据来源有三种，按 USB 状态自动选择：
//   1) USB 已开流：从 USB 环形缓冲取数据（本身就是左右交错的 16 位 PCM）
//   2) 已枚举成声卡但主机没开流：输出静音，等主机送数据
//   3) 根本没接电脑：播本地音效，每个样本同时写入左右两个声道，播到末尾回绕
static void Audio_FillHalf(uint16_t *dst, uint32_t count)
{
    uint32_t i;
    uint32_t frames;
    uint32_t got;
    int16_t sample;

    // 第 1 种情况：主机正在播音乐
    // 用带重采样的读取函数：电脑发 48000 帧/秒，我们只能播 46875 帧/秒，
    // 直接用原速取帧等于每秒硬丢 1125 个样本，会听到连续的"哒哒"声；
    // 插值重采样把这些差额摊成"读指针每次走 1.024 帧"，波形保持连续。
    // 读到的帧数不足时（欠载）后半段补 0，也就是静音，
    // 千万不能拿旧数据顶上，否则会听到一段重复的“嗡嗡”声。
    if (UsbAudio_IsStreaming() != 0u)
    {
        frames = count / 2u;                       // 1 帧 = 左右各 1 个半字
        got = UsbAudio_ReadResampled((int16_t *)dst, frames);

        for (i = got * 2u; i < count; i++)
        {
            dst[i] = 0;
        }

        // 统计这一块数据是不是无声，供 I2S2_SilenceMuteService() 判断
        {
            const int16_t *pcm = (const int16_t *)dst;
            uint32_t loud = 0u;

            for (i = 0u; i < got * 2u; i++)
            {
                if ((pcm[i] > I2S_SILENT_THRESHOLD) || (pcm[i] < -I2S_SILENT_THRESHOLD))
                {
                    loud = 1u;
                    break;
                }
            }

            if (loud != 0u)
            {
                s_silentFrames = 0u;
            }
            else if (s_silentFrames < I2S_SILENCE_MUTE_FRAMES)
            {
                /* 注意这里累加的是整块帧数 frames，不是 got：
                 * 主机暂停音乐时不会关闭音频流，只是不再送数据，
                 * 此时 UsbAudio_ReadResampled 返回 0 帧（欠载），
                 * 如果按 got 累加，计数器永远停在 0，软静音就不会触发，
                 * 表现为"暂停音乐后底噪反而更明显"。 */
                s_silentFrames += frames;
            }
        }
        return;
    }

    // 第 1.5 种情况：已经枚举成 USB 声卡，但主机此刻没开流
    // 必须输出静音，不能回头播本地音效。
    // 否则电脑暂停一下、切首歌、关掉播放器，耳机里就会突然响起音效，
    // 听起来像是"音乐停了，过一会儿又自己接着放"，其实是两个音源在打架。
    if (UsbAudio_AllowLocalEffect() == 0u)
    {
        for (i = 0; i < count; i++)
        {
            dst[i] = 0;
        }
        s_silentFrames = I2S_SILENCE_MUTE_FRAMES;   // 没开流：按无声处理，压低 DAC 音量
        return;
    }

    // 没有指定音效（effectLen == 0）：只让 DMA 跑，输出静音。
    // 开机自检音可能会在每次上电"哔"一声，不想要就走这条路。
    if ((effectSrc == 0) || (effectLen == 0u))
    {
        for (i = 0; i < count; i++)
        {
            dst[i] = 0;
        }
        s_silentFrames = I2S_SILENCE_MUTE_FRAMES;   // 没有音效：同样压低音量
        return;
    }

    // 第 2 种情况：没接电脑（或用充电器供电），播本地固定音效自检
    s_silentFrames = 0u;                            // 本地音效在播，不静音
    for (i = 0; i < count; i += 2u)
    {
        // 取一个单声道样本
        sample = effectSrc[effectPos];

        // 下标递增，到头后回绕
        effectPos++;
        if (effectPos >= effectLen)
        {
            effectPos = 0;
        }

        // 同一采样写进左、右两个半字
        dst[i]      = (uint16_t)sample;
        dst[i + 1u] = (uint16_t)sample;
    }
}

// 句间软静音服务：由低频任务周期调用（约 2ms 一次）
// 每次最多走一步（0x80 = 64dB），所以最多两步就能静音或恢复满幅。
// 放在低优先级任务里执行的原因：软件 I2C 写两个音量寄存器约需 540us，
// 放到 USB/音频任务里会挤占等时传输的时间，可能造成爆音。
void I2S2_SilenceMuteService(void)
{
    uint8_t target;

    // 第一次运行：取用户设定的增益作为当前值，避免上电就被拉低
    if (s_hpGain == I2S_HPGAIN_UNINIT)
    {
        s_hpGain = NAU8822_GetHpGainUser();
    }

    target = (s_silentFrames >= I2S_SILENCE_MUTE_FRAMES)
             ? 0u                          // 长时间无声 -> 耳机增益降到 -57dB
             : NAU8822_GetHpGainUser();    // 有声音 -> 恢复到用户设定

    if (s_hpGain == target)
    {
        return;
    }

    if (s_hpGain < target)
    {
        s_hpGain = (uint8_t)(((target - s_hpGain) > I2S_HPGAIN_STEP)
                             ? (s_hpGain + I2S_HPGAIN_STEP) : target);
    }
    else
    {
        s_hpGain = (uint8_t)(((s_hpGain - target) > I2S_HPGAIN_STEP)
                             ? (s_hpGain - I2S_HPGAIN_STEP) : target);
    }

    NAU8822_SetHpGainRaw(s_hpGain);
}

// 启动双缓冲流式播放
void I2S2_PlayEffect(const int16_t *src, uint32_t samples)
{
    DMA_InitTypeDef DMA_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    // 参数检查：src 与 samples 必须"同时给"或"同时为空"。
    // 同时为空是合法用法 —— 表示只启动 DMA/播放链路、输出静音
    // （I2S2_StartSilence 就这么用）。只给一个才算参数错误。
    if ((src == 0) != (samples == 0u))
    {
        return;
    }

    // 第1步：记录音效信息
    effectSrc = src;
    effectLen = samples;
    effectPos = 0;

    // 第2步：先把两个半区都填满，否则 DMA 启动瞬间会输出无效数据（爆音）
    Audio_FillHalf(&audioBuf[0], AUDIO_HALF_HALFWORDS);
    Audio_FillHalf(&audioBuf[AUDIO_HALF_HALFWORDS], AUDIO_HALF_HALFWORDS);

    // 第3步：使能 DMA1 时钟，先关闭通道再重新配置
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    DMA_Cmd(DMA1_Channel5, DISABLE);
    DMA_DeInit(DMA1_Channel5);

    // 第4步：配置 DMA1_Channel5
    // 缓冲区总长 960 个半字；DMA 走完 480 个时触发 HT，走完 960 个时触发 TC
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DR;
    DMA_InitStructure.DMA_MemoryBaseAddr     = (uint32_t)&audioBuf[0];
    DMA_InitStructure.DMA_DIR                = DMA_DIR_PeripheralDST;
    DMA_InitStructure.DMA_BufferSize         = AUDIO_BUF_HALFWORDS;
    DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_HalfWord;
    DMA_InitStructure.DMA_Mode               = DMA_Mode_Circular;
    DMA_InitStructure.DMA_Priority           = DMA_Priority_High;
    DMA_InitStructure.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel5, &DMA_InitStructure);

    // 第5步：清掉可能残留的中断标志，再使能半传输/传输完成中断
    DMA_ClearITPendingBit(DMA1_IT_GL5);
    DMA_ITConfig(DMA1_Channel5, DMA_IT_HT | DMA_IT_TC, ENABLE);

    // 第6步：配置 NVIC，使能 DMA1 通道 5 中断
    // 抢占优先级设为 1（低于 USB 中断），保证音频回填不会打断 USB 时序
    NVIC_InitStructure.NVIC_IRQChannel                   = DMA1_Channel5_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    // 第7步：使能 SPI2 发送 DMA 请求并启动通道
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, ENABLE);
    DMA_Cmd(DMA1_Channel5, ENABLE);
}


// 只启动播放链路，不播本地音效（输出静音）
void I2S2_StartSilence(void)
{
    I2S2_PlayEffect(0, 0u);
}

// DMA1 通道 5 中断服务函数
// 半传输：前半区已发完，填前半区
// 传输完成：后半区已发完，填后半区
void DMA1_Channel5_IRQHandler(void)
{
    if (DMA_GetITStatus(DMA1_IT_HT5) != RESET)
    {
        // 清标志必须放在填充之前，避免填充耗时期间重复进中断
        DMA_ClearITPendingBit(DMA1_IT_HT5);
        Audio_FillHalf(&audioBuf[0], AUDIO_HALF_HALFWORDS);
    }

    if (DMA_GetITStatus(DMA1_IT_TC5) != RESET)
    {
        DMA_ClearITPendingBit(DMA1_IT_TC5);
        Audio_FillHalf(&audioBuf[AUDIO_HALF_HALFWORDS], AUDIO_HALF_HALFWORDS);
    }
}
