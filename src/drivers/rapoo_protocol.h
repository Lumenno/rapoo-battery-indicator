#pragma once
// Passive Rapoo status reports, including the report ID (hidapi/Win32 convention).
// Kept independent of Win32 so captured frames can be checked without a mouse.
#include <cstddef>
#include <cstdint>

namespace Rapoo {

enum class Connection { Unknown, Receiver, Bluetooth, Cable };
enum class Profile { Nordic54L, LegacyVT3Pro };

struct Battery {
    int percent = -1;
    bool charging = false;
    Connection connection = Connection::Unknown;
    std::uint16_t mousePid = 0;
};

inline bool DecodeBattery(const std::uint8_t* data, std::size_t size, Battery& out,
                          Profile profile = Profile::Nordic54L,
                          std::size_t nordicHeader = 1) {
    if (!data || !size) return false;
    Battery result;
    unsigned state = 0, percent = 0;
    if (profile == Profile::Nordic54L && data[0] == 0x07) {
        // Nordic 54L: mode, DPI gear, X/Y DPI, battery state, percentage.
        if (nordicHeader > size || size - nordicHeader < 8) return false;
        const auto* payload = data + nordicHeader;
        const unsigned mode = payload[0] & 0x0F;
        if (mode > 2) return false;
        switch (mode) {
        case 0: result.connection = Connection::Receiver; break;
        case 1: result.connection = Connection::Bluetooth; break;
        case 2: result.connection = Connection::Cable; break;
        }
        state = payload[6];
        percent = payload[7];
        if (size - nordicHeader >= 14)
            result.mousePid = (std::uint16_t)(payload[12] | (payload[13] << 8));
    } else if (profile == Profile::LegacyVT3Pro && data[0] == 0xBB) {
        // VT3 Pro firmware: BB B0 mode DPI-lo DPI-hi battery-state percentage.
        if (size < 7 || data[1] != 0xB0) return false;
        state = data[5];
        percent = data[6];
    } else {
        return false;
    }
    // 0 = invalid/transition. Some older firmware uses 7F instead of a level while charging.
    if (state != 1 && state != 2) return false;
    if (percent > 100 && !(profile == Profile::LegacyVT3Pro && state == 2 && percent == 0x7F)) return false;
    result.charging = state == 2;
    if (percent <= 100) result.percent = (int)percent;
    out = result;
    return true;
}

inline bool DecodeVT2MaxReceiver(const std::uint8_t* data, std::size_t size, Battery& out) {
    if (!data || size < 9 || data[0] != 0x07) return false;
    // VT2 MAX 1.18 over its 24AE:1423 receiver reports state 0 with a
    // usable level. A paired 24AE:4623 cable capture reported the same
    // level with charging state 2. Limit this exception to that frame.
    constexpr std::uint16_t vt2MaxPid = 0x4623;
    const bool knownFrame = size == 19 && data[13] == (vt2MaxPid & 0xFF) &&
                            data[14] == (vt2MaxPid >> 8);
    if (knownFrame && (data[1] & 0x0F) == 0 && data[7] == 0) {
        if (data[8] == 0 || data[8] > 100) return false;
        out = Battery{data[8], false, Connection::Receiver, vt2MaxPid};
        return true;
    }
    // The Nordic dongle can prepend its B0 child-status marker to report 07.
    const std::size_t header = !knownFrame && data[1] == 0xB0 ? 2 : 1;
    Battery battery;
    if (!DecodeBattery(data, size, battery, Profile::Nordic54L, header) ||
        battery.connection != Connection::Receiver) return false;
    out = battery;
    return true;
}

}  // namespace Rapoo
