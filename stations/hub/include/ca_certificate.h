// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#pragma once

#define TLS_CA_TAG  1
#define SERVER_HOST "script.google.com"

const unsigned char g_google_ca[] = {
#include "google_root_ca.inc"
    0x00,
};
