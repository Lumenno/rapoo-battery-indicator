// GPL-3.0. See LICENSE and README.md for source attribution.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}
#include <atomic>
#include <cwchar>
#include <thread>
#include <vector>
#include "drivers/rapoo_protocol.h"

namespace {
constexpr UINT TrayMessage = WM_APP + 1;
constexpr int Waiting = -1;
constexpr int Disconnected = -2;
std::atomic<int> reading{Disconnected};
std::atomic<ULONGLONG> lastReport{0};
HANDLE stopEvent;
UINT taskbarCreated;
NOTIFYICONDATAW tray{};
bool trayAdded;
int displayed = -3;
bool displayedStale;

bool IsTarget(HANDLE device, DWORD& reportSize) {
    HIDD_ATTRIBUTES ids{};
    ids.Size = sizeof(ids);
    if (!HidD_GetAttributes(device, &ids) || ids.VendorID != 0x24AE ||
        ids.ProductID != 0x1423) return false;

    PHIDP_PREPARSED_DATA descriptor = nullptr;
    if (!HidD_GetPreparsedData(device, &descriptor)) return false;
    HIDP_CAPS caps{};
    const bool valid = HidP_GetCaps(descriptor, &caps) == HIDP_STATUS_SUCCESS;
    HidD_FreePreparsedData(descriptor);
    reportSize = caps.InputReportByteLength;
    return valid && caps.UsagePage == 0xFF00 && caps.Usage == 0x0002 &&
           (reportSize == 16 || reportSize == 19);
}

HANDLE OpenReceiver(DWORD& reportSize) {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO devices = SetupDiGetClassDevsW(&hid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE receiver = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA item{};
    item.cbSize = sizeof(item);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devices, nullptr, &hid, index, &item); ++index) {
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &item, nullptr, 0, &bytes, nullptr);
        if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> storage(bytes);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &item, detail, bytes, nullptr, nullptr)) continue;

        constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        HANDLE metadata = CreateFileW(detail->DevicePath, 0, share, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
        if (metadata == INVALID_HANDLE_VALUE) continue;
        const bool target = IsTarget(metadata, reportSize);
        CloseHandle(metadata);
        if (!target) continue;
        receiver = CreateFileW(detail->DevicePath, GENERIC_READ, share, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (receiver != INVALID_HANDLE_VALUE) break;
    }
    SetupDiDestroyDeviceInfoList(devices);
    return receiver;
}

void ReadReceiver(HANDLE receiver, DWORD reportSize, HANDLE completed) {
    BYTE report[19]{};
    OVERLAPPED operation{};
    operation.hEvent = completed;
    HANDLE events[] = {stopEvent, completed};
    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        ResetEvent(completed);
        DWORD received = 0;
        if (!ReadFile(receiver, report, reportSize, &received, &operation)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                CancelIoEx(receiver, &operation);
                GetOverlappedResult(receiver, &operation, &received, TRUE);
                break;
            }
            if (!GetOverlappedResult(receiver, &operation, &received, FALSE)) break;
        }
        if (!received) break;
        Rapoo::Battery battery;
        if (Rapoo::DecodeBattery(report, received, battery) &&
            battery.connection == Rapoo::Connection::Receiver) {
            lastReport = GetTickCount64();
            reading = battery.percent + (battery.charging ? 101 : 0);
        }
    }
}

void Monitor() {
    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completed) return;
    do {
        DWORD reportSize = 0;
        HANDLE receiver = OpenReceiver(reportSize);
        if (receiver != INVALID_HANDLE_VALUE) {
            reading = Waiting;
            ReadReceiver(receiver, reportSize, completed);
            CloseHandle(receiver);
            reading = Disconnected;
        }
    } while (WaitForSingleObject(stopEvent, 2000) == WAIT_TIMEOUT);
    CloseHandle(completed);
}

HICON MakeIcon(const wchar_t* label, COLORREF background) {
    const int size = 32;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP color = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const BYTE maskBits[128]{};
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits);
    HICON result = nullptr;
    if (color && mask) {
        auto previousBitmap = SelectObject(dc, color);
        RECT area{0, 0, size, size};
        HBRUSH brush = CreateSolidBrush(background);
        FillRect(dc, &area, brush);
        DeleteObject(brush);
        HFONT font = CreateFontW(wcslen(label) == 3 ? -17 : -22, 0, 0, 0, FW_BOLD, FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        auto previousFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, label, -1, &area, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        GdiFlush();
        auto argb = static_cast<DWORD*>(pixels);
        for (int i = 0; i < size * size; ++i) argb[i] |= 0xFF000000;
        SelectObject(dc, previousFont);
        DeleteObject(font);
        SelectObject(dc, previousBitmap);
        ICONINFO icon{TRUE, 0, 0, mask, color};
        result = CreateIconIndirect(&icon);
    }
    DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    return result;
}

void UpdateTray(HWND window) {
    const int value = reading;
    const bool stale = value >= 0 && GetTickCount64() > lastReport.load() + 60000;
    if (trayAdded && value == displayed && stale == displayedStale) return;
    wchar_t label[8] = L"?";
    COLORREF background = RGB(70, 75, 85);
    const bool charging = value >= 101;
    if (value >= 0) {
        const int percent = value % 101;
        std::swprintf(label, 8, L"%d", percent);
        std::swprintf(tray.szTip, 128, L"Rapoo VT2 MAX: %d%%%ls%ls", percent,
                      charging ? L" (заряжается)" : L"",
                      stale ? L" (последнее показание)" : L"");
        if (!stale) background = charging ? RGB(21, 108, 73)
                           : percent <= 15 ? RGB(166, 42, 49) : RGB(29, 80, 140);
    } else {
        std::swprintf(tray.szTip, 128, L"Rapoo VT2 MAX: %ls", value == Waiting
                       ? L"ожидание данных о заряде" : L"приёмник 2,4 ГГц не найден");
    }
    HICON icon = MakeIcon(label, background);
    if (!icon) return;
    tray.cbSize = sizeof(tray);
    tray.hWnd = window;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    tray.uCallbackMessage = TrayMessage;
    tray.hIcon = icon;
    if (!(trayAdded && Shell_NotifyIconW(NIM_MODIFY, &tray)))
        trayAdded = Shell_NotifyIconW(NIM_ADD, &tray);
    DestroyIcon(icon);
    displayed = value;
    displayedStale = stale;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == taskbarCreated) {
        trayAdded = false;
        UpdateTray(window);
        return 0;
    }
    switch (message) {
    case WM_TIMER:
        UpdateTray(window);
        return 0;
    case TrayMessage:
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            POINT point{};
            GetCursorPos(&point);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Выход");
            SetForegroundWindow(window);
            const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                point.x, point.y, 0, window, nullptr);
            DestroyMenu(menu);
            if (command == 1) DestroyWindow(window);
            else PostMessageW(window, WM_NULL, 0, 0);
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &tray);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\RapooVT2MaxBatteryTray");
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleton);
        return 0;
    }
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) return 1;

    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpfnWndProc = WindowProc;
    type.lpszClassName = L"RapooVT2MaxBatteryTray";
    if (!RegisterClassW(&type)) return 1;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"Rapoo VT2 MAX",
                                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window || !SetTimer(window, 1, 1000, nullptr)) return 1;
    UpdateTray(window);
    std::thread worker(Monitor);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    SetEvent(stopEvent);
    worker.join();
    CloseHandle(stopEvent);
    CloseHandle(singleton);
    return 0;
}
