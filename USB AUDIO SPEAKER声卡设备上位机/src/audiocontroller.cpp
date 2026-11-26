#include "audiocontroller.h"

#include <QAudioFormat>
#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#endif

namespace {

constexpr int kTestSampleRate = 48000;
constexpr int kMaxRecordSeconds = 120;

QString sampleFormatName(QAudioFormat::SampleFormat format)
{
    switch (format) {
    case QAudioFormat::UInt8:  return QStringLiteral("8 bit");
    case QAudioFormat::Int16:  return QStringLiteral("16 bit");
    case QAudioFormat::Int32:  return QStringLiteral("32 bit");
    case QAudioFormat::Float:  return QStringLiteral("Float");
    default:                   return QStringLiteral("未知格式");
    }
}

QByteArray convertPcmToFormat(const QByteArray &source, const QAudioFormat &sourceFormat,
                              const QAudioFormat &targetFormat)
{
    const int sourceChannels = sourceFormat.channelCount();
    const int targetChannels = targetFormat.channelCount();
    const int sourceRate = sourceFormat.sampleRate();
    const int targetRate = targetFormat.sampleRate();
    const int sourceBytes = sourceFormat.bytesPerSample();
    const int targetBytes = targetFormat.bytesPerSample();
    if (sourceChannels <= 0 || targetChannels <= 0 || sourceRate <= 0 || targetRate <= 0
        || sourceBytes <= 0 || targetBytes <= 0)
        return {};
    if (sourceChannels == targetChannels && sourceRate == targetRate
        && sourceFormat.sampleFormat() == targetFormat.sampleFormat())
        return source;

    const qint64 frameCount = source.size() / (sourceChannels * sourceBytes);
    if (frameCount <= 0)
        return {};

    const auto readSample = [&](qint64 frame, int channel) {
        const qsizetype offset = static_cast<qsizetype>((frame * sourceChannels + channel) * sourceBytes);
        const char *data = source.constData() + offset;
        switch (sourceFormat.sampleFormat()) {
        case QAudioFormat::UInt8:
            return (static_cast<int>(*reinterpret_cast<const uchar *>(data)) - 128) / 128.0;
        case QAudioFormat::Int16: {
            qint16 value;
            std::memcpy(&value, data, sizeof(value));
            return value / 32768.0;
        }
        case QAudioFormat::Int32: {
            qint32 value;
            std::memcpy(&value, data, sizeof(value));
            return value / 2147483648.0;
        }
        case QAudioFormat::Float: {
            float value;
            std::memcpy(&value, data, sizeof(value));
            return static_cast<double>(value);
        }
        default:
            return 0.0;
        }
    };
    const auto readMixed = [&](qint64 frame, int targetChannel) {
        if (targetChannels == 1 && sourceChannels > 1) {
            double sum = 0.0;
            for (int channel = 0; channel < sourceChannels; ++channel)
                sum += readSample(frame, channel);
            return sum / sourceChannels;
        }
        const int sourceChannel = sourceChannels == 1 ? 0 : qMin(targetChannel, sourceChannels - 1);
        return readSample(frame, sourceChannel);
    };

    const qint64 outputFrames = (frameCount * targetRate + sourceRate - 1) / sourceRate;
    if (outputFrames <= 0 || outputFrames > 50000000)
        return {};
    QByteArray converted;
    converted.resize(static_cast<qsizetype>(outputFrames * targetChannels * targetBytes));
    for (qint64 frame = 0; frame < outputFrames; ++frame) {
        const qint64 numerator = frame * sourceRate;
        const qint64 left = numerator / targetRate;
        const qint64 right = qMin(left + 1, frameCount - 1);
        const double fraction = static_cast<double>(numerator % targetRate) / targetRate;
        for (int channel = 0; channel < targetChannels; ++channel) {
            const double first = readMixed(left, channel);
            const double second = readMixed(right, channel);
            const double value = qBound(-1.0, first + (second - first) * fraction, 1.0);
            const qsizetype offset = static_cast<qsizetype>((frame * targetChannels + channel) * targetBytes);
            char *data = converted.data() + offset;
            switch (targetFormat.sampleFormat()) {
            case QAudioFormat::UInt8: {
                const quint8 sample = static_cast<quint8>(qRound((value + 1.0) * 127.5));
                std::memcpy(data, &sample, sizeof(sample));
                break;
            }
            case QAudioFormat::Int16: {
                const qint16 sample = static_cast<qint16>(qRound(value * 32767.0));
                std::memcpy(data, &sample, sizeof(sample));
                break;
            }
            case QAudioFormat::Int32: {
                const qint32 sample = static_cast<qint32>(value * 2147483647.0);
                std::memcpy(data, &sample, sizeof(sample));
                break;
            }
            case QAudioFormat::Float: {
                const float sample = static_cast<float>(value);
                std::memcpy(data, &sample, sizeof(sample));
                break;
            }
            default:
                return {};
            }
        }
    }
    return converted;
}

#ifdef Q_OS_WIN
class ComApartment
{
public:
    ComApartment() : m_hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment()
    {
        if (m_hr == S_OK || m_hr == S_FALSE)
            CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }
private:
    HRESULT m_hr;
};

IAudioEndpointVolume *acquireEndpointVolume(const QAudioDevice &audioDevice)
{
    if (audioDevice.isNull() || audioDevice.id().isEmpty())
        return nullptr;

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator)
        return nullptr;

    const QString id = QString::fromUtf8(audioDevice.id());
    IMMDevice *device = nullptr;
    hr = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(id.utf16()), &device);
    enumerator->Release();
    if (FAILED(hr) || !device)
        return nullptr;

    IAudioEndpointVolume *endpoint = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void **>(&endpoint));
    device->Release();
    return SUCCEEDED(hr) ? endpoint : nullptr;
}

bool queryEndpointVolume(const QAudioDevice &device, float &volume, bool &muted)
{
    ComApartment apartment;
    if (!apartment.usable())
        return false;
    IAudioEndpointVolume *endpoint = acquireEndpointVolume(device);
    if (!endpoint)
        return false;

    BOOL winMute = FALSE;
    const HRESULT volumeHr = endpoint->GetMasterVolumeLevelScalar(&volume);
    const HRESULT muteHr = endpoint->GetMute(&winMute);
    endpoint->Release();
    muted = winMute != FALSE;
    return SUCCEEDED(volumeHr) && SUCCEEDED(muteHr);
}

bool writeEndpointVolume(const QAudioDevice &device, float volume, bool muted)
{
    ComApartment apartment;
    if (!apartment.usable())
        return false;
    IAudioEndpointVolume *endpoint = acquireEndpointVolume(device);
    if (!endpoint)
        return false;

    const HRESULT volumeHr = endpoint->SetMasterVolumeLevelScalar(volume, nullptr);
    const HRESULT muteHr = endpoint->SetMute(muted ? TRUE : FALSE, nullptr);
    endpoint->Release();
    return SUCCEEDED(volumeHr) && SUCCEEDED(muteHr);
}
#else
bool queryEndpointVolume(const QAudioDevice &, float &, bool &) { return false; }
bool writeEndpointVolume(const QAudioDevice &, float, bool) { return false; }
#endif

} // namespace

AudioController::AudioController(QObject *parent)
    : QObject(parent)
{
    m_audioOutput.setVolume(1.0f);
    m_player.setAudioOutput(&m_audioOutput);

    const QString espeak = QStandardPaths::findExecutable(QStringLiteral("espeak-ng"));
    m_ttsEngine = espeak.isEmpty()
            ? QStringLiteral("Windows SAPI · 离线合成")
            : QStringLiteral("eSpeak NG · 开源离线合成（SAPI 备用）");

    connect(&m_mediaDevices, &QMediaDevices::audioOutputsChanged,
            this, &AudioController::refreshOutputDevices);
    connect(&m_mediaDevices, &QMediaDevices::audioInputsChanged,
            this, &AudioController::refreshInputDevices);

    connect(&m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
                setPlaying(state == QMediaPlayer::PlayingState);
            });
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::EndOfMedia) {
                    setPlaying(false);
                    if (!m_ttsBusy)
                        setStatus(QStringLiteral("播放完成"), false);
                }
            });
    connect(&m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &error) {
                setPlaying(false);
                setStatus(QStringLiteral("播放失败：%1").arg(error));
            });

    connect(&m_ttsProcess, &QProcess::finished,
            this, &AudioController::finishTts);
    connect(&m_ttsProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart && m_ttsBusy) {
                    setTtsBusy(false);
                    setStatus(QStringLiteral("无法启动语音合成组件"));
                }
            });
    connect(&m_sttProcess, &QProcess::finished,
            this, &AudioController::finishStt);
    connect(&m_sttProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart && m_sttBusy) {
                    m_sttTimeout.stop();
                    QFile::remove(m_sttScriptPath);
                    setSttBusy(false);
                    setStatus(QStringLiteral("无法启动离线语音识别组件"));
                }
            });
    m_sttTimeout.setSingleShot(true);
    connect(&m_sttTimeout, &QTimer::timeout, this, [this]() {
        if (!m_sttBusy)
            return;
        cancelSpeechToText();
        setStatus(QStringLiteral("语音识别超时，已恢复操作，请缩短录音后再试"));
    });

    m_recordTimer.setInterval(100);
    connect(&m_recordTimer, &QTimer::timeout,
            this, &AudioController::updateRecording);

    // Windows 音量键、系统滑块或其他软件调音量时，同步刷新上位机显示。
    // 只在数值真正变化时发信号，不写回端点，避免与拖动滑块形成循环。
    QTimer *endpointSyncTimer = new QTimer(this);
    endpointSyncTimer->setInterval(200);
    connect(endpointSyncTimer, &QTimer::timeout,
            this, &AudioController::syncEndpointVolume);
    endpointSyncTimer->start();

    connect(&m_hidStatus, &HidStatusMonitor::connectionChanged, this,
            [this](bool connected) {
                if (m_boardHidConnected == connected)
                    return;
                m_boardHidConnected = connected;
                if (!connected) {
                    m_boardKeyA = false;
                    m_boardKeyB = false;
                }
                emit boardStatusChanged();
                addLog(connected ? QStringLiteral("板载状态 HID 已连接，可显示 A/B 与 Codec 音量")
                                 : QStringLiteral("板载状态 HID 已断开，等待自动重连"));
            });
    connect(&m_hidStatus, &HidStatusMonitor::reportReceived, this,
            [this](const QByteArray &raw) {
                QByteArray body = raw;
                // 无 Report ID 的 Windows HID 输入通常是前导 0 + 8 字节报告体。
                if (body.size() >= 9 && quint8(body.at(0)) == 0)
                    body = body.mid(1, 8);
                if (body.size() < 8 || body.at(1) < 0 || quint8(body.at(1)) > 100)
                    return;
                const bool keyA = (quint8(body.at(0)) & 0x01) != 0;
                const bool keyB = (quint8(body.at(0)) & 0x02) != 0;
                const int volume = quint8(body.at(1));
                const bool keysChanged = keyA != m_boardKeyA || keyB != m_boardKeyB;
                const bool volumeChanged = volume != m_boardVolume;
                m_boardKeyA = keyA;
                m_boardKeyB = keyB;
                m_boardVolume = volume;
                if (keysChanged || volumeChanged)
                    emit boardStatusChanged();
                // 心跳只刷新界面，不写日志；仅记录真实按键边沿和音量变化。
                if (keysChanged)
                    addLog(QStringLiteral("板载按键：A(加音量)=%1，B(减音量)=%2")
                           .arg(keyA ? QStringLiteral("按下") : QStringLiteral("松开"))
                           .arg(keyB ? QStringLiteral("按下") : QStringLiteral("松开")));
                if (volumeChanged)
                    addLog(QStringLiteral("板载 Codec 音量：%1（每次按键变化 1）").arg(volume));
            });
    m_hidStatus.start();

    refreshDevices();
    addLog(QStringLiteral("上位机已启动，使用系统 UAC 音频接口"));
}

AudioController::~AudioController()
{
    m_sttTimeout.stop();
    if (m_recording)
        stopRecording();
    if (m_ttsProcess.state() != QProcess::NotRunning) {
        setTtsBusy(false);
        m_ttsProcess.kill();
        m_ttsProcess.waitForFinished(500);
    }
    if (m_sttProcess.state() != QProcess::NotRunning) {
        setSttBusy(false);
        m_sttProcess.kill();
        m_sttProcess.waitForFinished(500);
    }
}

bool AudioController::isUsbAudioDevice(const QAudioDevice &device)
{
    const QString name = device.description().toLower();
    return name.contains(QStringLiteral("usb audio"))
            || name.contains(QStringLiteral("nau88"))
            || name.contains(QStringLiteral("usb 音频"))
            || name.contains(QStringLiteral("usb speaker"));
}

QString AudioController::formatDescription(const QAudioDevice &device)
{
    if (device.isNull())
        return QStringLiteral("未检测到设备");
    const QAudioFormat format = device.preferredFormat();
    const QString channels = format.channelCount() == 1
            ? QStringLiteral("单声道")
            : QStringLiteral("%1 声道").arg(format.channelCount());
    return QStringLiteral("%1 Hz · %2 · %3")
            .arg(format.sampleRate())
            .arg(channels, sampleFormatName(format.sampleFormat()));
}

int AudioController::preferredDeviceIndex(const QList<QAudioDevice> &devices) const
{
    for (int i = 0; i < devices.size(); ++i) {
        if (isUsbAudioDevice(devices.at(i)))
            return i;
    }
    for (int i = 0; i < devices.size(); ++i) {
        if (devices.at(i).isDefault())
            return i;
    }
    return devices.isEmpty() ? -1 : 0;
}

void AudioController::refreshDevices()
{
    refreshOutputDevices();
    refreshInputDevices();

    if (m_hasUsbSpeaker || m_hasUsbMicrophone)
        setStatus(QStringLiteral("USB 声卡已就绪，可开始播放或录音"), false);
    else
        setStatus(QStringLiteral("未识别到 USB Audio Speaker，已列出系统可用声卡"), false);
}

void AudioController::refreshOutputDevices()
{
    QByteArray previousId;
    if (m_outputIndex >= 0 && m_outputIndex < m_outputDevices.size())
        previousId = m_outputDevices.at(m_outputIndex).id();

    m_outputDevices = QMediaDevices::audioOutputs();
    m_outputDeviceNames.clear();
    bool hasUsb = false;
    for (const QAudioDevice &device : m_outputDevices) {
        QString name = device.description();
        if (device.isDefault())
            name += QStringLiteral("（系统默认）");
        m_outputDeviceNames.append(name);
        hasUsb = hasUsb || isUsbAudioDevice(device);
    }

    int index = -1;
    for (int i = 0; i < m_outputDevices.size(); ++i) {
        if (m_outputDevices.at(i).id() == previousId) {
            index = i;
            break;
        }
    }
    if (index < 0)
        index = preferredDeviceIndex(m_outputDevices);

    m_hasUsbSpeaker = hasUsb;
    emit outputDevicesChanged();
    emit deviceStatusChanged();
    setOutputIndex(index);
}

void AudioController::refreshInputDevices()
{
    QByteArray previousId;
    if (m_inputIndex >= 0 && m_inputIndex < m_inputDevices.size())
        previousId = m_inputDevices.at(m_inputIndex).id();

    m_inputDevices = QMediaDevices::audioInputs();
    m_inputDeviceNames.clear();
    bool hasUsb = false;
    for (const QAudioDevice &device : m_inputDevices) {
        QString name = device.description();
        if (device.isDefault())
            name += QStringLiteral("（系统默认）");
        m_inputDeviceNames.append(name);
        hasUsb = hasUsb || isUsbAudioDevice(device);
    }

    int index = -1;
    for (int i = 0; i < m_inputDevices.size(); ++i) {
        if (m_inputDevices.at(i).id() == previousId) {
            index = i;
            break;
        }
    }
    if (index < 0)
        index = preferredDeviceIndex(m_inputDevices);

    m_hasUsbMicrophone = hasUsb;
    emit inputDevicesChanged();
    emit deviceStatusChanged();
    setInputIndex(index);
}

void AudioController::setOutputIndex(int index)
{
    if (index < 0 || index >= m_outputDevices.size()) {
        if (m_outputIndex != -1) {
            m_outputIndex = -1;
            m_outputFormat = QStringLiteral("未选择播放设备");
            emit outputIndexChanged();
        }
        return;
    }

    const QAudioDevice &device = m_outputDevices.at(index);
    if (m_audioSink && m_outputIndex >= 0 && m_outputIndex < m_outputDevices.size()
        && m_outputDevices.at(m_outputIndex).id() != device.id()) {
        stopAudioSink();
        setPlaying(false);
    }
    m_outputIndex = index;
    m_outputFormat = formatDescription(device);
    m_audioOutput.setDevice(device);

    float endpointVolume = 0.8f;
    bool endpointMuted = false;
    m_endpointVolumeAvailable = queryEndpointVolume(device, endpointVolume, endpointMuted);
    if (m_endpointVolumeAvailable) {
        m_volume = qBound(0, qRound(endpointVolume * 100.0f), 100);
        m_muted = endpointMuted;
    }
    const qreal appVolume = m_muted ? 0.0
            : (m_endpointVolumeAvailable ? 1.0 : m_volume / 100.0);
    m_audioOutput.setVolume(appVolume); // 避免系统端点与应用软件音量重复衰减。
    m_audioOutput.setMuted(m_muted);
    if (m_audioSink)
        m_audioSink->setVolume(appVolume);

    emit outputIndexChanged();
    emit volumeChanged();
    emit mutedChanged();
}

void AudioController::setInputIndex(int index)
{
    if (index < 0 || index >= m_inputDevices.size()) {
        if (m_inputIndex != -1) {
            m_inputIndex = -1;
            m_inputFormat = QStringLiteral("未选择录音设备");
            emit inputIndexChanged();
        }
        return;
    }

    m_inputIndex = index;
    m_inputFormat = formatDescription(m_inputDevices.at(index));
    emit inputIndexChanged();
}

void AudioController::syncEndpointVolume()
{
    if (m_outputIndex < 0 || m_outputIndex >= m_outputDevices.size())
        return;

    float endpointVolume = 0.0f;
    bool endpointMuted = false;
    if (!queryEndpointVolume(m_outputDevices.at(m_outputIndex), endpointVolume, endpointMuted))
        return;

    const int volume = qBound(0, qRound(endpointVolume * 100.0f), 100);
    const bool volumeDidChange = m_volume != volume;
    const bool mutedDidChange = m_muted != endpointMuted;
    const bool outputGainChanged = !m_endpointVolumeAvailable || mutedDidChange;

    m_endpointVolumeAvailable = true;
    m_volume = volume;
    m_muted = endpointMuted;

    // 系统端点负责实际音量；这里只跟随静音状态，避免应用音量重复衰减。
    if (outputGainChanged) {
        const qreal appVolume = m_muted ? 0.0 : 1.0;
        m_audioOutput.setVolume(appVolume);
        m_audioOutput.setMuted(m_muted);
        if (m_audioSink)
            m_audioSink->setVolume(appVolume);
    }

    if (volumeDidChange)
        emit volumeChanged();
    if (mutedDidChange)
        emit mutedChanged();
}

void AudioController::setVolume(int volume)
{
    volume = qBound(0, volume, 100);
    const bool changed = m_volume != volume;
    m_volume = volume;

    QAudioDevice device;
    if (m_outputIndex >= 0 && m_outputIndex < m_outputDevices.size())
        device = m_outputDevices.at(m_outputIndex);

    m_endpointVolumeAvailable = !device.isNull()
            && writeEndpointVolume(device, volume / 100.0f, m_muted);
    const qreal appVolume = m_muted ? 0.0
            : (m_endpointVolumeAvailable ? 1.0 : volume / 100.0);
    m_audioOutput.setVolume(appVolume);
    m_audioOutput.setMuted(m_muted);
    if (m_audioSink)
        m_audioSink->setVolume(appVolume);
    if (changed)
        emit volumeChanged();
}

void AudioController::setMuted(bool muted)
{
    m_muted = muted;

    QAudioDevice device;
    if (m_outputIndex >= 0 && m_outputIndex < m_outputDevices.size())
        device = m_outputDevices.at(m_outputIndex);

    m_endpointVolumeAvailable = !device.isNull()
            && writeEndpointVolume(device, m_volume / 100.0f, muted);
    const qreal appVolume = muted ? 0.0
            : (m_endpointVolumeAvailable ? 1.0 : m_volume / 100.0);
    m_audioOutput.setVolume(appVolume);
    m_audioOutput.setMuted(muted);
    if (m_audioSink)
        m_audioSink->setVolume(appVolume);

    if (!device.isNull()) {
        if (m_endpointVolumeAvailable) {
            setStatus(muted ? QStringLiteral("USB 声卡已静音")
                            : QStringLiteral("USB 声卡已取消静音"), false);
        } else {
            setStatus(muted ? QStringLiteral("USB 端点写入失败，仅对上位机播放静音")
                            : QStringLiteral("USB 端点写入失败，仅取消上位机播放静音"));
        }
    }
    emit mutedChanged();
}

void AudioController::setTtsRate(int rate)
{
    rate = qBound(80, rate, 250);
    if (m_ttsRate == rate)
        return;
    m_ttsRate = rate;
    emit ttsRateChanged();
}

bool AudioController::writeWavHeader(QFile &file, quint32 dataBytes,
                                     int sampleRate, int channels)
{
    if (!file.isOpen() || !file.seek(0))
        return false;

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    const quint16 bits = 16;
    const quint32 byteRate = static_cast<quint32>(sampleRate * channels * bits / 8);
    const quint16 blockAlign = static_cast<quint16>(channels * bits / 8);

    stream.writeRawData("RIFF", 4);
    stream << static_cast<quint32>(36 + dataBytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << static_cast<quint32>(16)
           << static_cast<quint16>(1)
           << static_cast<quint16>(channels)
           << static_cast<quint32>(sampleRate)
           << byteRate << blockAlign << bits;
    stream.writeRawData("data", 4);
    stream << dataBytes;
    return stream.status() == QDataStream::Ok;
}

bool AudioController::createTestTone(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    constexpr int channels = 2;
    constexpr double noteDuration = 0.65;
    constexpr double frequencies[] = {523.25, 659.25, 783.99, 1046.50};
    constexpr int sampleCount = static_cast<int>(kTestSampleRate * noteDuration * 4);
    QByteArray pcm;
    pcm.resize(sampleCount * channels * static_cast<int>(sizeof(qint16)));
    qint16 *samples = reinterpret_cast<qint16 *>(pcm.data());

    for (int i = 0; i < sampleCount; ++i) {
        const int note = std::min(3, static_cast<int>(i / (kTestSampleRate * noteDuration)));
        const double localTime = std::fmod(static_cast<double>(i) / kTestSampleRate, noteDuration);
        const double attack = std::min(1.0, localTime / 0.025);
        const double release = std::min(1.0, (noteDuration - localTime) / 0.08);
        const double envelope = std::max(0.0, std::min(attack, release));
        const double time = static_cast<double>(i) / kTestSampleRate;
        const double value = (std::sin(2.0 * M_PI * frequencies[note] * time)
                              + 0.18 * std::sin(4.0 * M_PI * frequencies[note] * time))
                             * 0.28 * envelope;
        const qint16 sample = static_cast<qint16>(qBound(-1.0, value, 1.0) * 32767.0);
        samples[i * 2] = sample;
        samples[i * 2 + 1] = sample;
    }

    const quint32 dataBytes = static_cast<quint32>(pcm.size());
    writeWavHeader(file, dataBytes, kTestSampleRate, channels);
    file.seek(44);
    return file.write(pcm) == pcm.size();
}

void AudioController::playFile(const QString &path, bool allowRetry)
{
    if (m_outputIndex < 0 || m_outputIndex >= m_outputDevices.size()) {
        setStatus(QStringLiteral("没有可用的播放设备"));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("播放失败：无法打开音频文件"));
        return;
    }
    const QByteArray wav = file.readAll();
    if (wav.size() < 12 || std::memcmp(wav.constData(), "RIFF", 4) != 0
        || std::memcmp(wav.constData() + 8, "WAVE", 4) != 0) {
        setStatus(QStringLiteral("播放失败：不是有效的 WAV 文件"));
        return;
    }

    const auto readU16 = [](const char *data) {
        const auto *p = reinterpret_cast<const uchar *>(data);
        return static_cast<quint16>(quint16(p[0]) | (quint16(p[1]) << 8));
    };
    const auto readU32 = [](const char *data) {
        const auto *p = reinterpret_cast<const uchar *>(data);
        return quint32(p[0]) | (quint32(p[1]) << 8)
               | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
    };

    QAudioFormat format;
    bool formatFound = false;
    QByteArray pcm;
    qsizetype offset = 12;
    while (offset + 8 <= wav.size()) {
        const char *chunkId = wav.constData() + offset;
        const quint32 chunkSize = readU32(chunkId + 4);
        offset += 8;
        if (chunkSize > static_cast<quint64>(wav.size() - offset)) {
            setStatus(QStringLiteral("播放失败：WAV 数据不完整"));
            return;
        }

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                setStatus(QStringLiteral("播放失败：WAV 格式信息不完整"));
                return;
            }
            const char *fmt = wav.constData() + offset;
            quint16 formatTag = readU16(fmt);
            if (formatTag == 0xfffe && chunkSize >= 40)
                formatTag = readU16(fmt + 24); // WAVEFORMATEXTENSIBLE 的子格式。
            const quint16 channels = readU16(fmt + 2);
            const quint32 sampleRate = readU32(fmt + 4);
            const quint16 bits = readU16(fmt + 14);

            QAudioFormat::SampleFormat sampleFormat = QAudioFormat::Unknown;
            if (formatTag == 1 && bits == 8)
                sampleFormat = QAudioFormat::UInt8;
            else if (formatTag == 1 && bits == 16)
                sampleFormat = QAudioFormat::Int16;
            else if (formatTag == 1 && bits == 32)
                sampleFormat = QAudioFormat::Int32;
            else if (formatTag == 3 && bits == 32)
                sampleFormat = QAudioFormat::Float;

            if (channels > 0 && sampleRate > 0 && sampleFormat != QAudioFormat::Unknown) {
                format.setChannelCount(channels);
                format.setSampleRate(static_cast<int>(sampleRate));
                format.setSampleFormat(sampleFormat);
                formatFound = format.isValid();
            }
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            pcm = wav.mid(offset, chunkSize);
        }
        offset += chunkSize + (chunkSize & 1u);
    }

    if (!formatFound || pcm.isEmpty()) {
        setStatus(QStringLiteral("播放失败：暂不支持该 WAV 编码格式"));
        return;
    }

    const QAudioDevice &device = m_outputDevices.at(m_outputIndex);
    QAudioFormat targetFormat = device.preferredFormat();
    if (!targetFormat.isValid() || !device.isFormatSupported(targetFormat))
        targetFormat = format;
    if (targetFormat.sampleRate() != format.sampleRate()
        || targetFormat.channelCount() != format.channelCount()
        || targetFormat.sampleFormat() != format.sampleFormat()) {
        const QByteArray converted = convertPcmToFormat(pcm, format, targetFormat);
        if (converted.isEmpty()) {
            setStatus(QStringLiteral("播放失败：音频格式转换失败"));
            return;
        }
        pcm = converted;
        format = targetFormat;
    }
    if (!device.isFormatSupported(format)) {
        setStatus(QStringLiteral("播放失败：所选声卡不支持 %1 Hz、%2 声道、%3")
                  .arg(format.sampleRate()).arg(format.channelCount())
                  .arg(sampleFormatName(format.sampleFormat())));
        return;
    }

    stopAudioSink();
    m_player.stop();
    m_playbackData = pcm;

    // 5 ms 边界淡化，避免非零采样点在开始或停止时产生爆音。
    const int channels = format.channelCount();
    const int bytesPerSample = format.bytesPerSample();
    const int frameCount = m_playbackData.size() / (channels * bytesPerSample);
    const int fadeFrames = qMin(format.sampleRate() / 200, frameCount / 2);
    if (format.sampleFormat() == QAudioFormat::Int16) {
        auto *samples = reinterpret_cast<qint16 *>(m_playbackData.data());
        for (int i = 0; i < fadeFrames; ++i) {
            const double gain = static_cast<double>(i) / fadeFrames;
            for (int channel = 0; channel < channels; ++channel) {
                const int first = i * channels + channel;
                const int last = (frameCount - 1 - i) * channels + channel;
                samples[first] = static_cast<qint16>(samples[first] * gain);
                samples[last] = static_cast<qint16>(samples[last] * gain);
            }
        }
    } else if (format.sampleFormat() == QAudioFormat::Float) {
        auto *samples = reinterpret_cast<float *>(m_playbackData.data());
        for (int i = 0; i < fadeFrames; ++i) {
            const float gain = static_cast<float>(i) / fadeFrames;
            for (int channel = 0; channel < channels; ++channel) {
                const int first = i * channels + channel;
                const int last = (frameCount - 1 - i) * channels + channel;
                samples[first] *= gain;
                samples[last] *= gain;
            }
        }
    }

    m_playbackBuffer.close();
    m_playbackBuffer.setBuffer(&m_playbackData);
    if (!m_playbackBuffer.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("播放失败：无法准备音频数据"));
        return;
    }

    m_audioSink = std::make_unique<QAudioSink>(device, format);
    QAudioSink *sink = m_audioSink.get();
    // 200 ms 缓冲减少长语音播放时的欠载和周期性哒哒声。
    sink->setBufferSize(format.sampleRate() * format.channelCount()
                        * format.bytesPerSample() / 5);
    const qreal outputVolume = m_muted ? 0.0
            : (m_endpointVolumeAvailable ? 1.0 : m_volume / 100.0);
    sink->setVolume(outputVolume);
    connect(sink, &QAudioSink::stateChanged, this,
            [this, sink, path, allowRetry](QAudio::State state) {
                if (!m_audioSink || m_audioSink.get() != sink)
                    return;
                if (state == QAudio::ActiveState) {
                    setPlaying(true);
                } else if (state == QAudio::IdleState) {
                    setPlaying(false);
                    if (!m_ttsBusy)
                        setStatus(QStringLiteral("播放完成"), false);
                } else if (state == QAudio::StoppedState && sink->error() != QAudio::NoError) {
                    setPlaying(false);
                    if (sink->error() == QAudio::IOError && allowRetry) {
                        setStatus(QStringLiteral("音频流中断，正在自动恢复…"), false);
                        QTimer::singleShot(300, this, [this, sink, path]() {
                            if (m_audioSink && m_audioSink.get() == sink)
                                playFile(path, false);
                        });
                        return;
                    }
                    setStatus(QStringLiteral("播放失败：无法输出到所选声卡（错误码 %1）")
                              .arg(static_cast<int>(sink->error())));
                }
            });
    sink->start(&m_playbackBuffer);
    if (sink->error() != QAudio::NoError) {
        setPlaying(false);
        setStatus(QStringLiteral("播放失败：无法启动所选声卡（错误码 %1）")
                  .arg(static_cast<int>(sink->error())));
        return;
    }
    setPlaying(true);
}

void AudioController::stopAudioSink()
{
    if (m_audioSink)
        m_audioSink->stop();
    m_audioSink.reset();
    m_playbackBuffer.close();
    m_playbackData.clear();
}

void AudioController::startTestTone()
{
    const QString dirPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                            + QStringLiteral("/USBAudioSpeaker");
    if (!QDir().mkpath(dirPath)) {
        setStatus(QStringLiteral("无法创建测试音频目录"));
        return;
    }
    const QString path = dirPath + QStringLiteral("/uac_test_tone.wav");
    if (!createTestTone(path)) {
        setStatus(QStringLiteral("测试音生成失败"));
        return;
    }

    if (m_outputIndex >= 0 && m_outputIndex < m_outputDevices.size())
        setStatus(QStringLiteral("正在通过「%1」播放 48 kHz 测试音")
                  .arg(m_outputDevices.at(m_outputIndex).description()));
    playFile(path);
}

void AudioController::stopPlayback()
{
    if (m_ttsProcess.state() != QProcess::NotRunning) {
        m_ttsProcess.kill();
        m_ttsProcess.waitForFinished(500);
        setTtsBusy(false);
    }
    if (m_sttBusy)
        cancelSpeechToText();
    m_player.stop();
    stopAudioSink();
    setPlaying(false);
    setStatus(QStringLiteral("已停止播放"), false);
}

void AudioController::speak(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        setStatus(QStringLiteral("请先输入要合成的文字"));
        return;
    }
    if (trimmed.size() > 2000) {
        setStatus(QStringLiteral("单次合成文字不能超过 2000 字"));
        return;
    }

    stopPlayback();
    const QString dirPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                            + QStringLiteral("/USBAudioSpeaker");
    if (!QDir().mkpath(dirPath)) {
        setStatus(QStringLiteral("无法创建语音合成目录"));
        return;
    }

    m_pendingTtsText = trimmed;
    m_ttsFilePath = dirPath + QStringLiteral("/tts_%1.wav")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    const QString espeak = QStandardPaths::findExecutable(QStringLiteral("espeak-ng"));
    m_ttsUsingEspeak = !espeak.isEmpty();
    setTtsBusy(true);
    setStatus(QStringLiteral("正在离线合成语音…"));

    if (m_ttsUsingEspeak) {
        m_ttsProcess.setProgram(espeak);
        m_ttsProcess.setArguments({QStringLiteral("-v"), QStringLiteral("cmn"),
                                   QStringLiteral("-s"), QString::number(m_ttsRate),
                                   QStringLiteral("-w"), m_ttsFilePath,
                                   m_pendingTtsText});
        m_ttsProcess.start();
    } else {
        startSapiTts();
    }
}

void AudioController::startSapiTts()
{
    // 用临时脚本和命名参数传递路径及正文，避免 -Command 下 $args 为空。
    // 脚本本身只含 ASCII，中文正文由 QProcess 作为独立参数传入。
    const QString scriptPath = m_ttsFilePath + QStringLiteral(".ps1");
    static const QByteArray script(
        "param(\r\n"
        "    [Parameter(Mandatory=$true)][string]$Path,\r\n"
        "    [Parameter(Mandatory=$true)][string]$Text,\r\n"
        "    [int]$Rate = 0\r\n"
        ")\r\n"
        "$ErrorActionPreference = 'Stop'\r\n"
        "Add-Type -AssemblyName System.Speech\r\n"
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer\r\n"
        "try {\r\n"
        "    $s.Rate = $Rate\r\n"
        "    $s.Volume = 65\r\n"
        "    $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(48000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Stereo)\r\n"
        "    $s.SetOutputToWaveFile($Path, $format)\r\n"
        "    $s.Speak($Text)\r\n"
        "} finally {\r\n"
        "    $s.Dispose()\r\n"
        "}\r\n");

    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || scriptFile.write(script) != script.size()) {
        const QString errorText = scriptFile.errorString();
        scriptFile.close();
        QFile::remove(scriptPath);
        setTtsBusy(false);
        setStatus(QStringLiteral("语音合成失败：无法创建临时脚本（%1）").arg(errorText));
        return;
    }
    scriptFile.close();

    const int sapiRate = qBound(-10, qRound((m_ttsRate - 165) / 8.5), 10);
    m_ttsUsingEspeak = false;
    m_ttsProcess.setProgram(QStringLiteral("powershell.exe"));
    m_ttsProcess.setArguments({QStringLiteral("-NoProfile"),
                               QStringLiteral("-NonInteractive"),
                               QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                               QStringLiteral("-File"), scriptPath,
                               QStringLiteral("-Path"), m_ttsFilePath,
                               QStringLiteral("-Text"), m_pendingTtsText,
                               QStringLiteral("-Rate"), QString::number(sapiRate)});
    m_ttsProcess.start();
}

void AudioController::finishTts(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!m_ttsBusy)
        return;

    const bool ok = exitStatus == QProcess::NormalExit && exitCode == 0
                    && QFileInfo::exists(m_ttsFilePath)
                    && QFileInfo(m_ttsFilePath).size() > 44;
    if (ok) {
        QFile::remove(m_ttsFilePath + QStringLiteral(".ps1"));
        setTtsBusy(false);
        setStatus(QStringLiteral("语音合成完成，正在通过所选声卡播放"));
        playFile(m_ttsFilePath);
        return;
    }

    const QString errorText = QString::fromLocal8Bit(m_ttsProcess.readAllStandardError()).trimmed();
    if (m_ttsUsingEspeak) {
        addLog(QStringLiteral("eSpeak NG 合成失败，自动切换到 Windows SAPI"));
        startSapiTts();
        return;
    }

    QFile::remove(m_ttsFilePath + QStringLiteral(".ps1"));
    setTtsBusy(false);
    setStatus(QStringLiteral("语音合成失败%1").arg(
            errorText.isEmpty() ? QStringLiteral("，请检查系统语音组件")
                                : QStringLiteral("：%1").arg(errorText.left(180))));
}

void AudioController::startRecording()
{
    if (m_recording)
        return;
    if (m_inputIndex < 0 || m_inputIndex >= m_inputDevices.size()) {
        setStatus(QStringLiteral("没有可用的录音设备，请检查麦克风接线与固件开关"));
        return;
    }

    stopPlayback();
    const QAudioDevice device = m_inputDevices.at(m_inputIndex);
    QAudioFormat format;
    format.setSampleRate(16000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        format = device.preferredFormat();
        format.setSampleFormat(QAudioFormat::Int16);
        if (!device.isFormatSupported(format)) {
            setStatus(QStringLiteral("该录音设备不支持 16 bit PCM，无法保存 WAV"));
            return;
        }
    }

    const QString dirPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                            + QStringLiteral("/USBAudioSpeaker/recordings");
    if (!QDir().mkpath(dirPath)) {
        setStatus(QStringLiteral("无法创建录音保存目录"));
        return;
    }
    const QString path = dirPath + QStringLiteral("/recording_%1.wav")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));

    m_recordFile.setFileName(path);
    if (!m_recordFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(QStringLiteral("无法创建录音文件"));
        return;
    }
    writeWavHeader(m_recordFile, 0, format.sampleRate(), format.channelCount());

    m_recordFormat = format;
    m_recordDataBytes = 0;
    m_recordSeconds = 0;
    setRecordLevel(0);
    m_audioSource = std::make_unique<QAudioSource>(device, format, this);
    m_recordIo = m_audioSource->start();
    if (!m_recordIo) {
        m_recordFile.close();
        m_audioSource.reset();
        setStatus(QStringLiteral("录音设备启动失败"));
        return;
    }

    connect(m_recordIo, &QIODevice::readyRead, this, [this]() {
        if (!m_recordIo || !m_recordFile.isOpen())
            return;
        const QByteArray data = m_recordIo->readAll();
        if (data.isEmpty())
            return;
        m_recordFile.write(data);
        m_recordDataBytes += static_cast<quint32>(data.size());

        const qint16 *samples = reinterpret_cast<const qint16 *>(data.constData());
        const int count = data.size() / static_cast<int>(sizeof(qint16));
        qint32 peak = 0;
        for (int i = 0; i < count; ++i)
            peak = std::max(peak, static_cast<qint32>(std::abs(static_cast<qint32>(samples[i]))));
        setRecordLevel(qBound(0, qRound(peak * 100.0 / 32768.0), 100));
    });

    connect(m_audioSource.get(), &QAudioSource::stateChanged, this,
            [this](QAudio::State state) {
                if (state == QAudio::StoppedState && m_recording
                    && m_audioSource && m_audioSource->error() != QAudio::NoError) {
                    addLog(QStringLiteral("录音设备异常停止，错误码 %1")
                           .arg(static_cast<int>(m_audioSource->error())));
                    stopRecording();
                }
            });

    m_recordElapsed.start();
    m_recordTimer.start();
    m_lastRecordingPath = path;
    m_recognizedText.clear();
    emit recognizedTextChanged();
    emit lastRecordingPathChanged();
    emit recordSecondsChanged();
    setRecording(true);
    m_inputFormat = QStringLiteral("%1 Hz · %2 声道 · 16 bit")
            .arg(format.sampleRate()).arg(format.channelCount());
    emit inputIndexChanged();
    setStatus(QStringLiteral("正在录音，最长 120 秒"));
}

void AudioController::updateRecording()
{
    if (!m_recording)
        return;
    const int seconds = static_cast<int>(m_recordElapsed.elapsed() / 1000);
    if (seconds != m_recordSeconds) {
        m_recordSeconds = seconds;
        emit recordSecondsChanged();
    }
    if (seconds >= kMaxRecordSeconds) {
        setStatus(QStringLiteral("已达到 120 秒上限，录音自动保存"));
        stopRecording();
    }
}

void AudioController::stopRecording()
{
    if (!m_recording)
        return;

    m_recordTimer.stop();
    setRecording(false);
    if (m_recordIo)
        disconnect(m_recordIo, nullptr, this, nullptr);
    if (m_audioSource)
        m_audioSource->stop();
    m_recordIo = nullptr;
    m_audioSource.reset();

    if (m_recordFile.isOpen()) {
        writeWavHeader(m_recordFile, m_recordDataBytes,
                       m_recordFormat.sampleRate(), m_recordFormat.channelCount());
        m_recordFile.close();
    }
    // 录音流完全释放后重新绑定输出端点，避免 GUI 会话保留旧音频状态。
    refreshOutputDevices();
    setRecordLevel(0);
    setStatus(QStringLiteral("录音已保存：%1").arg(m_lastRecordingPath));
}

void AudioController::importRecording(const QUrl &fileUrl)
{
    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty()) {
        setStatus(QStringLiteral("导入失败：请选择本机 WAV 录音文件"));
        return;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.size() <= 44) {
        setStatus(QStringLiteral("导入失败：请选择有效的 WAV 录音文件"));
        return;
    }

    stopPlayback();
    m_lastRecordingPath = info.absoluteFilePath();
    m_recordSeconds = 0;
    m_recognizedText.clear();
    emit lastRecordingPathChanged();
    emit recordSecondsChanged();
    emit recognizedTextChanged();
    setStatus(QStringLiteral("已导入录音：%1，可播放或转为文字").arg(info.fileName()));
}

void AudioController::playLastRecording(bool autoTranscribe)
{
    if (m_recording)
        stopRecording();
    if (m_lastRecordingPath.isEmpty() || !QFileInfo::exists(m_lastRecordingPath)
        || QFileInfo(m_lastRecordingPath).size() <= 44) {
        setStatus(QStringLiteral("暂无可播放的有效录音，请录音或导入文件"));
        return;
    }
    setStatus(QStringLiteral("正在播放选中录音"));
    playFile(m_lastRecordingPath);
    if (autoTranscribe)
        startTranscription(true);
}

void AudioController::transcribeLastRecording()
{
    startTranscription(false);
}

void AudioController::startTranscription(bool keepPlayback)
{
    if (m_sttBusy)
        return;
    if (m_recording)
        stopRecording();
    if (m_lastRecordingPath.isEmpty() || !QFileInfo::exists(m_lastRecordingPath)
        || QFileInfo(m_lastRecordingPath).size() <= 44) {
        setStatus(QStringLiteral("请先录音或导入有效 WAV 文件"));
        return;
    }

    // 手动识别会停止已有播放；播放时自动识别则保留当前音频流。
    if (!keepPlayback) {
        m_player.stop();
        stopAudioSink();
        setPlaying(false);
    }

    // 优先使用随程序发布的 whisper.cpp，识别全程在本机完成。
    const QString sttDirPath = QCoreApplication::applicationDirPath() + QStringLiteral("/stt");
    const QString whisperProgram = QDir(sttDirPath).filePath(QStringLiteral("whisper-cli.exe"));
    const QString whisperModel = QDir(sttDirPath).filePath(QStringLiteral("ggml-base.bin"));
    m_sttScriptPath.clear();
    m_sttUsingWhisper = QFileInfo::exists(whisperProgram) && QFileInfo::exists(whisperModel);

    m_recognizedText.clear();
    emit recognizedTextChanged();
    setSttBusy(true);
    if (m_sttUsingWhisper) {
        setStatus(QStringLiteral("正在使用本地 Whisper 识别最近录音…"));
        m_sttProcess.setWorkingDirectory(sttDirPath);
        m_sttProcess.setProgram(whisperProgram);
        // 使用相对模型路径，避免 whisper.cpp 在中文安装目录下打开模型失败。
        m_sttProcess.setArguments({QStringLiteral("-m"), QStringLiteral("ggml-base.bin"),
                                   QStringLiteral("-f"), m_lastRecordingPath,
                                   QStringLiteral("-l"), QStringLiteral("zh"),
                                   QStringLiteral("-nt"), QStringLiteral("-np")});
        m_sttTimeout.start(120000);
        m_sttProcess.start();
        return;
    }

    // 模型文件缺失时保留系统 SAPI 作为备用识别方式。
    m_sttProcess.setWorkingDirectory(QString());
    m_sttScriptPath = m_lastRecordingPath + QStringLiteral(".stt.ps1");
    static const QByteArray script(
        "param(\r\n"
        "    [Parameter(Mandatory=$true)][string]$Path\r\n"
        ")\r\n"
        "$ErrorActionPreference = 'Stop'\r\n"
        "Add-Type -AssemblyName System.Speech\r\n"
        "$culture = [System.Globalization.CultureInfo]::GetCultureInfo('zh-CN')\r\n"
        "$r = New-Object System.Speech.Recognition.SpeechRecognitionEngine -ArgumentList $culture\r\n"
        "try {\r\n"
        "    $r.SetInputToWaveFile($Path)\r\n"
        "    $grammar = New-Object System.Speech.Recognition.DictationGrammar\r\n"
        "    $r.LoadGrammar($grammar)\r\n"
        "    $result = $r.Recognize()\r\n"
        "    if ($null -eq $result -or [string]::IsNullOrWhiteSpace($result.Text)) {\r\n"
        "        exit 3\r\n"
        "    }\r\n"
        "    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8\r\n"
        "    [Console]::Out.Write($result.Text)\r\n"
        "} finally {\r\n"
        "    $r.Dispose()\r\n"
        "}\r\n");

    QFile scriptFile(m_sttScriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || scriptFile.write(script) != script.size()) {
        const QString errorText = scriptFile.errorString();
        scriptFile.close();
        QFile::remove(m_sttScriptPath);
        setStatus(QStringLiteral("语音识别失败：无法创建临时脚本（%1）").arg(errorText));
        return;
    }
    scriptFile.close();

    m_recognizedText.clear();
    emit recognizedTextChanged();
    setSttBusy(true);
    setStatus(QStringLiteral("正在离线识别最近录音…"));
    m_sttProcess.setProgram(QStringLiteral("powershell.exe"));
    m_sttProcess.setArguments({QStringLiteral("-NoProfile"),
                               QStringLiteral("-NonInteractive"),
                               QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                               QStringLiteral("-File"), m_sttScriptPath,
                               QStringLiteral("-Path"), m_lastRecordingPath});
    m_sttTimeout.start(30000);
    m_sttProcess.start();
}

void AudioController::cancelSpeechToText()
{
    if (!m_sttBusy && m_sttProcess.state() == QProcess::NotRunning)
        return;
    m_sttTimeout.stop();
    setSttBusy(false); // 防止 kill 触发 finished 时重复进入完成回调。
    if (m_sttProcess.state() != QProcess::NotRunning) {
        m_sttProcess.kill();
        m_sttProcess.waitForFinished(500);
    }
    QFile::remove(m_sttScriptPath);
    setStatus(QStringLiteral("已取消语音识别，可继续录音或播放"), false);
}

void AudioController::finishStt(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!m_sttBusy)
        return;

    m_sttTimeout.stop();
    QString text = QString::fromUtf8(m_sttProcess.readAllStandardOutput()).trimmed();
    if (text.startsWith(QChar(0xfeff)))
        text.remove(0, 1);
    const QByteArray errorBytes = m_sttProcess.readAllStandardError();
    const QString errorText = (m_sttUsingWhisper
            ? QString::fromUtf8(errorBytes)
            : QString::fromLocal8Bit(errorBytes)).trimmed();
    QFile::remove(m_sttScriptPath);
    setSttBusy(false);

    if (exitStatus == QProcess::NormalExit && exitCode == 0 && !text.isEmpty()) {
        m_recognizedText = text;
        emit recognizedTextChanged();
        setStatus(QStringLiteral("语音识别完成"));
        return;
    }
    if (exitCode == 3 || (m_sttUsingWhisper && exitStatus == QProcess::NormalExit
                          && exitCode == 0 && text.isEmpty())) {
        setStatus(QStringLiteral("没有识别到语音，请重新录音后再试"));
        return;
    }
    setStatus(QStringLiteral("语音识别失败%1").arg(
            errorText.isEmpty() ? QStringLiteral("，请重新录音后再试")
                                : QStringLiteral("：%1").arg(errorText.left(180))));
}

void AudioController::openRecordingFolder()
{
    QString path = m_lastRecordingPath;
    if (path.isEmpty()) {
        path = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
               + QStringLiteral("/USBAudioSpeaker/recordings");
        QDir().mkpath(path);
    } else {
        path = QFileInfo(path).absolutePath();
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void AudioController::openSystemSoundSettings()
{
#ifdef Q_OS_WIN
    if (!QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:sound"))))
        QProcess::startDetached(QStringLiteral("control.exe"), {QStringLiteral("mmsys.cpl")});
#else
    setStatus(QStringLiteral("请在操作系统声音设置中检查输入输出设备"));
#endif
}

void AudioController::clearLog()
{
    m_activityLog.clear();
    emit activityLogChanged();
}

void AudioController::setPlaying(bool playing)
{
    if (m_playing == playing)
        return;
    m_playing = playing;
    emit playingChanged();
}

void AudioController::setRecording(bool recording)
{
    if (m_recording == recording)
        return;
    m_recording = recording;
    emit recordingChanged();
}

void AudioController::setTtsBusy(bool busy)
{
    if (m_ttsBusy == busy)
        return;
    m_ttsBusy = busy;
    emit ttsBusyChanged();
}

void AudioController::setSttBusy(bool busy)
{
    if (m_sttBusy == busy)
        return;
    m_sttBusy = busy;
    emit sttBusyChanged();
}

void AudioController::setRecordLevel(int level)
{
    level = qBound(0, level, 100);
    if (m_recordLevel == level)
        return;
    m_recordLevel = level;
    emit recordLevelChanged();
}

void AudioController::setStatus(const QString &message, bool log)
{
    if (m_statusMessage != message) {
        m_statusMessage = message;
        emit statusMessageChanged();
    }
    if (log)
        addLog(message);
}

void AudioController::addLog(const QString &message)
{
    m_activityLog.prepend(QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), message));
    while (m_activityLog.size() > 40)
        m_activityLog.removeLast();
    emit activityLogChanged();
}
