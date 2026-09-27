cmake_minimum_required(VERSION 3.20.0)

# Flash with OpenOCD; the board default is STM32CubeProgrammer.
set(BOARD_FLASH_RUNNER openocd)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rc_io_card)

target_include_directories(app PRIVATE ../common/include src)
target_sources(app PRIVATE
  src/main.c
  src/arbiter.c
  src/leds.c
  src/io_bit.c
  src/mode.c
  ../common/proto.c
)
