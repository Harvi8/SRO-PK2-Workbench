#include "pk2/path.h"

#include <algorithm>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <codecvt>
#include <locale>
#endif
#include <stdexcept>
#include <sstream>
#include <vector>

namespace pk2 {

std::string normalizeArchivePath(const std::string& path) {
    std::string converted = path;
    std::replace(converted.begin(), converted.end(), '\\', '/');

    std::vector<std::string> parts;
    std::stringstream stream(converted);
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
            continue;
        }
        parts.push_back(part);
    }

    std::string result;
    for (const auto& item : parts) {
        if (!result.empty()) {
            result += '/';
        }
        result += item;
    }
    return result;
}

std::string joinArchivePath(const std::string& left, const std::string& right) {
    const auto lhs = normalizeArchivePath(left);
    const auto rhs = normalizeArchivePath(right);
    if (lhs.empty()) {
        return rhs;
    }
    if (rhs.empty()) {
        return lhs;
    }
    return lhs + "/" + rhs;
}

std::string archiveFileName(const std::string& path) {
    const auto normalized = normalizeArchivePath(path);
    const auto slash = normalized.find_last_of('/');
    if (slash == std::string::npos) {
        return normalized;
    }
    return normalized.substr(slash + 1);
}

std::string archiveParentPath(const std::string& path) {
    const auto normalized = normalizeArchivePath(path);
    const auto slash = normalized.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    return normalized.substr(0, slash);
}

std::wstring widenUtf8(const std::string& text) {
#ifdef _WIN32
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8,
                                         MB_ERR_INVALID_CHARS,
                                         text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr,
                                         0);
    if (size <= 0) {
        throw std::runtime_error("Invalid UTF-8 text.");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        size);
    return result;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.from_bytes(text);
#endif
}

std::string narrowUtf8(const std::wstring& text) {
#ifdef _WIN32
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8,
                                         WC_ERR_INVALID_CHARS,
                                         text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr,
                                         0,
                                         nullptr,
                                         nullptr);
    if (size <= 0) {
        throw std::runtime_error("Invalid UTF-16 text.");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        size,
                        nullptr,
                        nullptr);
    return result;
#else
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    return converter.to_bytes(text);
#endif
}

std::string pathUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    return narrowUtf8(path.wstring());
#else
    return path.u8string();
#endif
}

std::string pathGenericUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    return narrowUtf8(path.generic_wstring());
#else
    return path.generic_u8string();
#endif
}

std::string pathFileNameUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    return narrowUtf8(path.filename().wstring());
#else
    return path.filename().u8string();
#endif
}

std::filesystem::path archivePathPartToFilesystem(const std::string& part) {
#ifdef _WIN32
    try {
        return std::filesystem::path(widenUtf8(part));
    } catch (...) {
    }

    if (part.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_ACP,
                                         0,
                                         part.data(),
                                         static_cast<int>(part.size()),
                                         nullptr,
                                         0);
    if (size > 0) {
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_ACP,
                            0,
                            part.data(),
                            static_cast<int>(part.size()),
                            result.data(),
                            size);
        return std::filesystem::path(result);
    }

    std::wstring sanitized;
    sanitized.reserve(part.size());
    for (const auto ch : part) {
        const auto byte = static_cast<unsigned char>(ch);
        sanitized.push_back(byte < 32 ? L'_' : static_cast<wchar_t>(byte));
    }
    return std::filesystem::path(sanitized);
#else
    return std::filesystem::u8path(part);
#endif
}

} // namespace pk2
