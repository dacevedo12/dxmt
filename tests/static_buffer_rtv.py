#!/usr/bin/env python3
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(message, file=sys.stderr)
        sys.exit(1)


buffer_cpp = read("src/d3d11/d3d11_buffer.cpp")
view_hpp = read("src/d3d11/d3d11_view.hpp")
context_cpp = read("src/d3d11/d3d11_context_impl.cpp")
dxmt_buffer_hpp = read("src/dxmt/dxmt_buffer.hpp")
dxmt_buffer_cpp = read("src/dxmt/dxmt_buffer.cpp")

require(
    re.search(r"CreateRenderTargetView\s*\(\s*const D3D11_RENDER_TARGET_VIEW_DESC1", buffer_cpp) is not None,
    "D3D11Buffer must override CreateRenderTargetView for buffer-backed RTVs",
)
require(
    "D3D11_RTV_DIMENSION_BUFFER" in buffer_cpp,
    "D3D11Buffer::CreateRenderTargetView must validate D3D11_RTV_DIMENSION_BUFFER",
)
require(
    "Buffer *buffer_" in view_hpp and "bufferSlice()" in view_hpp,
    "D3D11RenderTargetView must carry buffer-backed view state",
)
require(
    "bufferRTVTexture" in context_cpp and "clear_res_cmd.begin" in context_cpp,
    "D3D11 immediate context must render and clear buffer-backed RTVs",
)
require(
    "WMTTextureUsage usage" in dxmt_buffer_hpp
    and "descriptor.usage" in dxmt_buffer_cpp
    and "WMTTextureUsageRenderTarget" in buffer_cpp,
    "DXMT buffer texture views must request render-target usage when needed",
)
require(
    "riid == __uuidof(ID3D11Texture2D)" in buffer_cpp
    and "TRACE(\"D3D11Resource(buffer): texture interface query \"" in buffer_cpp
    and "return E_NOINTERFACE;" in buffer_cpp,
    "D3D11Buffer texture QueryInterface probes must stay E_NOINTERFACE but log only at trace level",
)
