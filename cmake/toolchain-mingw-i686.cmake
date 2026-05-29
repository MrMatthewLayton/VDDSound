# Cross-compile vddsound.dll for 32-bit Windows XP from macOS (Homebrew MinGW-w64).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
set(TARGET_TRIPLE i686-w64-mingw32)

# Resolve the Homebrew prefix (/opt/homebrew on Apple Silicon, /usr/local on Intel).
execute_process(COMMAND brew --prefix mingw-w64
                OUTPUT_VARIABLE MINGW_PREFIX
                OUTPUT_STRIP_TRAILING_WHITESPACE)

set(CMAKE_C_COMPILER   ${TARGET_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${TARGET_TRIPLE}-g++)
set(CMAKE_RC_COMPILER  ${TARGET_TRIPLE}-windres)
set(CMAKE_DLLTOOL      ${TARGET_TRIPLE}-dlltool)

set(CMAKE_FIND_ROOT_PATH "${MINGW_PREFIX}/toolchain-i686/${TARGET_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
