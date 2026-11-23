// Types and helpers shared by the installer formats.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/nowide/iostream.hpp>

namespace fs = std::filesystem;
// Messages go to nowide::cout and nowide::cerr. On a Windows console these write Unicode whatever
// the console's code page is; everywhere else they are the standard streams and carry UTF-8.
namespace nowide = boost::nowide;
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct UsageError : Error {  // the command line asks for what cannot be done: the answer points to --help
    using Error::Error;
};

struct Options {
    fs::path input;     // the file to read: an installer, or another archive
    fs::path outdir;    // where extracted files go (absolute)
    fs::path sevenzip;  // 7z.dll / 7z.so; empty: look in the usual places
    fs::path root;      // the output folder of the whole run (absolute); a nested installer's outdir lies below it
    fs::path named;     // that folder the way the command line names it, which is how messages show it
    bool list = false, test = false, internals = false, recurse = false;
    bool quiet = false, verbose = false;  // print only what was asked for; also print technical details
    bool force = false;                   // write into a folder that already holds something
};

// The folder, below the output folder, for the internal files of -i: what an installer consists
// of besides the files it installs.
inline constexpr std::string_view kInternals = "_internals";

// Copies a plain record out of a byte range (file data is never cast in place).
template <class T>
T pod(View bytes) {
    if (bytes.size() < sizeof(T)) throw Error("unexpected end of data");
    T value;
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

inline std::string_view chars(View bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

std::string to_utf8(std::u16string_view text);
std::string shown(const fs::path& path);  // UTF-8 form of a path: for messages and for 7-Zip
Bytes read_range(const fs::path& file, uint64_t offset, uint64_t count);

// The rule for a folder that is about to be written to. One that already holds something is refused
// unless `force` is set: files of the same name would be replaced, and the result would be a mix of
// what was there and what is new. A file in its place is refused in any case. `named` is the folder
// as the error shows it.
void require_free_folder(const fs::path& folder, const std::string& named, bool force);

// Whether Windows takes a file name for a device, as it does with NUL, COM1 or aux.txt, whatever
// the case of the letters and whatever follows the first dot. A file of such a name would not be
// written into its folder: Windows would send the data to the device. guard_device_names() puts
// "_" in front of every such part of a path, which may have either kind of slash.
bool is_device_name(std::string_view utf8_name);
std::string guard_device_names(std::string_view utf8_path);

// Maps a name stored in an installer, such as "bin\x.dll", to a relative path
// with no root, drive or ".." parts, so it cannot leave the output directory,
// and with no part that Windows takes for a device.
// drop_braces turns Inno Setup's "{app}\x" into "app/x".
fs::path safe_relpath(std::u16string_view name, bool drop_braces = false);
fs::path safe_relpath(std::string_view utf8_name);

// Hands out the output paths so that no two files get the same one. Paths that differ only in the
// case of ASCII letters count as the same, as they are on Windows. A path that is taken gets ",2",
// ",3" and so on before its extension. `owner` tells the files apart: a file that asks again for a
// path it has is given that path.
// The folder for --internals belongs to this tool. claim() is for what the installer holds: a path
// that begins with that folder's name, in any case, is moved to "_internals,2". internal() is for
// the files this tool writes into the folder, and is given their paths below it.
class OutputNames {
public:
    std::string claim(const std::string& path, uint64_t owner);  // UTF-8, with either kind of slash
    fs::path claim(const fs::path& path, uint64_t owner);
    fs::path internal(const fs::path& path, uint64_t owner);

private:
    std::string unique(const std::string& path, uint64_t owner);
    std::map<std::string, uint64_t> taken_;  // path in lower case -> owner
};

// One namespace per installer format: detect() looks at the file, run() lists,
// tests or extracts it according to the options.
namespace inno {
bool detect(const fs::path& file);
void run(const Options& options);
}
namespace burn {
bool detect(const fs::path& file);
void run(const Options& options);
}
namespace msi {
bool detect(const fs::path& file);
void run(const Options& options);
}
namespace nsis {
bool detect(const Options& options);
void run(const Options& options);
}
namespace sfx {  // 7-Zip self-extracting archives
bool detect(const Options& options);
void run(const Options& options);
}
namespace archive {  // the fallback: any other archive the 7-Zip engine opens
bool detect(const Options& options);
void run(const Options& options);
}

// What --verbose adds to --version, as lines that each end in a newline: the compiler, the libraries
// linked into this program, and the 7-Zip engine it finds, which is loaded for that.
std::string about(const Options& options);

// What kind of file options.input is, and running the matching handler.
enum class Format { unknown, msi, burn, inno, nsis, sfx, archive };
Format detect(const Options& options);
void run(Format format, const Options& options);
