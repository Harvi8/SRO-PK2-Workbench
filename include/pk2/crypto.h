#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pk2 {

constexpr std::string_view kOfficialSroPassword = "169841";

std::string md5Hex(std::string_view text);

enum class BlockEndian {
    Big,
    Little
};

enum class KeyScheduleMode {
    Standard,
    JoymaxCompatible
};

class Blowfish {
public:
    explicit Blowfish(std::string_view key,
                      KeyScheduleMode mode = KeyScheduleMode::Standard);
    Blowfish(const std::uint8_t* key,
             std::size_t size,
             KeyScheduleMode mode = KeyScheduleMode::Standard);

    void encryptBlock(std::uint8_t* block, BlockEndian endian) const;
    void decryptBlock(std::uint8_t* block, BlockEndian endian) const;
    void encryptBuffer(std::vector<std::uint8_t>& buffer, BlockEndian endian) const;
    void decryptBuffer(std::vector<std::uint8_t>& buffer, BlockEndian endian) const;

private:
    void init(const std::uint8_t* key, std::size_t size, KeyScheduleMode mode);
    void encryptWords(std::uint32_t& left, std::uint32_t& right) const;
    void decryptWords(std::uint32_t& left, std::uint32_t& right) const;
    std::uint32_t f(std::uint32_t value) const;

    std::array<std::uint32_t, 18> p_{};
    std::array<std::array<std::uint32_t, 256>, 4> s_{};
};

} // namespace pk2
