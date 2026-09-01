#pragma once

#include <string>

#include "preproc_engine.h"

// Host services shared by every tool driver that runs the PA5 preprocessor:
// the stat-based file identity used for #pragma once and the build stamp
// behind __DATE__ and __TIME__.

bool PA5GetFileId(const std::string& path, PA5FileId& out_fileid);

PreprocBuildInfo PreprocHostBuildInfo();
