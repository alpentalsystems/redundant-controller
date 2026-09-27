cmake_minimum_required(VERSION 3.20)
project(rc_central C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -O2)

add_executable(rc-central src/main.c src/role.c src/bit.c src/cmd.c ../common/proto.c)
target_include_directories(rc-central PRIVATE ../common/include src)
install(TARGETS rc-central DESTINATION bin)
