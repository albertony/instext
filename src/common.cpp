#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <boost/nowide/utf/convert.hpp>

// Damaged text does not stop a conversion: what cannot be read becomes U+FFFD.
std::string to_utf8(std::u16string_view text) {
    return nowide::utf::convert_string<char>(text.data(), text.data() + text.size());
}

std::string shown(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

Bytes read_range(const fs::path& file, uint64_t offset, uint64_t count) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw Error("cannot open " + shown(file));
    Bytes data(static_cast<size_t>(count));
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (static_cast<size_t>(in.gcount()) != data.size()) throw Error("unexpected end of " + shown(file));
    return data;
}

void require_free_folder(const fs::path& folder, const std::string& named, bool force) {
    std::error_code ignored;
    if (!fs::exists(folder, ignored)) return;
    if (!fs::is_directory(folder, ignored)) throw UsageError("not a folder: " + named);
    if (!force && !fs::is_empty(folder, ignored))
        throw UsageError("the output folder is not empty: " + named + " (use --force to extract into it anyway)");
}

bool is_device_name(std::string_view utf8_name) {
    // What counts is the name up to its first dot or colon, without spaces at its end.
    std::string stem(utf8_name.substr(0, utf8_name.find_first_of(".:")));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (const std::string_view device : {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"})
        if (stem == device) return true;
    // COM and LPT with one digit, which may also be a superscript 1, 2 or 3 (two bytes in UTF-8).
    if (!stem.starts_with("COM") && !stem.starts_with("LPT")) return false;
    const std::string_view number = std::string_view(stem).substr(3);
    return (number.size() == 1 && number[0] >= '0' && number[0] <= '9') || number == "\xC2\xB9" ||
           number == "\xC2\xB2" || number == "\xC2\xB3";
}

std::string guard_device_names(std::string_view utf8_path) {
    std::string result;
    for (size_t start = 0; start <= utf8_path.size();) {
        const size_t end = std::min(utf8_path.find_first_of("\\/", start), utf8_path.size());
        const std::string_view part = utf8_path.substr(start, end - start);
        if (is_device_name(part)) result += '_';
        result += part;
        if (end < utf8_path.size()) result += utf8_path[end];
        start = end + 1;
    }
    return result;
}

fs::path safe_relpath(std::u16string_view name, bool drop_braces) {
    constexpr std::u16string_view invalid = u"<>:\"|?*";
    fs::path result;
    for (size_t start = 0; start <= name.size();) {
        const size_t end = std::min(name.find_first_of(u"\\/", start), name.size());
        std::u16string part;
        for (const char16_t c : name.substr(start, end - start))
            if (!drop_braces || (c != u'{' && c != u'}'))
                part += c < 0x20 || invalid.find(c) != invalid.npos ? u'_' : c;
        while (!part.empty() && (part.back() == u' ' || part.back() == u'.')) part.pop_back();
        if (is_device_name(to_utf8(part))) part.insert(part.begin(), u'_');
        if (!part.empty()) result /= fs::path(part);
        start = end + 1;
    }
    return result.empty() ? fs::path("_unnamed") : result;
}

std::string OutputNames::claim(const std::string& path, uint64_t owner) {
    // A first folder or file name that is the name of the folder for --internals gets ",2".
    const size_t end = std::min(path.find_first_of("/\\"), path.size());
    const bool reserved = std::ranges::equal(path.substr(0, end), kInternals, [](unsigned char a, char b) {
        return (a < 0x80 ? std::tolower(a) : a) == b;
    });
    return unique(reserved ? path.substr(0, end) + ",2" + path.substr(end) : path, owner);
}

fs::path OutputNames::internal(const fs::path& path, uint64_t owner) {
    const std::string given = unique(shown(fs::path(kInternals) / path), owner);
    return fs::path(std::u8string(given.begin(), given.end()));
}

std::string OutputNames::unique(const std::string& path, uint64_t owner) {
    // The extension starts at the last dot of the last component, unless that dot is its first character.
    const size_t name = path.find_last_of("/\\") + 1;
    size_t dot = path.rfind('.');
    if (dot == path.npos || dot <= name) dot = path.size();
    std::string candidate = path;
    for (int n = 2;; ++n) {
        std::string lower = candidate;
        std::ranges::transform(lower, lower.begin(),
                               [](unsigned char c) { return static_cast<char>(c < 0x80 ? std::tolower(c) : c); });
        if (taken_.try_emplace(lower, owner).first->second == owner) return candidate;
        candidate = path.substr(0, dot) + "," + std::to_string(n) + path.substr(dot);
    }
}

fs::path OutputNames::claim(const fs::path& path, uint64_t owner) {
    const std::string asked = shown(path), given = claim(asked, owner);
    return given == asked ? path : fs::path(std::u8string(given.begin(), given.end()));
}

fs::path safe_relpath(std::string_view utf8_name) {
    return safe_relpath(nowide::utf::convert_string<char16_t>(utf8_name.data(), utf8_name.data() + utf8_name.size()));
}
