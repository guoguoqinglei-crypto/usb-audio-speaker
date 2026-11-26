#ifndef AUDIO_KEY_H
#define AUDIO_KEY_H

#include <stdint.h>

/* A=PC4：板载 Codec 音量 +1；B=PC5：板载 Codec 音量 -1。
 * 两个按键均为上拉输入、低电平有效，分别由独立状态机消抖。 */
#define AUDIO_KEY_A  0U
#define AUDIO_KEY_B  1U

void AudioKeys_Init(uint8_t initialVolume);
void AudioKeys_Task(void);          /* 必须由同一个低优先级任务每 10ms 调用 */
uint8_t AudioKeys_GetVolume(void);  /* 板载音量 0~100，不是 Windows 软件音量 */
uint8_t AudioKeys_IsDown(uint8_t key);

#endif /* AUDIO_KEY_H */
