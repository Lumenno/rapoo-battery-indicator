# Rapoo VT2 MAX Battery Tray

Portable Windows 10/11 x64 utility for the Rapoo VT2 MAX 2.4 GHz receiver
(USB ID 24AE:1423). Hover over the tray icon for the percentage and current
status. Right-click and select «Выход» to quit. A question mark is shown
until the receiver supplies battery data. The utility opens the matching
HID status collection for reading and sends no HID commands.

Build with LLVM-MinGW using `sh build.sh`. To run the portable protocol test
on Linux, use `c++ -std=c++17 tests/rapoo_protocol_test.cpp -o /tmp/rapoo-test && /tmp/rapoo-test`.

The Rapoo protocol decoder and its tests come from
[Wireless Device Battery Monitor](https://github.com/krisgfx9/Wireless-Device-Battery-Monitor/tree/285d64254216857e94ca3ea3e1deb536f863e282).
The standalone utility and corresponding source are distributed under GPL v3
(see LICENSE).
