// stb_image configuration shared by gfx.c and stb_impl.c. vendor/stb_image.h is
// the v2.30 copy bundled with SOIL2 (from SwitchThemeInjector's sources); SOIL2
// added DDS/PVR/PKM/QOI/ext loaders, all turned off here so only stb's JPEG
// decoder is compiled.
#pragma once
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_DDS
#define STBI_NO_PVR
#define STBI_NO_PKM
#define STBI_NO_QOI
#define STBI_NO_EXT
#include "vendor/stb_image.h"
