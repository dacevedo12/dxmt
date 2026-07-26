#pragma once

#include "dxmt_context.hpp"
#include "dxmt_occlusion_query.hpp"

namespace dxmt::d3d12 {

// Opens an occlusion query around the draw that follows. `visibility_query` is
// moved into the encoder; the returned handle is the one that has to be closed
// by EndReplayDrawVisibilityQuery and is null when no query was requested.
[[nodiscard]] Rc<VisibilityResultQuery>
BeginReplayDrawVisibilityQuery(ArgumentEncodingContext &enc,
                               Rc<VisibilityResultQuery> &visibility_query);

// Closes the query opened by BeginReplayDrawVisibilityQuery. A null handle is
// ignored, so this can be called unconditionally after the draw.
void EndReplayDrawVisibilityQuery(
    ArgumentEncodingContext &enc,
    Rc<VisibilityResultQuery> &active_visibility_query);

} // namespace dxmt::d3d12
