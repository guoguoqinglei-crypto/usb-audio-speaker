#ifndef AUDIOCONTROLLER_H
#define AUDIOCONTROLLER_H

#include <QAudioDevice>
#include <QAudioOutput>
#include <QAudioSink>
#include <QAudioSource>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include "hidstatus.h"

#include "hidstatus.h"

#include <memory>

/**
 * @brief 声卡上位机核心控制器。
 *
 * 通过 Qt Multimedia 访问系统标准 UAC 设备，不依赖厂商私有驱动。
 * 播放和文字转语音共用 QMediaPlayer；录音使用 QAudioSource 并写成标准 PCM WAV。
 * Windows 下音量直接写入 WASAPI Endpoint Volume，属于系统软件音量。
 * 板载 A/B 键直接改变 NAU8822 增益，并通过独立厂商 HID 接口回传状态。
 */
class AudioController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QStringList outputDevices READ outputDevices NOTIFY outputDevicesChanged)
    Q_PROPERTY(QStringList inputDevices READ inputDevices NOTIFY inputDevicesChanged)
    Q_PROPERTY(int outputIndex READ outputIndex WRITE setOutputIndex NOTIFY outputIndexChanged)
    Q_PROPERTY(int inputIndex READ inputIndex WRITE setInputIndex NOTIFY inputIndexChanged)
    Q_PROPERTY(bool hasUsbSpeaker READ hasUsbSpeaker NOTIFY deviceStatusChanged)
    Q_PROPERTY(bool hasUsbMicrophone READ hasUsbMicrophone NOTIFY deviceStatusChanged)
    Q_PROPERTY(QString outputFormat READ outputFormat NOTIFY outputIndexChanged)
    Q_PROPERTY(QString inputFormat READ inputFormat NOTIFY inputIndexChanged)

    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(int boardVolume READ boardVolume NOTIFY boardStatusChanged)
    Q_PROPERTY(bool boardKeyA READ boardKeyA NOTIFY boardStatusChanged)
    Q_PROPERTY(bool boardKeyB READ boardKeyB NOTIFY boardStatusChanged)
    Q_PROPERTY(bool boardHidConnected READ boardHidConnected NOTIFY boardStatusChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(bool ttsBusy READ ttsBusy NOTIFY ttsBusyChanged)
    Q_PROPERTY(int ttsRate READ ttsRate WRITE setTtsRate NOTIFY ttsRateChanged)
    Q_PROPERTY(QString ttsEngine READ ttsEngine NOTIFY ttsEngineChanged)
    Q_PROPERTY(bool sttBusy READ sttBusy NOTIFY sttBusyChanged)
    Q_PROPERTY(QString recognizedText READ recognizedText NOTIFY recognizedTextChanged)

    Q_PROPERTY(int recordSeconds READ recordSeconds NOTIFY recordSecondsChanged)
    Q_PROPERTY(int recordLevel READ recordLevel NOTIFY recordLevelChanged)
    Q_PROPERTY(QString lastRecordingPath READ lastRecordingPath NOTIFY lastRecordingPathChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QStringList activityLog READ activityLog NOTIFY activityLogChanged)

public:
    explicit AudioController(QObject *parent = nullptr);
    ~AudioController() override;

    QStringList outputDevices() const { return m_outputDeviceNames; }
    QStringList inputDevices() const { return m_inputDeviceNames; }
    int outputIndex() const { return m_outputIndex; }
    int inputIndex() const { return m_inputIndex; }
    bool hasUsbSpeaker() const { return m_hasUsbSpeaker; }
    bool hasUsbMicrophone() const { return m_hasUsbMicrophone; }
    QString outputFormat() const { return m_outputFormat; }
    QString inputFormat() const { return m_inputFormat; }
    int volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    int boardVolume() const { return m_boardVolume; }
    bool boardKeyA() const { return m_boardKeyA; }
    bool boardKeyB() const { return m_boardKeyB; }
    bool boardHidConnected() const { return m_boardHidConnected; }
    bool playing() const { return m_playing; }
    bool recording() const { return m_recording; }
    bool ttsBusy() const { return m_ttsBusy; }
    int ttsRate() const { return m_ttsRate; }
    QString ttsEngine() const { return m_ttsEngine; }
    bool sttBusy() const { return m_sttBusy; }
    QString recognizedText() const { return m_recognizedText; }
    int recordSeconds() const { return m_recordSeconds; }
    int recordLevel() const { return m_recordLevel; }
    QString lastRecordingPath() const { return m_lastRecordingPath; }
    QString statusMessage() const { return m_statusMessage; }
    QStringList activityLog() const { return m_activityLog; }

    void setOutputIndex(int index);
    void setInputIndex(int index);
    void setVolume(int volume);
    void setMuted(bool muted);
    void setTtsRate(int rate);

    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void startTestTone();
    Q_INVOKABLE void stopPlayback();
    Q_INVOKABLE void speak(const QString &text);
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void playLastRecording(bool autoTranscribe);
    Q_INVOKABLE void importRecording(const QUrl &fileUrl);
    Q_INVOKABLE void transcribeLastRecording();
    Q_INVOKABLE void cancelSpeechToText();
    Q_INVOKABLE void openRecordingFolder();
    Q_INVOKABLE void openSystemSoundSettings();
    Q_INVOKABLE void clearLog();

signals:
    void outputDevicesChanged();
    void inputDevicesChanged();
    void outputIndexChanged();
    void inputIndexChanged();
    void deviceStatusChanged();
    void volumeChanged();
    void mutedChanged();
    void boardStatusChanged();
    void playingChanged();
    void recordingChanged();
    void ttsBusyChanged();
    void ttsRateChanged();
    void ttsEngineChanged();
    void sttBusyChanged();
    void recognizedTextChanged();
    void recordSecondsChanged();
    void recordLevelChanged();
    void lastRecordingPathChanged();
    void statusMessageChanged();
    void activityLogChanged();

private:
    void refreshOutputDevices();
    void refreshInputDevices();
    void syncEndpointVolume();
    int preferredDeviceIndex(const QList<QAudioDevice> &devices) const;
    static bool isUsbAudioDevice(const QAudioDevice &device);
    static QString formatDescription(const QAudioDevice &device);

    void playFile(const QString &path, bool allowRetry = true);
    void stopAudioSink();
    bool createTestTone(const QString &path);
    static bool writeWavHeader(QFile &file, quint32 dataBytes, int sampleRate, int channels);

    void updateRecording();
    void finishTts(int exitCode, QProcess::ExitStatus exitStatus);
    void startSapiTts();
    void startTranscription(bool keepPlayback);
    void finishStt(int exitCode, QProcess::ExitStatus exitStatus);
    void setSttBusy(bool busy);
    void setPlaying(bool playing);
    void setRecording(bool recording);
    void setTtsBusy(bool busy);
    void setRecordLevel(int level);
    void setStatus(const QString &message, bool log = true);
    void addLog(const QString &message);

    QMediaDevices m_mediaDevices;
    QList<QAudioDevice> m_outputDevices;
    QList<QAudioDevice> m_inputDevices;
    QStringList m_outputDeviceNames;
    QStringList m_inputDeviceNames;
    int m_outputIndex = -1;
    int m_inputIndex = -1;
    bool m_hasUsbSpeaker = false;
    bool m_hasUsbMicrophone = false;
    QString m_outputFormat = QStringLiteral("未选择播放设备");
    QString m_inputFormat = QStringLiteral("未选择录音设备");

    QMediaPlayer m_player;
    QAudioOutput m_audioOutput;
    std::unique_ptr<QAudioSink> m_audioSink;
    QByteArray m_playbackData;
    QBuffer m_playbackBuffer;
    bool m_playing = false;
    int m_volume = 80;
    bool m_muted = false;
    bool m_endpointVolumeAvailable = false;

    HidStatusMonitor m_hidStatus;
    int m_boardVolume = 0;
    bool m_boardKeyA = false;
    bool m_boardKeyB = false;
    bool m_boardHidConnected = false;

    QProcess m_ttsProcess;
    bool m_ttsBusy = false;
    bool m_ttsUsingEspeak = false;
    int m_ttsRate = 165;
    QString m_ttsEngine;
    QString m_pendingTtsText;
    QString m_ttsFilePath;

    QProcess m_sttProcess;
    QTimer m_sttTimeout;
    bool m_sttBusy = false;
    bool m_sttUsingWhisper = false;
    QString m_recognizedText;
    QString m_sttScriptPath;

    std::unique_ptr<QAudioSource> m_audioSource;
    QIODevice *m_recordIo = nullptr;
    QFile m_recordFile;
    QTimer m_recordTimer;
    QElapsedTimer m_recordElapsed;
    quint32 m_recordDataBytes = 0;
    QAudioFormat m_recordFormat;
    bool m_recording = false;
    int m_recordSeconds = 0;
    int m_recordLevel = 0;
    QString m_lastRecordingPath;

    QString m_statusMessage = QStringLiteral("正在扫描声卡设备…");
    QStringList m_activityLog;
};

#endif // AUDIOCONTROLLER_H
