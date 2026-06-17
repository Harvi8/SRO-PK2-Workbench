#include "pk2/crypto.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {

#include "blowfish_constants.inc"

std::uint32_t readBe32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::uint32_t readLe32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void writeBe32(std::uint8_t* bytes, std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>((value >> 24) & 0xff);
    bytes[1] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[2] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[3] = static_cast<std::uint8_t>(value & 0xff);
}

void writeLe32(std::uint8_t* bytes, std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>(value & 0xff);
    bytes[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    bytes[2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
    bytes[3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
}

std::uint32_t rotateLeft(std::uint32_t value, std::uint32_t amount) {
    return (value << amount) | (value >> (32 - amount));
}

std::uint32_t md5K(std::size_t index) {
    const auto value = std::fabs(std::sin(static_cast<double>(index + 1)));
    return static_cast<std::uint32_t>(value * 4294967296.0);
}

std::vector<std::uint8_t> joymaxCompatibleKey(const std::uint8_t* key, std::size_t size) {
    static constexpr std::array<std::uint8_t, 56> kJoymaxMask = {
        0x03, 0xf8, 0xe4, 0x44, 0x88, 0x99, 0x3f, 0x64,
        0xfe, 0x35};

    std::vector<std::uint8_t> transformed(key, key + size);
    const auto limit = std::min(transformed.size(), kJoymaxMask.size());
    for (std::size_t i = 0; i < limit; ++i) {
        transformed[i] ^= kJoymaxMask[i];
    }
    return transformed;
}

} // namespace

namespace pk2 {

std::string md5Hex(std::string_view text) {
    static constexpr std::uint32_t shifts[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    const auto bitLength = static_cast<std::uint64_t>(bytes.size()) * 8;
    bytes.push_back(0x80);
    while ((bytes.size() % 64) != 56) {
        bytes.push_back(0);
    }
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((bitLength >> (8 * i)) & 0xff));
    }

    std::uint32_t a0 = 0x67452301;
    std::uint32_t b0 = 0xefcdab89;
    std::uint32_t c0 = 0x98badcfe;
    std::uint32_t d0 = 0x10325476;

    for (std::size_t chunk = 0; chunk < bytes.size(); chunk += 64) {
        std::uint32_t m[16]{};
        for (std::size_t i = 0; i < 16; ++i) {
            m[i] = readLe32(bytes.data() + chunk + i * 4);
        }

        auto a = a0;
        auto b = b0;
        auto c = c0;
        auto d = d0;

        for (std::uint32_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::uint32_t g = 0;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | (~d));
                g = (7 * i) % 16;
            }

            const auto temp = d;
            d = c;
            c = b;
            b = b + rotateLeft(a + f + md5K(i) + m[g], shifts[i]);
            a = temp;
        }

        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    std::uint8_t digest[16]{};
    writeLe32(digest, a0);
    writeLe32(digest + 4, b0);
    writeLe32(digest + 8, c0);
    writeLe32(digest + 12, d0);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

Blowfish::Blowfish(std::string_view key, KeyScheduleMode mode) {
    init(reinterpret_cast<const std::uint8_t*>(key.data()), key.size(), mode);
}

Blowfish::Blowfish(const std::uint8_t* key, std::size_t size, KeyScheduleMode mode) {
    init(key, size, mode);
}

void Blowfish::init(const std::uint8_t* key, std::size_t size, KeyScheduleMode mode) {
    if (size == 0) {
        throw std::invalid_argument("Blowfish key must not be empty");
    }
    if (mode == KeyScheduleMode::JoymaxCompatible && size > 56) {
        throw std::invalid_argument("Joymax-compatible Blowfish keys must be 56 bytes or shorter");
    }

    auto transformedKey = mode == KeyScheduleMode::JoymaxCompatible
        ? joymaxCompatibleKey(key, size)
        : std::vector<std::uint8_t>(key, key + size);
    key = transformedKey.data();
    size = transformedKey.size();

    for (std::size_t i = 0; i < p_.size(); ++i) {
        p_[i] = static_cast<std::uint32_t>(parray[i]);
    }
    for (std::size_t i = 0; i < 256; ++i) {
        s_[0][i] = static_cast<std::uint32_t>(sbox0[i]);
        s_[1][i] = static_cast<std::uint32_t>(sbox1[i]);
        s_[2][i] = static_cast<std::uint32_t>(sbox2[i]);
        s_[3][i] = static_cast<std::uint32_t>(sbox3[i]);
    }

    std::size_t keyIndex = 0;
    for (auto& item : p_) {
        std::uint32_t word = 0;
        for (int j = 0; j < 4; ++j) {
            word = (word << 8) | key[keyIndex];
            keyIndex = (keyIndex + 1) % size;
        }
        item ^= word;
    }

    std::uint32_t left = 0;
    std::uint32_t right = 0;
    for (std::size_t i = 0; i < p_.size(); i += 2) {
        encryptWords(left, right);
        p_[i] = left;
        p_[i + 1] = right;
    }
    for (auto& box : s_) {
        for (std::size_t i = 0; i < box.size(); i += 2) {
            encryptWords(left, right);
            box[i] = left;
            box[i + 1] = right;
        }
    }
}

std::uint32_t Blowfish::f(std::uint32_t value) const {
    const auto a = (value >> 24) & 0xff;
    const auto b = (value >> 16) & 0xff;
    const auto c = (value >> 8) & 0xff;
    const auto d = value & 0xff;
    return ((s_[0][a] + s_[1][b]) ^ s_[2][c]) + s_[3][d];
}

void Blowfish::encryptWords(std::uint32_t& left, std::uint32_t& right) const {
    for (std::size_t i = 0; i < 16; ++i) {
        left ^= p_[i];
        right ^= f(left);
        std::swap(left, right);
    }
    std::swap(left, right);
    right ^= p_[16];
    left ^= p_[17];
}

void Blowfish::decryptWords(std::uint32_t& left, std::uint32_t& right) const {
    for (std::size_t i = 17; i > 1; --i) {
        left ^= p_[i];
        right ^= f(left);
        std::swap(left, right);
    }
    std::swap(left, right);
    right ^= p_[1];
    left ^= p_[0];
}

void Blowfish::encryptBlock(std::uint8_t* block, BlockEndian endian) const {
    auto left = endian == BlockEndian::Big ? readBe32(block) : readLe32(block);
    auto right = endian == BlockEndian::Big ? readBe32(block + 4) : readLe32(block + 4);
    encryptWords(left, right);
    if (endian == BlockEndian::Big) {
        writeBe32(block, left);
        writeBe32(block + 4, right);
    } else {
        writeLe32(block, left);
        writeLe32(block + 4, right);
    }
}

void Blowfish::decryptBlock(std::uint8_t* block, BlockEndian endian) const {
    auto left = endian == BlockEndian::Big ? readBe32(block) : readLe32(block);
    auto right = endian == BlockEndian::Big ? readBe32(block + 4) : readLe32(block + 4);
    decryptWords(left, right);
    if (endian == BlockEndian::Big) {
        writeBe32(block, left);
        writeBe32(block + 4, right);
    } else {
        writeLe32(block, left);
        writeLe32(block + 4, right);
    }
}

void Blowfish::encryptBuffer(std::vector<std::uint8_t>& buffer, BlockEndian endian) const {
    if ((buffer.size() % 8) != 0) {
        throw std::invalid_argument("Blowfish buffers must be 8-byte aligned");
    }
    for (std::size_t i = 0; i < buffer.size(); i += 8) {
        encryptBlock(buffer.data() + i, endian);
    }
}

void Blowfish::decryptBuffer(std::vector<std::uint8_t>& buffer, BlockEndian endian) const {
    if ((buffer.size() % 8) != 0) {
        throw std::invalid_argument("Blowfish buffers must be 8-byte aligned");
    }
    for (std::size_t i = 0; i < buffer.size(); i += 8) {
        decryptBlock(buffer.data() + i, endian);
    }
}

} // namespace pk2
