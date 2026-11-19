# Rapoo VT2 MAX Battery Tray

[Download the portable EXE for Windows x64](https://github.com/Lumenno/rapoo-battery-indicator/raw/refs/heads/main/bin/Rapoo-VT2-Max-Battery.exe)

Portable Windows 10/11 x64 battery indicator. The widget shows a mouse glyph
and percentage on the primary taskbar, beside the notification area. A dash
indicates that the mouse has not provided a valid battery reading. Right-click
to see the detected device, copy diagnostics, or select «Выход» to quit.

| Device | Connection | Status |
| --- | --- | --- |
| Rapoo VT2 MAX | 2.4 GHz receiver (`24AE:1423`) | Verified on hardware |
| ARDOR GAMING Prime X (`ARDW-PR3335-BK`) | USB cable / 2.4 GHz (`25A7:FA7B/FA7C`) | Experimental. IDs confirmed from the official package. Hardware verification pending. |

The utility detects the HID profile automatically. With both devices connected
it prefers a current Rapoo reading and can show ARDOR when Rapoo has no current
reading. The Prime X shares its USB IDs with other CompX mice, so the widget
identifies the detected profile as `ARDOR CompX`.

Rapoo reports are read passively. The ARDOR profile sends a battery-only
query on the matching HID collections. Neither profile needs vendor software.

Close any previous widget instance before starting this build.
To collect diagnostics from PowerShell, use:

    Start-Process -Wait .\Rapoo-VT2-Max-Battery.exe '--diagnose ".\rapoo-diagnostic.log"'

The capture runs for two minutes and can run while the widget or an older
instance is open. Move the mouse to wake it and reconnect the USB receiver
during the capture. The log records the supported devices' HID collections, radio or USB
battery report results, decoder decisions, and taskbar window
geometry. With no log path specified, it creates
`Rapoo-VT2-Max-Battery-<PID>.log` in `%TEMP%`.

On a VT2 MAX running mouse firmware 1.18, the `24AE:1423` receiver
reported `07` frames with state `00` and level `30` (48%). The same mouse
reported state `02` and level `30` when connected by USB and charging.
The receiver decoder accepts a nonzero level with state `00` only in
the 19-byte wireless frame with the embedded VT2 MAX product ID `4623`.

Build with LLVM-MinGW using `sh build.sh`. To run the portable protocol test
on Linux, use `c++ -std=c++17 tests/rapoo_protocol_test.cpp -o /tmp/rapoo-test && /tmp/rapoo-test`.

The Rapoo protocol decoder and its tests come from
[Wireless Device Battery Monitor](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/tree/285d64254216857e94ca3ea3e1deb536f863e282).
The CompX battery query and decoder were checked against
[ArdorBatteryTray](https://github.com/KhromotozzDevOut/ardor-mouse-battery-tray/blob/main/ArdorBatteryTray.cs)
([MIT notice](licenses/ArdorBatteryTray-MIT.txt)). The model's USB IDs come
from the [official Prime X software package](https://ardor-gaming.com/drivers/29378699/prim/).

The standalone utility and corresponding source are distributed under GPL v3
(see LICENSE).
