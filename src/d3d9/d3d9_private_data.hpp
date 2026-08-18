#pragma once

#include "../util/com/com_private_data.hpp"
#include "d3d9.h"

namespace dxmt {

// IDirect3DResource9 private-data plumbing, shared by every resource type
// (surface, texture, cube, volume, buffer). ComPrivateData is the DXGI/D3D11
// store: it speaks DXGI_ERROR_* and the DXGI size-query shape. These wrappers
// translate it to the D3D9 contract wined3d's d3d9_resource_*_private_data
// implements, so every resource routes through one path instead of
// re-deriving it.

inline HRESULT
D3D9SetPrivateData(ComPrivateData &store, REFGUID guid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  // D3DSPD_IUNKNOWN stores an interface pointer: D3D9 requires the size to be
  // exactly sizeof(IUnknown *) and rejects any other size with INVALIDCALL.
  if (Flags & D3DSPD_IUNKNOWN) {
    if (SizeOfData != sizeof(IUnknown *))
      return D3DERR_INVALIDCALL;
    return store.setInterface(guid, static_cast<const IUnknown *>(pData));
  }
  return store.setData(guid, static_cast<UINT>(SizeOfData), pData);
}

inline HRESULT
D3D9GetPrivateData(ComPrivateData &store, REFGUID guid, void *pData, DWORD *pSizeOfData) {
  // wined3d d3d9_resource_get_private_data: an absent tag returns
  // D3DERR_NOTFOUND and leaves the caller's size untouched; a present tag
  // always writes the stored size back, treats a null data pointer as a size
  // query (S_OK), and returns D3DERR_MOREDATA when the buffer is too small.
  // ComPrivateData surfaces the first two as DXGI_ERROR_*; map them, and skip
  // the size write-back on the not-found path so the caller's value survives.
  UINT size = pSizeOfData ? static_cast<UINT>(*pSizeOfData) : 0;
  HRESULT hr = store.getData(guid, &size, pData);
  if (hr == DXGI_ERROR_NOT_FOUND)
    return D3DERR_NOTFOUND;
  if (pSizeOfData)
    *pSizeOfData = size;
  if (hr == DXGI_ERROR_MORE_DATA)
    return D3DERR_MOREDATA;
  return hr;
}

inline HRESULT
D3D9FreePrivateData(ComPrivateData &store, REFGUID guid) {
  // ComPrivateData has no dedicated free; setData(0, nullptr) erases and
  // returns S_FALSE for an absent tag, which D3D9 reports as D3DERR_NOTFOUND.
  HRESULT hr = store.setData(guid, 0, nullptr);
  return hr == S_FALSE ? D3DERR_NOTFOUND : hr;
}

} // namespace dxmt
