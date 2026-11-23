// Test helper: writes a small Inno Setup installer (setup data format 7.1.1) in which all files
// share one chunk, as they do with solid compression, and every <step>-th file is empty.
//   makeinno <target> <number of files> <step> [<padding> [<copies> [<slice>]]] [+<outside>]
// An empty file starts at the same place in the chunk as the file after it, and the reader has to
// take the two in the right order. The files are not compressed, which the format allows.
// With <padding> other than 0, that many zero bytes follow the table of file locations, and the
// table is stored compressed: it then holds more than the header announces, and the reader has to
// stop there instead of unpacking all of it.
// With <copies>, every file is installed to that many more places, {app}\copy<n>\..., from the
// same stored data.
// With <slice>, the installer claims that its file data lies in separate slice files, in the slice
// of that number. No such file is written: the reader is to refuse a number that makes no sense.
// With +<outside> as the last argument, that many more entries follow whose data is not in the
// installer: {app}\outside<n>.txt, in turns a file to copy from elsewhere, one to download and an
// archive to extract at install time.
// The second file is dated 2001-02-03 and the third a thousand years later, a time that makes no
// sense either and that the reader is to leave alone. The other files have no date.
// The application's name holds an escape sequence and a character that reverses the direction of
// text, neither of which may reach the terminal. The layout is the one src/inno.cpp describes.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <boost/hash2/sha2.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>
#include <lzma.h>
#include <zlib.h>

namespace {

template <class T>
void put(std::string& out, T value) {  // numbers are stored with the low byte first
    for (size_t byte = 0; byte < sizeof(T); ++byte)
        out += static_cast<char>(static_cast<uint64_t>(value) >> (8 * byte));
}

uint32_t crc(std::string_view bytes) {
    const auto data = reinterpret_cast<const Bytef*>(bytes.data());
    return static_cast<uint32_t>(crc32(0, data, static_cast<uInt>(bytes.size())));
}

// The data as Inno Setup compresses a block: five property bytes, then a raw LZMA stream.
std::string packed(const std::string& data) {
    lzma_options_lzma options;
    lzma_lzma_preset(&options, 1);
    const lzma_filter filters[] = {{LZMA_FILTER_LZMA1, &options}, {LZMA_VLI_UNKNOWN, nullptr}};
    std::string out;
    put(out, static_cast<uint8_t>((options.pb * 5 + options.lp) * 9 + options.lc));
    put(out, options.dict_size);
    std::string stream(data.size() + data.size() / 4 + 4096, '\0');
    size_t size = 0;
    lzma_raw_buffer_encode(filters, nullptr, reinterpret_cast<const uint8_t*>(data.data()), data.size(),
                           reinterpret_cast<uint8_t*>(stream.data()), &size, stream.size());
    return out + stream.substr(0, size);
}

// A block of setup data: a header with its own CRC-32, then the data in pieces of up to 4096
// bytes that each follow their CRC-32.
std::string block(const std::string& plain, bool compressed = false) {
    const std::string data = compressed ? packed(plain) : plain;
    std::string pieces;
    for (size_t at = 0; at < data.size(); at += 4096) {
        const std::string piece = data.substr(at, 4096);
        put(pieces, crc(piece));
        pieces += piece;
    }
    std::string header;
    put(header, static_cast<int64_t>(pieces.size()));
    put(header, static_cast<uint8_t>(compressed));
    std::string out;
    put(out, crc(header));
    return out + header + pieces;
}

}  // namespace

int main(int argc, char** argv) {
    const boost::nowide::args utf8_arguments(argc, argv);
    int outside = 0;
    if (argc > 4 && argv[argc - 1][0] == '+') outside = std::atoi(argv[--argc] + 1);
    const bool usable = argc >= 4 && argc <= 7;
    const int count = usable ? std::atoi(argv[2]) : 0, step = usable ? std::atoi(argv[3]) : 0;
    const auto padding = static_cast<size_t>(argc >= 5 ? std::strtoull(argv[4], nullptr, 10) : 0);
    const int copies = argc >= 6 ? std::atoi(argv[5]) : 0;
    const bool spanned = argc == 7;
    const auto slice = static_cast<int32_t>(spanned ? std::atoi(argv[6]) : 0);
    if (count <= 0 || step <= 0 || copies < 0 || outside < 0) {
        boost::nowide::cerr << "usage: makeinno <target> <number of files> <step> [<padding> [<copies> [<slice>]]] "
                               "[+<outside>]\n";
        return 2;
    }
    const auto stamp = [](int in_year) {  // 3 February of that year, in units of 100 ns since 1601
        const std::chrono::sys_days day = std::chrono::year{in_year} / std::chrono::February / 3;
        return uint64_t{116444736000000000} + static_cast<uint64_t>(day.time_since_epoch().count()) * 864000000000;
    };

    std::vector<std::string> contents;
    for (int i = 0; i < count; ++i)  // file i: empty, or 100 + i times one letter
        contents.push_back(i % step == 0 ? std::string()
                                         : std::string(100 + static_cast<size_t>(i), static_cast<char>('a' + i % 26)));
    std::string data;  // the files one after the other: the chunk
    for (const std::string& content : contents) data += content;

    std::string entries, locations;
    // A [Files] entry: 15 strings, of which the second is the destination, one more string, and 81
    // bytes. Of those matter here the number of the location entry, which is -1 for data that is
    // not in the installer, and the options, which then say where the data comes from.
    const auto entry = [&](std::string_view name, int32_t location, uint64_t options) {
        for (int field = 0; field < 16; ++field) {
            const std::string_view text = field == 1 ? name : std::string_view();
            put(entries, static_cast<uint32_t>(text.size() * 2));
            for (const char c : text) put(entries, static_cast<uint16_t>(static_cast<unsigned char>(c)));
        }
        entries.append(53, '\0');
        put(entries, location);
        entries.append(15, '\0');  // attributes, the size of an external file, permissions, bitness
        put(entries, options);
        entries.append(1, '\0');  // the kind of file: an ordinary one
    };
    size_t offset = 0;
    for (int i = 0; i < count; ++i) {
        const std::string& content = contents[static_cast<size_t>(i)];
        for (int copy = 0; copy <= copies; ++copy) {
            const std::string folder = copy == 0 ? "" : "copy" + std::to_string(copy) + "\\";
            entry("{app}\\" + folder + "file" + std::to_string(i) + ".txt", static_cast<int32_t>(i), 0);
        }
        // The location entry: slices, where the chunk starts, where the file lies in it, its size, the
        // stored size of the chunk, the file's SHA-256, and 17 bytes of time, version and flags.
        put(locations, slice);
        put(locations, slice);
        put(locations, int64_t{0});
        put(locations, static_cast<int64_t>(offset));
        put(locations, static_cast<int64_t>(content.size()));
        put(locations, static_cast<int64_t>(data.size()));
        boost::hash2::sha2_256 sha;
        sha.update(content.data(), content.size());
        const auto digest = sha.result();
        locations.append(reinterpret_cast<const char*>(digest.data()), digest.size());
        put(locations, i == 1 ? stamp(2001) : i == 2 ? stamp(3001) : uint64_t{0});
        locations.append(9, '\0');
        offset += content.size();
    }

    // The first block: 38 strings, of which the first is the application's name and the others are
    // empty, and 4 empty texts; the 166 fixed bytes of the header, which begin with 17 entry counts,
    // files being the 9th kind and locations the 10th; the entries; and the counts of six groups of
    // wizard images.
    const std::u16string_view name = u"Made-up \x1B[31m\u202Einstaller";
    std::string setup;
    put(setup, static_cast<uint32_t>(name.size() * 2));
    for (const char16_t unit : name) put(setup, unit);
    setup.append((37 + 4) * 4, '\0');
    for (int n = 0; n < outside; ++n)  // the options "download" and "extract archive" are bits 30 and 31
        entry("{app}\\outside" + std::to_string(n) + ".txt", -1,
              n % 3 == 1 ? uint64_t{1} << 30 : n % 3 == 2 ? uint64_t{1} << 31 : uint64_t{0});
    for (int kind = 0; kind < 17; ++kind)
        put(setup, static_cast<int32_t>(kind == 8 ? count * (1 + copies) + outside : kind == 9 ? count : 0));
    setup.append(166 - 17 * 4, '\0');
    setup += entries;
    setup.append(6 * 4, '\0');

    std::string headers("Inno Setup Setup Data (7.1.1)");
    headers.resize(64, '\0');
    put(headers, crc(std::string(49, '\0')));  // the encryption header: nothing is encrypted
    headers.append(49, '\0');
    headers += block(setup) + block(locations + std::string(padding, '\0'), padding != 0);

    // The file: the start of a program header, the loader's offset table, the two blocks, the chunk.
    std::string file("MZ");
    file.resize(0x40, '\0');
    const size_t table = file.size(), offset0 = table + 64, offset1 = offset0 + headers.size();
    file.append("rDlPtS\xcd\xe6\xd7\x7b\x0b\x2a", 12);
    put(file, uint32_t{2});
    put(file, static_cast<int64_t>(offset1 + 4 + data.size()));  // size of the whole installer
    put(file, int64_t{0});                                        // no setup program inside
    put(file, uint32_t{0});
    put(file, uint32_t{0});
    put(file, static_cast<int64_t>(offset0));
    put(file, static_cast<int64_t>(spanned ? 0 : offset1));  // 0: the file data is in slice files
    put(file, uint32_t{0});
    put(file, crc(std::string_view(file).substr(table, 60)));
    file += headers;
    file += "zlb\x1a";
    file += data;

    boost::nowide::ofstream out(argv[1], std::ios::binary);
    out.write(file.data(), static_cast<std::streamsize>(file.size()));
    out.close();
    if (!out) {
        boost::nowide::cerr << "makeinno: cannot write " << argv[1] << '\n';
        return 1;
    }
    return 0;
}
