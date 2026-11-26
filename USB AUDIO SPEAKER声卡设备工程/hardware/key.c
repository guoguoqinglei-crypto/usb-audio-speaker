#include <key.h>

#include <stm32f10x.h>
#include <I2C.h>
#include <uart.h>

/* ============================================================================
 * A/B 板载音量键：两个相互独立的状态机
 * ----------------------------------------------------------------------------
 * IDLE -> PRESS_DEBOUNCE -> PRESSED -> RELEASE_DEBOUNCE -> IDLE
 * 每 10ms 调用一次，连续两个采样周期一致才确认边沿，即 20ms 消抖。
 * 仅在 IDLE 经消抖进入 PRESSED 时产生一次音量事件；PRESSED 状态保持期间
 * 不再触发，因此长按不连发，必须释放并重新按下才会产生下一次事件。
 * ========================================================================== */
#define AUDIO_KEY_COUNT                 2U
#define AUDIO_KEY_DEBOUNCE_COUNT        2U
#define AUDIO_KEY_STATE_IDLE            0U
#define AUDIO_KEY_STATE_PRESS_DEBOUNCE  1U
#define AUDIO_KEY_STATE_PRESSED         2U
#define AUDIO_KEY_STATE_RELEASE_DEBOUNCE 3U

#define AUDIO_KEY_MIN_VOLUME            0U
#define AUDIO_KEY_MAX_VOLUME            100U

typedef struct
{
    uint8_t state;
    uint8_t count;
} AudioKeyFsm;

static AudioKeyFsm s_keyFsm[AUDIO_KEY_COUNT];
static uint8_t s_boardVolume = AUDIO_KEY_MAX_VOLUME;

static uint8_t AudioKey_ReadPressed(uint8_t key)
{
    uint32_t pin = (key == AUDIO_KEY_A) ? GPIO_IDR_IDR4 : GPIO_IDR_IDR5;
    return ((GPIOC->IDR & pin) == 0U) ? 1U : 0U;
}

void AudioKeys_Init(uint8_t initialVolume)
{
    GPIO_InitTypeDef gpio;
    uint8_t key;

    if (initialVolume > AUDIO_KEY_MAX_VOLUME)
    {
        initialVolume = AUDIO_KEY_MAX_VOLUME;
    }
    s_boardVolume = initialVolume;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    gpio.GPIO_Pin = GPIO_Pin_4 | GPIO_Pin_5;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    gpio.GPIO_Mode = GPIO_Mode_IPU;       /* 内部上拉，按下接地 */
    GPIO_Init(GPIOC, &gpio);

    for (key = 0U; key < AUDIO_KEY_COUNT; key++)
    {
        s_keyFsm[key].state = AUDIO_KEY_STATE_IDLE;
        s_keyFsm[key].count = 0U;
    }
}

static void AudioKey_ApplyPress(uint8_t key)
{
    uint8_t oldVolume = s_boardVolume;

    /* 边界饱和：0 时减音量保持 0，100 时加音量保持 100，绝不回绕。 */
    if ((key == AUDIO_KEY_A) && (s_boardVolume < AUDIO_KEY_MAX_VOLUME))
    {
        s_boardVolume++;
    }
    else if ((key == AUDIO_KEY_B) && (s_boardVolume > AUDIO_KEY_MIN_VOLUME))
    {
        s_boardVolume--;
    }

    if (s_boardVolume != oldVolume)
    {
        /* 与软静音服务在同一个低优先级任务串行执行，避免软件 I2C 并发。 */
        NAU8822_SetVolume(s_boardVolume);
        UART1_Printf("[key] %s: board volume=%u\r\n",
                     (key == AUDIO_KEY_A) ? "A (+1)" : "B (-1)",
                     (unsigned)s_boardVolume);
    }
    else
    {
        UART1_Printf("[key] %s: volume limit=%u\r\n",
                     (key == AUDIO_KEY_A) ? "A" : "B",
                     (unsigned)s_boardVolume);
    }
}

void AudioKeys_Task(void)
{
    uint8_t key;

    for (key = 0U; key < AUDIO_KEY_COUNT; key++)
    {
        const uint8_t pressed = AudioKey_ReadPressed(key);
        AudioKeyFsm *fsm = &s_keyFsm[key];

        switch (fsm->state)
        {
            case AUDIO_KEY_STATE_IDLE:
                if (pressed != 0U)
                {
                    fsm->state = AUDIO_KEY_STATE_PRESS_DEBOUNCE;
                    fsm->count = 0U;
                }
                break;

            case AUDIO_KEY_STATE_PRESS_DEBOUNCE:
                if (pressed == 0U)
                {
                    fsm->state = AUDIO_KEY_STATE_IDLE;  /* 按下抖动，不触发 */
                }
                else if (++fsm->count >= AUDIO_KEY_DEBOUNCE_COUNT)
                {
                    fsm->state = AUDIO_KEY_STATE_PRESSED;
                    fsm->count = 0U;
                    AudioKey_ApplyPress(key);           /* 每次有效按下仅此一次 */
                }
                break;

            case AUDIO_KEY_STATE_PRESSED:
                if (pressed == 0U)
                {
                    fsm->state = AUDIO_KEY_STATE_RELEASE_DEBOUNCE;
                    fsm->count = 0U;
                }
                break;                                  /* 长按不连发 */

            case AUDIO_KEY_STATE_RELEASE_DEBOUNCE:
                if (pressed != 0U)
                {
                    fsm->state = AUDIO_KEY_STATE_PRESSED;
                }
                else if (++fsm->count >= AUDIO_KEY_DEBOUNCE_COUNT)
                {
                    fsm->state = AUDIO_KEY_STATE_IDLE;  /* 完整释放后才允许下次按下 */
                    fsm->count = 0U;
                }
                break;

            default:
                fsm->state = AUDIO_KEY_STATE_IDLE;
                fsm->count = 0U;
                break;
        }
    }
}

uint8_t AudioKeys_GetVolume(void)
{
    return s_boardVolume;
}

uint8_t AudioKeys_IsDown(uint8_t key)
{
    if ((key >= AUDIO_KEY_COUNT) ||
        ((s_keyFsm[key].state != AUDIO_KEY_STATE_PRESSED) &&
         (s_keyFsm[key].state != AUDIO_KEY_STATE_RELEASE_DEBOUNCE)))
    {
        return 0U;
    }
    return 1U;
}
