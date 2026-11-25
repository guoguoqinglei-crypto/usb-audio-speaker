#include <stdint.h>
#include <string.h>

#include <tusb.h>
#include <device/dcd.h>

#include <stm32f10x.h>

#include <usb_audio.h>
#include <usb_descriptors.h>
#include <I2C.h>
#if ENABLE_USB_MIC
#include <i2s3.h>
#endif
#include <uart.h>

/* ============================================================================
 * USB 音频扬声器 —— 设备层与音频数据层
 * ----------------------------------------------------------------------------
 * 数据流：
 *
 *   电脑 --USB 等时 OUT--> TinyUSB 内部 FIFO --USB 音频任务泵--> 本文件的环形缓冲
 *                                                             |
 *                                        DMA 半传输/完成中断（i2s.c）
 *                                                             v
 *                                                   I²S2 --> NAU88C22 --> 耳机
 *
 * 为什么中间还要再加一层环形缓冲：
 *   TinyUSB 的 FIFO 只有 4 包（4ms），而 I²S 的中断是严格每 1ms 触发一次，
 *   两者的节拍来自两个完全独立的时钟源（USB 48MHz / 晶振 + I²S 分频）。
 *   中间垫一层 1024 帧（约 21ms）的缓冲，才能吸收掉两边累积的相位差。
 *   这个相位差的本源是采样率偏差：电脑严格按 48000 帧/秒发，而我们实际只播
 *   46875 帧/秒，每秒多出 1125 帧，必须有地方存或者丢。
 * ========================================================================== */

/* 环形缓冲长度（单位：帧，1 帧 = 左 + 右各 1 个 int16 = 4 字节）
 * 必须是 2 的幂，这样索引用位与运算回绕，不用做除法。
 * 1024 帧 ≈ 21.8ms，足够吸收 USB 侧的调度抖动。 */
#define RING_FRAMES         1024u
#define RING_MASK           (RING_FRAMES - 1u)

/* 起播水位：缓冲里攒够 256 帧（约 5.5ms）才开始输出，
 * 防止刚开流时缓冲是空的、播一下就断，听感上是一串“哒哒”声。 */
#define START_WATER         256u

/* 每 1ms 主机送来的字节数：48000 ÷ 1000 × 2 声道 × 2 字节 = 192 */
#define PKT_BYTES           192u

/* 一包对应的帧数：192 ÷ 4 = 48 */
#define PKT_FRAMES          (PKT_BYTES / 4u)

/* 播放 Feature Unit 仅声明主通道静音；音量由 Windows 软件音频栈完成。 */
#define AUDIO_FEATURE_UNIT_ID   2u
#define UAC_CTRL_MUTE           0x01u
static volatile uint8_t s_usb_mute;

/*--------------------------------------------------------------------+
 * 环形缓冲
 *--------------------------------------------------------------------*/
static int16_t s_ring[RING_FRAMES * 2u];

/* 写指针由USB 音频任务推进，读指针由 DMA 中断推进。
 * 16 位访问在 Cortex-M3 上是原子的，单生产/单消费场景不需要关中断。 */
static volatile uint16_t s_head;
static volatile uint16_t s_tail;

/* 主机是否已切到 alt 1（开流） */
static volatile uint8_t  s_alt;
/* 是否已经开始输出 USB 数据（一旦开始就不再退回本地音效，避免中途跳变） */
static volatile uint8_t  s_started;

static volatile uint32_t s_overflow;     /* 缓冲满、丢帧的次数 */
static volatile uint32_t s_underrun;     /* 缓冲空、补静音的次数 */
static volatile uint32_t s_rx_bytes;     /* 累计从 USB 收到的字节数，用于判断主机到底有没有在送数据 */
static volatile uint32_t s_alt_changes;  /* 主机切换 alt 的次数，用于判断是不是主机反复开关流 */

#if ENABLE_USB_MIC
/* ===== 麦克风（录音）===== */
static volatile uint8_t s_mic_alt;       /* 1 = 主机正在录音（切到了 alt 1） */
static uint32_t s_micLastMs;             /* 上次推包的时刻（毫秒） */

/* 原始样本池：存 I2S3 收到的 46875 Hz 单声道样本 */
#define MIC_RAW_POOL     192
static int16_t  s_micRaw[MIC_RAW_POOL];
static uint16_t s_micRawN;
static uint32_t s_micPhase;     /* 读指针（Q16 定点，整数部分 = 第几个输入样本） */
static uint8_t  s_micChecked;   /* 上电接线自检只做一次 */

/* 重采样相位（Q16 定点）：每输出一个 16 kHz 样本，读指针前进 46875/16000 个输入样本 */
#define MIC_OUT_RATE     16000u
#define MIC_STEP_Q16     ((uint32_t)(((uint32_t)I2S3_SAMPLE_RATE << 16) / MIC_OUT_RATE))
#endif

/*--------------------------------------------------------------------+
 * 设备层：时钟、时基、中断
 *--------------------------------------------------------------------*/

/* TinyUSB 的 OPT_OS_NONE 配置要求应用提供毫秒时基；
 * 系统任务调度仍由 FreeRTOS 负责，两者互不影响。 */
static volatile uint32_t s_millis = 0;

uint32_t tusb_time_millis_api(void)
{
    return s_millis;
}

/* TinyUSB 与 USB 音频任务共用同一个毫秒时基。 */
uint32_t hal_time_ms(void)
{
    return s_millis;
}

/* TIM4 只维护 1ms 计数；SysTick 由 FreeRTOS 用作内核节拍。 */
void TIM4_IRQHandler(void)
{
    if ((TIM4->SR & TIM_SR_UIF) != 0U)
    {
        TIM4->SR = (uint16_t)~TIM_SR_UIF;
        s_millis++;
    }
}

/* 时基初始化保持幂等，main() 与 UsbAudio_Init() 均可安全调用。 */
static uint8_t s_timebase_started = 0;

void UsbAudio_TimeBaseInit(void)
{
    if (s_timebase_started != 0U)
    {
        return;
    }

    s_timebase_started = 1U;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);

    /* TIM4 时钟 = 72MHz，72 分频后 1MHz，计 1000 个数为 1ms */
    TIM4->PSC = 72U - 1U;
    TIM4->ARR = 1000U - 1U;
    TIM4->CNT = 0U;
    TIM4->EGR = TIM_EGR_UG;
    TIM4->SR  = 0U;

    TIM4->DIER |= TIM_DIER_UIE;
    TIM4->CR1  |= TIM_CR1_CEN;

    NVIC_SetPriority(TIM4_IRQn, 3);
    NVIC_EnableIRQ(TIM4_IRQn);
}

void UsbAudio_PreInit(void)
{
    GPIO_InitTypeDef gpio;

    /* PA12(D+) 输出低电平：外部 1.5k 上拉被拉低，主机认为设备未插入。
     * Codec 初始化要几百毫秒，期间没有 tud_task() 响应枚举，先“不插”
     * 可以避免主机枚举超时后留下一个无法识别的设备。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = GPIO_Pin_12;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &gpio);
    GPIO_ResetBits(GPIOA, GPIO_Pin_12);
}

void UsbAudio_Init(void)
{
    GPIO_InitTypeDef gpio;
    tusb_rhport_init_t dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };

    /* 第 1 步：先选好 USB 时钟分频（72MHz / 1.5 = 48MHz），再打开 USB 外设时钟。
     * RM0008 明确要求：USBPRE 必须在 USBEN 置位之前配置好。 */
    RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_1Div5);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USB, ENABLE);

    /* 第 2 步：PA11 = USB_DM，PA12 = USB_DP，复用推挽输出 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin   = GPIO_Pin_11 | GPIO_Pin_12;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &gpio);

    /* 第 3 步：启动 1ms 时基，并设置 USB 中断优先级 */
    UsbAudio_TimeBaseInit();
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 2);
    NVIC_SetPriority(USB_HP_CAN1_TX_IRQn, 2);

    /* 第 4 步：启动 TinyUSB 设备协议栈 */
    tusb_init(0, &dev_init);
}

/*--------------------------------------------------------------------+
 * 音频数据层
 *--------------------------------------------------------------------*/

/* 当前缓冲里有多少帧（head 追过 tail 时用位与回绕） */
uint16_t UsbAudio_BufferedFrames(void)
{
    return (uint16_t)((s_head - s_tail) & RING_MASK);
}

uint32_t UsbAudio_OverflowCount(void)
{
    return s_overflow;
}

uint32_t UsbAudio_UnderrunCount(void)
{
    return s_underrun;
}

/* 把主机来的数据压进环形缓冲；缓冲满时丢掉最旧的一帧。
 *
 * 为什么丢"最旧"而不是丢"新来的"：
 *   电脑每秒发 48000 帧，我们每秒只播 46875 帧，多出来的 1125 帧必须处理掉。
 *   如果整批丢新帧，一次会丢掉一整个 1ms 的包，每秒丢 23 次，
 *   听感是"哒、哒、哒"的连续卡顿；
 *   丢最旧的一帧则是把 1125 帧均匀摊开，每次只跳过一个样本（20.8µs），
 *   听感上只是极轻微的粗糙感，不会出现明显的断音。
 *
 * 唯一需要注意的是：这里会改 s_tail，而 s_tail 平时只由 DMA 中断改。
 * 两处都是"读-改-写"，必须短暂关掉该中断，否则指针错位就会出现
 * "播着播着停住、过一会儿又自己接上"这种玄学现象。 */
static void UsbAudio_Push(const int16_t *src, uint32_t frames)
{
    uint32_t i;

    for (i = 0; i < frames; i++)
    {
        uint16_t next = (uint16_t)((s_head + 1u) & RING_MASK);

        if (next == s_tail)
        {
            /* 缓冲满：丢弃最旧的一帧，为当前帧腾位置。
             * 关中断只有 3 条指令的窗口，对 USB 时序没有影响。 */
            NVIC_DisableIRQ(DMA1_Channel5_IRQn);
            s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
            NVIC_EnableIRQ(DMA1_Channel5_IRQn);
            s_overflow++;
        }

        s_ring[s_head * 2u]      = src[i * 2u];        /* 左声道 */
        s_ring[s_head * 2u + 1u] = src[i * 2u + 1u];   /* 右声道 */
        s_head = next;
    }
}

/* ============================================================================
 * 48 kHz -> 46.875 kHz 的软件重采样（线性插值）
 * ----------------------------------------------------------------------------
 * 问题的来源：
 *   电脑严格按 48000 帧/秒发；I²S 受 72MHz 整数分频限制只能播 46875 帧/秒。
 *   每秒多出 1125 帧，必须处理掉。
 *
 * 不能直接周期性丢帧：每次丢帧都会造成波形阶跃，形成可听见的杂音。
 * 这里让读指针平均每次前进 1.024 个输入帧，落在两个采样点之间时
 * 通过线性插值计算输出值，在保持波形连续的同时匹配输入、输出速率。
 *
 * 为什么音调不变（这一点容易想错）：
 *   看的是"每秒吞掉多少输入帧"，不是"每帧走多远"：
 *     输出 46875 帧/秒 × 每帧 1.024 输入帧 = 48000 输入帧/秒
 *   恰好等于电脑送来的速率，所以既没有积压也没有欠载，
 *   播放速度和音调都与原始文件一致。
 *   反过来，如果每帧只走 1.0 帧，就只吞 46875 帧/秒，
 *   缓冲会一直涨到溢出——那才是老师课上说的"错误示范"。
 *
 * 代价（必须知道）：
 *   严格来说降采样前应加低通滤波（抗混叠），这里省掉了，
 *   高频段会有一点混叠失真 —— 但比每秒 1125 次爆音好得多。
 *   另外本文件开头"128 帧转 125 帧"就是这个 1.024 的另一种写法：
 *   48000 × 125 / 128 = 46875，两者完全等价。
 *
 * 数值：
 *   48000 / 46875 = 1.024
 *   用 16.16 定点表示：1.024 × 65536 = 67108.86 -> 取 67109
 *   每输出一帧，小数部分累加 67109 - 65536 = 1573；
 *   累加到 65536 就多吞一帧，平均每 42.7 帧吞一次，正好是 2.34%。
 * ========================================================================== */

#define RESAMPLE_STEP     67109u    /* 1.024 的 16.16 定点表示 */
#define RESAMPLE_UNIT     65536u    /* 16.16 的 1.0 */

/* 分数指针的小数部分，必须跨调用保持，否则每个半区都会重新对齐，
 * 累积误差会变成周期性的"噗噗"声。 */
static uint32_t s_frac;

/* 从环形缓冲取 frames 个立体声帧，内部完成 48k -> 46.875k 的重采样。
 * 返回值是实际输出的帧数，小于 frames 说明欠载，调用方补静音。 */
uint32_t UsbAudio_ReadResampled(int16_t *dst, uint32_t frames)
{
    uint32_t n = 0;
    uint32_t avail;

    while (n < frames)
    {
        avail = (uint16_t)((s_head - s_tail) & RING_MASK);

        if (avail == 0u)
        {
            break;                      /* 缓冲空，交给调用方补 0 */
        }

        if (avail >= 2u)
        {
            /* 取当前帧和下一帧做插值 */
            uint16_t i0 = s_tail;
            uint16_t i1 = (uint16_t)((s_tail + 1u) & RING_MASK);
            int32_t  l0 = (int32_t)s_ring[i0 * 2u];
            int32_t  l1 = (int32_t)s_ring[i1 * 2u];
            int32_t  r0 = (int32_t)s_ring[i0 * 2u + 1u];
            int32_t  r1 = (int32_t)s_ring[i1 * 2u + 1u];

            /* 用 64 位乘法：差值最大 65535，乘 65535 会超出 int32 范围 */
            dst[n * 2u]      = (int16_t)(l0 + (int32_t)(((int64_t)(l1 - l0) * (int64_t)s_frac) >> 16));
            dst[n * 2u + 1u] = (int16_t)(r0 + (int32_t)(((int64_t)(r1 - r0) * (int64_t)s_frac) >> 16));
        }
        else
        {
            /* 只剩最后一帧，没有"下一帧"可插值，直接输出 */
            dst[n * 2u]      = (int16_t)s_ring[s_tail * 2u];
            dst[n * 2u + 1u] = (int16_t)s_ring[s_tail * 2u + 1u];
        }

        n++;

        /* 读指针前进 1 帧，小数部分前进 0.024 帧 */
        s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
        s_frac += (RESAMPLE_STEP - RESAMPLE_UNIT);

        /* 小数部分攒满 1 帧：多吞一帧，实现"跳过一个采样点" */
        if (s_frac >= RESAMPLE_UNIT)
        {
            s_frac -= RESAMPLE_UNIT;

            if (s_tail != s_head)
            {
                s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
            }
        }
    }

    if (n < frames)
    {
        s_underrun++;
    }

    /* 音量大小由 Windows 在 PCM 送入 USB 前完成；这里只处理主通道静音。 */
    if ((s_usb_mute != 0u) && (n != 0u))
    {
        memset(dst, 0, (size_t)n * 2u * sizeof(int16_t));
    }

    return n;
}

/* 不做重采样、直接按原速取帧（保留给调试对比用，正常路径不用） */
uint32_t UsbAudio_ReadFrames(int16_t *dst, uint32_t frames)
{
    uint32_t n = 0;

    while ((n < frames) && (s_tail != s_head))
    {
        dst[n * 2u]      = s_ring[s_tail * 2u];
        dst[n * 2u + 1u] = s_ring[s_tail * 2u + 1u];
        s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
        n++;
    }

    if (n < frames)
    {
        s_underrun++;
    }

    return n;
}

uint8_t UsbAudio_IsStreaming(void)
{
    /* 主机没开流：走本地固定音效 */
    if (s_alt == 0u)
    {
        return 0u;
    }

    /* 已经起播：保持走 USB 通路，欠载时由调用方补静音。
     * 这里不因为缓冲一时空就退回本地音效，否则会在两者之间反复跳。 */
    if (s_started != 0u)
    {
        return 1u;
    }

    /* 首次起播要等水位，避免开流瞬间断续 */
    if (UsbAudio_BufferedFrames() >= START_WATER)
    {
        s_started = 1u;
        return 1u;
    }

    return 0u;
}

/* 主机没开流时，要不要继续播本地固定音效？
 *
 * 置 1（推荐）：只要已经枚举成 USB 声卡，就保持静音。
 *   电脑暂停、切歌、关播放器时，耳机里安安静静，符合"这是一块声卡"的预期。
 * 置 0：没开流就回到本地音效。
 *   这个行为只在"单独演示 30 分那项（不接电脑、只听音效）"时才有用，
 *   接上电脑会让两个音源互相抢，听起来像"音乐停了又自己接着放"。
 */
#define AUDIO_LOCAL_EFFECT_ONLY_WITHOUT_USB   1

uint8_t UsbAudio_AllowLocalEffect(void)
{
#if AUDIO_LOCAL_EFFECT_ONLY_WITHOUT_USB
    /* 已经枚举成声卡：不再播本地音效，让主机来决定放什么 */
    return (tud_audio_mounted() == false) ? 1u : 0u;
#else
    return 1u;
#endif
}

/* 复位缓冲与状态（主机切 alt 时调用） */
static void UsbAudio_ResetBuffer(void)
{
    s_head = 0u;
    s_tail = 0u;
    s_started = 0u;
    s_frac = 0u;        /* 分数指针一起归零，避免下次开流从半帧处开始 */
}

/* 把主机 FIFO 里的数据搬到环形缓冲。
 * 每次只搬完整的帧（4 字节），不足一帧的零头留在 FIFO 里等下次，
 * 否则左右声道会错位，听起来是持续的“沙沙”声。 */
static void UsbAudio_Pump(void)
{
    int16_t  tmp[PKT_FRAMES * 2];      /* 192 字节 */
    uint16_t avail;

    if ((tud_audio_mounted() == false) || (s_alt == 0u))
    {
        return;
    }

    avail = tud_audio_available();

    while (avail >= 4u)
    {
        uint16_t chunk = (avail > PKT_BYTES) ? (uint16_t)PKT_BYTES : avail;
        uint16_t got;

        chunk = (uint16_t)((chunk / 4u) * 4u);
        got   = tud_audio_read(tmp, chunk);

        if (got < 4u)
        {
            break;
        }

        s_rx_bytes += got;
        UsbAudio_Push(tmp, (uint32_t)(got / 4u));
        avail = (uint16_t)(avail - got);
    }
}

#if ENABLE_USB_MIC
/* ==========================================================================
 * 麦克风取样：把 I2S3 收到的 46875 Hz 样本重采样成 16 kHz 送进 USB
 *
 * 【为什么要重采样】
 *   Codec 的 ADC 和 DAC 共用同一对 BCLK/LRCK，所以录音采样率被播放锁死在
 *   46875 Hz（I2S2 的分频决定的）。USB 麦克风声明的是标准的 16 kHz，
 *   两个速率不是整数倍关系，只能按相位比例抽点。
 *
 *   46875 / 16000 = 2.9296875，用 Q16 定点存成 MIC_STEP_Q16，
 *   每输出一个样本把读指针前进这么多，整数部分 = 取第几个输入样本。
 *   这是最近邻抽取，对语音足够；换成线性插值只会多几行代码，听感差异极小。
 * ========================================================================== */
static void UsbAudio_MicTask(void)
{
    int16_t  pkt[16];               /* 16 kHz × 1ms = 16 个样本 = 32 字节 */
    uint32_t now;
    uint8_t  guard = 0u;

    if ((s_mic_alt == 0u) || (tud_audio_mounted() == false))
    {
        s_micRawN = 0u;
        return;
    }

    now = s_millis;

    /* 上电 3 秒后自检一次：I2S3 到底有没有收到数据。
     * 没接线时这个值会一直是 0 —— 直接把结论打出来，省得去猜。 */
    if ((s_micChecked == 0u) && (now > 3000u))
    {
        s_micChecked = 1u;
        if (I2S3_TotalSamples() == 0u)
        {
            UART1_SendString("[mic] I2S3 no data - check ADCOUT/BCLK/LRCLK wires\r\n");
        }
        else
        {
            UART1_Printf("[mic] I2S3 receiving, samples=%lu\r\n",
                         (unsigned long)I2S3_TotalSamples());
        }
    }

    /* USB 音频任务被别的任务拖慢过久（比如刚上电），直接对齐到当前时刻，
     * 不要试图补上一大堆包 —— 那样会瞬间灌满 FIFO 反而更难听 */
    if ((uint32_t)(now - s_micLastMs) > 10u)
    {
        s_micLastMs = now;
    }

    if ((uint32_t)(now - s_micLastMs) < 1u)
    {
        return;                     /* 还不到 1ms，这一轮不推 */
    }

    /* 补齐原始样本池：I2S3 有数据就搬过来 */
    if (s_micRawN < MIC_RAW_POOL)
    {
        s_micRawN = (uint16_t)(s_micRawN +
                    I2S3_ReadLeft(&s_micRaw[s_micRawN],
                                  (uint16_t)(MIC_RAW_POOL - s_micRawN)));
    }

    /* 每毫秒推一包；最多补 3 包，避免卡顿后一次性追太多 */
    while (((uint32_t)(now - s_micLastMs) >= 1u) && (guard < 3u))
    {
        uint16_t n;
        uint16_t used;

        s_micLastMs++;
        guard++;

        for (n = 0u; n < 16u; n++)
        {
            uint32_t idx = s_micPhase >> 16;

            if ((uint32_t)(idx + 1u) >= (uint32_t)s_micRawN)
            {
                /* 原料不够（多半是三根线还没接）：保持上一个样本，
                 * 保证每包固定 32 字节，主机看到的是连续的静音而不是撕裂 */
                pkt[n] = (n > 0u) ? pkt[n - 1u] : (int16_t)0;
                continue;
            }

            pkt[n] = s_micRaw[idx];
            s_micPhase += MIC_STEP_Q16;
        }

        /* 丢掉已经用过的输入样本 */
        used = (uint16_t)(s_micPhase >> 16);
        if (used > 0u)
        {
            s_micRawN = (uint16_t)(s_micRawN - used);
            if (s_micRawN > 0u)
            {
                memmove(s_micRaw, &s_micRaw[used], (size_t)s_micRawN * sizeof(int16_t));
            }
            s_micPhase &= 0xFFFFu;
        }

        (void)tud_audio_n_write(0u, pkt, (uint16_t)sizeof(pkt));
    }
}
#endif /* ENABLE_USB_MIC */

void UsbAudio_Task(void)
{
    /* 第 1 步：处理 USB 协议栈事件（枚举、类请求、等时传输） */
    tud_task();

    /* 第 2 步：把收到的音频搬进环形缓冲 */
    UsbAudio_Pump();

    /* 音量与静音由 Windows 软件音频栈处理，不执行固件实体控制。 */
#if ENABLE_USB_MIC
    /* 第 4 步：录音方向 —— 把 I2S3 采到的麦克风数据按 16 kHz 推给主机 */
    UsbAudio_MicTask();
#endif
}

/*--------------------------------------------------------------------+
 * USB 音频类回调
 *--------------------------------------------------------------------*/

/* 主机切换 alt setting：alt 1 = 开始播放，alt 0 = 停止 */
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t itf = (uint8_t)(p_request->wIndex & 0xFFu);
    uint8_t alt = (uint8_t)(p_request->wValue & 0xFFu);

    (void)rhport;

    if (itf == ITF_NUM_AUDIO_STREAMING)
    {
        /* 新的播放会话从 alt 0 进入 alt 1 时，清除上次会话可能遗留的
         * 固件静音。若主机当前确实要求静音，会在开流后再次发送 Mute=1。 */
        if ((alt != 0u) && (s_alt == 0u))
        {
            s_usb_mute = 0u;
        }
        UsbAudio_ResetBuffer();
        s_alt = (alt != 0u) ? 1u : 0u;
        s_alt_changes++;
    }
#if ENABLE_USB_MIC
    else if (itf == ITF_NUM_AUDIO_MIC)
    {
        /* 主机开始录音时切到 alt 1，停止时回 alt 0 */
        s_mic_alt = (alt != 0u) ? 1u : 0u;
        s_micRawN  = 0u;
    }
#endif

    return true;
}

/* 关闭端点时必须区分播放和录音接口。
 * TinyUSB 对 IN/OUT 两个方向都会调用本回调；旧代码在麦克风关流时
 * 也清除 s_alt，误把仍在进行的扬声器播放复位。 */
bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t itf = (uint8_t)(p_request->wIndex & 0xFFu);

    (void)rhport;

    if (itf == ITF_NUM_AUDIO_STREAMING)
    {
        s_alt = 0u;
        UsbAudio_ResetBuffer();
    }
#if ENABLE_USB_MIC
    else if (itf == ITF_NUM_AUDIO_MIC)
    {
        s_mic_alt = 0u;
        s_micRawN = 0u;
    }
#endif

    return true;
}

/* 播放 Feature Unit 仅支持主通道 Mute；不打印日志、不访问软件 I²C。
 * Volume 未在描述符中声明，Windows 会在发送 USB 数据前完成音量缩放。 */
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff)
{
    (void)rhport;

    if (((uint8_t)(p_request->wIndex >> 8) != AUDIO_FEATURE_UNIT_ID) ||
        (p_request->bRequest != (uint8_t)AUDIO10_CS_REQ_SET_CUR) ||
        ((uint8_t)(p_request->wValue >> 8) != UAC_CTRL_MUTE) ||
        ((p_request->wValue & 0xFFu) != 0u) ||
        (p_request->wLength < 1u))
    {
        return false;
    }

    s_usb_mute = (pBuff[0] != 0u) ? 1u : 0u;
    return true;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t mute;

    if (((uint8_t)(p_request->wIndex >> 8) != AUDIO_FEATURE_UNIT_ID) ||
        (p_request->bRequest != (uint8_t)AUDIO10_CS_REQ_GET_CUR) ||
        ((uint8_t)(p_request->wValue >> 8) != UAC_CTRL_MUTE) ||
        ((p_request->wValue & 0xFFu) != 0u))
    {
        return false;
    }

    mute = s_usb_mute;
    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute, sizeof(mute));
}

/*--------------------------------------------------------------------+
 * USB 中断入口
 *--------------------------------------------------------------------*/

void USB_LP_CAN1_RX0_IRQHandler(void)
{
    dcd_int_handler(0);
}

void USB_HP_CAN1_TX_IRQHandler(void)
{
    dcd_int_handler(0);
}
