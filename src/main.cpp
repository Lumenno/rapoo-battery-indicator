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
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "drivers/rapoo_protocol.h"

namespace {
constexpr int Waiting = -1;
constexpr int Disconnected = -2;
constexpr wchar_t MutexName[] = L"Local\\RapooVT2MaxBatteryTray";
constexpr wchar_t WidgetClass[] = L"RapooBatteryWidget";
constexpr UINT_PTR WidgetTimer = 1;
constexpr DWORD DiagnosticDuration = 120000;
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
std::atomic<unsigned> acceptedCount{0};
FILE* diagnosticLog;
std::mutex logMutex;
ULONGLONG diagnosticStart;

void Log(const char* format, ...) {
    if (!diagnosticLog) return;
    const std::lock_guard<std::mutex> lock(logMutex);
    std::fprintf(diagnosticLog, "[%7llu ms] ", GetTickCount64() - diagnosticStart);
    va_list args;
    va_start(args, format);
    std::vfprintf(diagnosticLog, format, args);
    va_end(args);
    std::fputc('\n', diagnosticLog);
    std::fflush(diagnosticLog);
}

bool GetReceiverCaps(HANDLE device, HIDP_CAPS& caps) {
    HIDD_ATTRIBUTES ids{};
    ids.Size = sizeof(ids);
    if (!HidD_GetAttributes(device, &ids) || ids.VendorID != 0x24AE ||
        ids.ProductID != 0x1423) return false;

    PHIDP_PREPARSED_DATA descriptor = nullptr;
    if (!HidD_GetPreparsedData(device, &descriptor)) {
        Log("HidD_GetPreparsedData failed: %lu", GetLastError());
        return false;
    }
    const NTSTATUS status = HidP_GetCaps(descriptor, &caps);
    HidD_FreePreparsedData(descriptor);
    if (status != HIDP_STATUS_SUCCESS) Log("HidP_GetCaps failed: %08lX", static_cast<ULONG>(status));
    return status == HIDP_STATUS_SUCCESS;
}

HANDLE OpenReceiver(DWORD& reportSize) {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO devices = SetupDiGetClassDevsW(&hid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        Log("SetupDiGetClassDevs failed: %lu", GetLastError());
        return INVALID_HANDLE_VALUE;
    }
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
        std::wstring path(detail->DevicePath);
        std::transform(path.begin(), path.end(), path.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        if (path.find(L"hid#vid_24ae&pid_1423&") == std::wstring::npos) continue;

        constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        HANDLE metadata = CreateFileW(detail->DevicePath, 0, share, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
        if (metadata == INVALID_HANDLE_VALUE) {
            Log("collection #%lu metadata open failed: %lu", index, GetLastError());
            continue;
        }
        HIDP_CAPS caps{};
        const bool valid = GetReceiverCaps(metadata, caps);
        CloseHandle(metadata);
        if (!valid) continue;
        Log("collection #%lu usage=%04X:%04X input=%u output=%u feature=%u", index,
            caps.UsagePage, caps.Usage, caps.InputReportByteLength,
            caps.OutputReportByteLength, caps.FeatureReportByteLength);
        if (receiver != INVALID_HANDLE_VALUE || caps.UsagePage != 0xFF00 ||
            caps.Usage != 0x0002 || caps.InputReportByteLength == 0) continue;
        receiver = CreateFileW(detail->DevicePath, GENERIC_READ, share, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (receiver == INVALID_HANDLE_VALUE) {
            Log("collection #%lu read open failed: %lu", index, GetLastError());
            continue;
        }
        reportSize = caps.InputReportByteLength;
        Log("collection #%lu opened for passive reads", index);
        if (!diagnosticLog) break;
    }
    SetupDiDestroyDeviceInfoList(devices);
    return receiver;
}

void ReadReceiver(HANDLE receiver, DWORD reportSize, HANDLE completed) {
    std::vector<BYTE> report(reportSize);
    HANDLE events[] = {stopEvent, completed};
    while (WaitForSingleObject(stopEvent, 0) == WAIT_TIMEOUT) {
        ResetEvent(completed);
        OVERLAPPED operation{};
        operation.hEvent = completed;
        DWORD received = 0;
        if (!ReadFile(receiver, report.data(), reportSize, &received, &operation)) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                readError = error;
                Log("ReadFile failed: %lu", error);
                break;
            }
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                CancelIoEx(receiver, &operation);
                GetOverlappedResult(receiver, &operation, &received, TRUE);
                break;
            }
            if (!GetOverlappedResult(receiver, &operation, &received, FALSE)) {
                readError = GetLastError();
                Log("GetOverlappedResult failed: %lu", readError.load());
                break;
            }
        }
        if (!received) {
            Log("ReadFile returned an empty report");
            break;
        }
        ++reportCount;
        lastReportId = report[0];
        Rapoo::Battery battery;
        const bool accepted = Rapoo::DecodeVT2MaxReceiver(report.data(), received, battery);
        if (diagnosticLog) {
            std::string bytes;
            constexpr char digits[] = "0123456789ABCDEF";
            for (DWORD i = 0; i < std::min<DWORD>(received, 64); ++i) {
                bytes += digits[report[i] >> 4];
                bytes += digits[report[i] & 0x0F];
                bytes += ' ';
            }
            Log("input id=%02X length=%lu %s bytes=%s%s", report[0], received,
                accepted ? "accepted" : "ignored", bytes.c_str(), received > 64 ? "..." : "");
            if (accepted) Log("battery=%d%% charging=%d", battery.percent, battery.charging);
        }
        if (report[0] == 0x07) {
            ++statusCount;
            const DWORD offset = received >= 2 && report[1] == 0xB0 ? 1 : 0;
            if (received >= 9 + offset)
                lastStatusBytes = (static_cast<unsigned>(report[1 + offset]) << 16) |
                                  (static_cast<unsigned>(report[7 + offset]) << 8) | report[8 + offset];
        }
        if (accepted) {
            ++acceptedCount;
            lastReport = GetTickCount64();
            reading = battery.percent + (battery.charging ? 101 : 0);
        }
    }
}

void Monitor() {
    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completed) {
        Log("HID read event creation failed: %lu", GetLastError());
        return;
    }
    do {
        DWORD reportSize = 0;
        HANDLE receiver = OpenReceiver(reportSize);
        if (receiver != INVALID_HANDLE_VALUE) {
            inputLength = reportSize;
            readError = 0;
            reading = Waiting;
            ReadReceiver(receiver, reportSize, completed);
            CloseHandle(receiver);
            reading = Disconnected;
            Log("receiver handle closed");
        }
    } while (WaitForSingleObject(stopEvent, 2000) == WAIT_TIMEOUT);
    CloseHandle(completed);
}

void CopyDiagnostics(HWND window) {
    wchar_t text[350];
    std::swprintf(text, 350, L"Rapoo VT2 MAX 24AE:1423 | input=%lu | reports=%u | "
                  L"last ID=%02X | status 07=%u | accepted=%u | mode/state/level=%06X | read error=%lu",
                  inputLength.load(), reportCount.load(), lastReportId.load(),
                  statusCount.load(), acceptedCount.load(), lastStatusBytes.load(), readError.load());
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

COLORREF TaskbarTextColor() {
    HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW), 0, nullptr};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) return GetSysColor(COLOR_BTNTEXT);

    DWORD light = 0;
    DWORD size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

HFONT CreateWidgetFont(HDC dc, int height) {
    const auto create = [height](const wchar_t* name) {
        return CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH, name);
    };
    HFONT font = create(L"Segoe UI Variable Text");
    if (font) {
        const HGDIOBJ old = SelectObject(dc, font);
        wchar_t face[LF_FACESIZE]{};
        GetTextFaceW(dc, LF_FACESIZE, face);
        SelectObject(dc, old);
        if (wcsncmp(face, L"Segoe UI Variable", 17) == 0) return font;
        DeleteObject(font);
    }
    return create(L"Segoe UI");
}

bool PaintWidget(int width, int height, UINT dpi, int value, bool stale, COLORREF color) {
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    BITMAPINFO format{};
    format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format.bmiHeader.biWidth = width;
    format.bmiHeader.biHeight = -height;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    format.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP surface = CreateDIBSection(dc, &format, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!surface) {
        DeleteDC(dc);
        return false;
    }
    const HGDIOBJ oldBitmap = SelectObject(dc, surface);
    PatBlt(dc, 0, 0, width, height, BLACKNESS);
    const auto scale = [dpi](int pixels) { return std::max(1, MulDiv(pixels, dpi, 96)); };
    const int left = scale(6);
    const int mouseWidth = scale(12);
    const int mouseHeight = std::min(height, scale(16));
    const int top = (height - mouseHeight) / 2;
    const HPEN pen = CreatePen(PS_SOLID, scale(1), RGB(255, 255, 255));
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, left, top, left + mouseWidth, top + mouseHeight, scale(8), scale(8));
    MoveToEx(dc, left + mouseWidth / 2, top + scale(3), nullptr);
    LineTo(dc, left + mouseWidth / 2, top + mouseHeight / 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    wchar_t label[8] = L"—";
    if (value >= 0) std::swprintf(label, 8, L"%d%%", value % 101);
    const HFONT font = CreateWidgetFont(dc, scale(12));
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT text{left + mouseWidth + scale(5), 0, width, height};
    DrawTextW(dc, label, -1, &text, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    GdiFlush();

    // Convert the grayscale glyph mask to premultiplied BGRA. An alpha of 1
    // in empty pixels lets the taskbar show through and keeps the menu clickable.
    auto* pixels = static_cast<DWORD*>(bits);
    const unsigned opacity = stale ? 180 : 255;
    for (int index = 0; index < width * height; ++index) {
        const unsigned coverage = pixels[index] & 0xFFU;
        const unsigned alpha = std::max(1U, coverage * opacity / 255);
        const unsigned red = GetRValue(color) * alpha / 255;
        const unsigned green = GetGValue(color) * alpha / 255;
        const unsigned blue = GetBValue(color) * alpha / 255;
        pixels[index] = (alpha << 24) | (red << 16) | (green << 8) | blue;
    }

    SIZE size{width, height};
    POINT origin{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const bool painted = UpdateLayeredWindow(widget, nullptr, nullptr, &size, dc, &origin,
                                              0, &blend, ULW_ALPHA) != FALSE;
    SelectObject(dc, oldBitmap);
    DeleteObject(surface);
    DeleteDC(dc);
    return painted;
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
                    ? L"Rapoo VT2 MAX — последнее показание" : displayed >= 101
                    ? L"Rapoo VT2 MAX — заряжается" : L"Rapoo VT2 MAX");
        AppendMenuW(menu, MF_STRING, 1, L"Копировать диагностику");
        AppendMenuW(menu, MF_STRING, 2, L"Выход");
        SetForegroundWindow(window);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                            point.x, point.y, 0, window, nullptr);
        PostMessageW(window, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (command == 1) CopyDiagnostics(window);
        if (command == 2) PostQuitMessage(0);
        return 0;
    }
    if (message == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct Taskbar {
    HWND window = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND tray = window ? FindWindowExW(window, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    RECT bounds{};
    RECT notification{};
    MONITORINFO monitor{sizeof(MONITORINFO), {}, {}, 0};

    bool Read() {
        return window && tray && IsWindowVisible(window) && IsWindowVisible(tray) &&
               GetWindowRect(window, &bounds) && GetWindowRect(tray, &notification) &&
               GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    }
};

void UpdateWidget(HINSTANCE instance) {
    Taskbar taskbar;
    RECT visible{};
    if (!taskbar.Read() ||
        !IntersectRect(&visible, &taskbar.bounds, &taskbar.monitor.rcMonitor)) {
        ShowWindow(widget, SW_HIDE);
        return;
    }
    const RECT& bounds = taskbar.bounds;
    const RECT& notification = taskbar.notification;
    const bool vertical = bounds.bottom - bounds.top > bounds.right - bounds.left;
    const HWND bar = taskbar.window;
    const UINT dpi = GetDpiForWindow(bar);
    const int height = std::min<int>(MulDiv(30, dpi, 96), bounds.bottom - bounds.top);
    const int width = std::min<int>(MulDiv(56, dpi, 96), bounds.right - bounds.left);
    const int gap = MulDiv(4, dpi, 96);
    const LONG x = vertical ? bounds.left + (bounds.right - bounds.left - width) / 2
                            : notification.left - width - gap;
    const LONG y = vertical ? notification.top - height - gap
                            : bounds.top + (bounds.bottom - bounds.top - height) / 2;
    if (width <= 0 || height <= 0 || x < visible.left || y < visible.top ||
        x + width > visible.right || y + height > visible.bottom) {
        ShowWindow(widget, SW_HIDE);
        return;
    }
    const bool recreate = !IsWindow(widget) || GetParent(widget) != bar;
    if (recreate) {
        if (IsWindow(widget)) DestroyWindow(widget);
        const auto previousDpi = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(bar));
        widget = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY,
                                 WidgetClass, L"Rapoo VT2 MAX", WS_CHILD | WS_CLIPSIBLINGS,
                                 0, 0, width, height, bar, nullptr, instance, nullptr);
        if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
        if (!widget) return;
    }

    const RECT target{x, y, x + width, y + height};
    RECT current{};
    GetWindowRect(widget, &current);
    const bool moved = !EqualRect(&current, &target);
    const bool shown = IsWindowVisible(widget);
    if (moved || !shown) {
        POINT position{x, y};
        MapWindowPoints(nullptr, bar, &position, 1);
        SetWindowPos(widget, HWND_TOP, position.x, position.y, width, height, SWP_NOACTIVATE);
    }

    const int value = reading;
    const bool stale = value >= 0 && GetTickCount64() > lastReport.load() + 60000;
    const COLORREF color = TaskbarTextColor();
    static COLORREF paintedColor = CLR_INVALID;
    static UINT paintedDpi = 0;
    if (recreate || !shown || moved || value != displayed || stale != displayedStale ||
        color != paintedColor || dpi != paintedDpi) {
        if (!PaintWidget(width, height, dpi, value, stale, color)) return;
        displayed = value;
        displayedStale = stale;
        paintedColor = color;
        paintedDpi = dpi;
        if (!shown) ShowWindow(widget, SW_SHOWNOACTIVATE);
    }
}

void LogWindow(const char* name, HWND window) {
    RECT bounds{};
    const BOOL visible = IsWindowVisible(window);
    GetWindowRect(window, &bounds);
    const POINT center{bounds.left + (bounds.right - bounds.left) / 2,
                       bounds.top + (bounds.bottom - bounds.top) / 2};
    const HWND hit = visible ? WindowFromPoint(center) : nullptr;
    Log("%s hwnd=%p visible=%d rect=(%ld,%ld)-(%ld,%ld) center-hit=%p",
        name, static_cast<void*>(window), visible, bounds.left, bounds.top,
        bounds.right, bounds.bottom, static_cast<void*>(hit));
}

void LogTaskbar() {
    Taskbar taskbar;
    LogWindow("Shell_TrayWnd", taskbar.window);
    LogWindow("TrayNotifyWnd", taskbar.tray);
    HWND current = FindWindowW(WidgetClass, nullptr);
    if (!current && taskbar.window)
        current = FindWindowExW(taskbar.window, nullptr, WidgetClass, nullptr);
    LogWindow("RapooBatteryWidget", current);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER && wparam == WidgetTimer) {
        UpdateWidget(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)));
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int Diagnose(const wchar_t* path) {
    diagnosticLog = _wfopen(path, L"w");
    if (!diagnosticLog) {
        MessageBoxW(nullptr, path, L"Не удалось открыть журнал Rapoo", MB_OK | MB_ICONERROR);
        return 1;
    }
    diagnosticStart = GetTickCount64();
    Log("Rapoo VT2 MAX 24AE:1423 passive capture, %lu seconds", DiagnosticDuration / 1000);
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, MutexName);
    const DWORD mutexError = running ? ERROR_SUCCESS : GetLastError();
    Log("widget mutex present=%d error=%lu", running != nullptr, mutexError);
    if (running) CloseHandle(running);
    LogTaskbar();

    std::thread worker(Monitor);
    for (DWORD elapsed = 0; elapsed < DiagnosticDuration; elapsed += 10000) {
        Sleep(10000);
        Log("totals reports=%u id07=%u accepted=%u", reportCount.load(), statusCount.load(),
            acceptedCount.load());
        LogTaskbar();
    }
    SetEvent(stopEvent);
    worker.join();
    Log("final totals reports=%u id07=%u accepted=%u", reportCount.load(), statusCount.load(),
        acceptedCount.load());
    Log("capture finished");
    std::fclose(diagnosticLog);
    diagnosticLog = nullptr;
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return 1;
    const bool diagnose = count >= 2 && wcscmp(args[1], L"--diagnose") == 0;
    std::wstring logPath = count >= 3 ? args[2] : L"";
    LocalFree(args);

    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) return 1;
    if (diagnose) {
        if (logPath.empty()) {
            wchar_t temp[MAX_PATH]{};
            const DWORD length = GetTempPathW(MAX_PATH, temp);
            if (length == 0 || length >= MAX_PATH) return 1;
            logPath = std::wstring(temp) + L"Rapoo-VT2-Max-Battery-" +
                      std::to_wstring(GetCurrentProcessId()) + L".log";
        }
        const int result = Diagnose(logPath.c_str());
        CloseHandle(stopEvent);
        return result;
    }

    HANDLE singleton = CreateMutexW(nullptr, FALSE, MutexName);
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleton);
        CloseHandle(stopEvent);
        MessageBoxW(nullptr, L"Другой экземпляр индикатора уже запущен.", L"Rapoo VT2 MAX",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    WNDCLASSW widgetType{};
    widgetType.hInstance = instance;
    widgetType.lpfnWndProc = WidgetProc;
    widgetType.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    widgetType.lpszClassName = WidgetClass;
    if (!RegisterClassW(&widgetType)) return 1;
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpfnWndProc = WindowProc;
    type.lpszClassName = L"RapooVT2MaxBatteryTray";
    if (!RegisterClassW(&type)) return 1;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"Rapoo VT2 MAX",
                                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    if (!SetTimer(window, WidgetTimer, 250, nullptr)) {
        MessageBoxW(nullptr, L"Не удалось запустить виджет панели задач.", L"Rapoo VT2 MAX",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    UpdateWidget(instance);
    std::thread worker(Monitor);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (IsWindow(widget)) DestroyWindow(widget);
    DestroyWindow(window);
    SetEvent(stopEvent);
    worker.join();
    CloseHandle(stopEvent);
    CloseHandle(singleton);
    return 0;
}
