#include "hidstatus.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <setupapi.h>
/* MinGW 的 HID 头文件没有 C++ 链接保护，必须显式使用 C 链接。 */
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <QString>
#include <vector>
#endif

namespace {
#ifdef Q_OS_WIN
struct HidPathInfo {
    QString path;
    DWORD inputLength = 0;
};

HidPathInfo findStatusInterface()
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devices = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                             DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE)
        return {};

    HidPathInfo result;
    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);
    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid, index, &interfaceData);
         ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0,
                                         &required, nullptr);
        if (required == 0)
            continue;
        std::vector<unsigned char> storage(required);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail,
                                               required, nullptr, nullptr))
            continue;

        HANDLE handle = CreateFileW(detail->DevicePath,
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            continue;

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps{};
        const bool identity = HidD_GetAttributes(handle, &attributes)
                              && attributes.VendorID == 0xCAFE
                              && attributes.ProductID == 0x4002;
        const bool collection = identity && HidD_GetPreparsedData(handle, &preparsed)
                                && HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS
                                && caps.UsagePage == 0xFF00 && caps.Usage == 1;
        if (collection) {
            result.path = QString::fromWCharArray(detail->DevicePath);
            result.inputLength = caps.InputReportByteLength;
        }
        if (preparsed)
            HidD_FreePreparsedData(preparsed);
        CloseHandle(handle);
        if (collection)
            break;
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}
#endif
} // namespace

HidStatusMonitor::HidStatusMonitor(QObject *parent) : QThread(parent) {}

HidStatusMonitor::~HidStatusMonitor()
{
    requestInterruption();
    if (isRunning())
        wait(2500);
}

void HidStatusMonitor::run()
{
#ifdef Q_OS_WIN
    while (!isInterruptionRequested()) {
        const HidPathInfo info = findStatusInterface();
        if (info.path.isEmpty()) {
            msleep(500);
            continue;
        }

        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(info.path.utf16()),
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            msleep(500);
            continue;
        }
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            CloseHandle(handle);
            break;
        }

        emit connectionChanged(true);
        std::vector<unsigned char> buffer(qMax<DWORD>(info.inputLength, 9), 0);
        bool deviceLost = false;
        while (!isInterruptionRequested() && !deviceLost) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = event;
            ResetEvent(event);
            DWORD bytes = 0;
            const BOOL started = ReadFile(handle, buffer.data(),
                                          static_cast<DWORD>(buffer.size()),
                                          &bytes, &overlapped);
            if (!started && GetLastError() != ERROR_IO_PENDING) {
                deviceLost = true;
                break;
            }

            if (!started) {
                const DWORD waitResult = WaitForSingleObject(event, 500);
                if (waitResult == WAIT_TIMEOUT) {
                    CancelIoEx(handle, &overlapped);
                    GetOverlappedResult(handle, &overlapped, &bytes, TRUE);
                    continue; // 周期性检查退出标志和插拔状态。
                }
                if (waitResult != WAIT_OBJECT_0
                    || !GetOverlappedResult(handle, &overlapped, &bytes, FALSE)) {
                    deviceLost = true;
                    break;
                }
            }
            if (bytes > 0) {
                emit reportReceived(QByteArray(reinterpret_cast<const char *>(buffer.data()),
                                               static_cast<qsizetype>(bytes)));
            }
        }
        emit connectionChanged(false);
        CloseHandle(event);
        CloseHandle(handle);
        if (!isInterruptionRequested())
            msleep(500);
    }
#else
    // 固件状态接口的当前实现面向 Windows；其他平台只保留音频功能。
    emit connectionChanged(false);
#endif
}
