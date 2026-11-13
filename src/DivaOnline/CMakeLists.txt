# DivaOnline.dva: Aime card + ALL.Net (ARTEMiS) support for PDAFT 7.10 under PD Loader.
#
# Built with MinGW (llvm-mingw or GCC), like segatools itself: the plugin is plain C and
# shares nothing with the game's MSVC runtime, so it isn't part of AFTMods.sln. See
# build-mingw.sh, or:
#   cmake -S src/DivaOnline -B build/divaonline -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=<mingw toolchain file>
#   cmake --build build/divaonline      -> build/divaonline/DivaOnline.dva
cmake_minimum_required(VERSION 3.20)
project(DivaOnline LANGUAGES C RC)

if(NOT MINGW)
    message(FATAL_ERROR "DivaOnline builds with MinGW (llvm-mingw or x86_64-w64-mingw32-gcc), like segatools.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# No NDEBUG: segatools' util/dprintf.h compiles every log line out when it's defined.
set(CMAKE_C_FLAGS_RELEASE "-O2")

set(CAPN ${CMAKE_CURRENT_SOURCE_DIR}/third_party/capnhook)
set(SEGA ${CMAKE_CURRENT_SOURCE_DIR}/third_party/segatools)

add_library(DivaOnline SHARED
    src/dllmain.c
    src/card.c
    src/reader.c
    src/fixups.c
    src/tlac.c
    src/devicezip.c
    src/zipread.c
    src/dprintf.c
    src/DivaOnline.rc

    # capnhook (MIT): IAT hooks, the I/O request layer and the serial port model
    ${CAPN}/hook/hr.c
    ${CAPN}/hook/iobuf.c
    ${CAPN}/hook/iohook.c
    ${CAPN}/hook/pe.c
    ${CAPN}/hook/peb.c
    ${CAPN}/hook/table.c
    ${CAPN}/hooklib/serial.c
    ${CAPN}/hooklib/uart.c

    # segatools (public domain)
    ${SEGA}/util/crc.c
    ${SEGA}/util/dump.c
    ${SEGA}/util/str.c
    ${SEGA}/hooklib/dns.c
    ${SEGA}/hooklib/path.c
    ${SEGA}/hooklib/reg.c
    ${SEGA}/hooklib/setupapi.c
    ${SEGA}/platform/clock.c
    ${SEGA}/platform/config.c
    ${SEGA}/platform/dns.c
    ${SEGA}/platform/hwmon.c
    ${SEGA}/platform/hwreset.c
    ${SEGA}/platform/misc.c
    ${SEGA}/platform/netenv.c
    ${SEGA}/platform/nusec.c
    ${SEGA}/platform/pcbid.c
    ${SEGA}/platform/vfs.c
    ${SEGA}/amex/config.c
    ${SEGA}/amex/ds.c
    ${SEGA}/amex/eeprom.c
    ${SEGA}/amex/gpio.c
    ${SEGA}/amex/guid.c
    ${SEGA}/amex/nvram.c
    ${SEGA}/amex/sram.c
    ${SEGA}/board/config.c
    ${SEGA}/board/sg-cmd.c
    ${SEGA}/board/sg-frame.c
    ${SEGA}/board/sg-led.c
    ${SEGA}/board/sg-nfc.c
    ${SEGA}/board/sg-reader.c
    ${SEGA}/iccard/aime.c
    ${SEGA}/iccard/felica.c
)

target_include_directories(DivaOnline PRIVATE src ${SEGA} ${CAPN})

# segatools' meson.build project arguments
target_compile_definitions(DivaOnline PRIVATE
    COBJMACROS WIN32_LEAN_AND_MEAN _WIN32_WINNT=_WIN32_WINNT_WIN7 MINGW_HAS_SECURE_API=1)
# segatools builds with precompiled.h as its prefix header (meson c_pch)
target_compile_options(DivaOnline PRIVATE "SHELL:-include ${SEGA}/precompiled.h" -ffunction-sections -fdata-sections -Wall -Wno-unused-function)

set_target_properties(DivaOnline PROPERTIES PREFIX "" SUFFIX ".dva" OUTPUT_NAME DivaOnline)
target_link_options(DivaOnline PRIVATE
    -static -static-libgcc -Wl,--gc-sections -Wl,--exclude-all-symbols)
target_link_libraries(DivaOnline PRIVATE setupapi shlwapi ws2_32 iphlpapi dnsapi)
