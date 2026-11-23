# InstExt - Installer Extractor

InstExt is a command-line tool that extracts the contents of Windows installers without running them. It
recognizes the kind of installer from the file itself, and can also just list or test what is inside. A file that
is no installer but an archive that 7-Zip opens, such as a `.zip`, `.7z` or `.rar` file, is extracted as well.

| Installer | Extension | What you get | Engine |
| --- | --- | --- | --- |
| Inno Setup 7.0.0 to 7.1.1 | `.exe` | The `[Files]` entries under their destination names | Built-in |
| Windows Installer package | `.msi` | The files in their installed folder layout | Built-in and 7-Zip |
| WiX Burn bundle (WiX 3 and later) | `.exe` | The `.msi`, `.cab` and other files inside | Built-in and 7-Zip |
| NSIS | `.exe` | The files as 7-Zip lists them | 7-Zip |
| 7-Zip self-extracting archive | `.exe` | The files of the archive | 7-Zip |
| Any other archive that 7-Zip opens | `.zip`, `.7z`, `.rar` and more | The files as 7-Zip lists them | 7-Zip |

Inno Setup installers are read by InstExt alone. The other formats also need the 7-Zip engine: the library
`7z.dll` or `7z.so`, from an installed 7-Zip or from a copy next to `instext`. Without it they stop with an error.
See [The 7-Zip engine](#the-7-zip-engine).

It is written in C++20 and builds with Visual Studio on Windows and with GCC on Linux.

## Usage

```text
instext [OPTIONS] FILE
```

`FILE` is the installer, or the archive, to read.

| Option | Effect |
| --- | --- |
| `-o`, `--output-dir DIR` | Extract to `DIR`. The default is `FILE.extracted`, next to the file. |
| `--force` | Extract into an output folder that is not empty, see [What is written](#what-is-written). |
| `-l`, `--list` | List the files and extract nothing. |
| `-t`, `--test` | Decode everything and verify what the format allows, but write nothing. |
| `-i`, `--internals` | Also write the installer's internal files, see [What is written](#what-is-written). |
| `-r`, `--recurse` | Also extract the installers found inside a Burn bundle. |
| `-q`, `--quiet` | Print only what was asked for: the list with `-l`, otherwise nothing but errors. |
| `-v`, `--verbose` | Also print technical details of the installer, see [What is printed](#what-is-printed). |
| `--checksum VALUE` | Compare the file with a checksum before reading it, see [Verification](#verification). |
| `--7zip FILE` | The 7-Zip engine to load, see [The 7-Zip engine](#the-7-zip-engine). |
| `-h`, `--help` | Print the options and exit. |
| `--version` | Print the version and exit; with `-v` also the libraries and the 7-Zip engine in use. |

`-l` takes precedence over `-t`: with both, the files are only listed.

On failure InstExt prints a line that starts with `error:` and exits with status 1. A command line it cannot
act on, such as an unknown option, an installer that does not exist or an output folder that already holds
something, gives status 2.

### What is printed

The report says what the file is, what it holds and what was done with it. With `-l` the list of files follows in
place of the last part, one file per line with its size in bytes. A line that ends in a colon introduces a list,
whose items are indented, such as the files that an installer names but does not carry. The report goes to
standard output and errors go to standard error.

`-v` adds the technical details, right after the line that says what the file is: the format, compression and
encryption of an Inno Setup installer, the code page and cabinets of a Windows Installer package, the parts of a
Burn bundle (its engine, its bootstrapper application and its containers) with the packages it would install, in
their order, and the settings of a 7-Zip self-extracting archive. The report then ends as it does without `-v`,
with what there is to extract and what was done. Every report also starts with the file it is about, followed by
the confirmation that it matches a checksum given with `--checksum`, and with `-r` the run says what each payload
of a bundle turned out to be. `-q` leaves only what was asked for. With `-l` that is the list and nothing else,
which suits a script; otherwise nothing is printed, and the exit status tells the result.

Sizes in the report are rounded, in binary units: 1 MiB is 1,048,576 bytes, which Windows shows as 1 MB. `-v` adds
the exact number of bytes.

The output folder is printed the way the command line names it: as it was given with `-o`, and otherwise as the
installer was given, with `.extracted` added.

File names are printed as Unicode text, whatever the code page of a Windows console is. Output that is redirected
to a file or to another program is UTF-8. In names and other text that comes from the installer, control characters
and characters that change the direction of text are printed as `?`.

### Examples

These examples were made on Linux. On Windows the paths are printed with backslashes.

List an Inno Setup installer:

```console
$ instext -l innosetup-7.1.1-x64.exe
Inno Setup 7.1.1 - Inno Setup installer
136 files, 50.3 MiB
        3062  tmp/isdonate.bmp
        7838  tmp/isdonate_dark.bmp
        3062  tmp/ismail.bmp
        7838  tmp/ismail_dark.bmp
        1521  app/license.txt
     8876960  app/ISIDE.exe
...
```

Check a Windows Installer package without writing anything:

```console
$ instext -t gh_2.105.0_windows_amd64.msi
GitHub CLI 2.105.0 - Windows Installer package
1 file, 40.0 MiB
Tested: no errors found
```

Compare an installer with the checksum its publisher gives, before anything else is done with it. That the
checksum matches is said only with `-v`, as here; one that does not match ends the run with an error:

```console
$ instext -v --checksum sha256:8a5a6711c3766a6e5fb74f280ba0e3551e719ce94538d901fdbd1f8f9920b3f0 innosetup-7.1.1-x64.exe
Reading innosetup-7.1.1-x64.exe
SHA-256 of the file matches the checksum given
Inno Setup 7.1.1 - Inno Setup installer
Setup data 7.1.1, compression lzma2, encryption none
137 entries in [Files], 136 stored files
136 files, 50.3 MiB (52782767 bytes)
Not embedded:
  Uninstaller (generated at install time)
Extracted to innosetup-7.1.1-x64.exe.extracted; all SHA-256 hashes match
```

Extract a Burn bundle, and then the packages inside it:

```console
$ instext -r wix314.exe
WiX Toolset v3.14.1.8722 3.14.1.8722 - WiX Burn bundle
16 payloads, 30.6 MiB; 1 not embedded
Not embedded:
  redist/NDP462-KB3151800-x86-x64-AllOS-ENU.exe (external: downloaded or shipped next to the bundle)
Extracted to wix314.exe.extracted; all sizes and hashes match the manifest

WiX Toolset v3.14 Core 3.14.8722 - Windows Installer package
244 files, 28.3 MiB
Extracted to wix314.exe.extracted/data/core.msi.extracted
...
```

See what that bundle would install, with the other technical details:

```console
$ instext -t -v wix314.exe
Reading wix314.exe
WiX Toolset v3.14.1.8722 3.14.1.8722 - WiX Burn bundle
Burn engine 3.x
Bootstrapper application with 34 internal files
1 attached container
Install chain:
  NetFx462Redist (ExePackage, 1 payload, not embedded)
  core.msi (MsiPackage, 2 payloads)
...
16 payloads, 30.6 MiB (32133190 bytes); 1 not embedded
Not embedded:
  redist/NDP462-KB3151800-x86-x64-AllOS-ENU.exe (external: downloaded or shipped next to the bundle)
Tested: all sizes and hashes match the manifest
```

## What is written

For Inno Setup, MSI and Burn, InstExt builds every output path itself. A drive letter, a leading slash or `..`
in a stored name cannot take a file out of the output folder, and characters that Windows does not allow in file
names become `_`. NSIS installers and 7-Zip archives are extracted through bit7z, which refuses to write an entry
outside the output folder. For every format, a name that Windows takes for a device, such as `NUL`, `COM1` or
`aux.txt`, gets a `_` in front: Windows would otherwise send the data of that file to the device.

The output folder has to be new or empty. One that already holds something is refused before the file is read:
files of the same name would be replaced, and the result would be a mix of what was there and what is new. With
`--force` such a folder is used as it is: files of the same name are overwritten, and whatever else is in it
stays. `-l` and `-t` write nothing and take any folder. With `-r` the same rule holds for the folder of each
nested installer.

When several files share one name, for example alternatives chosen at install time, the later ones get `,2`, `,3`
and so on before the extension. Extracted files get the modification time that the installer records for them.

With `-i`, the installer's internal files are written too, into a folder `_internals` inside the output folder:
what an installer consists of besides the files it installs. What that is depends on the format and is said below.
The folder is for those files alone: what an installer itself holds under the top-level name `_internals` is
written under `_internals,2`, with or without `-i`.

**Inno Setup.** Constants become folders: `{app}\ISCC.exe` is written as `app/ISCC.exe`. Entries whose data is not
in the installer are listed as not embedded, under the names they would have been written to: files that are
downloaded or copied from elsewhere at install time and, with `-v`, the uninstaller, which an installer only makes
when it runs. With `-i`, the folder `_internals` receives what else the installer carries: the setup engine
(`setup.e32` or `setup.e64`), the compiled `[Code]` script (`CompiledCode.bin`), license and information texts,
wizard images and helper DLLs.

**Windows Installer.** 7-Zip on its own shows the files of a package under their internal keys, such as
`ExtensionXsdBalSchema2010`, in one flat folder. InstExt decodes the `File`, `Component` and `Directory` tables
and writes the files where the package would install them, for example `Program Files/GitHub CLI/gh.exe`. A
standard folder without a name of its own keeps its identifier, for example `ProgramFilesFolder`. Cabinets are
read from inside the package, or from next to it when the package refers to external ones. The report counts the
files that are there to be extracted. The others are listed as not embedded: a cabinet that is missing next to
the package, with the number of its files, and files that are in no cabinet at all.

**WiX Burn.** The payloads, which are the files a bundle carries, are written under the names in the bundle's
manifest. This is the same content that `wix burn extract` produces. Payloads that are not in the bundle itself
are listed as not embedded, with the reason in WiX's words: `external` for a file of its own, which the bundle
downloads or expects next to it, or in a `detached container`, a cabinet file of its own. InstExt does not look
for those files. With `-v` the report also lists the bundle's install chain: its packages, which are the
installers and programs it would run, in that order, each made up of one or more of the payloads and named as the
bundle's author named it. With `-i`, the folder `_internals` receives the manifest (`manifest.xml`) and the files
of the bootstrapper application. With `-r`, every payload that is itself a supported installer is extracted into
`<payload>.extracted`.

**NSIS.** The files are those the 7-Zip engine finds. Folders named `$...`, such as `$PLUGINSDIR`, stand for NSIS
variables.

**7-Zip self-extracting archive.** The files of the archive. If the installer stub carries settings, such as the
program to run after extraction, `-v` shows them and `-i` writes them to `_internals/settings.txt`.

**Other archives.** The files are those the 7-Zip engine finds, one level deep: a `.tar.gz` file gives the `.tar`
inside it, and a virtual disk such as a `.vhd` file gives its disk image. A program counts as an archive when a
7z, RAR or ZIP archive is attached to it, as self-extracting archives have.

## Verification

What is extracted is checked against what the installer records about it. `-t` runs the same checks without
writing anything.

| Installer | Check |
| --- | --- |
| Inno Setup | SHA-256 of every file |
| WiX Burn | Size and SHA-1 or SHA-512 of every payload |
| 7-Zip self-extracting archive | CRC-32 of every file |
| NSIS | CRC-32 of the installer as a whole, unless it was built without one |
| Windows Installer | Checksums of the data blocks in its cabinets; file names and folders are not covered |
| Other archives | What the archive format has, such as the CRC-32 of every file in ZIP, 7z and RAR archives |

A failed check stops the run with an error, and files written up to that point remain in the output folder.
Digital signatures are not verified, so these checks guard against damage, not against tampering.

With `--checksum`, the file is first compared with a checksum that you have for it, typically from the page it was
downloaded from. Give the hexadecimal digits of the file's SHA-256, SHA-512, SHA-1 or MD5: either alone, in which
case their number tells the kind, or behind the kind, as GitHub writes it: `sha256:8a5a67...`. Nothing in the file
is interpreted before the checksum has matched, and a file that does not match is refused with an error. Nothing
is printed about a checksum that matches, unless `-v` is given. A match says that the file is the one the checksum
was made for. That also covers what the checks above cannot, such as the file names in a Windows Installer package,
but it is worth only as much as the source of the checksum. MD5 and SHA-1 guard against damage alone, because a
file can be made to match them. Only the file on the command line is compared, not the installers that `-r` finds
inside it.

## Safety

InstExt treats an installer as data. It does not run the installer, a script in it or a program it carries. It
starts no other program and does not use the network. So nothing that an installer does when it is run happens:
no changes to the registry or the system, no services, no downloads.

An installer is a file from someone else, and the readers are written with that in mind:

- Output paths are built so that they stay inside the output folder, see [What is written](#what-is-written).
- Nothing is written into a folder that already holds something, unless `--force` is given.
- Sizes, counts and offsets in the file are treated as untrusted and checked before use. A file that is damaged
  or cut short ends the run with an error.
- Text from the file cannot steer the terminal: control characters are printed as `?`.
- What is extracted is compared with what the installer records about it, and `--checksum` compares the file with
  a checksum of yours before anything in it is interpreted, see [Verification](#verification).

Every change is built and tested on Windows and on Linux, also with damaged files and with made-up installers
that carry impossible values. The Linux run uses sanitizers that stop on memory errors and undefined behaviour,
and two static analyzers read the sources, see [Testing](#testing).

This lowers the risk. It is no guarantee:

- Extracting does not make a file harmless. A program that comes out of an installer is as trustworthy as the
  installer was, and InstExt scans nothing.
- Digital signatures are not verified, so a tampered installer is not recognized as such.
- Every format except Inno Setup is read with the 7-Zip engine, which is not part of InstExt. Its robustness is
  that of the 7-Zip version in use. The copy in a release is as old as the release: replace or delete that
  `7z.dll` to use a newer one, see [The 7-Zip engine](#the-7-zip-engine).
- The program is written in C++ and has had neither an independent review nor systematic fuzzing.

For a file that you have reason to distrust, use a machine or a sandbox that you can throw away.

## The 7-Zip engine

Every format except Inno Setup is read with the 7-Zip engine: the library `7z.dll` on Windows, or `7z.so` on
Linux, that is part of [7-Zip](https://www.7-zip.org/). InstExt loads it only when a format needs it, so the
program alone is enough for Inno Setup installers.

The engine is looked for in this order:

1. The file given with `--7zip`, and then nowhere else
2. The folder of the `instext` program
3. The folder where 7-Zip installs itself: `%ProgramFiles%\7-Zip` on Windows; `/usr/lib/7zip`, `/usr/lib64/7zip`
   and `/usr/local/lib/7zip` on Linux

`instext --version -v` tells which engine that leads to, and the version of it. Without `-v` the engine is not
looked for.

A release of InstExt for Windows has a `7z.dll` next to `instext.exe`, so it works without an installed 7-Zip,
and by the order above that copy is the one used. The notes of the release say which 7-Zip version it is. You can
replace the file with another version, delete it so that an installed 7-Zip is found instead, or leave it and
name another one with `--7zip`.

It must be the complete `7z` library. The smaller `7za` and `7zxa` libraries only handle 7z archives. It must
also match InstExt in architecture: a 64-bit program loads a 64-bit library. On Linux it must come from 7-Zip
23.01 or later; the `7z.so` of p7zip and of older 7-Zip versions does not work.

## Building

You need:

- CMake 3.21 or later
- A C++20 compiler; so far it has been built with the compiler of Visual Studio (MSVC) and with GCC
- The [vcpkg](https://github.com/microsoft/vcpkg) package manager, which supplies the libraries listed in `vcpkg.json`
- Ninja, for the commands below; the CMake tools of Visual Studio include it

During the first configure step, vcpkg downloads and builds the libraries, which takes a few minutes.

### Windows

Open the "x64 Native Tools Command Prompt" of Visual Studio, so that the compiler is found, and go to the folder
of this repository. With vcpkg in `C:\vcpkg`:

```bat
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Release ^
    -D CMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake ^
    -D VCPKG_TARGET_TRIPLET=x64-windows-static-md ^
    -D VCPKG_INSTALL_OPTIONS=--no-print-usage ^
    -D CMAKE_INSTALL_PREFIX=.
cmake --build build --target install
```

The `^` continues a command on the next line. In PowerShell, end those lines with a backtick instead.

This leaves the program in `bin\instext.exe`. The version, which `instext --version` prints and Windows shows in
the file's properties, comes from the `project()` line in `CMakeLists.txt`. The copyright line there comes from
`LICENSE.txt`.

The triplet decides how the libraries that vcpkg supplies, such as liblzma and pugixml, are linked.
`x64-windows-static-md` builds them into `instext.exe`, which then depends only on the Visual C++ Redistributable.
With a triplet that builds DLLs, such as `x64-windows`, the install step copies those DLLs next to the program.

The 7-Zip engine is a different matter: it is never linked into the program, whatever the triplet. `7z.dll` always
stays a separate file, which InstExt loads when a format needs it. If 7-Zip is installed in its default folder,
the install step copies its `7z.dll` next to the program; see `INSTEXT_7ZIP_ENGINE` below.

### Linux

With vcpkg in `~/vcpkg`:

```sh
cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Release \
    -D CMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -D VCPKG_INSTALL_OPTIONS=--no-print-usage
cmake --build build
```

This leaves the program in `build/instext`. To build some of the libraries, vcpkg needs `pkg-config`.

### Options

These CMake options matter here:

- `VCPKG_TARGET_TRIPLET`: whether the libraries that vcpkg supplies are linked into the program or built as DLLs,
  as described for Windows above. It has no effect on the 7-Zip engine.
- `VCPKG_INSTALL_OPTIONS`: extra options for vcpkg. `--no-print-usage` keeps it from printing usage notes for
  every library during the configure step.
- `CMAKE_INSTALL_PREFIX`: where `--target install` puts the result. The program goes into `bin` below it.
- `INSTEXT_7ZIP_ENGINE`: the engine file to install next to the program. On Windows the default is the `7z.dll`
  of an installed 7-Zip, if there is one. Set it to an empty value to install none.
- `BUILD_TESTING`: set it to `OFF` to build the program without the tests and their helper programs.

Install with `cmake --build build --target install`, which builds first. A bare `cmake --install build` copies
whatever was built last. The install step also installs `LICENSE.txt` and `THIRD-PARTY-NOTICES.txt`: next to the
program on Windows, so that the folder stands on its own, and in `share/doc/instext` below the prefix on Linux.
See [License](#license) and [Third-party components](#third-party-components).

## Testing

The tests run `instext` on real installers and compare the result with what is known about them:

```sh
ctest --test-dir build --output-on-failure
```

The first run downloads about 75 MiB of installers from the release pages of their projects into `tests/samples`,
which git ignores, and checks each against the SHA-256 hash in `tests/samples.cmake`. A run leaves about 280 MiB
of extracted files in the build folder. The tests use the engine given with `INSTEXT_7ZIP_ENGINE`, or else the one
that InstExt finds by itself. They also check that damaged copies and a bundle that is cut short are refused,
that Inno Setup installers are read with no engine in reach, and that file names outside ASCII pass through the
command line unharmed. Some small Inno Setup installers and Burn bundles are made by the tests themselves.

On GitHub, the workflow in `.github/workflows/build.yml` builds the program and runs these tests for every push to
`main`, for every pull request and three times a month by schedule. It does so twice: on Windows with the commands
above, where the `bin` folder is kept as an artifact of the run, and on Linux as a debug build with the address
and undefined behaviour sanitizers of GCC. The Linux job also reads the sources with
[Cppcheck](https://cppcheck.sourceforge.io/) and with [Clang-Tidy](https://clang.llvm.org/extra/clang-tidy/), and
marks what they find at the lines concerned.

The workflow also drafts a release on GitHub, in two cases and only after both jobs have passed. Started by hand
on `main` with the option for a draft release ticked, it names the release after the version in `CMakeLists.txt`,
as in `v0.1.0`, and GitHub creates the tag of that name when the draft is published. That takes a version that
has no tag yet. A pushed tag that starts with `v` gets a draft as well, and has to name that version. The draft
holds a zip of the `bin` folder from the Windows job and a file with the SHA-256 checksum of that zip. Its notes
list the commits since the tag before and give the output of `instext --version --verbose`. Publishing the draft
is done by hand.

## Limitations

- **Inno Setup versions.** Only the setup data formats `7.0.0.3` (Inno Setup 7.0.0 to 7.1.0) and `7.1.1` are
  known. Inno Setup changes this format in many releases, so a newer version needs an update of the reader; the
  comment at the top of `src/inno.cpp` says which Inno Setup source files define it. Installers from Inno Setup 6
  and older are rejected with an error.
- **Inno Setup features.** Encrypted installers, installers built with `UseSetupLdr=no` and modified Inno Setup
  builds are not supported. The `[Code]` script comes out as compiled bytecode, and the install script is not
  reconstructed. An installer whose setup data, meaning its tables and not its files, exceeds 256 MiB is refused.
- **Windows Installer.** Only files are extracted, not registry entries or shortcuts. Text in a code page other
  than UTF-8 or Windows-1252 is read as Windows-1252, which can garble file names. Files that are stored outside
  cabinets are listed, not copied. Patches (`.msp`) and merge modules (`.msm`) are untested.
- **WiX Burn.** Only bundles with cabinet containers are read.
- **NSIS and 7-Zip archives.** These are read as far as the 7-Zip engine reads them. The NSIS script is not
  recovered.
- **Other installers.** InstallShield and other kinds are not recognized. Neither is a cabinet that is attached
  to a program.
- **Other archives.** They are read one level deep, and archives that need a password are refused.
- **Long paths on Windows.** A file whose full path in the output folder is longer than about 260 characters
  cannot be written, and the run stops with an error. Extract to a folder with a short path.

## Source layout

| File | Content |
| --- | --- |
| `src/main.cpp` | Command line and format detection |
| `src/inno.cpp` | Inno Setup reader |
| `src/msi.cpp` | Windows Installer tables |
| `src/burn.cpp` | WiX Burn bundles |
| `src/sevenzip.cpp` | Loading the 7-Zip engine; NSIS, 7-Zip self-extracting and other archives |
| `src/common.hpp`, `src/common.cpp` | Shared types and helpers |
| `src/report.hpp`, `src/report.cpp` | What is printed: the lines of a report |
| `src/hash.hpp`, `src/hash.cpp` | Checksums: the `--checksum` option and the hashes of Burn payloads |
| `src/about.cpp` | What `--version -v` adds: compiler, libraries and the engine found |
| `src/instext.rc.in` | Version information for the Windows program |
| `tests/` | The tests, and the list of installers they download |
| `.github/workflows/build.yml` | The builds, test runs and static analysis on GitHub |

The format readers were written from these descriptions of the formats:

- Inno Setup: its sources at [jrsoftware/issrc](https://github.com/jrsoftware/issrc), `Projects/Src`
- WiX Burn: the Burn engine of the [WiX Toolset](https://github.com/wixtoolset/wix),
  `src/burn/engine/section.cpp`
- Windows Installer: the table and string storage as implemented by [Wine](https://www.winehq.org/), `dlls/msi`

## Third-party components

These libraries are linked into the program:

| Library | Used for | License |
| --- | --- | --- |
| [bit7z](https://github.com/rikyoz/bit7z) | Talking to the 7-Zip engine | MPL-2.0 |
| [liblzma](https://tukaani.org/xz/) (XZ Utils) | Inno Setup installers compressed with LZMA or LZMA2 | 0BSD |
| [zlib](https://zlib.net/) | Inno Setup installers compressed with zlib | Zlib |
| [bzip2](https://sourceware.org/bzip2/) | Inno Setup installers compressed with bzip2 | bzip2-1.0.6 |
| [pugixml](https://pugixml.org/) | The manifest of a Burn bundle | MIT |
| [Boost.Hash2](https://www.boost.org/library/latest/hash2/) | SHA-1, SHA-256 and SHA-512 hashes | BSL-1.0 |
| [Boost.Nowide](https://www.boost.org/library/latest/nowide/) | Text conversion, Unicode on Windows | BSL-1.0 |
| [Boost.Program_options](https://www.boost.org/library/latest/program_options/) | The command line | BSL-1.0 |

These three Boost libraries depend on further Boost libraries, all under the same license.
`THIRD-PARTY-NOTICES.txt`, described next, lists them.

`NOTICE.txt` acknowledges the works that the format readers were written from. When the program is built, the
license texts of the libraries above are appended to it, as vcpkg recorded them for the versions that were built.
The result is installed with the program as `THIRD-PARTY-NOTICES.txt`.

The 7-Zip engine is not part of the program. It is loaded at run time and has its own license: the GNU LGPL, with
an unRAR restriction and BSD licenses for parts of the code. When the engine is installed next to the program,
`THIRD-PARTY-NOTICES.txt` also holds 7-Zip's license information and says where its source code is, at
[www.7-zip.org](https://www.7-zip.org/). If you pass on `7z.dll` together with InstExt, pass that file on too.

## License

InstExt is licensed under the MIT License, see [LICENSE.txt](LICENSE.txt). The libraries it is built with and
the 7-Zip engine have their own licenses, see [Third-party components](#third-party-components).
