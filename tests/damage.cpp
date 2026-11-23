// Test helper: copies a file and damages the copy. One bit is flipped in the byte at the offset
// or, with "cut" as the last argument, the copy ends at the offset.
//   damage <source> <target> <offset of the byte> [cut]
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <boost/nowide/args.hpp>
#include <boost/nowide/iostream.hpp>

int main(int argc, char** argv) {
    const boost::nowide::args utf8_arguments(argc, argv);  // on Windows the arguments only arrive as UTF-8 this way
    const bool cut = argc == 5 && std::string_view(argv[4]) == "cut";
    if (argc != 4 && !cut) {
        boost::nowide::cerr << "usage: damage <source> <target> <offset of the byte> [cut]\n";
        return 2;
    }
    const auto path = [](std::string_view utf8) {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    };
    const std::filesystem::path source = path(argv[1]), target = path(argv[2]);
    std::error_code error;
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
    const auto offset = std::strtoull(argv[3], nullptr, 10);
    if (cut) {
        if (!error) std::filesystem::resize_file(target, offset, error);
        if (error) boost::nowide::cerr << "damage: cannot make " << argv[2] << '\n';
        return error ? 1 : 0;
    }
    std::fstream file(target, std::ios::in | std::ios::out | std::ios::binary);
    char byte = 0;
    file.seekg(static_cast<std::streamoff>(offset));
    file.get(byte);
    file.seekp(static_cast<std::streamoff>(offset));
    file.put(static_cast<char>(byte ^ 1));
    file.close();
    if (error || !file) {
        boost::nowide::cerr << "damage: cannot make " << argv[2] << '\n';
        return 1;
    }
    return 0;
}
