#include "pk2/archive.h"
#include "pk2/crypto.h"
#include "pk2/path.h"
#include "pk2/server_config.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string readText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> readPrefix(const fs::path& path, std::size_t size) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    std::vector<std::uint8_t> bytes(size);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(input.gcount() == static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

fs::path testRoot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("clean_pk2_tests_" + std::to_string(stamp));
}

void testMd5() {
    assert(pk2::md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    assert(pk2::md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
}

void testBlowfishKnownVector() {
    const std::uint8_t zeroKey[8]{};
    std::uint8_t block[8]{};
    pk2::Blowfish cipher(zeroKey, sizeof(zeroKey));
    cipher.encryptBlock(block, pk2::BlockEndian::Big);
    const std::uint8_t expected[8] = {0x4e, 0xf9, 0x97, 0x45, 0x61, 0x98, 0xdd, 0x78};
    for (std::size_t i = 0; i < 8; ++i) {
        assert(block[i] == expected[i]);
    }
    cipher.decryptBlock(block, pk2::BlockEndian::Big);
    for (const auto byte : block) {
        assert(byte == 0);
    }
}

void testArchiveRoundTrip() {
    const auto root = testRoot();
    const auto source = root / "source";
    const auto extracted = root / "extracted";
    const auto archivePath = root / "roundtrip.pk2";

    writeText(source / "hello.txt", "hello pk2");
    writeText(source / "nested" / "world.txt", "nested data");

    auto archive = pk2::Pk2Archive::createNew("169841");
    archive.importFile(source / "hello.txt", "Data/hello.txt");
    archive.importFolder(source / "nested", "Data/nested");
    archive.saveAs(archivePath);
    const auto originalHeader = readPrefix(archivePath, 256);
    assert(originalHeader[30] == 2);
    assert(originalHeader[31] == 0);
    assert(originalHeader[32] == 0);
    assert(originalHeader[33] == 0);
    assert(originalHeader[34] == 1);

    {
        std::ofstream padding(archivePath, std::ios::binary | std::ios::app);
        assert(padding);
        std::vector<char> bytes(4096, 0);
        padding.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const auto paddedArchiveSize = fs::file_size(archivePath);

    auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    const auto entries = reopened.listTree();
    assert(entries.size() == 4);
    assert(reopened.find("Data/hello.txt").has_value());
    assert(reopened.find("Data/nested/world.txt").has_value());
    const auto helloBytes = reopened.readFile("Data/hello.txt");
    assert(std::string(helloBytes.begin(), helloBytes.end()) == "hello pk2");
    reopened.importFileBytes({'u', 'p', 'd', 'a', 't', 'e', 'd'}, "Data/hello.txt");
    const auto updatedBytes = reopened.readFile("Data/hello.txt");
    assert(std::string(updatedBytes.begin(), updatedBytes.end()) == "updated");
    reopened.save();
    assert(fs::file_size(archivePath) == paddedArchiveSize);
    assert(readPrefix(archivePath, 256) == originalHeader);
    const auto savedBytes = reopened.readFile("Data/hello.txt");
    assert(std::string(savedBytes.begin(), savedBytes.end()) == "updated");

    reopened.importFileBytes({'u', 'p', 'd', 'a', 't', 'e', 'd', ' ', 'a', 'g', 'a', 'i', 'n'},
                             "Data/hello.txt");
    reopened.save();
    reopened = pk2::Pk2Archive::open(archivePath, "169841");
    assert(readPrefix(archivePath, 256) == originalHeader);
    const auto twiceSavedBytes = reopened.readFile("Data/hello.txt");
    assert(std::string(twiceSavedBytes.begin(), twiceSavedBytes.end()) == "updated again");
    const auto untouchedBytes = reopened.readFile("Data/nested/world.txt");
    assert(std::string(untouchedBytes.begin(), untouchedBytes.end()) == "nested data");

    reopened.extract("Data", extracted, true, pk2::OverwritePolicy::Replace);
    assert(readText(extracted / "Data" / "hello.txt") == "updated again");
    assert(readText(extracted / "Data" / "nested" / "world.txt") == "nested data");

    reopened.deleteEntry("Data/hello.txt");
    assert(!reopened.find("Data/hello.txt").has_value());

    fs::remove_all(root);
}

void testServerConfigRoundTrip() {
    pk2::ServerConfig original;
    original.contentId = 22;
    original.version = 188;
    original.port = 15779;
    original.versionEndian = pk2::BlockEndian::Little;
    original.divisions = {
        {"Private SRO", {"127.0.0.1", "gateway.example.com"}},
        {"Second Division", {"10.0.0.25"}},
    };

    const auto divisionInfo = pk2::serializeDivisionInfo(original);
    const auto gatePort = pk2::serializeGatePort(original);
    const auto version = pk2::serializeServerVersion(original);
    assert(gatePort.size() == 8);
    assert(version.size() == 1024);
    assert(std::all_of(version.begin() + 12, version.end(), [](std::uint8_t byte) {
        return byte == 0;
    }));

    const auto parsed = pk2::parseServerConfig(divisionInfo, gatePort, version);
    assert(parsed.contentId == 22);
    assert(parsed.version == 188);
    assert(parsed.port == 15779);
    assert(parsed.versionEndian == pk2::BlockEndian::Little);
    assert(parsed.versionBlockAtOffset4);
    assert(parsed.divisions.size() == 2);
    assert(parsed.divisions[0].name == "Private SRO");
    assert(parsed.divisions[0].gateways.size() == 2);
    assert(parsed.divisions[0].gateways[1] == "gateway.example.com");
    assert(parsed.divisions[1].gateways[0] == "10.0.0.25");

    original.versionEndian = pk2::BlockEndian::Big;
    const auto bigEndianVersion = pk2::serializeServerVersion(original);
    const auto parsedBig = pk2::parseServerConfig(divisionInfo, gatePort, bigEndianVersion);
    assert(parsedBig.version == 188);
    assert(parsedBig.versionEndian == pk2::BlockEndian::Big);

    original.versionFile.assign(1024, 0);
    original.versionFile[0] = 0x53;
    original.versionFile[100] = 0x7a;
    const auto preservedVersion = pk2::serializeServerVersion(original);
    const auto parsedPreserved = pk2::parseServerConfig(divisionInfo, gatePort, preservedVersion);
    assert(parsedPreserved.versionFile[0] == 0x53);
    assert(parsedPreserved.versionFile[100] == 0x7a);
}

void testMultiBlockArchiveRoundTrip() {
    const auto root = testRoot();
    const auto source = root / "bulk-source";
    const auto archivePath = root / "bulk.pk2";

    auto archive = pk2::Pk2Archive::createNew("169841");
    for (int i = 0; i < 45; ++i) {
        const auto fileName = "file_" + std::to_string(i) + ".txt";
        const auto sourcePath = source / fileName;
        writeText(sourcePath, "bulk " + std::to_string(i));
        archive.importFile(sourcePath, "Bulk/" + fileName);
    }
    archive.saveAs(archivePath);

    const auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    assert(reopened.find("Bulk/file_0.txt").has_value());
    assert(reopened.find("Bulk/file_44.txt").has_value());
    assert(reopened.children("Bulk").size() == 45);

    fs::remove_all(root);
}

void testCanonicalRootBlockWinsOverHeaderScanCandidate() {
    const auto root = testRoot();
    const auto source = root / "source";
    const auto archivePath = root / "ambiguous-root.pk2";

    auto archive = pk2::Pk2Archive::createNew("169841");
    for (int i = 0; i < 25; ++i) {
        const auto fileName = "file_" + std::to_string(i) + ".txt";
        const auto sourcePath = source / fileName;
        writeText(sourcePath, "nested " + std::to_string(i));
        archive.importFile(sourcePath, "folder/" + fileName);
    }
    archive.saveAs(archivePath);

    {
        std::fstream file(archivePath, std::ios::binary | std::ios::in | std::ios::out);
        assert(file);
        const std::uint32_t childBlockOffset = 256 + 2560;
        std::uint8_t bytes[4] = {
            static_cast<std::uint8_t>(childBlockOffset & 0xff),
            static_cast<std::uint8_t>((childBlockOffset >> 8) & 0xff),
            static_cast<std::uint8_t>((childBlockOffset >> 16) & 0xff),
            static_cast<std::uint8_t>((childBlockOffset >> 24) & 0xff),
        };
        file.seekp(24, std::ios::beg);
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    const auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    const auto rootChildren = reopened.children("");
    assert(rootChildren.size() == 1);
    assert(rootChildren[0].type == pk2::EntryType::Folder);
    assert(rootChildren[0].path == "folder");
    assert(reopened.find("folder/file_0.txt").has_value());

    fs::remove_all(root);
}

void testUnicodeFilesystemPathUtf8() {
#ifdef _WIN32
    const fs::path path = fs::path(L"C:\\pk2") /
                          L"\u062A\u062C\u0631\u0628\u0629" /
                          L"\u6D4B\u8BD5.txt";
    const auto text = pk2::pathUtf8(path);
    assert(text.find("C:\\pk2") != std::string::npos);
    assert(text.find("\xD8\xAA\xD8\xAC\xD8\xB1\xD8\xA8\xD8\xA9") != std::string::npos);
    assert(text.find("\xE6\xB5\x8B\xE8\xAF\x95.txt") != std::string::npos);
#endif
}

void testUnicodeFilesystemArchiveRoundTrip() {
#ifdef _WIN32
    const auto root = testRoot() / L"\u062A\u062C\u0631\u0628\u0629";
    const auto source = root / L"\u6D4B\u8BD5-source";
    const auto extracted = root / L"\u062E\u0631\u0648\u062C";
    const auto archivePath = root / L"\u0645\u0644\u0641.pk2";

    writeText(source / L"\u6D4B\u8BD5.txt", "unicode file");
    writeText(source / L"\u0641\u0631\u0639" / L"nested.txt", "unicode nested");

    auto archive = pk2::Pk2Archive::createNew("169841");
    archive.importFolder(source, "Unicode");
    archive.saveAs(archivePath);

    auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    assert(reopened.find("Unicode/\xE6\xB5\x8B\xE8\xAF\x95.txt").has_value());
    reopened.extract("Unicode", extracted, true, pk2::OverwritePolicy::Replace);
    assert(readText(extracted / "Unicode" / L"\u6D4B\u8BD5.txt") == "unicode file");
    assert(readText(extracted / "Unicode" / L"\u0641\u0631\u0639" / "nested.txt") == "unicode nested");

    fs::remove_all(root);
#endif
}

void testLegacyByteArchiveNameExtraction() {
#ifdef _WIN32
    const auto root = testRoot();
    const auto source = root / "source.txt";
    const auto archivePath = root / "legacy-byte-name.pk2";
    const auto extracted = root / "extracted";
    const std::string rawName = std::string("legacy_") + static_cast<char>(0xe9) + ".txt";

    writeText(source, "legacy byte name");

    auto archive = pk2::Pk2Archive::createNew("169841");
    archive.importFile(source, "Raw/" + rawName);
    archive.saveAs(archivePath);

    const auto reopened = pk2::Pk2Archive::open(archivePath, "169841");
    reopened.extract("Raw", extracted, true, pk2::OverwritePolicy::Replace);
    const auto expectedPath = extracted / "Raw" / pk2::archivePathPartToFilesystem(rawName);
    assert(readText(expectedPath) == "legacy byte name");

    fs::remove_all(root);
#endif
}

} // namespace

int main() {
    testMd5();
    testBlowfishKnownVector();
    testArchiveRoundTrip();
    testServerConfigRoundTrip();
    testMultiBlockArchiveRoundTrip();
    testCanonicalRootBlockWinsOverHeaderScanCandidate();
    testUnicodeFilesystemPathUtf8();
    testUnicodeFilesystemArchiveRoundTrip();
    testLegacyByteArchiveNameExtraction();
    return 0;
}
