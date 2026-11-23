// Loading the 7-Zip engine, and what it reads on its own: NSIS installers,
// 7-Zip self-extracting archives and, as a fallback, any other archive.
#include "sevenzip.hpp"

#include "report.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <optional>

#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

fs::path program_folder() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    return fs::path(buffer).parent_path();
#else
    std::error_code error;
    const fs::path program = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::current_path() : program.parent_path();
#endif
}

std::vector<fs::path> engine_candidates(const Options& options) {
    if (!options.sevenzip.empty()) return {options.sevenzip};
#ifdef _WIN32
    std::vector<fs::path> candidates{program_folder() / "7z.dll"};
    // Then 7-Zip's own folder. ProgramFiles is the Program Files folder for
    // programs of this one's bitness, and only a 7z.dll of that bitness loads.
    std::wstring folder(32768, L'\0');
    folder.resize(GetEnvironmentVariableW(L"ProgramFiles", folder.data(), static_cast<DWORD>(folder.size())));
    if (!folder.empty() && folder.size() < 32768) candidates.push_back(fs::path(folder) / "7-Zip" / "7z.dll");
    return candidates;
#else
    // Only where 7-Zip itself installs: the 7z.so of p7zip, like that of 7-Zip
    // before 23.01, has another interface layout than the one bit7z is built for.
    return {program_folder() / "7z.so", "/usr/lib/7zip/7z.so", "/usr/lib64/7zip/7z.so", "/usr/local/lib/7zip/7z.so"};
#endif
}

fs::path engine_file;  // where the engine was loaded from

// The engine, loaded on first use from the first candidate that can be loaded. Null if none can,
// and `tried` then lists the candidates.
const bit7z::Bit7zLibrary* load_engine(const Options& options, std::string& tried) {
    static std::unique_ptr<bit7z::Bit7zLibrary> engine;
    if (engine) return engine.get();
    for (const fs::path& candidate : engine_candidates(options)) {
        try {
            engine = std::make_unique<bit7z::Bit7zLibrary>(shown(candidate));
            engine_file = candidate;
            return engine.get();
        } catch (const bit7z::BitException&) {
            tried += (tried.empty() ? "" : ", ") + shown(candidate);
        }
    }
    return nullptr;
}

// The version a loaded engine reports about itself, as "26.06". 7-Zip has a function for that since
// 23.01; an older engine gives an empty text. The engine is opened once more for the question, which
// only counts another user of the library that bit7z keeps loaded, and that count is left as it is.
std::string engine_version(const fs::path& file) {
    using namespace bit7z;  // outside Windows the types of a property are bit7z's
#ifdef _WIN32
    using Ask = HRESULT(WINAPI*)(PROPID, PROPVARIANT*);
    const HMODULE library = LoadLibraryW(file.c_str());
    if (library == nullptr) return {};
    const auto ask = reinterpret_cast<Ask>(GetProcAddress(library, "GetModuleProp"));
#else
    using Ask = HRESULT (*)(PROPID, PROPVARIANT*);
    void* const library = dlopen(file.c_str(), RTLD_NOW);
    if (library == nullptr) return {};
    const auto ask = reinterpret_cast<Ask>(dlsym(library, "GetModuleProp"));
#endif
    constexpr PROPID version = 1;          // NModulePropID::kVersion in 7-Zip's ICoder.h: major << 16 | minor
    constexpr unsigned short number = 19;  // VT_UI4, the type of that property: 32 bits, unsigned
    PROPVARIANT value{};
    if (ask == nullptr || ask(version, &value) != S_OK || value.vt != number) return {};
    const unsigned major = value.ulVal >> 16, minor = value.ulVal & 0xFFFF;
    return std::to_string(major) + (minor < 10 ? ".0" : ".") + std::to_string(minor);
}

}  // namespace

const bit7z::Bit7zLibrary& sevenzip(const Options& options) {
    std::string tried;
    if (const bit7z::Bit7zLibrary* engine = load_engine(options, tried)) return *engine;
    throw Error("cannot load the 7-Zip engine (tried " + tried + "); pass its location with --7zip");
}

EngineInfo engine_info(const Options& options) {
    std::string tried;
    if (load_engine(options, tried) == nullptr) return {{}, {}, engine_candidates(options)};
    return {engine_file, engine_version(engine_file), {}};
}

namespace {

bool opens_as(const Options& options, const bit7z::BitInFormat& format) {
    try {
        const bit7z::BitArchiveReader archive{sevenzip(options), shown(options.input), format};
        return true;
    } catch (const bit7z::BitException&) {
        return false;
    }
}

// Lists, tests or extracts an archive 7-Zip has opened. Several files may share
// a name (alternatives chosen at install time); the later ones get ",2", ",3",
// ... as elsewhere in this tool, where 7-Zip itself would overwrite the first.
// `details` are further lines for --verbose, each ending in a newline, that come before the line
// about what the archive holds; `verified` is what the last line of the report says about checks.
void run_archive(const Options& options, const bit7z::BitArchiveReader& archive, std::string_view kind,
                 std::string_view details, std::string_view verified) {
    std::map<uint32_t, std::string> names;  // item index -> output path
    OutputNames taken;
    uint64_t bytes = 0;
    for (const auto& item : archive) {
        if (item.isDir()) continue;
        names[item.index()] = taken.claim(guard_device_names(item.path()), item.index());
        bytes += item.size();
    }
    info(options) << heading("", kind) << '\n';
    detail(options) << "Handled by the external 7-Zip engine\n" << details;
    info(options) << contents(options, names.size(), "file", bytes) << '\n';
    if (options.list) {
        for (const auto& item : archive)
            if (!item.isDir()) nowide::cout << padded(item.size()) << "  " << printable(names[item.index()]) << '\n';
        return;
    }
    info(options) << std::flush;  // the report so far is to be seen while the work goes on
    if (options.test)
        archive.test();
    else
        archive.extractTo(shown(options.outdir), [&](const bit7z::BitArchiveItem& item) {
            const auto found = names.find(item.index());
            return found == names.end() ? item.path() : found->second;
        });
    info(options) << outcome(options, names.size(), names.size(), "file", verified) << '\n';
}

// The settings a 7-Zip installer stub acts on after unpacking, e.g. which
// program to run. They sit as text between the stub and the archive:
// ";!@Install@!UTF-8!" ... ";!@InstallEnd@!".
std::string sfx_settings(const fs::path& file) {
    const Bytes head = read_range(file, 0, std::min<uint64_t>(fs::file_size(file), 8 << 20));
    constexpr std::string_view begin = ";!@Install@!", end = ";!@InstallEnd@!";
    const size_t from = chars(head).find(begin), to = chars(head).find(end);
    if (from == std::string_view::npos || to == std::string_view::npos || to < from) return {};
    return std::string(chars(head).substr(from, to + end.size() - from));
}

// An NSIS installer ends its data with a CRC-32 of the file from byte 512 up to
// that point, unless it was built with "CRCCheck off", and tests it when it
// starts. (One build option of NSIS includes the first 512 bytes as well.) The
// data begins on a 512-byte boundary with the "first header": flags,
// 0xDEADBEEF, "NullsoftInst", the size of the header block and the size of
// everything from the first header on. See Source/exehead/fileform.c in the
// NSIS sources. 7-Zip does not look at this checksum.
// Returns whether it matches; no value: the installer carries none.
std::optional<bool> nsis_checksum(const fs::path& file) {
    constexpr uint32_t kNoCrc = 4, kForceCrc = 8, kKnownFlags = 15;
    constexpr size_t kHeaderSize = 28;
    std::ifstream in(file, std::ios::binary);
    if (!in) throw Error("cannot open " + shown(file));
    const uint64_t file_size = fs::file_size(file);
    Bytes block(1 << 16);          // a multiple of 512, so every block starts on such a boundary
    uint64_t end = 0;              // where the stored CRC-32 is; 0: the first header is not found yet
    uLong skipping = 0, whole = 0; // CRC-32 so far, without and with the first 512 bytes
    for (uint64_t pos = 0; end == 0 || pos < end;) {
        const auto count = static_cast<size_t>(std::min<uint64_t>(block.size(), (end != 0 ? end : file_size) - pos));
        if (count == 0) return std::nullopt;  // the whole file holds no first header
        in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(count));
        if (static_cast<size_t>(in.gcount()) != count) throw Error("unexpected end of " + shown(file));
        size_t use = count;
        for (size_t at = 0; end == 0 && at + kHeaderSize <= count; at += 512) {
            const View header = View(block).subspan(at, kHeaderSize);
            const auto word = [&](size_t n) { return pod<uint32_t>(header.subspan(n * 4)); };
            if ((word(0) & ~kKnownFlags) != 0 || word(1) != 0xDEADBEEF || word(2) != 0x6C6C754E ||
                word(3) != 0x74666F73 || word(4) != 0x74736E49)
                continue;
            if ((word(0) & kNoCrc) != 0 && (word(0) & kForceCrc) == 0) return std::nullopt;
            const auto following = pod<int32_t>(header.subspan(24));  // includes the 4 bytes of the CRC-32
            if (following < static_cast<int32_t>(kHeaderSize + 4)) return false;
            const uint64_t data_end = pos + at + static_cast<uint64_t>(following);
            if (data_end > file_size) return false;  // the file is cut short
            end = data_end - 4;
            use = static_cast<size_t>(std::min<uint64_t>(count, end - pos));
        }
        const size_t skipped = pos < 512 ? static_cast<size_t>(std::min<uint64_t>(512 - pos, use)) : 0;
        whole = crc32(whole, block.data(), static_cast<uInt>(use));
        skipping = crc32(skipping, block.data() + skipped, static_cast<uInt>(use - skipped));
        pos += use;
    }
    uint8_t stored[4];
    in.clear();
    in.seekg(static_cast<std::streamoff>(end));
    in.read(reinterpret_cast<char*>(stored), sizeof stored);
    if (in.gcount() != static_cast<std::streamsize>(sizeof stored)) throw Error("unexpected end of " + shown(file));
    const auto expected = pod<uint32_t>(View(stored, sizeof stored));
    return expected == static_cast<uint32_t>(skipping) || expected == static_cast<uint32_t>(whole);
}

}  // namespace

namespace nsis {

bool detect(const Options& options) { return opens_as(options, bit7z::BitFormat::Nsis); }

void run(const Options& options) {
    const bit7z::BitArchiveReader archive{sevenzip(options), shown(options.input), bit7z::BitFormat::Nsis};
    std::string verified = options.test ? "no errors found; " : "";
    if (!options.list) {  // the files themselves carry no checksums; the installer as a whole usually does
        const std::optional<bool> checksum = nsis_checksum(options.input);
        if (checksum && !*checksum)
            throw Error("the installer's own checksum (CRC-32) does not match; the file is damaged or incomplete");
        verified += checksum ? "the installer's own checksum (CRC-32) matches"
                             : "the installer has no checksum of its own";
    }
    run_archive(options, archive, kind_name(Format::nsis), "", verified);
}

}  // namespace nsis

namespace sfx {

// 7-Zip looks for a 7z archive after the EXE's own code and validates its header.
bool detect(const Options& options) { return opens_as(options, bit7z::BitFormat::SevenZip); }

void run(const Options& options) {
    const bit7z::BitArchiveReader archive{sevenzip(options), shown(options.input), bit7z::BitFormat::SevenZip};
    const std::string settings = sfx_settings(options.input);
    std::vector<std::string> lines;  // one setting per line, e.g. RunProgram="setup.exe"
    for (size_t at = 0; at < settings.size();) {
        const size_t stop = std::min(settings.find('\n', at), settings.size());
        std::string_view line = std::string_view(settings).substr(at, stop - at);
        line = line.substr(0, line.find('\r'));
        if (!line.empty() && line.front() != ';') lines.push_back(printable(line));
        at = stop + 1;
    }
    const std::string note = listed("Stub settings:", lines);  // what the stub does once the files are out
    run_archive(options, archive, kind_name(Format::sfx), note, options.test ? "no errors found" : "");
    if (options.internals && !options.list && !options.test && !settings.empty()) {
        const fs::path file = options.outdir / kInternals / "settings.txt";
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << settings;
        info(options) << wrote_internals(options, 1) << '\n';
    }
}

}  // namespace sfx

namespace archive {

// The fallback for files that are none of the installers above. bit7z picks
// the format from the file's name and signature, and looks inside a program
// for an attached 7z, RAR, cabinet or ZIP archive, as self-extractors have. A
// program with nothing attached would open as a list of its own sections,
// which is no use here.
bool detect(const Options& options) {
    try {
        const bit7z::BitArchiveReader archive{sevenzip(options), shown(options.input), bit7z::BitFormat::Auto};
        const bit7z::BitInFormat& format = archive.detectedFormat();
        return format != bit7z::BitFormat::Pe && format != bit7z::BitFormat::Elf && format != bit7z::BitFormat::Macho;
    } catch (const bit7z::BitException&) {
        return false;
    }
}

void run(const Options& options) {
    const bit7z::BitArchiveReader archive{sevenzip(options), shown(options.input), bit7z::BitFormat::Auto};
    run_archive(options, archive, kind_name(Format::archive), "", options.test ? "no errors found" : "");
}

}  // namespace archive
