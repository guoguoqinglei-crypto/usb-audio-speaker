#include <stdint.h>

/* ---- FreeRTOS ---- */
#include <FreeRTOS.h>
#include <task.h>

/* ---- 板级与音频 ---- */
#include <usb_audio.h>      /* USB 音频声卡（UAC 1.0）+ 1ms 时基 */
#include <uart.h>           /* 调试串口 USART1 */
#include <delay.h>          /* 微秒/毫秒延时（已改为 TIM7，不再占用 SysTick） */
#include <I2C.h>            /* 软件 I²C 与 NAU8822 驱动 */
#include <i2s.h>            /* I²S2 初始化与 DMA 播放 */
#include <tusb_config.h>    /* ENABLE_USB_MIC：要不要初始化录音通路 */

#if ENABLE_USB_MIC
#include <i2s3.h>           /* I2S3 从机接收（录音数据通路） */
#endif

/* ============================================================================
 * 一、参数
 * ========================================================================== */

/* Codec 固定增益（0~100 映射到寄存器 0x00~0x3F）。
 * 音量调节现在由 Windows 在软件层做，这里只决定"最大能多响"：
 *   80 -> 寄存器 0x32 ≈ -7 dB（偏小）
 *   90 -> 寄存器 0x38 ≈ -1 dB（接近满输出，留一点余量防削顶） */
#define AUDIO_VOLUME        90

#define UART_BAUD_RATE      921600

/* Codec 上电后等电源稳定再读器件 ID */
#define CODEC_POWER_SETTLE_MS  100
/* 读不到器件 ID 时的重试间隔 */
#define I2C_RETRY_DELAY_MS     100

/* ============================================================================
 * 二、FreeRTOS 任务定义
 * ----------------------------------------------------------------------------
 * USB_TASK  优先级 3：每 1ms 推进 TinyUSB 协议栈并搬运音频数据。
 * MUTE_TASK 优先级 1：每 2ms 执行句间软静音，降低切换爆音与底噪。
 * DIAG_TASK 优先级 1：每 5 秒输出缓冲区、欠载、溢出与堆余量统计。
 *
 * 播放数据由 I2S2 DMA 半传输/完成中断搬运，录音数据由 I2S3 DMA 接收；
 * DMA 与任务调度并行工作，USB 任务只负责协议栈和缓冲区交互。
 * ========================================================================== */

#define USB_TASK_PRIO       3
#define USB_TASK_STACK      512u        /* 512 字 = 2KB */

#define DIAG_TASK_PRIO      1
#define DIAG_TASK_STACK     160u        /* 160 字 = 640B */
#define DIAG_PERIOD_MS      5000u

/* 句间软静音任务：优先级低于 USB 任务，2ms 一次。
 * 内部会调软件 I2C 改 Codec 音量（约 540us），放低优先级是为了
 * 让 USB 任务随时能抢占它，不影响等时传输。 */
#define MUTE_TASK_PRIO      1
#define MUTE_TASK_STACK     256u        /* 256 字 = 1KB */
#define MUTE_PERIOD_MS      2u

/* USB / 音频主任务：不能长时间阻塞，否则主机等时包会丢 */
static void vUsbTask(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        UsbAudio_Task();                /* tud_task + 搬数据 + 应用音量 */

        /* 1 个 tick = 1ms：TinyUSB 的等时传输按 1ms 一包，
         * 这个节拍和 USB 帧对齐，抖动足够小 */
        vTaskDelay(1);
    }
}

/* 诊断任务：周期打印音频链路健康状况 */
static void vDiagTask(void *pvParameters)
{
    TickType_t lastWake = xTaskGetTickCount();
    uint32_t   underrun;
    uint32_t   overflow;
    uint16_t   buffered;

    (void)pvParameters;

    for (;;)
    {
        underrun = UsbAudio_UnderrunCount();
        overflow = UsbAudio_OverflowCount();
        buffered = UsbAudio_BufferedFrames();

        UART1_Printf("[rtos] buf=%u under=0x%lX over=0x%lX heap=%u\r\n",
                     (unsigned)buffered,
                     (unsigned long)underrun,
                     (unsigned long)overflow,
                     (unsigned)xPortGetFreeHeapSize());

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(DIAG_PERIOD_MS));
    }
}

/* 句间软静音任务：消除"每句话开口时哒一下"。
 * 主机送来连续无声数据时把 DAC 数字音量分步拉低（彻底安静、无底噪），
 * 一旦恢复有声数据再分步拉回 0dB（渐变，所以没有切换爆音）。 */
static void vMuteTask(void *pvParameters)
{
    TickType_t lastWake = xTaskGetTickCount();

    (void)pvParameters;

    for (;;)
    {
        I2S2_SilenceMuteService();
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(MUTE_PERIOD_MS));
    }
}

/* ============================================================================
 * 三、FreeRTOS 钩子
 * ========================================================================== */

/* 堆不够（xTaskCreate / 队列创建失败）会进这里 */
void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    UART1_SendString("\r\n[malloc FAILED] heap exhausted!\r\n");
    for (;;)
    {
    }
}

/* 栈溢出（configCHECK_FOR_STACK_OVERFLOW = 2）会进这里 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    taskDISABLE_INTERRUPTS();
    UART1_Printf("\r\n[stack OVERFLOW] task=%s\r\n", pcTaskName);
    for (;;)
    {
    }
}

/* ============================================================================
 * 四、main：硬件初始化（阻塞，启动调度器之前）→ 创建任务 → 启动调度器
 * ========================================================================== */

int main(void)
{
    uint16_t chip_id = 0;       /* 读回的器件 ID，期望 0x01A */
    uint8_t  i2c_status;        /* 0 = 通信正常，非 0 = 失败 */
    uint8_t  retry;             /* 重试计数 */

    /* ------------------------------------------------------------------
     * 第 0 步：NVIC 优先级分组必须先设成 Group 4
     * ------------------------------------------------------------------
     * FreeRTOS 使用 4 位抢占优先级、没有子优先级；
     * 如果沿用 ST 例程默认的分组，configMAX_SYSCALL_INTERRUPT_PRIORITY
     * 的屏蔽关系会算错，PendSV/SysTick 可能被意外屏蔽导致调度异常。
     * 这一步必须排在所有外设中断初始化之前。
     * ------------------------------------------------------------------ */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    /* 第 0.1 步：把 USB 的 D+ 拉低，让主机"看不见"这台设备 ——
     * Codec 初始化要几百毫秒，这段时间没有任务去响应枚举，
     * 先断开可以避免主机枚举超时后在设备管理器里留下无法识别的设备。 */
    UsbAudio_PreInit();

    /* ------------------------------------------------------------------
     * 第 1 步：初始化调试串口 + 1ms 时基（TIM4 中断）
     * ------------------------------------------------------------------ */
    UART1_Init(UART_BAUD_RATE);
    UsbAudio_TimeBaseInit();

    /* ------------------------------------------------------------------
     * 第 2 步：初始化软件 I²C（PB6 = SCL，PB7 = SDA）
     * ------------------------------------------------------------------
     * 不用 STM32 硬件 I²C：F1 系列的硬件 I²C 存在总线锁死缺陷，
     * 会出现数据线被拉低后无法释放、发送无响应的现象。
     * ------------------------------------------------------------------ */
    I2C1_Master_Init();
    Delay_ms(CODEC_POWER_SETTLE_MS);

    /* ------------------------------------------------------------------
     * 第 3 步：读取 Codec 器件 ID，确认 I²C 链路真的通了
     * ------------------------------------------------------------------
     * 只有 ID 正确才继续配置和播放。若本次上电时序较慢就持续重试，
     * 绝不进入"USB 已枚举、I²S/DMA 却没启动"的假就绪状态。
     * ------------------------------------------------------------------ */
    i2c_status = 1;
    retry = 0;
    while (i2c_status != 0)
    {
        i2c_status = NAU8822_CheckCommunication(&chip_id);
        if (i2c_status == 0)
        {
            break;              /* ID 正确，开始初始化音频通路 */
        }
        if (retry < 0xFFu)
        {
            retry++;
        }
        Delay_ms(I2C_RETRY_DELAY_MS);
    }

    if (i2c_status == 0)
    {
        UART1_Printf("NAU8822 I2C OK, ID=0x%04X\r\n", chip_id);

        /* 第 4 步：配置 Codec 内部寄存器（软复位、电源与偏置、
         * 音频接口格式 16bit/标准 I²S、从机时钟、DAC 与耳机通路） */
        NAU8822_Init();

        /* 第 5 步：设置耳机输出音量 */
        NAU8822_SetVolume(AUDIO_VOLUME);

        /* 第 6 步：初始化 I²S2（主发送、Philips、16 位、46.875kHz） */
        I2S2_Init();

        /* NAU8822 Rev 3.3 要求：耳机通路已使能且 MCLK 出现后，
         * 至少等待 250 ms 再发送 I²S 信号。这里留 300 ms 裕量；
         * 延时期间 USB 仍处于断开状态，主机不会提前打开音频流。 */
        Delay_ms(300);

        /* 第 7 步：启动 DMA 双缓冲播放（只跑 DMA，不播开机自检音） */
        I2S2_StartSilence();
        UART1_SendString("I2S audio playing\r\n");

#if ENABLE_USB_MIC
        /* 第 7.1 步：录音通路（麦克风 -> PGA -> ADC -> ADCOUT -> I2S3）
         * 硬件前提：ADCOUT / BCLK / LRCLK 分别接到
         *           PB5(I2S3_SD) / PB3(I2S3_CK) / PA15(I2S3_WS) */
        NAU8822_EnableMic();
        I2S3_Init();
        UART1_SendString("mic path ready (ADCOUT -> I2S3)\r\n");
#endif
    }
    else
    {
        /* I²C 不通时不启动 I²S，直接报错，避免无声且无从排查 */
        UART1_SendString("NAU8822 I2C FAIL\r\n");
    }

    /* 第 8 步：启动 USB 声卡
     * 必须放在音频通路全部就绪之后：USB 一挂上主机就开始枚举，
     * 如果这时候 Codec 还没配好，会先出一声爆音。 */
    UsbAudio_Init();
    UART1_SendString("USB audio device started\r\n");

    /* ------------------------------------------------------------------
     * 第 9 步：创建任务并启动调度器
     * ------------------------------------------------------------------
     * 返回值不判断是否失败：configUSE_MALLOC_FAILED_HOOK=1，
     * 堆不够会直接进 vApplicationMallocFailedHook 报错并停机。
     * ------------------------------------------------------------------ */
    UART1_Printf("[rtos] heap free before tasks = %u bytes\r\n",
                 (unsigned)xPortGetFreeHeapSize());

    (void)xTaskCreate(vUsbTask,  "USB",  USB_TASK_STACK,  NULL, USB_TASK_PRIO,  NULL);
    (void)xTaskCreate(vDiagTask, "DIAG", DIAG_TASK_STACK, NULL, DIAG_TASK_PRIO, NULL);
    (void)xTaskCreate(vMuteTask, "MUTE", MUTE_TASK_STACK, NULL, MUTE_TASK_PRIO, NULL);

    UART1_Printf("[rtos] heap free after tasks  = %u bytes\r\n",
                 (unsigned)xPortGetFreeHeapSize());
    UART1_SendString("[rtos] starting scheduler\r\n");

    vTaskStartScheduler();          /* 正常情况下不会返回 */

    /* 只有堆彻底耗尽导致空闲任务创建失败才会走到这里 */
    UART1_SendString("[rtos] scheduler start FAILED\r\n");
    for (;;)
    {
    }
}
