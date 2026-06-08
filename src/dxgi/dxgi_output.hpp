#pragma once
#include "dxmt_gamma.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"

namespace dxmt {

struct MTLDXGIOutput : public MTLDXGIObject<IDXGIOutput6> {
  virtual const DXMTGammaRamp *STDMETHODCALLTYPE GetGammaRamp() = 0;
};

} // namespace dxmt
