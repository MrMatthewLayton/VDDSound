cmake_minimum_required(VERSION 3.16)
project(vddsound C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Generate the ntvdm.exe import library from the hand-written .def with dlltool.
set(NTVDM_DEF    "${CMAKE_SOURCE_DIR}/defs/ntvdm.def")
set(NTVDM_IMPLIB "${CMAKE_BINARY_DIR}/libntvdm.a")
add_custom_command(
    OUTPUT  "${NTVDM_IMPLIB}"
    COMMAND "${CMAKE_DLLTOOL}" -k -d "${NTVDM_DEF}" -l "${NTVDM_IMPLIB}"
    DEPENDS "${NTVDM_DEF}"
    COMMENT "Generating libntvdm.a from ntvdm.def"
    VERBATIM)
add_custom_target(ntvdm_implib DEPENDS "${NTVDM_IMPLIB}")

set(VDD_SOURCES
    src/vdd_entry.c
    src/logger.c
    src/midi_module.c
    src/opl_module.c
    src/sb_module.c
    src/audio_out.c
    src/crt0.c           # DLL entry + mem* (CRT-free, XP has no UCRT)
    src/opl3.c)          # vendored Nuked OPL3 (LGPL-2.1+, kept separable)

add_library(vddsound SHARED ${VDD_SOURCES})
add_dependencies(vddsound ntvdm_implib)
set_target_properties(vddsound PROPERTIES PREFIX "" OUTPUT_NAME "vddsound" SUFFIX ".dll")

# Our code is clean under -Wall -Wextra; the vendored core is built relaxed.
set_source_files_properties(
    src/vdd_entry.c src/logger.c src/midi_module.c
    src/opl_module.c src/sb_module.c src/audio_out.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wpedantic")

# Our hand-rolled mem* must not be turned back into self-referential calls.
set_source_files_properties(src/crt0.c PROPERTIES
    COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")

# Link CRT-free: -nostdlib drops the msvcrt/UCRT startup and default libs; we
# provide the entry point ourselves and pull only XP-present system DLLs plus
# static libgcc (for 64-bit divide / stack probes). Order matters for ld.
# --exclude-all-symbols: the VDD exposes nothing by name (NTVDM loads it by
# path and enters via DllMain); don't leak the vendored OPL core's symbols.
target_link_options(vddsound PRIVATE -nostdlib -Wl,--exclude-all-symbols)
target_link_libraries(vddsound PRIVATE
    "${NTVDM_IMPLIB}"
    winmm
    dsound
    user32
    kernel32
    gcc)
