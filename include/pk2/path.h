#pragma once

#include <filesystem>
#include <string>

namespace pk2 {

std::string normalizeArchivePath(const std::string& path);
std::string joinArchivePath(const std::string& left, const std::string& right);
std::string archiveFileName(const std::string& path);
std::string archiveParentPath(const std::string& path);
std::wstring widenUtf8(const std::string& text);
std::string narrowUtf8(const std::wstring& text);
std::string pathUtf8(const std::filesystem::path& path);
std::string pathGenericUtf8(const std::filesystem::path& path);
std::string pathFileNameUtf8(const std::filesystem::path& path);
std::filesystem::path archivePathPartToFilesystem(const std::string& part);

} // namespace pk2
