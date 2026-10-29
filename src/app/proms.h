// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Andy Taylor
// Flash PROM images: which files in a folder are Ultra 5/10 images (by the
// version string OpenBoot carries), and the newest of them.
#pragma once

#include <string>
#include <vector>

#include "app/settings.h"

namespace uv::app {

struct PromImage {
    std::string path;
    std::string version;  // OpenBoot's, e.g. "3.31"; empty: not an Ultra 5/10 image
    std::string date;     // when it was built, e.g. "2001/07/25"
};

// Identifies one image file; `version` is empty for anything else.
PromImage identify_prom(const std::string& path);
// Every Ultra 5/10 image in `folder`, newest first.
std::vector<PromImage> scan_proms(const std::string& folder);
// The folder searched for images: the setting, else "roms" in the current
// folder if there is one, else the user's UltraViolet roms folder.
std::string prom_folder(const Settings& s);
// The newest image in the PROM folder; false, with a message saying what is
// needed and where it was looked for, if there is none.
bool select_prom(const Settings& s, PromImage& out, std::string& error);
bool read_prom(const PromImage& p, std::vector<u8>& data, std::string& error);

}  // namespace uv::app
