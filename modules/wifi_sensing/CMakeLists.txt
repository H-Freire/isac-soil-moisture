# SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

# SPDX-License-Identifier: MIT

zephyr_library()

zephyr_include_directories(include)
zephyr_library_sources(src/wifi_sensing.c src/lora.c)
