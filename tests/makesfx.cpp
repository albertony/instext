// Test helper: writes a self-extracting 7-Zip installer of the kind that carries settings.
//   makesfx <target> <7z archive>
// The result has the start of a program, the settings that such an installer's stub acts on after
// extraction, between their two markers, and the archive. The stub itself is left out: no reader
// runs it. One of the lines between the markers is a comment, which is no setting.
#include <iterator>
#include <string>

#include <boost/nowide/args.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>

int main(int argc, char** argv) {
    const boost::nowide::args utf8_arguments(argc, argv);  // on Windows the arguments only arrive as UTF-8 this way
    if (argc != 3) {
        boost::nowide::cerr << "usage: makesfx <target> <7z archive>\n";
        return 2;
    }
    boost::nowide::ifstream in(argv[2], std::ios::binary);
    const std::string archive((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string file("MZ");
    file.resize(512, '\0');
    file += ";!@Install@!UTF-8!\nTitle=\"Made-up installer\"\n;a comment\nRunProgram=\"setup.exe\"\n;!@InstallEnd@!";
    file += archive;
    boost::nowide::ofstream out(argv[1], std::ios::binary);
    out.write(file.data(), static_cast<std::streamsize>(file.size()));
    out.close();
    if (!in || archive.empty() || !out) {
        boost::nowide::cerr << "makesfx: cannot make " << argv[1] << " from " << argv[2] << '\n';
        return 1;
    }
    return 0;
}
