// GPL-3.0. See LICENSE and README.md for source attribution.
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000003
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <thread>
#include <vector>
#include "drivers/rapoo_protocol.h"

namespace {
constexpr int Waiting = -1;
constexpr int Disconnected = -2;
std::atomic<int> reading{Disconnected};
std::atomic<ULONGLONG> lastReport{0};
HANDLE stopEvent;
HWND widget;
std::atomic<DWORD> inputLength{0};
std::atomic<unsigned> reportCount{0};
std::atomic<unsigned> statusCount{0};
std::atomic<unsigned> lastReportId{0};
std::atomic<unsigned> lastStatusBytes{0};
std::atomic<DWORD> readError{0};
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
           (reportSize >= 9 && reportSize <= 64);
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
    BYTE report[64]{};
    HANDLE events[] = {stopEvent, completed};
    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        ResetEvent(completed);
        OVERLAPPED operation{};
        operation.hEvent = completed;
        DWORD received = 0;
        if (!ReadFile(receiver, report, reportSize, &received, &operation)) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                readError = error;
                break;
            }
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                CancelIoEx(receiver, &operation);
                GetOverlappedResult(receiver, &operation, &received, TRUE);
                break;
            }
            if (!GetOverlappedResult(receiver, &operation, &received, FALSE)) {
                readError = GetLastError();
                break;
            }
        }
        if (!received) break;
        ++reportCount;
        lastReportId = report[0];
        if (report[0] != 0x07 || received < 9) continue;
        ++statusCount;

        // Rapoo's receiver also defines 07 B0 for a status report with one extra header byte.
        if (report[1] == 0xB0 && received >= 10) {
            std::memmove(report + 1, report + 2, received - 2);
            --received;
        }
        lastStatusBytes = (static_cast<unsigned>(report[1]) << 16) |
                          (static_cast<unsigned>(report[7]) << 8) | report[8];
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
            inputLength = reportSize;
            reading = Waiting;
            ReadReceiver(receiver, reportSize, completed);
            CloseHandle(receiver);
            reading = Disconnected;
        }
    } while (WaitForSingleObject(stopEvent, 2000) == WAIT_TIMEOUT);
    CloseHandle(completed);
}

void CopyDiagnostics(HWND window) {
    wchar_t text[350];
    std::swprintf(text, 350, L"Rapoo VT2 MAX 24AE:1423 | input=%lu | reports=%u | "
                  L"last ID=%02X | status 07=%u | mode/state/level=%06X | read error=%lu",
                  inputLength.load(), reportCount.load(), lastReportId.load(),
                  statusCount.load(), lastStatusBytes.load(), readError.load());
    const SIZE_T bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return;
    void* destination = GlobalLock(memory);
    if (destination) {
        std::memcpy(destination, text, bytes);
        GlobalUnlock(memory);
        if (OpenClipboard(window)) {
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) memory = nullptr;
            CloseClipboard();
        }
    }
    if (memory) GlobalFree(memory);
}

LRESULT CALLBACK WidgetProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_RBUTTONUP) {
        POINT point{};
        GetCursorPos(&point);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, displayed == Disconnected
                    ? L"Rapoo VT2 MAX — приёмник не найден" : displayed == Waiting
                    ? L"Rapoo VT2 MAX — ожидание данных" : displayedStale
                    ? L"Rapoo VT2 MAX — последнее показание" : L"Rapoo VT2 MAX");
        AppendMenuW(menu, MF_STRING, 1, L"Копировать диагностику");
        AppendMenuW(menu, MF_STRING, 2, L"Выход");
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                            point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (command == 1) CopyDiagnostics(window);
        if (command == 2) PostQuitMessage(0);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT area{};
        GetClientRect(window, &area);
        const int h = area.bottom;
        const int mouseWidth = MulDiv(16, h, 30);
        const int mouseHeight = MulDiv(22, h, 30);
        const int left = MulDiv(6, h, 30);
        const int top = (h - mouseHeight) / 2;
        const COLORREF foreground = displayedStale ? RGB(174, 185, 199) : RGB(242, 247, 255);
        HBRUSH background = CreateSolidBrush(RGB(29, 43, 61));
        FillRect(dc, &area, background);
        HPEN pen = CreatePen(PS_SOLID, std::max(1, h / 18), foreground);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, left, top, left + mouseWidth, top + mouseHeight, mouseWidth, mouseWidth);
        MoveToEx(dc, left + mouseWidth / 2, top + 3, nullptr);
        LineTo(dc, left + mouseWidth / 2, top + mouseHeight / 2);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        DeleteObject(background);

        wchar_t label[8] = L"—";
        if (displayed >= 0) std::swprintf(label, 8, L"%d%%", displayed % 101);
        HFONT font = CreateFontW(-MulDiv(15, h, 30), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, displayed >= 101 && !displayedStale ? RGB(145, 234, 184) : foreground);
        area.left = left + mouseWidth + MulDiv(5, h, 30);
        DrawTextW(dc, label, -1, &area, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void UpdateWidget(HINSTANCE instance) {
    const HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HWND tray = bar ? FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT bounds{}, notification{};
    if (!tray || !GetClientRect(bar, &bounds) || !GetWindowRect(tray, &notification)) return;
    if (!IsWindow(widget) || GetParent(widget) != bar) {
        widget = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, L"RapooBatteryWidget", nullptr,
                                 WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                                 bar, nullptr, instance, nullptr);
        if (!widget) return;
        SetLayeredWindowAttributes(widget, 0, 255, LWA_ALPHA);
    }
    POINT anchor{notification.left, notification.top};
    ScreenToClient(bar, &anchor);
    const bool vertical = bounds.bottom > bounds.right;
    const UINT dpi = GetDpiForWindow(bar);
    const int height = std::min<int>(MulDiv(30, dpi, 96), bounds.bottom);
    const int width = std::min<int>(MulDiv(69, dpi, 96), bounds.right);
    if (width <= 0 || height <= 0) return;
    const int gap = MulDiv(4, dpi, 96);
    const int x = vertical ? (bounds.right - width) / 2 : std::max<int>(0, anchor.x - width - gap);
    const int y = vertical ? std::max<int>(0, anchor.y - height - gap) : (bounds.bottom - height) / 2;
    SetWindowPos(widget, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    const int value = reading;
    const bool stale = value >= 0 && GetTickCount64() > lastReport.load() + 60000;
    if (value != displayed || stale != displayedStale) {
        displayed = value;
        displayedStale = stale;
        InvalidateRect(widget, nullptr, FALSE);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER) {
        UpdateWidget(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)));
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
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

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW widgetType{};
    widgetType.hInstance = instance;
    widgetType.lpfnWndProc = WidgetProc;
    widgetType.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    widgetType.lpszClassName = L"RapooBatteryWidget";
    if (!RegisterClassW(&widgetType)) return 1;
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpfnWndProc = WindowProc;
    type.lpszClassName = L"RapooVT2MaxBatteryTray";
    if (!RegisterClassW(&type)) return 1;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"Rapoo VT2 MAX",
                                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window || !SetTimer(window, 1, 1000, nullptr)) return 1;
    UpdateWidget(instance);
    std::thread worker(Monitor);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (IsWindow(widget)) DestroyWindow(widget);
    SetEvent(stopEvent);
    worker.join();
    CloseHandle(stopEvent);
    CloseHandle(singleton);
    return 0;
}
