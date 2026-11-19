#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace Ardor {

constexpr std::uint16_t Vendor = 0x25A7;
constexpr std::uint16_t Cable = 0xFA7B;
constexpr std::uint16_t Receiver = 0xFA7C;
constexpr std::size_t ReportSize = 17;
constexpr std::array<std::uint8_t, ReportSize> BatteryQuery{
    0x08, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x49
};

inline bool DecodeBattery(const std::uint8_t* bytes, std::size_t length,
                          int& percent, bool& charging) {
    if (!bytes || length != ReportSize || bytes[0] != 0x09 ||
        bytes[1] != BatteryQuery[1] || bytes[6] > 100) return false;
    unsigned checksum = 0;
    for (std::size_t i = 0; i < length; ++i) checksum += bytes[i];
    if ((checksum & 0xFF) != 0x55) return false;
    percent = bytes[6];
    charging = bytes[7] != 0;
    return true;
}

}  // namespace Ardor
