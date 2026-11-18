# Rapoo VT2 MAX Battery Tray

Portable Windows 10/11 x64 utility for the Rapoo VT2 MAX 2.4 GHz receiver
(USB ID 24AE:1423). The widget shows a mouse glyph and percentage on the
primary taskbar, beside the notification area. A dash indicates that the
receiver has not provided a valid battery reading. Right-click the widget
to copy HID report counters for troubleshooting or select «Выход» to quit.
The utility opens the matching HID status collection for reading and sends
no HID commands.

Close any previous Rapoo battery tray instance before starting this build.
If the widget shows a dash, wake the mouse and briefly cycle its DPI setting
back to the original value before copying diagnostics.

Build with LLVM-MinGW using `sh build.sh`. To run the portable protocol test
on Linux, use `c++ -std=c++17 tests/rapoo_protocol_test.cpp -o /tmp/rapoo-test && /tmp/rapoo-test`.

The Rapoo protocol decoder and its tests come from
[Wireless Device Battery Monitor](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/tree/285d64254216857e94ca3ea3e1deb536f863e282).
The standalone utility and corresponding source are distributed under GPL v3
(see LICENSE).
