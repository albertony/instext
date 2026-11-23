// Inno Setup 7 installers.
//
// Supported: setup data formats "7.0.0.3" (Inno Setup 7.0.0 - 7.1.0) and
// "7.1.1", 32- and 64-bit installers, including disk spanning (setup-N.bin).
// Not supported: encrypted installers, UseSetupLdr=no, Inno Setup 6 and older,
// modified Inno Setup builds.
//
// Every layout is transcribed from the Inno Setup sources
// (github.com/jrsoftware/issrc, tag is-7_1_1, Projects/Src: Shared.Struct.pas,
// Shared.SetupEntFunc.pas, Compression.Base.pas, Setup.MainFunc.pas,
// Setup.FileExtractor.pas). 7-Zip is not involved in this format.

#include "report.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <utility>

#include <boost/hash2/sha2.hpp>
#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>

namespace {

static_assert(std::endian::native == std::endian::little, "records are copied straight from the file");

constexpr std::string_view kLoaderSig{"rDlPtS\xcd\xe6\xd7\x7b\x0b\x2a", 12};  // SetupLdrOffsetTableID
constexpr std::string_view kChunkSig{"zlb\x1a", 4};       // ZLIBID, starts every file data chunk
constexpr std::string_view kSliceSig{"idskb32\x1a", 8};   // DiskSliceID, starts every setup-N.bin
constexpr std::string_view kMethods[] = {"stored", "zlib", "bzip2", "lzma", "lzma2"};  // TSetupCompressMethod

// The records below are Delphi "packed record"s; enumerations and sets of up to
// 8 members are one byte. The static_assert after them guards the sizes.
// Their fields are unaligned: read them by value and never bind a reference to
// one (std::min, std::max and std::pair's constructor all take references).
#pragma pack(push, 1)
struct OffsetTable {  // TSetupLdrOffsetTable, revision 2
    char id[12];
    uint32_t version;
    int64_t total_size, offset_exe;
    uint32_t exe_size, exe_crc;
    int64_t offset0, offset1;  // setup-0 data (headers) and setup-1 data (files)
    uint32_t padding, table_crc;
};
struct EncryptionHeader {  // CRC, then TSetupEncryptionHeader
    uint32_t crc;
    uint8_t use, salt[16];
    int32_t iterations;
    uint8_t nonce[24];
    int32_t password_test;
};
struct BlockHeader {  // CRC, then TCompressedBlockHeader
    uint32_t crc;
    int64_t stored_size;
    uint8_t compressed;
};
struct VersionData {  // TSetupVersionData
    uint32_t win, nt;
    uint16_t service_pack;
};
struct HeaderTail {  // TSetupHeader after its strings
    int32_t count[17];  // one per entry kind, in the order of kKinds
    uint32_t compiled_code_version;
    VersionData min_version, only_below_version;
    int32_t wizard_size_percent_x, wizard_size_percent_y;
    uint8_t wizard_dark_style, wizard_image_alpha_format;
    int32_t wizard_back_colors[6];
    uint8_t wizard_image_opacity, wizard_back_image_opacity, wizard_light_control_styling;
    int64_t extra_disk_space_required;
    int32_t slices_per_disk;
    uint8_t uninstall_log_mode, dir_exists_warning, privileges_required, privileges_required_overrides,
        show_language_dialog, language_detection_method, compress_method, disable_dir_page,
        disable_program_group_page;
    int64_t uninstall_display_size;
    uint64_t options;
};
struct FileTail {  // TSetupFileEntry after its strings
    uint8_t hash[32], verification_type;  // rest of the nested TSetupFileVerification
    VersionData min_version, only_below_version;
    int32_t location_entry, attribs;
    int64_t external_size;
    int16_t permissions_entry;
    uint8_t bitness;
    uint64_t options;
    uint8_t file_type;
};
struct Location {  // TSetupFileLocationEntry
    int32_t first_slice, last_slice;
    int64_t start_offset, chunk_suboffset, original_size, chunk_compressed_size;
    uint8_t sha256[32];
    uint64_t timestamp;  // FILETIME
    uint32_t file_version_ms, file_version_ls;
    uint8_t flags;
};
#pragma pack(pop)
static_assert(sizeof(OffsetTable) == 64 && sizeof(EncryptionHeader) == 53 && sizeof(BlockHeader) == 13 &&
              sizeof(HeaderTail) == 166 && sizeof(FileTail) == 81 && sizeof(Location) == 89);

constexpr uint8_t kCallOptimized = 4, kEncrypted = 8, kCompressed = 16;           // Location::flags
constexpr uint64_t kDownload = 1ull << 30, kExtractArchive = 1ull << 31;          // FileTail::options
constexpr uint8_t kUninstaller = 1;                                               // FileTail::file_type
// More than any installer's setup data, which is its tables and not its files. Decoding stops
// there, so that a small file cannot make the program take any amount of memory.
constexpr uint64_t kSetupDataLimit = uint64_t{256} << 20;

// String fields at the start of TSetupHeader. 7.1.1 dropped BaseFilename; that
// is the only layout difference between the two formats.
constexpr std::string_view kHeaderStrings[] = {
    "AppName", "AppVerName", "AppId", "AppCopyright", "AppPublisher", "AppPublisherURL", "AppSupportPhone",
    "AppSupportURL", "AppUpdatesURL", "AppVersion", "DefaultDirName", "DefaultGroupName", "BaseFilename",
    "UninstallFilesDir", "UninstallDisplayName", "UninstallDisplayIcon", "AppMutex", "DefaultUserInfoName",
    "DefaultUserInfoOrg", "DefaultUserInfoSerial", "AppReadmeFile", "AppContact", "AppComments",
    "AppModifyPath", "CreateUninstallRegKey", "Uninstallable", "CloseApplicationsFilter", "SetupMutex",
    "ChangesEnvironment", "ChangesAssociations", "ArchitecturesAllowed", "ArchitecturesInstallIn64BitMode",
    "CloseApplicationsFilterExcludes", "SevenZipLibraryName", "UsePreviousAppDir", "UsePreviousGroup",
    "UsePreviousSetupType", "UsePreviousTasks", "UsePreviousUserInfo"};
constexpr std::string_view kHeaderTexts[] = {"LicenseText", "InfoBeforeText", "InfoAfterText",
                                             "CompiledCodeText"};

// Entry kinds in the order their counts appear in the header, which is also the
// order they are stored in: String fields, AnsiString fields, fixed bytes.
struct Kind {
    std::string_view name;
    int strings = 0, ansi = 0;
    size_t fixed = 0;
};
constexpr Kind kKinds[17] = {
    {"Language", 4, 4, 19}, {"CustomMessage", 2, 0, 4}, {"Permission", 0, 1, 0}, {"Type", 4, 0, 30},
    {"Component", 5, 0, 39}, {"Task", 6, 0, 23}, {"Dir", 7, 0, 27}, {"ISSigKey", 3, 0, 0},
    {"File", 15, 1, sizeof(FileTail)},
    {"FileLocation", 0, 0, sizeof(Location)},  // stored separately, in the 2nd block
    {"Icon", 13, 0, 48}, {"Ini", 10, 0, 21}, {"Registry", 9, 0, 30}, {"InstallDelete", 7, 0, 21},
    {"UninstallDelete", 7, 0, 21}, {"Run", 14, 0, 28}, {"UninstallRun", 14, 0, 28}};
constexpr size_t kFile = 8, kFileLocation = 9;

uint32_t crc32_of(View bytes) {
    return static_cast<uint32_t>(::crc32(0, bytes.data(), static_cast<uInt>(bytes.size())));
}

class File {
public:
    explicit File(const fs::path& path) : in_(path, std::ios::binary) {
        if (!in_) throw Error("cannot open " + shown(path));
        size_ = fs::file_size(path);
    }
    uint64_t size() const { return size_; }
    size_t read(std::span<uint8_t> out) {  // continues where the last read ended
        in_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<size_t>(in_.gcount());
    }
    Bytes read_at(uint64_t pos, uint64_t count) {
        if (pos > size_ || count > size_ - pos) throw Error("read past the end of the file");
        Bytes out(static_cast<size_t>(count));
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(pos));
        if (read(out) != out.size()) throw Error("short read");
        return out;
    }

private:
    std::ifstream in_;
    uint64_t size_ = 0;
};

// One compressed stream: run() decodes from `in` into `out`, advances both and
// returns true once the stream has ended.
struct Decoder {
    virtual ~Decoder() = default;
    virtual bool run(View& in, std::span<uint8_t>& out) = 0;
};

struct LzmaDecoder final : Decoder {  // raw LZMA1 / LZMA2 as Inno stores them
    lzma_stream s = LZMA_STREAM_INIT;
    lzma_options_lzma opt{};
    LzmaDecoder(bool lzma2, View props) {
        if (lzma2) {  // 1 byte: encoded dictionary size
            if (props.empty() || props[0] > 40) throw Error("bad LZMA2 properties");
            opt.dict_size = props[0] == 40 ? UINT32_MAX : (2u | (props[0] & 1u)) << (props[0] / 2 + 11);
        } else {  // 5 bytes: lc/lp/pb, then the dictionary size
            if (props.size() < 5 || props[0] >= 225) throw Error("bad LZMA properties");
            opt.lc = props[0] % 9;
            opt.lp = props[0] / 9 % 5;
            opt.pb = props[0] / 45;
            opt.dict_size = std::max<uint32_t>(pod<uint32_t>(props.subspan(1)), 4096);
        }
        const lzma_filter filters[] = {{lzma2 ? LZMA_FILTER_LZMA2 : LZMA_FILTER_LZMA1, &opt},
                                       {LZMA_VLI_UNKNOWN, nullptr}};
        if (lzma_raw_decoder(&s, filters) != LZMA_OK) throw Error("cannot set up the LZMA decoder");
    }
    ~LzmaDecoder() override { lzma_end(&s); }
    bool run(View& in, std::span<uint8_t>& out) override {
        s.next_in = in.data(), s.avail_in = in.size();
        s.next_out = out.data(), s.avail_out = out.size();
        const lzma_ret result = lzma_code(&s, LZMA_RUN);
        in = in.last(s.avail_in), out = out.last(s.avail_out);
        if (result != LZMA_OK && result != LZMA_STREAM_END && result != LZMA_BUF_ERROR)
            throw Error("file data does not decompress (LZMA)");
        return result == LZMA_STREAM_END;
    }
};

struct ZlibDecoder final : Decoder {
    z_stream s{};
    ZlibDecoder() {
        if (inflateInit(&s) != Z_OK) throw Error("cannot set up zlib");
    }
    ~ZlibDecoder() override { inflateEnd(&s); }
    bool run(View& in, std::span<uint8_t>& out) override {
        const auto in_size = static_cast<uInt>(std::min<size_t>(in.size(), 1u << 30));
        const auto out_size = static_cast<uInt>(std::min<size_t>(out.size(), 1u << 30));
        s.next_in = const_cast<Bytef*>(in.data()), s.avail_in = in_size;
        s.next_out = out.data(), s.avail_out = out_size;
        const int result = inflate(&s, Z_NO_FLUSH);
        in = in.subspan(in_size - s.avail_in), out = out.subspan(out_size - s.avail_out);
        if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
            throw Error("file data does not decompress (zlib)");
        return result == Z_STREAM_END;
    }
};

struct Bzip2Decoder final : Decoder {
    bz_stream s{};
    Bzip2Decoder() {
        if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK) throw Error("cannot set up bzip2");
    }
    ~Bzip2Decoder() override { BZ2_bzDecompressEnd(&s); }
    bool run(View& in, std::span<uint8_t>& out) override {
        const auto in_size = static_cast<unsigned>(std::min<size_t>(in.size(), 1u << 30));
        const auto out_size = static_cast<unsigned>(std::min<size_t>(out.size(), 1u << 30));
        s.next_in = const_cast<char*>(reinterpret_cast<const char*>(in.data())), s.avail_in = in_size;
        s.next_out = reinterpret_cast<char*>(out.data()), s.avail_out = out_size;
        const int result = BZ2_bzDecompress(&s);
        in = in.subspan(in_size - s.avail_in), out = out.subspan(out_size - s.avail_out);
        if (result != BZ_OK && result != BZ_STREAM_END) throw Error("file data does not decompress (bzip2)");
        return result == BZ_STREAM_END;
    }
};

[[noreturn]] void too_large(uint64_t limit) {
    throw Error("a setup data block holds more than " + std::to_string(limit) + " bytes");
}

// Decodes a whole raw LZMA1 stream that has neither a size field nor an end
// marker: it is complete when the input is used up and output space is left.
// It may hold `limit` bytes; the buffer never grows past one byte more.
Bytes lzma1_all(View raw, uint64_t limit) {
    if (raw.size() < 5) throw Error("bad LZMA properties");
    LzmaDecoder decoder(false, raw.first(5));
    View in = raw.subspan(5);
    const auto capped = [&](uint64_t size) { return static_cast<size_t>(std::min(size, limit + 1)); };
    Bytes out(capped(std::max<uint64_t>(raw.size() * 4, 1 << 16)));
    size_t have = 0;
    for (;;) {
        std::span<uint8_t> room(out.data() + have, out.size() - have);
        const bool ended = decoder.run(in, room);
        have = out.size() - room.size();
        if (have > limit) too_large(limit);
        if (ended || (in.empty() && !room.empty())) break;
        if (room.empty()) out.resize(capped(uint64_t{out.size()} * 2));
    }
    out.resize(have);
    return out;
}

// TCompressedBlockReader: a CRC'd header (stored size, compressed flag), then
// pieces of up to 4096 bytes that are each preceded by their own CRC-32.
// Advances `pos` past the block. The block may hold `limit` bytes of data.
Bytes read_block(File& file, uint64_t& pos, uint64_t limit) {
    const Bytes head = file.read_at(pos, sizeof(BlockHeader));
    const auto header = pod<BlockHeader>(head);
    if (crc32_of(View(head).subspan(4)) != header.crc || header.stored_size < 0)
        throw Error("bad block header at offset " + std::to_string(pos));
    pos += sizeof(BlockHeader);
    const Bytes stored = file.read_at(pos, static_cast<uint64_t>(header.stored_size));
    pos += stored.size();
    Bytes raw;
    raw.reserve(stored.size());
    for (size_t at = 0; at < stored.size();) {
        if (stored.size() - at < 5) throw Error("truncated block");
        const View piece = View(stored).subspan(at + 4, std::min<size_t>(4096, stored.size() - at - 4));
        if (crc32_of(piece) != pod<uint32_t>(View(stored).subspan(at))) throw Error("block CRC mismatch");
        raw.insert(raw.end(), piece.begin(), piece.end());
        at += 4 + piece.size();
    }
    if (header.compressed) return lzma1_all(raw, limit);
    if (raw.size() > limit) too_large(limit);
    return raw;
}

// TransformCallInstructions(Encode=False): turn the absolute addresses the
// compiler put after x86 CALL/JMP opcodes back into relative ones.
void undo_call_transform(std::span<uint8_t> p, uint32_t addr_offset) {
    if (p.size() < 5) return;
    const size_t size = p.size() - 4;
    for (size_t i = 0; i < size;) {
        if (p[i] != 0xE8 && p[i] != 0xE9) {
            ++i;
            continue;
        }
        ++i;
        if (p[i + 3] == 0x00 || p[i + 3] == 0xFF) {
            uint32_t rel = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16);
            rel -= (addr_offset + static_cast<uint32_t>(i) + 4) & 0xFFFFFF;
            if (rel & 0x800000) p[i + 3] = static_cast<uint8_t>(~p[i + 3]);
            p[i] = static_cast<uint8_t>(rel);
            p[i + 1] = static_cast<uint8_t>(rel >> 8);
            p[i + 2] = static_cast<uint8_t>(rel >> 16);
        }
        i += 4;
    }
}

// Sequential, bounds-checked reader over decompressed setup data.
class Reader {
public:
    explicit Reader(View data) : data_(data) {}
    View take(size_t count) {
        if (count > data_.size() - pos_) throw Error("setup data ends early (wrong record layout?)");
        pos_ += count;
        return data_.subspan(pos_ - count, count);
    }
    template <class T>
    T value() {
        return pod<T>(take(sizeof(T)));
    }
    View blob() { return take(value<uint32_t>()); }  // 32-bit length, then the bytes
    std::u16string string() {                        // same, holding UTF-16
        const View bytes = blob();
        if (bytes.size() % 2) throw Error("odd string length in setup data");
        std::u16string text(bytes.size() / 2, u'\0');
        std::memcpy(text.data(), bytes.data(), bytes.size());
        return text;
    }
    size_t remaining() const { return data_.size() - pos_; }

private:
    View data_;
    size_t pos_ = 0;
};

using Sink = std::function<void(int index, View block, bool finished)>;

class Installer {
public:
    explicit Installer(const fs::path& installer) : path(installer), file(installer) {
        read_offset_table();
        read_setup_data();
    }
    File& slice(int number);
    Bytes engine();
    void extract(const std::vector<int>& wanted, const Sink& sink);

    struct FileEntry {
        std::u16string dest;
        FileTail fixed{};
    };
    fs::path path;
    File file;
    OffsetTable table{};
    HeaderTail tail{};
    bool spanned = false;  // file data is in setup-N.bin files
    std::string version, encryption;
    std::string_view method;
    std::map<std::string_view, std::u16string> header;
    std::map<std::string_view, Bytes> texts;
    std::vector<FileEntry> files;
    std::vector<Location> locations;
    std::array<std::vector<Bytes>, 6> images;
    Bytes decompressor_dll, sevenzip_dll;

private:
    void read_offset_table();
    void read_setup_data();
    int slice_number_ = -1;
    std::unique_ptr<File> slice_;
};

// TSetupLdrOffsetTable lives in RCDATA resource 11111 of the loader; a
// CRC-validated scan for its signature finds it as well.
void Installer::read_offset_table() {
    constexpr uint64_t window = 4 << 20, overlap = sizeof(OffsetTable) - 1;
    for (uint64_t base = 0; base < file.size(); base += window) {
        const Bytes buffer = file.read_at(base, std::min(window + overlap, file.size() - base));
        const std::string_view text = chars(buffer);
        for (size_t at = text.find(kLoaderSig); at != text.npos && at + sizeof(OffsetTable) <= buffer.size();
             at = text.find(kLoaderSig, at + 1)) {
            const auto candidate = pod<OffsetTable>(View(buffer).subspan(at));
            if (candidate.version != 2 || crc32_of(View(buffer).subspan(at, 60)) != candidate.table_crc) continue;
            table = candidate;
            if (table.total_size < 0 || static_cast<uint64_t>(table.total_size) > file.size())
                throw Error("installer is truncated (" + std::to_string(file.size()) + " of " +
                            std::to_string(table.total_size) + " bytes)");
            spanned = table.offset1 == 0;
            return;
        }
    }
    throw Error("no Inno Setup 6.5+/7 loader offset table found (not an Inno Setup installer, or an older version)");
}

void Installer::read_setup_data() {
    uint64_t pos = static_cast<uint64_t>(table.offset0);
    const Bytes id = file.read_at(pos, 64);
    const std::string setup_id(chars(id).substr(0, chars(id).find('\0')));
    constexpr std::string_view prefix = "Inno Setup Setup Data (";
    if (setup_id.starts_with(prefix) && setup_id.ends_with(')'))
        version = setup_id.substr(prefix.size(), setup_id.size() - prefix.size() - 1);
    if (version != "7.0.0.3" && version != "7.1.1")
        throw Error("unsupported setup data format '" + setup_id + "' (known: 7.0.0.3, 7.1.1)");
    pos += 64;

    const Bytes encryption_bytes = file.read_at(pos, sizeof(EncryptionHeader));
    const auto encryption_header = pod<EncryptionHeader>(encryption_bytes);
    if (crc32_of(View(encryption_bytes).subspan(4)) != encryption_header.crc) throw Error("bad encryption header");
    encryption = encryption_header.use == 0 ? "none" : encryption_header.use == 1 ? "files" : "full";
    if (encryption_header.use >= 2)
        throw Error("installer is fully encrypted (password needed even to read its file list); not supported");
    pos += sizeof(EncryptionHeader);

    const Bytes block1 = read_block(file, pos, kSetupDataLimit);  // header + all entries but locations
    Reader reader(block1);
    for (const auto name : kHeaderStrings)
        if (name != "BaseFilename" || version == "7.0.0.3") header[name] = reader.string();
    for (const auto name : kHeaderTexts) {
        const View text = reader.blob();
        texts[name].assign(text.begin(), text.end());
    }
    tail = reader.value<HeaderTail>();
    if (tail.compress_method >= std::size(kMethods)) throw Error("unknown compression method");
    method = kMethods[tail.compress_method];

    for (size_t kind = 0; kind < std::size(kKinds); ++kind) {
        if (kind == kFileLocation) continue;
        for (int32_t n = 0; n < tail.count[kind]; ++n) {
            std::u16string dest;
            for (int s = 0; s < kKinds[kind].strings; ++s) {
                if (kind == kFile && s == 1)
                    dest = reader.string();  // DestName
                else
                    reader.blob();
            }
            for (int s = 0; s < kKinds[kind].ansi; ++s) reader.blob();
            const View fixed = reader.take(kKinds[kind].fixed);
            if (kind == kFile) files.push_back({std::move(dest), pod<FileTail>(fixed)});
        }
    }
    for (auto& group : images) {  // 3 groups of wizard images, then the same 3 for dynamic dark mode
        const auto count = reader.value<int32_t>();  // -1: same as the non-dark group
        for (int32_t n = 0; n < count; ++n) {
            const View image = reader.blob();
            group.emplace_back(image.begin(), image.end());
        }
    }
    const auto dll = [&] {
        const View bytes = reader.blob();
        return Bytes(bytes.begin(), bytes.end());
    };
    if (method == "zlib" || method == "bzip2") decompressor_dll = dll();
    if (!header["SevenZipLibraryName"].empty()) sevenzip_dll = dll();
    if (reader.remaining() != 0)
        throw Error(std::to_string(reader.remaining()) + " unparsed bytes after the last record");

    // The file location entries are a block of their own, of a size that the header has just given.
    const int32_t count = tail.count[kFileLocation];
    if (count < 0) throw Error("unexpected size of the file location table");
    const uint64_t expected = static_cast<uint64_t>(count) * sizeof(Location);
    const Bytes block2 = read_block(file, pos, std::min(expected, kSetupDataLimit));
    if (block2.size() != expected) throw Error("unexpected size of the file location table");
    locations.resize(static_cast<size_t>(count));
    if (count != 0) std::memcpy(locations.data(), block2.data(), block2.size());
    for (const Location& loc : locations)
        if (loc.start_offset < 0 || loc.chunk_suboffset < 0 || loc.original_size < 0 || loc.chunk_compressed_size < 0)
            throw Error("corrupt file location entry");
    for (const auto& entry : files)
        if (entry.fixed.location_entry >= count) throw Error("file entry points past the location table");
}

// TFileExtractor.OpenSlice: the file that holds slice `number`.
File& Installer::slice(int number) {
    if (!spanned) return file;  // the installer itself
    // Inno Setup allows 1 to 26 slices per disk and tells them apart by the letters a to z.
    const int per_disk = tail.slices_per_disk > 1 ? tail.slices_per_disk : 1;
    if (number < 0 || per_disk > 26) throw Error("the installer's slice numbers are corrupt");
    if (!slice_ || slice_number_ != number) {
        std::string suffix = "-" + std::to_string(int64_t{number} / per_disk + 1);
        if (per_disk != 1) suffix += static_cast<char>('a' + number % per_disk);
        fs::path name = path;
        name.replace_extension();
        name += suffix + ".bin";
        auto opened = std::make_unique<File>(name);
        const Bytes head = opened->read_at(0, 16);  // DiskSliceID + TDiskSliceHeader
        if (chars(head).substr(0, 8) != kSliceSig || pod<uint64_t>(View(head).subspan(8)) != opened->size())
            throw Error("not a valid slice file: " + shown(name));
        slice_ = std::move(opened);
        slice_number_ = number;
    }
    return *slice_;
}

// The real setup program (setup.e32 / setup.e64) that the loader unpacks.
Bytes Installer::engine() {
    uint64_t pos = static_cast<uint64_t>(table.offset_exe);
    Bytes data = read_block(file, pos, std::min<uint64_t>(table.exe_size, kSetupDataLimit));
    if (data.size() < table.exe_size) throw Error("setup engine is truncated");
    data.resize(table.exe_size);
    undo_call_transform(data, 0);
    if (crc32_of(data) != table.exe_crc) throw Error("setup engine CRC mismatch");
    return data;
}

// The stored bytes of one file data chunk. Normally they sit inside the
// installer; with disk spanning they are in setup-N.bin slice files, and a
// chunk may run on from one slice into the next.
class ChunkSource {
public:
    ChunkSource(Installer& installer, const Location& loc)
        : installer_(installer), slice_(loc.first_slice), last_slice_(loc.last_slice) {
        left_ = static_cast<uint64_t>(loc.chunk_compressed_size);
        file_ = &installer.slice(slice_);
        const uint64_t pos = static_cast<uint64_t>(installer.table.offset1) + static_cast<uint64_t>(loc.start_offset);
        if (chars(file_->read_at(pos, 4)) != kChunkSig)
            throw Error("no file data chunk at offset " + std::to_string(loc.start_offset));
    }
    size_t read(std::span<uint8_t> out) {  // 0 at the end of the chunk
        out = out.first(static_cast<size_t>(std::min<uint64_t>(out.size(), left_)));
        size_t total = 0;
        while (total < out.size()) {
            const size_t count = file_->read(out.subspan(total));
            total += count, left_ -= count;
            if (count != 0) continue;
            if (!installer_.spanned || slice_ >= last_slice_) throw Error("file data ends early");
            file_ = &installer_.slice(++slice_);
        }
        return total;
    }

private:
    Installer& installer_;
    File* file_ = nullptr;
    int slice_, last_slice_;
    uint64_t left_ = 0;
};

// Decompressed view of one chunk. With solid compression a chunk holds many
// files back to back, so it is read strictly forwards.
class ChunkStream {
public:
    ChunkStream(ChunkSource source, std::string_view method) : source_(source) {
        uint8_t props[5];
        if (method == "lzma")
            decoder_ = std::make_unique<LzmaDecoder>(false, View(props, source_.read(props)));
        else if (method == "lzma2")
            decoder_ = std::make_unique<LzmaDecoder>(true, View(props, source_.read(std::span(props, 1))));
        else if (method == "zlib")
            decoder_ = std::make_unique<ZlibDecoder>();
        else if (method == "bzip2")
            decoder_ = std::make_unique<Bzip2Decoder>();
    }
    void read(std::span<uint8_t> out) {  // fills `out` completely
        while (!out.empty()) {
            if (in_.empty() && !source_done_) {
                buffer_.resize(1 << 16);
                buffer_.resize(source_.read(buffer_));
                in_ = buffer_;
                source_done_ = in_.empty();
            }
            const size_t before = out.size();
            if (!decoder_) {  // stored
                const size_t count = std::min(in_.size(), out.size());
                std::memcpy(out.data(), in_.data(), count);
                in_ = in_.subspan(count), out = out.subspan(count);
            } else if (!ended_) {
                ended_ = decoder_->run(in_, out);
            }
            if (out.size() == before && (ended_ || (source_done_ && in_.empty())))
                throw Error("file data chunk ends early");
        }
    }
    void skip(uint64_t count) {
        Bytes scratch(1 << 16);
        for (; count != 0; count -= std::min<uint64_t>(count, scratch.size()))
            read(std::span(scratch).first(static_cast<size_t>(std::min<uint64_t>(count, scratch.size()))));
    }

private:
    ChunkSource source_;
    std::unique_ptr<Decoder> decoder_;  // null: stored
    Bytes buffer_;
    View in_;
    bool source_done_ = false, ended_ = false;
};

// Decodes the location entries in `wanted`. The sink receives each file's data
// in order, then one call with finished=true once its SHA-256 has been verified.
void Installer::extract(const std::vector<int>& wanted, const Sink& sink) {
    std::map<std::pair<int32_t, int64_t>, std::vector<int>> chunks;  // (first slice, offset) -> entries
    for (const int index : wanted) {
        const int32_t first_slice = locations[index].first_slice;  // copies: see the note on packed fields
        const int64_t start_offset = locations[index].start_offset;
        chunks[{first_slice, start_offset}].push_back(index);
    }
    Bytes block(1 << 16);
    for (auto& [key, members] : chunks) {
        const Location& first = locations[members.front()];
        if (first.flags & kEncrypted) throw Error("file data is encrypted; not supported");
        ChunkStream stream(ChunkSource(*this, first), first.flags & kCompressed ? method : "stored");
        // The stream only moves forwards, so the files are taken in the order they lie in the chunk. An
        // empty file starts where the next one does and has to be the first of the two.
        std::ranges::sort(members, {}, [&](int index) {
            const int64_t offset = locations[index].chunk_suboffset, size = locations[index].original_size;
            return std::pair(offset, size);
        });
        uint64_t done = 0;  // how far into the chunk the stream has been read
        for (const int index : members) {
            const Location& loc = locations[index];
            const auto start = static_cast<uint64_t>(loc.chunk_suboffset);
            auto left = static_cast<uint64_t>(loc.original_size);
            if (start < done && left != 0)
                throw Error("location entry " + std::to_string(index) + " overlaps the file before it in its chunk");
            if (start > done) {
                stream.skip(start - done);
                done = start;
            }
            done += left;
            boost::hash2::sha2_256 sha;
            uint32_t address = 0;
            while (left != 0) {
                // 64 KiB steps: the transform restarts at each buffer boundary
                const auto part = std::span(block).first(static_cast<size_t>(std::min<uint64_t>(left, block.size())));
                stream.read(part);
                if (loc.flags & kCallOptimized) {
                    undo_call_transform(part, address);
                    address += static_cast<uint32_t>(part.size());
                }
                sha.update(part.data(), part.size());
                sink(index, part, false);
                left -= part.size();
            }
            const auto digest = sha.result();
            if (std::memcmp(digest.data(), loc.sha256, sizeof loc.sha256) != 0)
                throw Error("SHA-256 mismatch in location entry " + std::to_string(index));
            sink(index, {}, true);
        }
    }
}

struct Plan {
    struct Skipped {
        std::string dest, reason;  // where it would have been written, why it is not embedded
        bool generated = false;    // the uninstaller: no installer carries it, so it is only a detail
    };
    std::map<int, std::vector<fs::path>> outputs;  // location index -> relative paths
    std::vector<Skipped> skipped;
};

Plan plan_outputs(const Installer& installer) {
    Plan plan;
    OutputNames names;  // the same name for other data gets ",2", ",3", ...
    for (const auto& entry : installer.files) {
        const int index = entry.fixed.location_entry;
        if (index < 0) {
            const bool uninstaller = entry.fixed.file_type == kUninstaller;
            const bool nameless = uninstaller && entry.dest.empty();  // as in some installers: listed by what it is
            plan.skipped.push_back(
                {nameless             ? "Uninstaller"
                 : entry.dest.empty() ? std::string()
                                      : shown(safe_relpath(entry.dest, true)),  // as in the listing: "app/x"
                 nameless                                  ? "generated at install time"
                 : uninstaller                             ? "uninstaller, generated at install time"
                 : entry.fixed.options & kDownload         ? "downloaded at install time"
                 : entry.fixed.options & kExtractArchive   ? "archive extracted at install time"
                                                           : "external file, copied from outside the installer",
                 uninstaller});
            continue;
        }
        // "{app}\\x" -> app/x. The stored file is the owner: the same name for the same data is one output.
        const fs::path rel = names.claim(safe_relpath(entry.dest, true), static_cast<uint64_t>(index));
        auto& outputs = plan.outputs[index];
        if (std::ranges::find(outputs, rel) == outputs.end()) outputs.push_back(rel);
    }
    return plan;
}

// FILETIME (100 ns ticks since 1601) -> last-write time; taken as UTC even if
// the installer stored local time. Only a time from 1970 to 2199 is set: others make
// no sense for a file, and far ones are more than every system's file clock can hold.
void set_modified(const fs::path& path, uint64_t filetime) {
    using namespace std::chrono;
    using Ticks = duration<int64_t, std::ratio<1, 10'000'000>>;
    constexpr uint64_t unix_epoch = 116444736000000000;
    constexpr Ticks latest = sys_days{year{2200} / January / 1}.time_since_epoch();
    if (filetime <= unix_epoch || filetime - unix_epoch >= static_cast<uint64_t>(latest.count())) return;
    const sys_time<Ticks> when{Ticks{static_cast<int64_t>(filetime - unix_epoch)}};
    fs::last_write_time(path, time_point_cast<fs::file_time_type::duration>(clock_cast<file_clock>(when)));
}

size_t write_internals(Installer& installer, const fs::path& outdir) {
    const Bytes engine = installer.engine();
    const bool is64 = engine.size() > 0x40 && pod<uint32_t>(View(engine).subspan(0x3C)) + 6ull <= engine.size() &&
                      pod<uint16_t>(View(engine).subspan(pod<uint32_t>(View(engine).subspan(0x3C)) + 4)) == 0x8664;
    std::map<std::string, Bytes> items{{is64 ? "setup.e64" : "setup.e32", engine},
                                       {"CompiledCode.bin", installer.texts["CompiledCodeText"]},  // IFPS bytecode
                                       {"License.txt", installer.texts["LicenseText"]},
                                       {"InfoBefore.txt", installer.texts["InfoBeforeText"]},
                                       {"InfoAfter.txt", installer.texts["InfoAfterText"]},
                                       {"_isdecmp.dll", installer.decompressor_dll},
                                       {"_is7z.dll", installer.sevenzip_dll}};
    constexpr std::string_view groups[] = {"WizardImage", "WizardSmallImage", "WizardBackImage",
                                           "WizardImageDark", "WizardSmallImageDark", "WizardBackImageDark"};
    for (size_t group = 0; group < installer.images.size(); ++group)
        for (size_t n = 0; n < installer.images[group].size(); ++n) {
            const Bytes& image = installer.images[group][n];
            const bool png = chars(image).starts_with("\x89PNG");
            items[std::string(groups[group]) + "-" + std::to_string(n) + (png ? ".png" : ".bmp")] = image;
        }
    fs::create_directories(outdir);
    size_t written = 0;
    for (const auto& [name, data] : items) {
        if (data.empty()) continue;
        std::ofstream out(outdir / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) throw Error("cannot write " + name);
        ++written;
    }
    return written;
}

}  // namespace

namespace inno {

// Every Inno Setup loader carries the offset-table signature in its first megabytes.
bool detect(const fs::path& file) {
    const Bytes head = read_range(file, 0, std::min<uint64_t>(fs::file_size(file), 8 << 20));
    return chars(head).find(kLoaderSig) != std::string_view::npos;
}

void run(const Options& options) {
    const bool test = options.test;
    const fs::path& outdir = options.outdir;
    Installer installer(options.input);
    const Plan plan = plan_outputs(installer);
    std::vector<int> wanted;
    uint64_t output_files = 0, bytes = 0;
    for (const auto& [index, outputs] : plan.outputs) {
        wanted.push_back(index);
        output_files += outputs.size();  // a stored file counts once for every place it goes to, and so does its size
        bytes += static_cast<uint64_t>(installer.locations[index].original_size) * outputs.size();
    }
    const auto missing = std::ranges::count_if(plan.skipped, [](const auto& entry) { return !entry.generated; });
    info(options) << heading(to_utf8(installer.header["AppName"]) + ' ' + to_utf8(installer.header["AppVersion"]),
                             kind_name(Format::inno))
                  << '\n';
    detail(options) << "Setup data " << installer.version << ", compression " << installer.method << ", encryption "
                    << installer.encryption << (installer.spanned ? ", disk spanning" : "") << '\n'
                    << installer.files.size() << (installer.files.size() == 1 ? " entry" : " entries")
                    << " in [Files], " << amount(plan.outputs.size(), "stored file") << '\n';
    info(options) << contents(options, output_files, "file", bytes)
                  << (missing != 0 ? "; " + std::to_string(missing) + " not embedded" : "") << '\n';
    // What is not embedded. The uninstaller is in this list only with --verbose: no installer carries it.
    std::vector<std::string> absent;
    for (const auto& entry : plan.skipped)
        if (!entry.generated || options.verbose)
            absent.push_back(entry.dest.empty() ? entry.reason : printable(entry.dest) + " (" + entry.reason + ')');
    info(options) << listed("Not embedded:", absent);
    if (options.list) {
        for (const auto& [index, outputs] : plan.outputs)
            for (const auto& rel : outputs)
                nowide::cout << padded(static_cast<uint64_t>(installer.locations[index].original_size)) << "  "
                             << printable(shown(rel)) << '\n';
        return;
    }
    info(options) << std::flush;  // the report so far is to be seen while the work goes on

    // A stored file is written to its first destination. Once its hash has matched, the other
    // destinations get copies of that file, so only one file is open at any time.
    std::ofstream out;
    installer.extract(wanted, [&](int index, View block, bool finished) {
        if (test) return;
        const auto& outputs = plan.outputs.at(index);
        const fs::path first = outdir / outputs.front();
        if (!out.is_open()) {
            fs::create_directories(first.parent_path());
            out.open(first, std::ios::binary);
            if (!out) throw Error("cannot create " + shown(outputs.front()));
        }
        out.write(reinterpret_cast<const char*>(block.data()), static_cast<std::streamsize>(block.size()));
        if (!finished) return;
        out.close();
        if (!out) throw Error("cannot write file data for location entry " + std::to_string(index));
        for (const auto& rel : outputs) {
            const fs::path target = outdir / rel;
            std::error_code error;
            // (two destinations can be one file: on Windows, names that differ only in case)
            if (rel != outputs.front() && !fs::equivalent(first, target, error)) {
                fs::create_directories(target.parent_path());
                fs::copy_file(first, target, fs::copy_options::overwrite_existing, error);
                if (error) throw Error("cannot create " + shown(rel));
            }
            set_modified(target, installer.locations[index].timestamp);
        }
    });
    info(options) << outcome(options, output_files, output_files, "file", "all SHA-256 hashes match") << '\n';
    if (options.internals && !test)
        info(options) << wrote_internals(options, write_internals(installer, outdir / kInternals)) << '\n';
}

}  // namespace inno
