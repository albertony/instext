// WiX Burn bundles: the setup.exe bootstrappers that carry MSI and other packages.
//
// Layout, from WiX's src/burn/engine/section.cpp (the same in WiX 3 and later):
//   engine        an ordinary EXE that has a ".wixburn" section describing the rest
//   container 0   a cabinet with the bootstrapper application: files "0", "u0", "u1", ...
//   signature     of the engine, if it was signed
//   container 1+  cabinets with the payloads: files "a0", "a1", ...
//   signature     of the whole bundle
// File "0" of container 0 is an XML manifest. It maps those names to real ones
// and records the size and hash of every payload. This is what
// "wix burn extract" unpacks.
#include "hash.hpp"
#include "report.hpp"
#include "sevenzip.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <streambuf>

#include <pugixml.hpp>

namespace {

struct SectionHeader {  // BURN_SECTION_HEADER; one 32-bit size per container follows it
    uint32_t magic, version;
    uint8_t bundle_code[16];
    uint32_t stub_size, original_checksum, signature_offset, signature_size, format, containers;
};
static_assert(sizeof(SectionHeader) == 48);
constexpr uint32_t kMagic = 0x00F14300, kCabinet = 1;

struct Range {
    uint64_t offset = 0, size = 0;
};

// Finds the ".wixburn" section and returns where each container lies in the file.
std::optional<std::vector<Range>> read_layout(const fs::path& file) {
    const uint64_t file_size = fs::file_size(file);
    if (file_size < 0x40) return std::nullopt;
    const Bytes dos = read_range(file, 0, 0x40);
    const uint64_t pe = pod<uint32_t>(View(dos).subspan(0x3C));
    if (chars(dos).substr(0, 2) != "MZ" || pe + 24 > file_size) return std::nullopt;
    const Bytes coff = read_range(file, pe, 24);
    if (chars(coff).substr(0, 4) != std::string_view("PE\0\0", 4)) return std::nullopt;
    const uint64_t count = pod<uint16_t>(View(coff).subspan(6)), table = pe + 24 + pod<uint16_t>(View(coff).subspan(20));
    if (table + count * 40 > file_size) return std::nullopt;
    const Bytes sections = read_range(file, table, count * 40);
    for (size_t i = 0; i < count; ++i) {
        const View section = View(sections).subspan(i * 40, 40);
        if (chars(section).substr(0, 8) != ".wixburn") continue;
        const uint64_t raw_size = pod<uint32_t>(section.subspan(16)), raw_at = pod<uint32_t>(section.subspan(20));
        if (raw_size < sizeof(SectionHeader) || raw_at + raw_size > file_size) throw Error("corrupt .wixburn section");
        const Bytes raw = read_range(file, raw_at, raw_size);
        const auto header = pod<SectionHeader>(raw);
        if (header.magic != kMagic) throw Error("unexpected magic number in the .wixburn section");
        if (header.format != kCabinet)
            throw Error("Burn container format " + std::to_string(header.format) + " is not supported (only cabinets)");
        if (header.containers == 0 || sizeof(SectionHeader) + header.containers * 4ull > raw.size())
            throw Error("corrupt .wixburn section");
        const auto size_of = [&](uint32_t n) { return pod<uint32_t>(View(raw).subspan(sizeof(SectionHeader) + n * 4)); };
        // Container 0 follows the engine; the others follow the engine's signature.
        std::vector<Range> layout{{header.stub_size, size_of(0)}};
        uint64_t next = header.signature_offset ? uint64_t{header.signature_offset} + header.signature_size
                                                : uint64_t{header.stub_size} + size_of(0);
        for (uint32_t n = 1; n < header.containers; ++n) {
            layout.push_back({next, size_of(n)});
            next += size_of(n);
        }
        return layout;
    }
    return std::nullopt;
}

// A seekable, read-only view of one byte range of a file, so that 7-Zip can
// open a cabinet in the middle of the bundle.
class RangeBuffer final : public std::streambuf {
public:
    RangeBuffer(const fs::path& file, Range range) : in_(file, std::ios::binary), range_(range), buffer_(1 << 16) {
        if (!in_) throw Error("cannot open " + shown(file));
        setg(buffer_.data(), buffer_.data(), buffer_.data());
    }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        if (end_ >= range_.size) return traits_type::eof();
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(range_.offset + end_));
        in_.read(buffer_.data(), static_cast<std::streamsize>(std::min<uint64_t>(buffer_.size(), range_.size - end_)));
        const std::streamsize got = in_.gcount();
        if (got <= 0) return traits_type::eof();
        end_ += static_cast<uint64_t>(got);
        setg(buffer_.data(), buffer_.data(), buffer_.data() + got);
        return traits_type::to_int_type(*gptr());
    }
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode) override {
        const off_type start = static_cast<off_type>(end_) - (egptr() - eback());  // range position of the buffer
        const off_type base = direction == std::ios_base::beg   ? 0
                              : direction == std::ios_base::cur ? start + (gptr() - eback())
                                                                : static_cast<off_type>(range_.size);
        const off_type target = base + offset;
        if (target < 0 || static_cast<uint64_t>(target) > range_.size) return pos_type(off_type(-1));
        if (target >= start && target <= static_cast<off_type>(end_)) {
            setg(eback(), eback() + (target - start), egptr());  // still inside the buffer
        } else {
            end_ = static_cast<uint64_t>(target);
            setg(buffer_.data(), buffer_.data(), buffer_.data());
        }
        return pos_type(target);
    }
    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
        return seekoff(off_type(position), std::ios_base::beg, mode);
    }

private:
    std::ifstream in_;
    Range range_;
    std::vector<char> buffer_;
    uint64_t end_ = 0;  // range position just past the buffered bytes
};

// Receives a payload from 7-Zip: hashes it and, unless testing, stores it.
class Sink final : public std::streambuf {
public:
    Sink(Hasher& hasher, std::ofstream* file) : hasher_(hasher), file_(file) {}
    uint64_t size = 0;

protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        hasher_.update(data, static_cast<size_t>(count));
        if (file_) file_->write(data, count);
        size += static_cast<uint64_t>(count);
        return count;
    }
    int_type overflow(int_type value) override {
        if (!traits_type::eq_int_type(value, traits_type::eof())) {
            const char byte = traits_type::to_char_type(value);
            xsputn(&byte, 1);
        }
        return traits_type::not_eof(value);
    }
    // bit7z measures each write with tellp(); without an answer it reports
    // "0 bytes written" to 7-Zip, which then retries the same write forever.
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode) override {
        return offset == 0 && direction == std::ios_base::cur ? pos_type(static_cast<off_type>(size))
                                                              : pos_type(off_type(-1));
    }

private:
    Hasher& hasher_;
    std::ofstream* file_;
};

struct Payload {
    std::string id, name, source, container, hash, packaging;  // as the manifest gives them: see read_payload()
    uint64_t size = 0;
    bool has_size = false;  // bootstrapper files are listed without size and hash in WiX 4 and later
    bool download = false;  // the manifest gives an address to fetch it from
    fs::path target;        // where it is written, below the output folder
};

Payload read_payload(const pugi::xml_node& node) {
    return {.id = node.attribute("Id").value(),
            .name = node.attribute("FilePath").value(),
            .source = node.attribute("SourcePath").value(),
            .container = node.attribute("Container").value(),
            .hash = node.attribute("Hash").value(),
            .packaging = node.attribute("Packaging").value(),
            .size = node.attribute("FileSize").as_ullong(),
            .has_size = !node.attribute("FileSize").empty(),
            .download = !node.attribute("DownloadUrl").empty(),
            .target = {}};
}

// Extracts cabinet item `index` to `target` (or nowhere when testing) and
// checks it against the size and hash in the manifest, where it gives them.
void unpack(const bit7z::BitArchiveReader& cabinet, uint32_t index, const Payload& payload, const fs::path& target,
            bool test) {
    // The manifest gives a SHA-1 in WiX 3 and a SHA-512 in WiX 4 and later; the length of the hash
    // says which it is.
    const size_t digits = payload.hash.size();
    Hasher hasher(digits == 64 ? Hasher::Kind::sha256 : digits == 128 ? Hasher::Kind::sha512 : Hasher::Kind::sha1);
    std::ofstream file;
    if (!test) {
        fs::create_directories(target.parent_path());
        file.open(target, std::ios::binary);
        if (!file) throw Error("cannot create " + shown(target));
    }
    Sink sink(hasher, test ? nullptr : &file);
    std::ostream out(&sink);
    cabinet.extractTo(out, index);
    if (!test) {
        file.close();
        if (!file) throw Error("cannot write " + shown(target));
        using namespace std::chrono;
        fs::last_write_time(target, time_point_cast<fs::file_time_type::duration>(
                                        clock_cast<file_clock>(cabinet.itemAt(index).lastWriteTime())));
    }
    std::string expected = payload.hash;
    std::ranges::transform(expected, expected.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if ((payload.has_size && sink.size != payload.size) || (!expected.empty() && hasher.hex() != expected))
        throw Error(payload.name + " does not match the size and hash in the bundle's manifest");
}

}  // namespace

namespace burn {

bool detect(const fs::path& file) { return read_layout(file).has_value(); }

void run(const Options& options) {
    const std::vector<Range> layout = read_layout(options.input).value();
    // Every container that the engine's section lists has to be in the file in full. A bundle that
    // was cut short would otherwise only seem to carry fewer payloads.
    const uint64_t file_size = fs::file_size(options.input);
    uint64_t needed = 0;
    for (const Range& range : layout) needed = std::max(needed, range.offset + range.size);
    if (needed > file_size)
        throw Error("the bundle is truncated (" + std::to_string(file_size) + " of at least " +
                    std::to_string(needed) + " bytes)");
    const bit7z::Bit7zLibrary& engine = sevenzip(options);
    const auto open = [&](const Range& range, std::unique_ptr<RangeBuffer>& buffer, std::unique_ptr<std::istream>& stream) {
        buffer = std::make_unique<RangeBuffer>(options.input, range);
        stream = std::make_unique<std::istream>(buffer.get());
        return std::make_unique<bit7z::BitArchiveReader>(engine, *stream, bit7z::BitFormat::Cab);
    };

    // The bootstrapper container and, in it, the manifest.
    std::unique_ptr<RangeBuffer> ux_buffer;
    std::unique_ptr<std::istream> ux_stream;
    const auto ux = open(layout[0], ux_buffer, ux_stream);
    std::map<std::string, uint32_t> ux_items;
    for (const auto& item : *ux) ux_items[item.path()] = item.index();
    if (!ux_items.count("0")) throw Error("the bundle has no manifest");
    Bytes manifest_bytes;
    ux->extractTo(manifest_bytes, ux_items["0"]);
    pugi::xml_document manifest;
    if (!manifest.load_buffer(manifest_bytes.data(), manifest_bytes.size())) throw Error("cannot read the bundle's manifest");
    const pugi::xml_node root = manifest.document_element();

    // WiX has two pairs of words here. A payload is "embedded", in a container, or "external", a
    // file of its own. A container is "attached", a part of the bundle, or "detached", a cabinet
    // file of its own. What this tool calls embedded, as for the other formats, is what the bundle
    // itself holds: an embedded payload in an attached container.
    std::map<std::string, uint32_t> attached;     // container id -> index in the layout
    std::map<std::string, std::string> detached;  // container id -> the name of its file
    for (const pugi::xml_node node : root.children("Container")) {
        const std::string id = node.attribute("Id").value();
        if (node.attribute("AttachedIndex"))
            attached[id] = node.attribute("AttachedIndex").as_uint();
        else
            detached[id] = node.attribute("FilePath").value();
    }
    std::vector<Payload> payloads, skipped;
    std::set<std::string> held;  // the ids of the payloads that are in the bundle
    uint64_t bytes = 0;
    for (const pugi::xml_node node : root.children("Payload")) {
        const Payload payload = read_payload(node);
        const auto container = attached.find(payload.container);
        const bool inside = payload.packaging == "embedded" && container != attached.end();
        if (inside && container->second >= layout.size())
            throw Error("the manifest places " + payload.name + " in attached container " +
                        std::to_string(container->second) + ", which the bundle does not have");
        (inside ? payloads : skipped).push_back(payload);
        if (inside) held.insert(payload.id), bytes += payload.size;
    }
    // The bootstrapper application, the program that shows the bundle's user interface: its files
    // are payloads too, in the first container, next to the manifest. What -i writes of it is the
    // manifest and those of its files that this container holds.
    std::vector<Payload> ux_payloads;
    for (const pugi::xml_node node : root.child("UX").children("Payload"))
        if (Payload payload = read_payload(node); ux_items.contains(payload.source))
            ux_payloads.push_back(std::move(payload));
    // Every file that is written gets a path of its own: payloads that share a name get ",2", ",3", ...
    OutputNames names;
    uint64_t owner = 0;
    names.internal("manifest.xml", owner++);  // in the folder for -i, where it comes first
    for (Payload& payload : payloads) payload.target = names.claim(safe_relpath(payload.name), owner++);

    const pugi::xml_node arp = root.child("Registration").child("Arp");
    const std::string product =
        std::string(arp.attribute("DisplayName").value()) + ' ' + arp.attribute("DisplayVersion").value();
    // What the bundle is, what it consists of in the order of the file (the engine, which is the
    // program at its front, the bootstrapper application, the containers) and what it carries.
    // Payloads are its files. Packages are what its chain would install or run, in this order, each
    // from one or more of the payloads; they are listed by the name that the bundle's author gave
    // them, with their kind and the number of their payloads. All of that is for --verbose and
    // comes first, so that the report ends as it does without it: what there is to extract, what
    // is not there, and what was done.
    info(options) << heading(product, kind_name(Format::burn)) << '\n';
    detail(options) << "Burn engine " << printable(root.attribute("EngineVersion").as_string("3.x")) << '\n'
                    << "Bootstrapper application with " << amount(ux_payloads.size() + 1, "internal file") << '\n'
                    << amount(layout.size() - 1, "attached container")
                    << (detached.empty() ? "" : ", " + amount(detached.size(), "detached container")) << '\n';
    std::vector<std::string> chain;
    for (const pugi::xml_node package : root.child("Chain").children()) {
        size_t all = 0, elsewhere = 0;
        for (const pugi::xml_node reference : package.children("PayloadRef")) {
            ++all;
            elsewhere += !held.contains(reference.attribute("Id").value());
        }
        // "core.msi (MsiPackage, 2 payloads)", and after that ", not embedded" or ", 1 not embedded"
        std::string about = printable(package.name());
        if (all != 0) about += ", " + amount(all, "payload");
        if (all != 0 && elsewhere == all)
            about += ", not embedded";
        else if (elsewhere != 0)
            about += ", " + std::to_string(elsewhere) + " not embedded";
        chain.push_back(printable(package.attribute("Id").value()) + " (" + about + ')');
    }
    detail(options) << listed("Install chain:", chain);
    info(options) << contents(options, payloads.size(), "payload", bytes)
                  << (skipped.empty() ? "" : "; " + std::to_string(skipped.size()) + " not embedded") << '\n';
    std::vector<std::string> outside;  // the payloads that are not in the bundle, each with where it is instead
    for (const Payload& payload : skipped) {
        std::string where = payload.download ? "external: downloaded or shipped next to the bundle"
                                             : "external: shipped next to the bundle";
        if (payload.packaging == "embedded") {
            const auto file = detached.find(payload.container);
            where = "in the detached container " + printable(file != detached.end() ? file->second : payload.container);
        }
        outside.push_back(printable(shown(safe_relpath(payload.name))) + " (" + where + ')');  // as in the listing
    }
    info(options) << listed("Not embedded:", outside);
    if (options.list) {
        for (const Payload& payload : payloads)
            nowide::cout << padded(payload.size) << "  " << printable(shown(payload.target)) << '\n';
        return;
    }
    info(options) << std::flush;  // the report so far is to be seen while the work goes on

    std::vector<fs::path> written;
    const auto take = [&](const bit7z::BitArchiveReader& cabinet, uint32_t index) {  // all payloads of one container
        std::map<std::string, uint32_t> items;
        for (const auto& item : cabinet) items[item.path()] = item.index();
        for (const Payload& payload : payloads) {
            if (attached.at(payload.container) != index) continue;
            const auto item = items.find(payload.source);
            if (item == items.end()) throw Error("payload " + payload.name + " is missing from its container");
            const fs::path target = options.outdir / payload.target;
            unpack(cabinet, item->second, payload, target, options.test);
            written.push_back(target);
        }
    };
    take(*ux, 0);  // the bootstrapper's container, where payloads are only in an unusual bundle
    for (uint32_t index = 1; index < layout.size(); ++index) {
        std::unique_ptr<RangeBuffer> buffer;
        std::unique_ptr<std::istream> stream;
        take(*open(layout[index], buffer, stream), index);
    }
    info(options) << outcome(options, written.size(), payloads.size(), "payload",
                             "all sizes and hashes match the manifest")
                  << '\n';

    if (options.internals && !options.test) {  // the bootstrapper application, under its real names
        const fs::path folder = options.outdir / kInternals;
        fs::create_directories(folder);
        std::ofstream(folder / "manifest.xml", std::ios::binary)
            .write(reinterpret_cast<const char*>(manifest_bytes.data()), static_cast<std::streamsize>(manifest_bytes.size()));
        for (const Payload& payload : ux_payloads) {
            const fs::path target = names.internal(safe_relpath(payload.name), owner++);
            unpack(*ux, ux_items.at(payload.source), payload, options.outdir / target, false);
        }
        info(options) << wrote_internals(options, ux_payloads.size() + 1) << '\n';
    }

    if (options.recurse && !options.test) {  // installers among the payloads
        struct Nested {
            Options options;
            Format format = Format::unknown;
            std::string name;
        };
        std::vector<Nested> installers;
        size_t failures = 0;
        const auto failed = [&](const std::string& name, const std::exception& error) {
            nowide::cerr << "error: " << name << ": " << printable(error.what()) << '\n';
            ++failures;
        };
        std::vector<std::string> found;  // what every payload turned out to be
        for (const fs::path& payload : written) {
            Options nested = options;
            nested.input = payload;
            (nested.outdir = payload) += ".extracted";
            const std::string name = printable(shown(payload.lexically_relative(options.outdir)));
            try {
                const Format format = detect(nested);
                // Two kinds of payload are left as they are: what is no installer that is read here, be
                // it an ordinary file or a setup program of another kind, and a plain archive, such as
                // the cabinet of an MSI package, whose files that package's own extraction already places.
                const std::string_view kind = format == Format::unknown   ? "nothing that InstExt reads"
                                              : format == Format::archive ? "an archive, left as it is"
                                                                          : kind_name(format);
                found.push_back(name + " (" + std::string(kind) + ')');
                if (format != Format::unknown && format != Format::archive)
                    installers.push_back({nested, format, name});
            } catch (const std::exception& error) {
                failed(name, error);
            }
        }
        detail(options) << listed("Looking for installers among the " + amount(written.size(), "payload") + ':', found);
        for (const Nested& nested : installers) {
            try {
                // Its folder lies in a folder that was new, so only a payload of this bundle can have
                // put something there. That is as little to be written over as anything else.
                require_free_folder(nested.options.outdir, place(nested.options, nested.options.outdir),
                                    nested.options.force);
                // Its report follows, set apart by an empty line. The folder in its last line says which
                // file it is about, and with --verbose its first line does.
                info(options) << '\n';
                detail(options) << reading(nested.options) << std::endl;
                ::run(nested.format, nested.options);
            } catch (const std::exception& error) {
                failed(nested.name, error);
            }
        }
        if (failures != 0) throw Error(amount(failures, "nested installer") + " could not be extracted");
    }
}

}  // namespace burn
