// What "instext --version --verbose" adds to the version: the compiler and the libraries this
// program was built with, and the 7-Zip engine it finds when it runs.
#include "report.hpp"
#include "sevenzip.hpp"

#include <boost/version.hpp>
#include <bzlib.h>
#include <lzma.h>
#include <pugixml.hpp>
#include <zlib.h>

namespace {

std::string dotted(unsigned major, unsigned minor, unsigned patch) {
    return std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
}

std::string compiler() {
#if defined(__clang__)
    return "Clang " + dotted(__clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(_MSC_FULL_VER)
    return "MSVC " + dotted(_MSC_FULL_VER / 10000000, _MSC_FULL_VER / 100000 % 100, _MSC_FULL_VER % 100000);
#elif defined(__GNUC__)
    return "GCC " + dotted(__GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    return "an unknown compiler";
#endif
}

// What the program runs on, which is also what a 7-Zip engine has to be made for.
constexpr std::string_view architecture =
#if defined(_M_X64) || defined(__x86_64__)
    "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    "ARM64";
#elif defined(_M_IX86) || defined(__i386__)
    "x86";
#else
    "another architecture";
#endif

}  // namespace

std::string about(const Options& options) {
    const std::string_view bzip2 = BZ2_bzlibVersion();  // "1.0.8, 13-Jul-2019"
    std::string pugixml = std::to_string(PUGIXML_VERSION / 1000) + '.' + std::to_string(PUGIXML_VERSION / 10 % 100);
    if (PUGIXML_VERSION % 10 != 0) pugixml += '.' + std::to_string(PUGIXML_VERSION % 10);  // 1161 is 1.16.1

    const std::vector<std::string> libraries = {
        "bit7z " INSTEXT_BIT7Z_VERSION,  // from CMake: the headers of bit7z do not tell its version
        "Boost " + dotted(BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100),
        "bzip2 " + std::string(bzip2.substr(0, bzip2.find(','))),
        "liblzma " + std::string(lzma_version_string()),
        "pugixml " + pugixml,
        "zlib " + std::string(zlibVersion()),
    };
    const std::string text = "Built with " + compiler() + " for " + std::string(architecture) + '\n' +
                             listed("Linked libraries:", libraries);

    const EngineInfo engine = engine_info(options);
    if (engine.file.empty()) {
        std::vector<std::string> tried;
        for (const fs::path& file : engine.tried) tried.push_back(printable(shown(file)));
        return text + listed("External 7-Zip engine could not be loaded from:", tried);
    }
    const std::string version = engine.version.empty() ? "of unknown version" : engine.version;
    return text + listed("External 7-Zip engine " + version + " loaded from:", {printable(shown(engine.file))});
}
