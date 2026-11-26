#ifndef HIDSTATUS_H
#define HIDSTATUS_H

#include <QByteArray>
#include <QThread>

/* 后台只读监测声卡的厂商 HID 状态接口。
 * Windows 重叠读不会阻塞 GUI；设备拔出后自动重新枚举。 */
class HidStatusMonitor : public QThread
{
    Q_OBJECT
public:
    explicit HidStatusMonitor(QObject *parent = nullptr);
    ~HidStatusMonitor() override;

signals:
    void reportReceived(const QByteArray &report);
    void connectionChanged(bool connected);

protected:
    void run() override;
};

#endif // HIDSTATUS_H
