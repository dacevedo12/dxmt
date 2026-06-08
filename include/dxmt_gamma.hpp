#pragma once

#include <cstdint>

namespace dxmt {

constexpr uint32_t DXMT_GAMMA_CP_COUNT = 1024;

struct DXMTGammaRamp {
  float red[DXMT_GAMMA_CP_COUNT];
  float green[DXMT_GAMMA_CP_COUNT];
  float blue[DXMT_GAMMA_CP_COUNT];
  uint64_t version;
};

} // namespace dxmt
