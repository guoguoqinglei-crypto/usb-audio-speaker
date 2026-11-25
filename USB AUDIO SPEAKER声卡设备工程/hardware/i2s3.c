/**
  ******************************************************************************
  * @file    i2s3.c
  * @brief   I2S3 录音接收（SPI3 复用为 I2S3，从机接收 + DMA 双缓冲）
  *
  * 数据流：
  *   Codec ADCOUT -> PB5 -> I2S3_SD -> DMA2_Channel1 -> 双缓冲 -> 上层取走
  *
  * 双缓冲的做法：
  *   DMA 以循环模式填一整块内存，填到一半触发 HT 中断、填满触发 TC 中断。
  *   中断里只做一件事：把对应半区标记为"可读"。上层读到后清标记。
  *   这样中断里不做任何拷贝，USB 音频任务慢一拍也不会把数据搞坏
  *   （实在来不及读，那一半会被覆盖，用 overrun 计数记录下来）。
  ******************************************************************************
  */

#include <stm32f10x.h>
#include <i2s3.h>

/* 每半个缓冲区存多少个立体声帧。
 * 128 帧 ≈ 128 / 46875 = 2.73 ms 的数据量，
 * USB 音频任务里 USB 任务每毫秒都会跑，留出两倍余量足够。 */
#define I2S3_HALF_FRAMES   128u

/* 每帧 2 个 int16（左、右交错） */
#define I2S3_FRAME_WORDS   (I2S3_HALF_FRAMES * 2u)

static int16_t s_dmaBuf[2][I2S3_FRAME_WORDS];

static volatile uint8_t  s_ready[2];    /* 1 = 该半区已填满，可读 */
static volatile uint8_t  s_rdIdx;       /* 上层下一次读哪个半区 */
static volatile uint32_t s_overrun;     /* 还没读就被覆盖的次数 */
static volatile uint32_t s_samples;     /* 累计收到的样本数（接线自检用） */

/* ==========================================================================
 * 初始化
 * ========================================================================== */
void I2S3_Init(void)
{
    GPIO_InitTypeDef gpio;
    DMA_InitTypeDef  dma;

    /* ---------------- 第1步：关掉 JTAG，释放 PA15 / PB3 ----------------
     * 复位后 PA15=JTDI、PB3=JTDO，这两个脚不受 GPIO 控制。
     * 只保留 SWD（PA13/PA14 就是板上的下载口 IO35/IO36），
     * 烧录和调试照常可用；换来的 PA15/PB3 正好给 I2S3 用。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    GPIO_PinRemapConfig(GPIO_Remap_SWJ_JTAGDisable, ENABLE);

    /* ---------------- 第2步：配置 I2S3 三个引脚 ----------------
     * PA15 = I2S3_WS（帧时钟，从机模式下是输入）
     * PB3  = I2S3_CK（位时钟，从机模式下是输入）
     * PB5  = I2S3_SD（数据，接收方向是输入）
     * 参考手册表 23：从机模式这三个脚都配置成浮空输入。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode  = GPIO_Mode_IN_FLOATING;

    gpio.GPIO_Pin = GPIO_Pin_15;                /* PA15 - WS */
    GPIO_Init(GPIOA, &gpio);

    gpio.GPIO_Pin = GPIO_Pin_3 | GPIO_Pin_5;    /* PB3 - CK, PB5 - SD */
    GPIO_Init(GPIOB, &gpio);

    /* ---------------- 第3步：配置 SPI3 为 I2S3 从机接收 ----------------
     * I2SMOD[11]  = 1   选择 I2S 模式
     * I2SCFG[9:8] = 01  从机接收（00=从发送 01=从接收 10=主发送 11=主接收）
     * PCMSYNC[7]  = 0   短帧同步（仅 PCM 标准有效）
     * I2SSTD[5:4] = 00  Philips 标准 I2S
     * CKPOL[3]    = 0   空闲时钟低电平
     * DATLEN[2:1] = 00  16 位数据
     * CHLEN[0]    = 0   16 位声道帧
     * I2SE[10] 最后再打开。 */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI3, ENABLE);

    SPI3->I2SCFGR = (1u << 11) |
                    (1u << 8)  |
                    (0u << 4)  |
                    (0u << 3)  |
                    (0u << 1)  |
                    (0u << 0);

    /* I2SPR 在从机模式下只对主时钟输出有意义，保持默认 0 */
    SPI3->I2SPR = 0u;

    /* ---------------- 第4步：配置 DMA2 通道1（SPI3/I2S3_RX）----------------
     * 参考手册表 59：SPI3/I2S3_RX 的 DMA 请求固定在 DMA2 通道 1。
     * 循环模式 + 半字（16 位）+ 存储器自增，一次搬 I2S3_FRAME_WORDS 个半字。 */
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA2, ENABLE);

    DMA_DeInit(DMA2_Channel1);
    DMA_StructInit(&dma);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&SPI3->DR;
    dma.DMA_MemoryBaseAddr     = (uint32_t)&s_dmaBuf[0][0];
    dma.DMA_DIR                = DMA_DIR_PeripheralSRC;      /* 外设 -> 内存 */
    dma.DMA_BufferSize         = (uint32_t)(I2S3_FRAME_WORDS * 2u);  /* 两个半区一起算 */
    dma.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_HalfWord;
    dma.DMA_Mode               = DMA_Mode_Circular;
    dma.DMA_Priority           = DMA_Priority_High;
    dma.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(DMA2_Channel1, &dma);

    /* 半传输 + 传输完成 两个中断都要 */
    DMA_ITConfig(DMA2_Channel1, DMA_IT_HT | DMA_IT_TC, ENABLE);

    NVIC_SetPriority(DMA2_Channel1_IRQn, 2);
    NVIC_EnableIRQ(DMA2_Channel1_IRQn);

    DMA_Cmd(DMA2_Channel1, ENABLE);

    /* ---------------- 第5步：打开 SPI3 的 DMA 接收请求和 I2S3 ----------------
     * 顺序很重要：先让 DMA 就位，再使能 I2S，否则头几个样本会丢。 */
    SPI_I2S_DMACmd(SPI3, SPI_I2S_DMAReq_Rx, ENABLE);
    SPI3->I2SCFGR |= (1u << 10);                /* I2SE = 1 */
}

/* ==========================================================================
 * DMA 中断：只做标记，不搬数据
 * ========================================================================== */
void DMA2_Channel1_IRQHandler(void)
{
    /* 前半满：s_dmaBuf[0] 可以读了 */
    if (DMA_GetITStatus(DMA2_IT_HT1) != RESET)
    {
        if (s_ready[0] != 0u)
        {
            s_overrun++;                        /* 上一份还没读走就被覆盖 */
        }
        s_ready[0] = 1u;
        s_samples = (uint32_t)(s_samples + I2S3_HALF_FRAMES);
        DMA_ClearITPendingBit(DMA2_IT_HT1);
    }

    /* 后半满：s_dmaBuf[1] 可以读了 */
    if (DMA_GetITStatus(DMA2_IT_TC1) != RESET)
    {
        if (s_ready[1] != 0u)
        {
            s_overrun++;
        }
        s_ready[1] = 1u;
        s_samples = (uint32_t)(s_samples + I2S3_HALF_FRAMES);
        DMA_ClearITPendingBit(DMA2_IT_TC1);
    }
}

/* ==========================================================================
 * 取数据
 * ========================================================================== */
uint16_t I2S3_ReadLeft(int16_t *dst, uint16_t maxSamples)
{
    uint16_t n = 0u;
    uint16_t i;
    uint8_t  idx;

    if ((s_ready[0] == 0u) && (s_ready[1] == 0u))
    {
        return 0u;                              /* 还没攒够一份 */
    }

    idx = s_rdIdx;
    if (s_ready[idx] == 0u)
    {
        idx ^= 1u;                              /* 换另一半 */
        if (s_ready[idx] == 0u)
        {
            return 0u;
        }
    }

    /* 只取左声道：交错数据里下标 0、2、4… 是左声道 */
    for (i = 0u; (i < I2S3_HALF_FRAMES) && (n < maxSamples); i++)
    {
        dst[n++] = s_dmaBuf[idx][i * 2u];
    }

    s_ready[idx] = 0u;
    s_rdIdx = (uint8_t)(idx ^ 1u);

    return n;
}

uint32_t I2S3_OverrunCount(void)
{
    return s_overrun;
}

/* 累计收到的样本数。上电几秒后如果还是 0，就说明三根线没接通
 * （或者接错了位置），录音一定没有声音 —— 拿这个值判断最直接。 */
uint32_t I2S3_TotalSamples(void)
{
    return s_samples;
}
