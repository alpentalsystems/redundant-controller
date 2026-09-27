cmake_minimum_required(VERSION 3.20)
project(rc_host_tests C)

enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(test_proto test_proto.c ${REPO}/common/proto.c)
target_include_directories(test_proto PRIVATE ${REPO}/common/include)
add_test(NAME proto COMMAND test_proto)

add_executable(test_arbiter test_arbiter.c ${REPO}/io-card/src/arbiter.c)
target_include_directories(test_arbiter PRIVATE ${REPO}/common/include ${REPO}/io-card/src)
add_test(NAME arbiter COMMAND test_arbiter)

add_executable(test_role test_role.c ${REPO}/central/src/role.c)
target_include_directories(test_role PRIVATE ${REPO}/common/include ${REPO}/central/src)
add_test(NAME role COMMAND test_role)

add_executable(test_io_bit test_io_bit.c ${REPO}/io-card/src/io_bit.c)
target_include_directories(test_io_bit PRIVATE ${REPO}/io-card/src)
add_test(NAME io_bit COMMAND test_io_bit)

add_executable(test_mode test_mode.c ${REPO}/io-card/src/mode.c)
target_include_directories(test_mode PRIVATE ${REPO}/common/include ${REPO}/io-card/src)
add_test(NAME mode COMMAND test_mode)

add_executable(test_bit test_bit.c ${REPO}/central/src/bit.c)
target_include_directories(test_bit PRIVATE ${REPO}/central/src)
add_test(NAME bit COMMAND test_bit)

add_executable(test_cmd test_cmd.c ${REPO}/central/src/cmd.c ${REPO}/central/src/bit.c)
target_include_directories(test_cmd PRIVATE ${REPO}/common/include ${REPO}/central/src)
add_test(NAME cmd COMMAND test_cmd)

add_test(NAME rcctl COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test_rcctl.py)

add_executable(test_logrec test_logrec.c ${REPO}/common/logrec.c ${REPO}/common/proto.c)
target_include_directories(test_logrec PRIVATE ${REPO}/common/include)
add_test(NAME logrec COMMAND test_logrec)

add_executable(test_logstore test_logstore.c ${REPO}/central/src/logstore.c ${REPO}/common/logrec.c
	       ${REPO}/common/proto.c)
target_include_directories(test_logstore PRIVATE ${REPO}/common/include ${REPO}/central/src)
add_test(NAME logstore COMMAND test_logstore)
