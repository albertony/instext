// instext - extracts the contents of Windows installers without running them.
//
//   Inno Setup 7   own reader (inno.cpp)
//   NSIS           the 7-Zip engine (sevenzip.cpp)
//   7-Zip SFX      the 7-Zip engine (sevenzip.cpp)
//   MSI            7-Zip for the containers, own table decoding for the paths (msi.cpp)
//   WiX Burn       own reader for the bundle, 7-Zip for its cabinets (burn.cpp)
//   anything else  the 7-Zip engine, if it opens the file as an archive (sevenzip.cpp)
//
// Libraries, all from vcpkg: bit7z (which loads 7z.dll / 7z.so at run time),
// liblzma, zlib, bzip2, pugixml, and Boost's Hash2, Nowide and Program_options.
#include "hash.hpp"
#include "report.hpp"

#include <boost/nowide/args.hpp>
#include <boost/program_options.hpp>

Format detect(const Options& options) {
    if (msi::detect(options.input)) return Format::msi;
    if (fs::file_size(options.input) >= 2 && chars(read_range(options.input, 0, 2)) == "MZ") {
        if (burn::detect(options.input)) return Format::burn;
        if (inno::detect(options.input)) return Format::inno;
        if (nsis::detect(options)) return Format::nsis;  // from here on 7-Zip is asked, so these come last
        if (sfx::detect(options)) return Format::sfx;
    }
    return archive::detect(options) ? Format::archive : Format::unknown;
}

void run(Format format, const Options& options) {
    switch (format) {
        case Format::msi: return msi::run(options);
        case Format::burn: return burn::run(options);
        case Format::inno: return inno::run(options);
        case Format::nsis: return nsis::run(options);
        case Format::sfx: return sfx::run(options);
        case Format::archive: return archive::run(options);
        case Format::unknown:
            throw Error("neither an installer this tool understands (Inno Setup 7, NSIS, MSI, WiX Burn, 7-Zip SFX) "
                        "nor an archive that 7-Zip opens");
    }
}

int main(int argc, char** argv) {
    namespace po = boost::program_options;
    const nowide::args utf8_arguments(argc, argv);  // on Windows the arguments only arrive as UTF-8 through this
    const auto path = [](const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); };

    Options options;
    std::string file, outdir, sevenzip, checksum;
    po::options_description described("Options");
    described.add_options()
        ("help,h", "print this help message and exit")
        ("version", "print the version and exit; with -v also the libraries and the 7-Zip engine in use")
        ("output-dir,o", po::value(&outdir)->value_name("DIR"), "extract here (default: FILE.extracted)")
        ("force", po::bool_switch(&options.force),
         "extract into an output folder that is not empty, replacing files of the same name")
        ("list,l", po::bool_switch(&options.list), "list files only")
        ("test,t", po::bool_switch(&options.test),
         "decode everything and verify what the format allows, write nothing")
        ("internals,i", po::bool_switch(&options.internals),
         "also write the installer's internal files to a folder _internals (Inno Setup engine and [Code], Burn "
         "bootstrapper application, 7-Zip SFX settings)")
        ("recurse,r", po::bool_switch(&options.recurse), "also extract the installers found inside a Burn bundle")
        ("quiet,q", po::bool_switch(&options.quiet),
         "print only what was asked for: the list with -l, otherwise nothing but errors")
        ("verbose,v", po::bool_switch(&options.verbose), "also print technical details of the installer")
        ("checksum", po::value(&checksum)->value_name("VALUE"),
         "compare the file with this checksum before reading it: the hexadecimal digits of its MD5, SHA-1, "
         "SHA-256 or SHA-512, alone or as in sha256:...")
        ("7zip", po::value(&sevenzip)->value_name("FILE"),
         "the 7-Zip engine, 7z.dll or 7z.so (default: next to this program, then 7-Zip's install folder)");
    po::options_description accepted;  // FILE is a positional argument and stays out of the list of options
    accepted.add(described).add_options()("file", po::value(&file));
    po::positional_options_description positional;
    positional.add("file", 1);
    const auto usage = [](std::string_view message) {  // the answer to a command line that cannot be acted on
        nowide::cerr << "error: " << printable(message) << "\nRun with --help for more information.\n";
        return 2;
    };

    try {
        // Option names are to be written out in full: a shortened one that fits today may fit two options later.
        const int style = po::command_line_style::default_style & ~po::command_line_style::allow_guessing;
        po::variables_map values;
        po::store(po::command_line_parser(argc, argv).options(accepted).positional(positional).style(style).run(),
                  values);
        po::notify(values);
        options.sevenzip = path(sevenzip);
        // The first line of --help and of --version, with the version of project() in CMakeLists.txt.
        constexpr std::string_view title = "InstExt " INSTEXT_VERSION "\n";
        if (values.count("help") != 0) {
            // The list of options is Boost's work: it adds the colon to the heading given above, and
            // the usage part here is written to look the same.
            nowide::cout << title
                         << "\nExtracts the contents of Inno Setup 7, NSIS, MSI, WiX Burn and 7-Zip SFX\n"
                            "installers without running them, and of any other archive that 7-Zip opens.\n\n"
                         << listed("Usage:", {"instext [OPTIONS] FILE"}) << '\n'
                         << described;
            return 0;
        }
        if (values.count("version") != 0) {
            nowide::cout << title;
            if (options.verbose) nowide::cout << about(options);  // only this looks for the 7-Zip engine
            return 0;
        }
        if (file.empty()) throw UsageError("no file given");
        if (options.quiet && options.verbose) throw UsageError("--quiet and --verbose exclude each other");
        options.input = path(file);
        std::error_code ignored;
        if (!fs::is_regular_file(options.input, ignored))
            throw UsageError((fs::exists(options.input, ignored) ? "not a file: " : "no such file: ") + file);
        options.named = path(outdir);
        if (options.named.empty()) (options.named = options.input) += ".extracted";
        options.root = options.outdir = fs::absolute(options.named);
        // The output folder is looked at before anything of the file is read. -l and -t write nothing.
        if (!options.list && !options.test)
            require_free_folder(options.outdir, shown(options.named), options.force);
        detail(options) << reading(options) << std::endl;  // flushed: it is to be seen while the work goes on
        if (values.count("checksum") != 0) {
            const std::string kind = verify_checksum(options.input, checksum);  // throws unless the file matches
            detail(options) << kind << " of the file matches the checksum given\n";
        }
        run(detect(options), options);
        return 0;
    } catch (const po::too_many_positional_options_error&) {  // which Boost would call "positional options"
        return usage("only one file can be given");
    } catch (const po::error& error) {  // the command line itself is wrong: in Boost's eyes,
        return usage(error.what());
    } catch (const UsageError& error) {  // or in ours
        return usage(error.what());
    } catch (const std::exception& error) {
        nowide::cerr << "error: " << printable(error.what()) << '\n';
        return 1;
    }
}
