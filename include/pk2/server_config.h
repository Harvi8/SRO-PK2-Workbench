#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pk2/crypto.h"

namespace pk2 {

struct ServerDivision {
    std::string name;
    std::vector<std::string> gateways;
};

struct ServerConfig {
    std::uint8_t contentId{};
    std::uint32_t version{};
    std::uint16_t port{};
    BlockEndian versionEndian{BlockEndian::Little};
    bool versionBlockAtOffset4{true};
    std::vector<std::uint8_t> versionFile;
    std::vector<ServerDivision> divisions;
};

ServerConfig parseServerConfig(const std::vector<std::uint8_t>& divisionInfo,
                               const std::vector<std::uint8_t>& gatePort,
                               const std::vector<std::uint8_t>& encryptedVersion);

std::vector<std::uint8_t> serializeDivisionInfo(const ServerConfig& config);
std::vector<std::uint8_t> serializeGatePort(const ServerConfig& config);
std::vector<std::uint8_t> serializeServerVersion(const ServerConfig& config);

} // namespace pk2
