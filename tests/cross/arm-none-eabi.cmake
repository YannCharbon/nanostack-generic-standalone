# Toolchain for the cross-build check: arm-none-eabi-gcc from PATH, Cortex-M33.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -ffunction-sections -fdata-sections -Os")
# nosys: newlib's system calls are stubs, there is no OS.
set(CMAKE_EXE_LINKER_FLAGS_INIT "--specs=nosys.specs")
