# The MiSTer's DE10-Nano: a Cortex-A9 with NEON, 32-bit hard-float Linux (Debian's arm-linux-gnueabihf-gcc)
#   cmake -G Ninja -B build-mister -DCMAKE_TOOLCHAIN_FILE=port/cmake/mister.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(MVC2_MISTER ON CACHE BOOL "the MiSTer front end")
