# Rapoo VT2 MAX Battery Tray

Portable Windows 10/11 x64 utility for the Rapoo VT2 MAX 2.4 GHz receiver
(USB ID 24AE:1423). The widget shows a mouse glyph and percentage on the
primary taskbar, beside the notification area. A dash indicates that the
receiver has not provided a valid battery reading. Right-click the widget
to copy HID report counters for troubleshooting or select «Выход» to quit.
The utility opens the matching HID status collection for reading and sends
no HID commands.

Close any previous Rapoo battery tray instance before starting this build.
To collect diagnostics from PowerShell, use:

    Start-Process -Wait .\Rapoo-VT2-Max-Battery.exe '--diagnose ".\rapoo-diagnostic.log"'

The capture runs for two minutes and can run while the widget or an older
instance is open. Move the mouse to wake it and reconnect the USB receiver
during the capture. The log records the receiver's HID collections, read
results, input report IDs and sizes, decoder decisions, and taskbar window
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
The standalone utility and corresponding source are distributed under GPL v3
(see LICENSE).
