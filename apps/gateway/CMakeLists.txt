# SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_LIST_DIR}/../../modules/wifi_sensing)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gateway)

target_sources(app PRIVATE src/main.c)
