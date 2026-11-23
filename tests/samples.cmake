# Downloads the installers that the tests run on, unless they are already
# there. ctest runs this first; by hand: cmake -P tests/samples.cmake
# Every file is pinned to one release and checked against its SHA-256 hash, so
# the numbers the tests expect stay true.
if(NOT DEFINED folder)
    set(folder "${CMAKE_CURRENT_LIST_DIR}/samples")
endif()

function(sample url sha256)
    get_filename_component(name "${url}" NAME)
    if(NOT EXISTS "${folder}/${name}")
        message(STATUS "Downloading ${name}")
    endif()
    file(DOWNLOAD "${url}" "${folder}/${name}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON)
endfunction()

sample(https://github.com/jrsoftware/issrc/releases/download/is-7_1_1/innosetup-7.1.1-x64.exe
       8a5a6711c3766a6e5fb74f280ba0e3551e719ce94538d901fdbd1f8f9920b3f0)
sample(https://github.com/jrsoftware/issrc/releases/download/is-6_7_4/innosetup-6.7.4.exe
       a549929319f9120716677c685535ca4671e4b937b5fdc87e2585f32d8eb30e8c)
sample(https://github.com/cli/cli/releases/download/v2.105.0/gh_2.105.0_windows_amd64.msi
       d18367836c7dcf689ad2052730acf3ec6b9e8b1259a8bae3d512584f680d7525)
sample(https://github.com/rainmeter/rainmeter/releases/download/v4.5.28.3936/Rainmeter-4.5.28.exe
       de6e010f8c28882bc28dcd8f319c899f34dfddec51e1e4dd4e7a23f68ebfe14d)
sample(https://github.com/ip7z/7zip/releases/download/26.06/7z2606-x64.exe
       9b697f65ecc2d9b0152f5ee96d72dde522a7827690349d9307935dacf40f42ec)
sample(https://github.com/wixtoolset/wix3/releases/download/wix3141rtm/wix314.exe
       6bf6d03d6923d9ef827ae1d943b90b42b8ebb1b0f68ef6d55f868fa34c738a29)
