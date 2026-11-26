#include <string.h>

#include <tusb.h>
#include <usb_descriptors.h>

/* ============================================================================
 * USB 描述符：UAC 1.0 音频扬声器（48 kHz / 16 bit / 立体声）
 * ----------------------------------------------------------------------------
 * 描述符全部用原始字节写出，没有用宏，原因是：
 *   1) UAC 1.0 的字段（终端类型、格式类型、端点属性）必须逐字节对齐，
 *      写宏反而看不清哪个字节对应哪个字段；
 *   2) 答辩时可以逐字节讲清楚每一个字段的含义。
 *
 * 整个配置描述符共 110 字节，结构如下：
 *
 *   偏移  长度  内容
 *   0x00   9    配置描述符
 *   0x09   9    AC 接口（Interface 0，0 个端点）
 *   0x12   9    AC 类接口头（声明 AC 总共 40 字节、下面包含哪个 AS 接口）
 *   0x1B  12    输入终端（USB 流，0x0101）
 *   0x27  10    功能单元（静音 + 音量）
 *   0x31   9    输出终端（扬声器，0x0301）
 *   0x3A   9    AS 接口 alt 0（空闲，0 个端点）
 *   0x43   9    AS 接口 alt 1（开流，1 个端点）
 *   0x4C   7    AS 类接口（PCM 格式、关联到输入终端 1）
 *   0x53  11    格式类型 I（2 声道、16 位、48000 Hz）
 *   0x5E   9    标准等时 OUT 端点（192 字节/包，每 1ms 一包）
 *   0x67   7    类特定端点描述符（采样率锁定方式等）
 * ========================================================================== */

/* 使用独立的声卡 PID，避免与其他 USB 示例设备共用主机驱动缓存。
 * CDC 调试模式使用另一个 PID，确保 Windows 按新的设备类重新枚举。 */
#define USB_VID             0xCafe
#if ENABLE_USB_CDC
/* CDC 模式再换一个 PID：Windows 会把"同一个 VID/PID 换了设备类"的情况
 * 当成驱动不匹配，直接显示"该设备无法启动(代码 10)"。
 * 换 PID 等于换了一张身份证，主机侧会重新走一遍枚举。 */
#define USB_PID             0x4003
#define USB_DEV_CLASS       0x02        /* bDeviceClass = CDC（通信设备类） */
#else
#define USB_PID             0x4002
#define USB_DEV_CLASS       0x00        /* 音频类信息写在接口描述符里 */
#endif
#define USB_BCD             0x0200

/*--------------------------------------------------------------------+
 * 设备描述符
 *--------------------------------------------------------------------*/
static const tusb_desc_device_t desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,

    /* 音频类信息写在接口描述符里，设备级三个字段都填 0；
     * CDC 模式则要在设备级标 0x02，否则 Windows 不认作串口。 */
    .bDeviceClass       = USB_DEV_CLASS,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    /* V1.4：增加 IN-only 厂商 HID，回传 A/B 与板载 Codec 音量。
     * UAC Feature Unit 仍只声明主通道静音，Windows 软件音量保持不变。
     * 提升 bcdDevice 让 Windows 重新读取描述符，避免沿用旧版缓存。 */
    .bcdDevice          = 0x0104,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/*--------------------------------------------------------------------+
 * 配置描述符（AC + AS 两个音频接口，打开 ENABLE_AUDIO_STATUS_HID 时再加一个 HID）
 *--------------------------------------------------------------------*/
/* 配置描述符总长度与接口数量跟着 ENABLE_AUDIO_STATUS_HID 走：
 *   纯音频   110 字节 / 2 个接口（AC + AS）
 *   音频+HID 142 字节 / 3 个接口
 *     = 110 + 接口 9 + HID 描述符 9 + 中断 IN 端点 7 + 中断 OUT 端点 7
 * 这两个值必须和下面的字节数组同步改，否则主机读到的长度对不上，
 * 枚举会停在"正在配置设备"那一步 —— 所以文末有编译期断言兜底。 */
#if ENABLE_USB_CDC
  #define CONFIG_TOTAL_LEN   67
  #define CONFIG_NUM_ITF     2
#elif ENABLE_AUDIO_STATUS_HID && ENABLE_USB_MIC
  /* 184B 音频 + 9B 接口 + 9B HID + 7B 中断 IN = 209B，4 个接口 */
  #define CONFIG_TOTAL_LEN   209
  #define CONFIG_NUM_ITF     4
#elif ENABLE_AUDIO_STATUS_HID
  /* 110B 音频 + 25B IN-only HID = 135B，3 个接口 */
  #define CONFIG_TOTAL_LEN   135
  #define CONFIG_NUM_ITF     3
#elif ENABLE_USB_MIC
  /* 扬声器 + 麦克风：184 字节 / 3 个接口
   *   = 原来的 110
   *   + AC 头多 1 字节（bInCollection=2 要多一个 baInterfaceNr）
   *   + 麦克风输入终端 12 + 输出终端 9（这两个属于 AC 接口）
   *   + 麦克风流接口 47（alt0 9 + alt1 9 + AS头 7 + 格式 11 + 端点 9 + 类端点 7） */
  #define CONFIG_TOTAL_LEN   184
  #define CONFIG_NUM_ITF     3
#else
  #define CONFIG_TOTAL_LEN   110
  #define CONFIG_NUM_ITF     2
#endif

static uint8_t const desc_configuration[] =
{
#if ENABLE_USB_CDC
    /* =============== CDC 虚拟串口（67 字节，2 个接口） ===============
     * CDC 规定必须拆成两个接口，缺一个主机就不认：
     *   Interface 0 = 通信接口（Communication Class），只走控制与通知
     *   Interface 1 = 数据接口（Data Class），真正的收发在这里
     */

    /* ---------------- 配置描述符（9 字节） ----------------
     * wTotalLength = 67，bNumInterfaces = 2，总线供电，最大 100mA */
    0x09, 0x02, 0x43, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,

    /* ---------------- 通信接口（9 字节） ----------------
     * bInterfaceNumber = 0、bNumEndpoints = 1（只有那个通知端点）
     * bInterfaceClass = 0x02（CDC）、bInterfaceSubClass = 0x02（ACM，调制解调器）
     * bInterfaceProtocol = 0x01（AT 命令，最通用的那种） */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,

    /* ---------------- 头部功能描述符（5 字节） ----------------
     * bcdCDC = 0x0110（CDC 1.10） */
    0x05, 0x24, 0x00, 0x10, 0x01,

    /* ---------------- 呼叫管理（5 字节） ----------------
     * bmCapabilities = 0（设备自己不管呼叫，主机说了算）
     * bDataInterface = 1（数据在接口 1） */
    0x05, 0x24, 0x01, 0x00, 0x01,

    /* ---------------- ACM 功能（4 字节） ----------------
     * bmCapabilities = 0x02：支持 Set_Line_Coding / Get_Line_Coding 和
     * 串口状态通知。Windows 的 usbser.sys 会先发这两条来"设置波特率"，
     * 不支持的话它照样能连，但有的上位机会一直等应答。 */
    0x04, 0x24, 0x02, 0x02,

    /* ---------------- 联合功能（5 字节） ----------------
     * 声明"接口 0（主）和接口 1（从）是一对"，主机靠这条把两者绑起来 */
    0x05, 0x24, 0x06, 0x00, 0x01,

    /* ---------------- 通知端点（7 字节） ----------------
     * IN 端点 0x81，中断传输，16 字节/包，bInterval = 10（10ms 一次）
     * 只用来送串口状态（如 DCD/DSR），不装日志数据 */
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x0A,

    /* ---------------- 数据接口（9 字节） ----------------
     * bInterfaceNumber = 1、bNumEndpoints = 2（一收一发）
     * bInterfaceClass = 0x0A（CDC-Data） */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,

    /* ---------------- 批量 OUT 端点（7 字节） ----------------
     * 端点 0x02，批量传输，64 字节/包 —— 主机 -> 板子 */
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,

    /* ---------------- 批量 IN 端点（7 字节） ----------------
     * 端点 0x82，批量传输，64 字节/包 —— 板子 -> 主机
     * 日志就是从这条出去的 */
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,
#else
    /* ---------------- 配置描述符（9 字节） ----------------
     * bLength 9、bDescriptorType 0x02
     * wTotalLength = CONFIG_TOTAL_LEN（110 或 142，见上面的宏）
     * bNumInterfaces = CONFIG_NUM_ITF（2 或 3）
     * bConfigurationValue = 1、iConfiguration = 0
     * bmAttributes = 0x80（总线供电）
     * bMaxPower = 0x32（50 × 2mA = 100mA）
     * 长度和接口数用宏算，避免手工改两处对不上 */
    0x09, 0x02, (uint8_t)(CONFIG_TOTAL_LEN & 0xFF), (uint8_t)(CONFIG_TOTAL_LEN >> 8),
    CONFIG_NUM_ITF, 0x01, 0x00, 0x80, 0x32,

    /* ---------------- AC 接口描述符（9 字节） ----------------
     * bInterfaceNumber = 0（音频控制接口）
     * bAlternateSetting = 0
     * bNumEndpoints = 0（AC 接口不传数据，只有控制通道）
     * bInterfaceClass = 0x01 AUDIO
     * bInterfaceSubClass = 0x01 AUDIOCONTROL
     * bInterfaceProtocol = 0 */
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,

    /* ---------------- AC 类接口头（9 字节） ----------------
     * bDescriptorSubtype = 0x01 HEADER
     * bcdADC = 0x0100（版本 1.0）
     * wTotalLength = 0x0028 = 40 字节
     *   = 本头 9 + 输入终端 12 + 功能单元 10 + 输出终端 9
     * bInCollection = 1（本功能下挂 1 个流接口）
     * baInterfaceNr(1) = 1（就是 Interface 1，音频流接口） */
#if ENABLE_USB_MIC
    /* 带麦克风时：
     *   bLength = 10（多一个 baInterfaceNr）
     *   wTotalLength = 62，= 头 10 + 播放输入终端 12 + 功能单元 10
     *                    + 播放输出终端 9 + 麦克风输入终端 12 + 麦克风输出终端 9
     *   bInCollection = 2
     *   baInterfaceNr(1) = 1（播放流接口）
     *   baInterfaceNr(2) = 2（麦克风流接口） */
    0x0A, 0x24, 0x01, 0x00, 0x01, 0x3E, 0x00, 0x02, 0x01, 0x02,
#else
    0x09, 0x24, 0x01, 0x00, 0x01, 0x28, 0x00, 0x01, 0x01,
#endif

    /* ---------------- 输入终端（12 字节） ----------------
     * bTerminalID = 1（后面功能单元和 AS 接口都引用这个编号）
     * wTerminalType = 0x0101（USB streaming，即来自主机的音频流）
     * bAssocTerminal = 0
     * bNrChannels = 2（立体声）
     * wChannelConfig = 0x0003（bit0 左声道、bit1 右声道）
     * iChannelNames = 0、iTerminal = 0 */
    0x0C, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,

    /* ---------------- 功能单元（10 字节） ----------------
     * bDescriptorSubtype = 0x06 FEATURE_UNIT
     * bUnitID = 2，bSourceID = 1，bControlSize = 1
     * bmaControls(0) = 0x01：主通道仅支持静音。
     * 左右声道不单独声明控制，也不声明硬件音量。
     * Windows 在音频引擎中完成 PCM 音量缩放；固件只响应主通道静音，
     * 因而拖动音量滑块不会触发 UAC 音量请求或固件增益乘法。 */
    0x0A, 0x24, 0x06, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,

    /* ---------------- 输出终端（9 字节） ----------------
     * bTerminalID = 3
     * wTerminalType = 0x0301（Speaker，扬声器）
     * bAssocTerminal = 0
     * bSourceID = 2（数据来自功能单元 2）
     * iTerminal = 0 */
    0x09, 0x24, 0x03, 0x03, 0x01, 0x03, 0x00, 0x02, 0x00,

#if ENABLE_USB_MIC
    /* ---------------- 麦克风输入终端（12 字节） ----------------
     * bTerminalID = 4
     * wTerminalType = 0x0201（Microphone，模拟麦克风）
     * bNrChannels = 1（单声道）
     * wChannelConfig = 0（单声道不声明声道位置）
     * 数据从这里进入 AC 功能，后面接到"USB 流输出终端 5" */
    0x0C, 0x24, 0x02, 0x04, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,

    /* ---------------- 麦克风输出终端（9 字节） ----------------
     * bTerminalID = 5
     * wTerminalType = 0x0101（USB streaming，音频流发往主机）
     * bSourceID = 4（来源是麦克风输入终端）
     * 下面的麦克风流接口通过 bTerminalLink = 5 关联到这里 */
    0x09, 0x24, 0x03, 0x05, 0x01, 0x01, 0x00, 0x04, 0x00,
#endif

    /* ---------------- AS 接口 alt 0（9 字节） ----------------
     * 空闲状态：bNumEndpoints = 0，不占用 USB 带宽
     * bInterfaceSubClass = 0x02 AUDIOSTREAMING */
    0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,

    /* ---------------- AS 接口 alt 1（9 字节） ----------------
     * 开流状态：bNumEndpoints = 1，主机切到这个 alt 才开始送音频 */
    0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,

    /* ---------------- AS 类接口（7 字节） ----------------
     * bDescriptorSubtype = 0x01 AS_GENERAL
     * bTerminalLink = 1（关联到输入终端 1）
     * bDelay = 0
     * wFormatTag = 0x0001（PCM，未压缩） */
    0x07, 0x24, 0x01, 0x01, 0x00, 0x01, 0x00,

    /* ---------------- 格式类型 I（11 字节） ----------------
     * bDescriptorSubtype = 0x02 FORMAT_TYPE
     * bFormatType = 0x01（TYPE_I，即标准 PCM）
     * bNrChannels = 2
     * bSubframeSize = 2（每个采样占 2 字节）
     * bBitResolution = 16（有效位数 16 bit）
     * bSamFreqType = 1（只支持一个采样率）
     * tSamFreq = 0x00BB80 = 48000 Hz（小端，低字节在前）
     * 注意：这里必须写标准值 48000，不能写实际的 46875，
     *       否则主机不会把它识别成 48kHz 声卡。 */
    0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00,

    /* ---------------- 标准等时 OUT 端点（9 字节） ----------------
     * bEndpointAddress = 0x01（端点 1，OUT 方向）
     * bmAttributes = 0x09：低 2 位 01 = 等时传输，
     *                3:2 位 10 = 自适应（adaptive，不需要反馈端点）
     * wMaxPacketSize = 0x00C0 = 192 字节/包
     * bInterval = 1（每 1 个帧周期，即 1ms 一包）
     * bRefresh = 0、bSynchAddress = 0（没有同步端点） */
    0x09, 0x05, 0x01, 0x09, 0xC0, 0x00, 0x01, 0x00, 0x00,

    /* ---------------- 类特定端点描述符（7 字节） ----------------
     * bDescriptorType = 0x25 CS_ENDPOINT
     * bDescriptorSubtype = 0x01 EP_GENERAL
     * bmAttributes = 0（不使用采样率控制、不启用音高控制）
     * bLockDelayUnits = 0、wLockDelay = 0（不关心锁定时延） */
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

#if ENABLE_USB_MIC
    /* =============== 麦克风流接口（Interface 2，47 字节） =============== */

    /* alt 0：空闲，0 个端点（主机不录音时用这个） */
    0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,

    /* alt 1：开流，1 个端点（主机点"开始录音"后切到这里） */
    0x09, 0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,

    /* AS 类接口：bTerminalLink = 5（关联到麦克风输出终端） */
    0x07, 0x24, 0x01, 0x05, 0x00, 0x01, 0x00,

    /* 格式类型 I：1 声道 / 每采样 2 字节 / 16 bit / 16000 Hz
     * tSamFreq = 0x003E80 = 16000（小端 80 3E 00）
     * 说明：Codec 的 ADC 实际跟着 I2S 时钟跑 46875 Hz，
     *       固件里做了重采样再按 16 kHz 送上来，所以这里写 16000。 */
    0x0B, 0x24, 0x02, 0x01, 0x01, 0x02, 0x10, 0x01, 0x80, 0x3E, 0x00,

    /* 标准等时 IN 端点：0x82，32 字节/包（16000×1×2÷1000），每 1ms 一包
     * bmAttributes = 0x05：低 2 位 01 = 等时，3:2 位 01 = 异步（麦克风常用）
     * 异步意味着设备自己控制采样节奏，不需要反馈端点。 */
    0x09, 0x05, EPNUM_AUDIO_IN, 0x05, 0x20, 0x00, 0x01, 0x00, 0x00,

    /* 类特定端点描述符 */
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
#endif

#if ENABLE_AUDIO_STATUS_HID
    /* ---------------- 声卡状态 HID 接口（9 字节） ----------------
     * 接口号由宏确定：启用麦克风时为 3；只有一个中断 IN 端点。 */
    0x09, 0x04, ITF_NUM_HID, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,

    /* ---------------- HID 描述符（9 字节） ----------------
     * bcdHID = 0x0111（HID 1.11）
     * bCountryCode = 0
     * bNumDescriptors = 1（只声明了报告描述符这一种）
     * bDescriptorType = 0x22（REPORT）
     * wDescriptorLength = 报告描述符长度（小端）
     * 注意：报告描述符本体不能塞进配置描述符里，主机枚举时会
     *       单独发 GET_DESCRIPTOR(0x22) 来取，这里只告诉它有多长。 */
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
    (uint8_t)(AUDIO_STATUS_REPORT_DESC_LEN & 0xFF),
    (uint8_t)(AUDIO_STATUS_REPORT_DESC_LEN >> 8),

    /* ---------------- HID 中断 IN 端点（7 字节） ----------------
     * EP3 IN，固定 8 字节报告，1ms 轮询；没有 OUT，也不使用 Report ID。 */
    0x07, 0x05, EPNUM_HID_IN, 0x03, AUDIO_STATUS_REPORT_LEN, 0x00, 0x01,
#endif
#endif  /* !ENABLE_USB_CDC */
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

/* 编译期检查：数组实际长度必须等于 wTotalLength，写错一个字节就编译报错。
 * 这是整个描述符里最容易出错的地方，让编译器帮忙盯着。 */
TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
                 "configuration descriptor length mismatch");

/*--------------------------------------------------------------------+
 * 字符串描述符
 *--------------------------------------------------------------------*/
static const char *const string_desc_arr[] =
{
    (const char[]){0x09, 0x04},     /* 0: 语言 ID（英文 - 美国） */
    "STM32 Audio Lab",              /* 1: 厂商 */
#if ENABLE_USB_CDC
    "STM32 BT Debug (CDC)",         /* 2: 产品名，会显示在“端口”那一栏 */
#else
    "USB Audio Speaker (NAU88C22)", /* 2: 产品名，会显示在 Windows 声音设备里 */
#endif
    "000001",                       /* 3: 序列号 */
};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    static uint16_t desc_str[32];
    uint8_t chr_count;

    if (index == 0)
    {
        /* 语言 ID 描述符只包含一个 16 位数据 */
        desc_str[1] = 0x0409;
        chr_count = 1;
    }
    else
    {
        if (index >= (uint8_t)(sizeof(string_desc_arr) / sizeof(string_desc_arr[0])))
        {
            return NULL;
        }

        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31)
        {
            chr_count = 31;
        }

        for (uint8_t i = 0; i < chr_count; i++)
        {
            desc_str[1 + i] = (uint16_t)str[i];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2U * chr_count + 2U));
    return desc_str;
}
