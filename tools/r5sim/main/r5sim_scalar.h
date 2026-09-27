// Forced ahead of the render path's sources in this image only: QEMU has no
// PIE (the first ee.vldbc.16 is an IllegalInstruction), so the renderer takes
// its scalar arm here. Rendering is not what the model measures -- it only has
// to consume each turn's submission, or the next patch() is BUSY.
#pragma once
#include "sdkconfig.h"
#undef CONFIG_IDF_TARGET_ESP32S3
