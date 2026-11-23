// Test helper: writes a small WiX Burn bundle.
//   makeburn <target> <sha1|sha256|sha512> <payload name>...
// The bundle has the container of the bootstrapper application, which holds only the manifest,
// and one attached container with the payloads. Payload number n holds the text "payload n" and
// has a hash of the given kind in the manifest: SHA-1 as in WiX 3, SHA-512 as in WiX 4 and later.
// Names may repeat: payloads that share a name must not overwrite one another. A payload whose
// name ends in ".msi" starts with the signature of a compound file instead, so that it is taken
// for a Windows Installer package that then cannot be read. A name can start with where the
// payload is, if not in the bundle: "external:" for a file of its own, "download:" for such a file
// with an address to fetch it from, "detached:" for a place in a container that is a file of its
// own, made.cab. None of those files is written. The cabinets are not compressed. The layout is
// the one src/burn.cpp describes.
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/hash2/sha1.hpp>
#include <boost/hash2/sha2.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>

namespace {

template <class T>
void put(std::string& out, T value) {  // numbers are stored with the low byte first
    for (size_t byte = 0; byte < sizeof(T); ++byte)
        out += static_cast<char>(static_cast<uint64_t>(value) >> (8 * byte));
}

// The checksum of a block of cabinet data: its 32-bit words combined with exclusive or, and what
// is left over at the end read with the high byte first.
uint32_t checksum(std::string_view bytes, uint32_t sum) {
    const auto at = [&](size_t index) { return static_cast<uint32_t>(static_cast<unsigned char>(bytes[index])); };
    size_t index = 0;
    for (; index + 4 <= bytes.size(); index += 4)
        sum ^= at(index) | at(index + 1) << 8 | at(index + 2) << 16 | at(index + 3) << 24;
    uint32_t rest = 0;
    for (; index < bytes.size(); ++index) rest = rest << 8 | at(index);
    return sum ^ rest;
}

// A cabinet with one folder, whose files are stored as they are.
std::string cabinet(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string entries, data;
    for (const auto& [name, content] : files) {
        put(entries, static_cast<uint32_t>(content.size()));
        put(entries, static_cast<uint32_t>(data.size()));  // where the file starts in the folder
        put(entries, uint16_t{0});                         // the folder
        put(entries, uint16_t{0x5B78});                    // date: 2025-11-24
        put(entries, uint16_t{0x6000});                    // time: 12:00
        put(entries, uint16_t{0x20});                      // attributes
        entries += name;
        entries += '\0';
        data += content;
    }
    std::string blocks;
    uint16_t count = 0;
    for (size_t start = 0; start < data.size(); start += 32768, ++count) {
        const std::string block = data.substr(start, 32768);
        std::string sizes;
        put(sizes, static_cast<uint16_t>(block.size()));  // stored
        put(sizes, static_cast<uint16_t>(block.size()));  // unpacked
        put(blocks, checksum(sizes, checksum(block, 0)));
        blocks += sizes + block;
    }
    const uint32_t first_entry = 36 + 8, first_block = first_entry + static_cast<uint32_t>(entries.size());
    std::string out("MSCF");
    put(out, uint32_t{0});
    put(out, first_block + static_cast<uint32_t>(blocks.size()));  // size of the cabinet
    put(out, uint32_t{0});
    put(out, first_entry);
    put(out, uint32_t{0});
    put(out, uint8_t{3});  // format version 1.3
    put(out, uint8_t{1});
    put(out, uint16_t{1});  // folders
    put(out, static_cast<uint16_t>(files.size()));
    put(out, uint16_t{0});       // flags
    put(out, uint16_t{0x1234});  // set id
    put(out, uint16_t{0});       // number in the set
    put(out, first_block);       // the folder: where its data starts, how many blocks, not compressed
    put(out, count);
    put(out, uint16_t{0});
    return out + entries + blocks;
}

template <class Hash>
std::string hex(const std::string& data) {
    Hash hash;
    hash.update(data.data(), data.size());
    const auto digest = hash.result();
    std::string out;
    for (size_t index = 0; index < digest.size(); ++index) {
        out += "0123456789ABCDEF"[digest.data()[index] >> 4];
        out += "0123456789ABCDEF"[digest.data()[index] & 15];
    }
    return out;
}

std::string quoted(std::string_view text) {  // as the value of an XML attribute
    std::string out = "\"";
    for (const char c : text) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '"')
            out += "&quot;";
        else
            out += c;
    }
    return out + '"';
}

}  // namespace

int main(int argc, char** argv) {
    const boost::nowide::args utf8_arguments(argc, argv);
    const std::string_view kind = argc > 2 ? argv[2] : "";
    if (argc < 4 || (kind != "sha1" && kind != "sha256" && kind != "sha512")) {
        boost::nowide::cerr << "usage: makeburn <target> <sha1|sha256|sha512> "
                               "[external:|download:|detached:]<payload name>...\n";
        return 2;
    }

    std::vector<std::pair<std::string, std::string>> payloads;  // name in the container, content
    std::string manifest = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                           "<BurnManifest xmlns=\"http://schemas.microsoft.com/wix/2008/Burn\">\n"
                           "<UX />\n"
                           "<Container Id=\"Attached\" FilePath=\"made.exe\" AttachedIndex=\"1\" Attached=\"yes\" />\n";
    std::string chain;
    bool detached = false;
    for (int index = 3; index < argc; ++index) {
        const std::string number = std::to_string(index - 3);
        std::string_view name = argv[index], where;
        for (const std::string_view prefix : {"external:", "download:", "detached:"})
            if (name.starts_with(prefix)) where = prefix, name.remove_prefix(prefix.size());
        const bool package = name.ends_with(".msi");
        const std::string content = (package ? "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1" : "") + ("payload " + number + "\n");
        const std::string hash = kind == "sha1"     ? hex<boost::hash2::sha1_160>(content)
                                 : kind == "sha256" ? hex<boost::hash2::sha2_256>(content)
                                                    : hex<boost::hash2::sha2_512>(content);
        manifest += "<Payload Id=\"p" + number + "\" FilePath=" + quoted(name) + " FileSize=\"" +
                    std::to_string(content.size()) + "\" Hash=\"" + hash + "\"";
        if (where == "external:" || where == "download:")
            manifest += " Packaging=\"external\"";
        else
            manifest += " Packaging=\"embedded\" SourcePath=\"a" + number + "\" Container=\"" +
                        (where.empty() ? "Attached" : "Detached") + "\"";
        manifest += where == "download:" ? " DownloadUrl=\"https://example.invalid/\" />\n" : " />\n";
        chain += "<MsiPackage Id=\"p" + number + "\"><PayloadRef Id=\"p" + number + "\" /></MsiPackage>";
        if (where.empty()) payloads.emplace_back("a" + number, content);
        detached |= where == "detached:";
    }
    if (detached) manifest += "<Container Id=\"Detached\" FilePath=\"made.cab\" Attached=\"no\" />\n";
    manifest += "<Registration><Arp DisplayName=\"Made-up Bundle\" DisplayVersion=\"1.0.0\" /></Registration>\n"
                "<Chain>" + chain + "</Chain>\n</BurnManifest>\n";
    const std::string first = cabinet({{"0", manifest}}), second = cabinet(payloads);

    // The engine: just enough of a program for its section table to be found. It has one section,
    // ".wixburn", whose content says where the two containers lie.
    const uint32_t header = 0x40, section = header + 24 + 40, section_size = 48 + 2 * 4;
    std::string file("MZ");
    file.resize(0x3C, '\0');
    put(file, header);
    file.append("PE\0\0", 4);
    put(file, uint16_t{0x8664});  // for x64
    put(file, uint16_t{1});       // sections
    file.append(12, '\0');
    put(file, uint16_t{0});  // no optional header
    put(file, uint16_t{0x22});
    file += ".wixburn";
    put(file, section_size);
    put(file, uint32_t{0x1000});
    put(file, section_size);  // size and place of the section in the file
    put(file, section);
    file.append(12, '\0');
    put(file, uint32_t{0x40000040});
    put(file, uint32_t{0x00F14300});  // the section: magic number, version, bundle code,
    put(file, uint32_t{2});
    file.append(16, '\x11');
    put(file, section + section_size);  // size of the engine,
    file.append(12, '\0');              // checksum and signature: none,
    put(file, uint32_t{1});             // containers are cabinets,
    put(file, uint32_t{2});             // their number and sizes
    put(file, static_cast<uint32_t>(first.size()));
    put(file, static_cast<uint32_t>(second.size()));
    file += first + second;

    boost::nowide::ofstream out(argv[1], std::ios::binary);
    out.write(file.data(), static_cast<std::streamsize>(file.size()));
    out.close();
    if (!out) {
        boost::nowide::cerr << "makeburn: cannot write " << argv[1] << '\n';
        return 1;
    }
    return 0;
}
