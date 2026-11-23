// The checksums this tool computes: one state for any of the four kinds, and the comparison of a
// file with a checksum from the command line.
#pragma once

#include "common.hpp"

#include <variant>

#include <boost/hash2/md5.hpp>
#include <boost/hash2/sha1.hpp>
#include <boost/hash2/sha2.hpp>

class Hasher {
public:
    enum class Kind { md5, sha1, sha256, sha512 };
    explicit Hasher(Kind kind);
    void update(const void* data, size_t size);
    std::string hex();  // the checksum of all that was given, in lower case; to be asked for once, at the end

private:
    std::variant<boost::hash2::md5_128, boost::hash2::sha1_160, boost::hash2::sha2_256, boost::hash2::sha2_512> state_;
};

// Compares the file with a checksum from the command line: the hexadecimal digits of its MD5,
// SHA-1, SHA-256 or SHA-512, on their own or behind the kind and a colon, as in "sha256:...".
// Without a kind the number of digits tells it. Returns the name of the kind, such as "SHA-256".
// A file that does not match is an Error; a text that cannot be a checksum is a UsageError. This
// is done before anything in the file is interpreted, so a file that is not the expected one is
// refused unread.
std::string verify_checksum(const fs::path& file, std::string given);
