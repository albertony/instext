#include "hash.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace {

struct Named {
    Hasher::Kind kind = Hasher::Kind::md5;
    std::string_view prefix, name;
    size_t digits = 0;
};
constexpr Named kinds[] = {{Hasher::Kind::md5, "md5", "MD5", 32},
                           {Hasher::Kind::sha1, "sha1", "SHA-1", 40},
                           {Hasher::Kind::sha256, "sha256", "SHA-256", 64},
                           {Hasher::Kind::sha512, "sha512", "SHA-512", 128}};

}  // namespace

Hasher::Hasher(Kind kind) {
    switch (kind) {
        case Kind::md5: break;  // what the state is to begin with
        case Kind::sha1: state_.emplace<boost::hash2::sha1_160>(); break;
        case Kind::sha256: state_.emplace<boost::hash2::sha2_256>(); break;
        case Kind::sha512: state_.emplace<boost::hash2::sha2_512>(); break;
    }
}

void Hasher::update(const void* data, size_t size) {
    std::visit([&](auto& state) { state.update(data, size); }, state_);
}

std::string Hasher::hex() {
    return std::visit(
        [](auto& state) {
            const auto digest = state.result();
            std::string text;
            for (size_t i = 0; i < digest.size(); ++i) {
                text += "0123456789abcdef"[digest.data()[i] >> 4];
                text += "0123456789abcdef"[digest.data()[i] & 15];
            }
            return text;
        },
        state_);
}

std::string verify_checksum(const fs::path& file, std::string given) {
    std::ranges::transform(given, given.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const size_t colon = given.find(':');
    std::string prefix = colon == given.npos ? "" : given.substr(0, colon);
    std::erase(prefix, '-');  // "sha-256" is "sha256"
    const std::string digits = given.substr(colon == given.npos ? 0 : colon + 1);
    const auto kind = std::ranges::find_if(
        kinds, [&](const Named& k) { return prefix.empty() ? digits.size() == k.digits : prefix == k.prefix; });
    if (kind == std::end(kinds) || digits.size() != kind->digits ||
        digits.find_first_not_of("0123456789abcdef") != digits.npos)
        throw UsageError("a checksum is the 32, 40, 64 or 128 hexadecimal digits of an MD5, SHA-1, SHA-256 or "
                         "SHA-512, alone or behind its kind as in sha256:...");

    Hasher hasher(kind->kind);
    std::ifstream in(file, std::ios::binary);
    if (!in) throw Error("cannot open " + shown(file));
    std::vector<char> block(1 << 20);
    for (;;) {
        in.read(block.data(), static_cast<std::streamsize>(block.size()));
        const std::streamsize count = in.gcount();
        if (count <= 0) break;
        hasher.update(block.data(), static_cast<size_t>(count));
    }
    if (in.bad()) throw Error("cannot read " + shown(file));
    const std::string actual = hasher.hex();
    if (actual != digits)
        throw Error("the " + std::string(kind->name) + " of the file is " + actual + ", not " + digits);
    return std::string(kind->name);
}
