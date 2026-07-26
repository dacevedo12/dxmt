#include "d3d12_draw_visibility_scope.hpp"

#include <utility>

namespace dxmt::d3d12 {

Rc<VisibilityResultQuery>
BeginReplayDrawVisibilityQuery(ArgumentEncodingContext &enc,
                               Rc<VisibilityResultQuery> &visibility_query) {
  Rc<VisibilityResultQuery> active_visibility_query;
  if (visibility_query) {
    active_visibility_query = visibility_query;
    enc.beginVisibilityResultQuery(std::move(visibility_query));
    enc.bumpVisibilityResultOffset();
  }
  return active_visibility_query;
}

void
EndReplayDrawVisibilityQuery(
    ArgumentEncodingContext &enc,
    Rc<VisibilityResultQuery> &active_visibility_query) {
  if (active_visibility_query) {
    enc.endVisibilityResultQuery(std::move(active_visibility_query));
    enc.bumpVisibilityResultOffset();
  }
}

} // namespace dxmt::d3d12
