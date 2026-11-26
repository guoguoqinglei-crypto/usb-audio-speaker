import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: 1180
    height: 800
    minimumWidth: 940
    minimumHeight: 680
    visible: true
    title: qsTr("USB Audio Speaker · 声卡控制台")
    color: "#0a1020"

    property color panel: "#141d31"
    property color panelSoft: "#1a2540"
    property color line: "#2b3955"
    property color textMain: "#f2f6ff"
    property color textMuted: "#91a0bc"
    property color accent: "#64d8cb"
    property color accentBlue: "#6aa8ff"
    property color danger: "#ff7d8a"

    function formatSeconds(total) {
        const minutes = Math.floor(total / 60).toString().padStart(2, "0")
        const seconds = (total % 60).toString().padStart(2, "0")
        return minutes + ":" + seconds
    }

    FileDialog {
        id: recordingFileDialog
        title: qsTr("选择要播放或识别的录音")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("WAV 音频文件 (*.wav)")]
        onAccepted: audio.importRecording(selectedFile)
    }

    component Card: Rectangle {
        color: window.panel
        radius: 16
        border.color: window.line
        border.width: 1
    }

    component CardTitle: RowLayout {
        property string index: "01"
        property string title: ""
        spacing: 10
        Label {
            text: parent.index
            color: window.accent
            font.pixelSize: 12
            font.bold: true
            background: Rectangle {
                color: "#173d43"
                radius: 6
            }
            leftPadding: 8
            rightPadding: 8
            topPadding: 4
            bottomPadding: 4
        }
        Label {
            text: parent.title
            color: window.textMain
            font.pixelSize: 18
            font.bold: true
            Layout.fillWidth: true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 18

        RowLayout {
            Layout.fillWidth: true
            spacing: 14

            Rectangle {
                width: 48
                height: 48
                radius: 14
                color: window.accent
                Label {
                    anchors.centerIn: parent
                    text: "UAC"
                    color: "#09201f"
                    font.bold: true
                    font.pixelSize: 15
                }
            }
            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true
                Label {
                    text: qsTr("USB Audio Speaker")
                    color: window.textMain
                    font.pixelSize: 25
                    font.bold: true
                }
                Label {
                    text: qsTr("STM32F103 + NAU8822 · 播放 / 录音 / 离线语音")
                    color: window.textMuted
                    font.pixelSize: 13
                }
            }
            Rectangle {
                Layout.preferredWidth: statusText.implicitWidth + 42
                Layout.preferredHeight: 34
                radius: 17
                color: audio.hasUsbSpeaker ? "#143b3a" : "#302a35"
                border.color: audio.hasUsbSpeaker ? "#2c6963" : "#57404f"
                Row {
                    anchors.centerIn: parent
                    spacing: 8
                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        anchors.verticalCenter: parent.verticalCenter
                        color: audio.hasUsbSpeaker ? window.accent : window.danger
                    }
                    Label {
                        id: statusText
                        text: audio.hasUsbSpeaker ? qsTr("USB 声卡已连接") : qsTr("等待 USB 声卡")
                        color: audio.hasUsbSpeaker ? window.accent : "#f0b2bb"
                        font.pixelSize: 12
                    }
                }
            }
        }

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: scroll.availableWidth
                spacing: 18

                GridLayout {
                    Layout.fillWidth: true
                    columns: window.width < 1060 ? 1 : 2
                    columnSpacing: 18
                    rowSpacing: 18

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: 18

                        Card {
                            Layout.fillWidth: true
                            implicitHeight: deviceColumn.implicitHeight + 36
                            ColumnLayout {
                                id: deviceColumn
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 13

                                CardTitle {
                                    index: "01"
                                    title: qsTr("设备连接")
                                    Button {
                                        text: qsTr("刷新设备")
                                        onClicked: audio.refreshDevices()
                                    }
                                }

                                Label { text: qsTr("播放设备 / 耳机输出"); color: window.textMuted }
                                ComboBox {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 42
                                    model: audio.outputDevices
                                    currentIndex: audio.outputIndex
                                    onActivated: audio.outputIndex = currentIndex
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: audio.outputFormat
                                    color: window.textMuted
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }

                                Rectangle { Layout.fillWidth: true; height: 1; color: window.line }

                                Label { text: qsTr("录音设备 / 麦克风输入"); color: window.textMuted }
                                ComboBox {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 42
                                    model: audio.inputDevices
                                    currentIndex: audio.inputIndex
                                    onActivated: audio.inputIndex = currentIndex
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: audio.inputFormat
                                    color: window.textMuted
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Button {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 40
                                        text: audio.playing ? qsTr("当前有音频正在播放") : qsTr("播放 48 kHz 测试音")
                                        enabled: audio.outputIndex >= 0 && !audio.sttBusy
                                        onClicked: audio.startTestTone()
                                    }
                                    Button {
                                        text: qsTr("系统声音设置")
                                        onClicked: audio.openSystemSoundSettings()
                                    }
                                }
                            }
                        }

                        Card {
                            Layout.fillWidth: true
                            implicitHeight: ttsColumn.implicitHeight + 36
                            ColumnLayout {
                                id: ttsColumn
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 13

                                CardTitle { index: "03"; title: qsTr("文字转语音") }
                                Label {
                                    text: qsTr("离线生成 WAV，并从上方选定的 USB 声卡播放")
                                    color: window.textMuted
                                    font.pixelSize: 12
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 130
                                    color: window.panelSoft
                                    radius: 10
                                    border.color: ttsInput.activeFocus ? window.accent : window.line
                                    TextArea {
                                        id: ttsInput
                                        anchors.fill: parent
                                        anchors.margins: 8
                                        text: qsTr("你好，这是一台基于 STM32 和 NAU8822 的 USB 声卡。文字转语音测试成功。")
                                        placeholderText: qsTr("输入要朗读的中文或英文…")
                                        wrapMode: TextArea.Wrap
                                        color: window.textMain
                                        placeholderTextColor: window.textMuted
                                        background: null
                                        font.pixelSize: 15
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: qsTr("语速"); color: window.textMuted }
                                    Slider {
                                        Layout.fillWidth: true
                                        from: 80
                                        to: 250
                                        stepSize: 5
                                        value: audio.ttsRate
                                        onMoved: audio.ttsRate = value
                                    }
                                    Label {
                                        text: Math.round(audio.ttsRate) + " 字/分"
                                        color: window.textMain
                                        Layout.preferredWidth: 66
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("引擎：") + audio.ttsEngine
                                    color: window.accentBlue
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Button {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 42
                                        text: audio.ttsBusy ? qsTr("正在合成…") : qsTr("合成并通过声卡朗读")
                                        enabled: !audio.ttsBusy && audio.outputIndex >= 0
                                        onClicked: audio.speak(ttsInput.text)
                                    }
                                    BusyIndicator {
                                        running: audio.ttsBusy
                                        visible: running
                                        Layout.preferredWidth: 34
                                        Layout.preferredHeight: 34
                                    }
                                    Button {
                                        text: qsTr("停止")
                                        enabled: audio.playing || audio.ttsBusy || audio.sttBusy
                                        onClicked: audio.stopPlayback()
                                    }
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: 18

                        Card {
                            Layout.fillWidth: true
                            implicitHeight: volumeColumn.implicitHeight + 36
                            ColumnLayout {
                                id: volumeColumn
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 12
                                CardTitle { index: "02"; title: qsTr("音量与板载 A/B 键") }
                                RowLayout {
                                    Label { text: qsTr("Windows 软件音量"); color: window.textMuted; Layout.fillWidth: true }
                                    Label {
                                        text: audio.muted ? qsTr("已静音") : audio.volume + "%"
                                        color: audio.muted ? window.danger : window.accent
                                        font.pixelSize: 22
                                        font.bold: true
                                    }
                                }
                                Slider {
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 100
                                    stepSize: 1
                                    value: audio.volume
                                    onMoved: audio.volume = value
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    CheckBox {
                                        text: qsTr("静音")
                                        checked: audio.muted
                                        onToggled: audio.muted = checked
                                    }
                                    Item { Layout.fillWidth: true }
                                    Label {
                                        text: qsTr("系统端点音量，与板载 Codec 音量独立")
                                        color: window.textMuted
                                        font.pixelSize: 11
                                    }
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: window.line }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: qsTr("板载 Codec 音量")
                                        color: window.textMuted
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        text: audio.boardHidConnected ? audio.boardVolume + " / 100" : qsTr("等待状态接口")
                                        color: audio.boardHidConnected ? window.accentBlue : window.textMuted
                                        font.bold: true
                                    }
                                }
                                ProgressBar {
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 100
                                    value: audio.boardHidConnected ? audio.boardVolume : 0
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 10
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 44
                                        radius: 8
                                        color: audio.boardKeyA ? "#245f59" : window.panelSoft
                                        border.color: audio.boardKeyA ? window.accent : window.line
                                        Label {
                                            anchors.centerIn: parent
                                            text: qsTr("A · 加音量 +1")
                                            color: audio.boardKeyA ? window.accent : window.textMain
                                            font.bold: audio.boardKeyA
                                        }
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 44
                                        radius: 8
                                        color: audio.boardKeyB ? "#294d70" : window.panelSoft
                                        border.color: audio.boardKeyB ? window.accentBlue : window.line
                                        Label {
                                            anchors.centerIn: parent
                                            text: qsTr("B · 减音量 -1")
                                            color: audio.boardKeyB ? window.accentBlue : window.textMain
                                            font.bold: audio.boardKeyB
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("每次有效按下只变化 1，长按不连发，范围 0～100")
                                    color: window.textMuted
                                    font.pixelSize: 11
                                    wrapMode: Text.Wrap
                                }
                            }
                        }

                        Card {
                            Layout.fillWidth: true
                            implicitHeight: recordColumn.implicitHeight + 36
                            ColumnLayout {
                                id: recordColumn
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 12
                                CardTitle {
                                    index: "04"
                                    title: qsTr("麦克风录音")
                                    Label {
                                        text: window.formatSeconds(audio.recordSeconds)
                                        color: audio.recording ? window.danger : window.textMain
                                        font.pixelSize: 20
                                        font.bold: true
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Rectangle {
                                        width: 10
                                        height: 10
                                        radius: 5
                                        color: audio.recording ? window.danger : window.textMuted
                                        SequentialAnimation on opacity {
                                            running: audio.recording
                                            loops: Animation.Infinite
                                            NumberAnimation { to: 0.25; duration: 600 }
                                            NumberAnimation { to: 1.0; duration: 600 }
                                        }
                                    }
                                    Label {
                                        text: audio.recording ? qsTr("正在采集 PCM 数据") : qsTr("就绪 · 自动保存为 WAV")
                                        color: window.textMuted
                                        Layout.fillWidth: true
                                    }
                                }
                                ProgressBar {
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 100
                                    value: audio.recordLevel
                                }
                                Label { text: qsTr("输入电平"); color: window.textMuted; font.pixelSize: 11 }
                                Button {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 44
                                    text: audio.recording ? qsTr("停止并保存录音") : qsTr("开始录音")
                                    enabled: audio.inputIndex >= 0 && !audio.ttsBusy && !audio.sttBusy
                                    onClicked: audio.recording ? audio.stopRecording() : audio.startRecording()
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: audio.lastRecordingPath.length > 0
                                    text: audio.lastRecordingPath
                                    color: window.textMuted
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Button {
                                        Layout.fillWidth: true
                                        text: qsTr("播放选中录音")
                                        enabled: audio.lastRecordingPath.length > 0 && audio.outputIndex >= 0
                                        onClicked: audio.playLastRecording(autoTranscribe.checked)
                                    }
                                    Button {
                                        Layout.fillWidth: true
                                        text: qsTr("导入录音文件")
                                        enabled: !audio.recording && !audio.ttsBusy && !audio.sttBusy
                                        onClicked: recordingFileDialog.open()
                                    }
                                }
                                CheckBox {
                                    id: autoTranscribe
                                    checked: true
                                    text: qsTr("播放时自动转文字（本地识别，结果自动显示）")
                                    palette.text: window.textMain
                                }
                                Button {
                                    Layout.fillWidth: true
                                    text: qsTr("打开录音目录（仅管理文件）")
                                    onClicked: audio.openRecordingFolder()
                                }
                                Button {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 40
                                    text: audio.sttBusy ? qsTr("取消语音识别") : qsTr("语音转文字（识别当前录音）")
                                    enabled: audio.sttBusy || (audio.lastRecordingPath.length > 0 && !audio.recording)
                                    onClicked: audio.sttBusy ? audio.cancelSpeechToText() : audio.transcribeLastRecording()
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("本地 Whisper 中文识别，录音不会上传")
                                    color: window.textMuted
                                    font.pixelSize: 11
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 100
                                    visible: audio.lastRecordingPath.length > 0
                                    color: window.panelSoft
                                    radius: 10
                                    border.color: sttOutput.activeFocus ? window.accent : window.line
                                    TextArea {
                                        id: sttOutput
                                        anchors.fill: parent
                                        anchors.margins: 8
                                        text: audio.recognizedText
                                        placeholderText: audio.sttBusy ? qsTr("正在识别，请稍候…") : qsTr("识别结果会显示在这里，可选择文字复制")
                                        wrapMode: TextArea.Wrap
                                        readOnly: true
                                        color: window.textMain
                                        placeholderTextColor: window.textMuted
                                        background: null
                                        font.pixelSize: 14
                                    }
                                }
                                Button {
                                    Layout.fillWidth: true
                                    visible: audio.recognizedText.length > 0
                                    text: qsTr("复制识别结果")
                                    onClicked: {
                                        sttOutput.selectAll()
                                        sttOutput.copy()
                                        sttOutput.deselect()
                                    }
                                }
                            }
                        }

                        Card {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            implicitHeight: 240
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 10
                                CardTitle {
                                    index: "05"
                                    title: qsTr("运行记录")
                                    Button { text: qsTr("清空"); onClicked: audio.clearLog() }
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: "#0e1628"
                                    radius: 10
                                    ListView {
                                        anchors.fill: parent
                                        anchors.margins: 10
                                        clip: true
                                        model: audio.activityLog
                                        spacing: 7
                                        delegate: Label {
                                            width: ListView.view.width
                                            text: modelData
                                            color: window.textMuted
                                            font.pixelSize: 11
                                            wrapMode: Text.WrapAnywhere
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    radius: 12
                    color: "#101a2d"
                    border.color: window.line
                    Label {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        text: audio.statusMessage
                        color: window.accent
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("标准 UAC 1.0 设备 · 无需厂商驱动 · 录音需启用 ENABLE_USB_MIC 并连接 ADCOUT / BCLK / LRCLK")
            color: window.textMuted
            font.pixelSize: 11
            horizontalAlignment: Text.AlignHCenter
        }
    }
}
