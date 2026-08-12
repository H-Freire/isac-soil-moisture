# SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

# SPDX-License-Identifier: CC-BY-SA-4.0

cmake_minimum_required(VERSION 3.20.0)

list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/wifi_sensing)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sink)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE include)

set(gen_dir ${ZEPHYR_BINARY_DIR}/include/generated/)

generate_inc_file_for_target(app src/google-root-ca.pem
                                 ${gen_dir}/google_root_ca.inc)
