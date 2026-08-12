// SPDX-FileCopyrightText: 2026 Henrique Freire <hfreire10@hotmail.com>

// SPDX-License-Identifier: MIT

#pragma once

#define TLS_CA_TAG 1

const unsigned char g_google_ca[] = {
#include "google_root_ca.inc"
};
