/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "d3d9.h"

#include <algorithm>
#include <cstring>

// Pure D3D9 query-contract decisions factored out of MTLD3D9Query / CreateQuery
// so the per-type data size, the creatable-type matrix and the CREATED-state
// poison fill stand free of a Metal device (the
// d3d9_viewport.hpp / d3d9_gamma.hpp shape). References: wined3d (PRIMARY)
// dlls/d3d9/query.c (d3d9_query_init size overrides, the 0xdd CREATED-state
// rewrite) + dlls/wined3d/query.c (wined3d_query_create support set); DXVK
// (secondary) src/d3d9/d3d9_query.cpp.

namespace dxmt {

// Byte count GetDataSize reports for a query type; the buffer size an app sizes
// to the documented type. wined3d's per-type table with the two d3d9-layer
// overrides (query.c: OCCLUSION -> DWORD, DISJOINT -> BOOL). Unknown / unhandled
// types report 0 so the app can tell the slot is unusable.
inline DWORD
d3d9_query_data_size(D3DQUERYTYPE type) {
  switch (type) {
  case D3DQUERYTYPE_OCCLUSION:
    return sizeof(DWORD); // pixel count
  case D3DQUERYTYPE_EVENT:
    return sizeof(BOOL); // signaled flag
  case D3DQUERYTYPE_TIMESTAMP:
    return sizeof(UINT64); // GPU clock tick
  case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    return sizeof(BOOL);
  case D3DQUERYTYPE_TIMESTAMPFREQ:
    return sizeof(UINT64);
  default:
    return 0;
  }
}

// The creatable-type matrix CreateQuery answers: the five types dxmt backs
// (OCCLUSION, EVENT, and the three TIMESTAMP-family), everything else
// D3DERR_NOTAVAILABLE. wined3d accepts the enum range in query_init but
// wined3d_query_create fails the rest, so native-on-non-NVIDIA and dxmt both
// return NOTAVAILABLE (DXVK additionally fakes VCACHE; not adopted here).
inline bool
d3d9_query_supported(D3DQUERYTYPE type) {
  return type == D3DQUERYTYPE_OCCLUSION || type == D3DQUERYTYPE_EVENT || type == D3DQUERYTYPE_TIMESTAMP ||
         type == D3DQUERYTYPE_TIMESTAMPDISJOINT || type == D3DQUERYTYPE_TIMESTAMPFREQ;
}

// The CREATED-state GetData poison: wined3d returns INVALIDCALL for a query
// polled before its first Issue(END), which the d3d9 wrapper rewrites to S_OK
// after zeroing the whole caller span and stamping the leading data_size bytes
// with 0xdd (query.c). Caller guards pData != null.
inline void
d3d9_poison_created_query(void *pData, DWORD dwSize, DWORD dataSize) {
  std::memset(pData, 0, dwSize);
  std::memset(pData, 0xdd, std::min<DWORD>(dwSize, dataSize));
}

} // namespace dxmt
