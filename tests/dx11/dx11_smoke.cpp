/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_3.h>
#include <dxgi1_2.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define check_hr(expr) check_hr_(__LINE__, #expr, (expr))
#define check_hr_eq(expr, expected) check_hr_eq_(__LINE__, #expr, (expr), (expected))
#define check_true(expr) check_true_(__LINE__, #expr, !!(expr))

static void check_hr_(int line, const char *expr, HRESULT hr)
{
  if (SUCCEEDED(hr)) {
    printf("ok %d - %s hr=%#lx\n", line, expr, (unsigned long)hr);
    return;
  }

  printf("not ok %d - %s hr=%#lx\n", line, expr, (unsigned long)hr);
  ++g_failures;
}

static void check_hr_eq_(int line, const char *expr, HRESULT hr, HRESULT expected)
{
  if (hr == expected) {
    printf("ok %d - %s hr=%#lx\n", line, expr, (unsigned long)hr);
    return;
  }

  printf("not ok %d - %s hr=%#lx expected=%#lx\n",
         line, expr, (unsigned long)hr, (unsigned long)expected);
  ++g_failures;
}

static void check_true_(int line, const char *expr, bool value)
{
  if (value) {
    printf("ok %d - %s\n", line, expr);
    return;
  }

  printf("not ok %d - %s\n", line, expr);
  ++g_failures;
}

template <typename T>
static void release_object(T **object)
{
  if (*object) {
    (*object)->Release();
    *object = nullptr;
  }
}

static HRESULT create_device(ID3D11Device **device, ID3D11DeviceContext **context)
{
  static const D3D_FEATURE_LEVEL feature_levels[] = {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
  };

  D3D_FEATURE_LEVEL created_level;
  return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels,
                           ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
                           device, &created_level, context);
}

static void test_device_and_buffer(void)
{
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  ID3D11Device1 *device1 = nullptr;
  ID3D11Buffer *buffer = nullptr;
  D3D11_BUFFER_DESC desc = {};
  D3D11_SUBRESOURCE_DATA initial_data = {};

  HRESULT hr = create_device(&device, &context);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  check_hr(device->QueryInterface(__uuidof(ID3D11Device1), (void **)&device1));

  static const uint32_t data[] = { 1, 2, 3, 4 };
  desc.ByteWidth = sizeof(data);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

  initial_data.pSysMem = data;
  check_hr(device->CreateBuffer(&desc, &initial_data, &buffer));

done:
  release_object(&buffer);
  release_object(&device1);
  release_object(&context);
  release_object(&device);
}

static void test_clear_and_readback(void)
{
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  ID3D11Texture2D *texture = nullptr;
  ID3D11Texture2D *readback = nullptr;
  ID3D11RenderTargetView *rtv = nullptr;
  D3D11_TEXTURE2D_DESC texture_desc = {};
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const float clear_color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };

  HRESULT hr = create_device(&device, &context);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  texture_desc.Width = 4;
  texture_desc.Height = 4;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_DEFAULT;
  texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  check_hr(device->CreateTexture2D(&texture_desc, nullptr, &texture));
  if (!texture)
    goto done;

  check_hr(device->CreateRenderTargetView(texture, nullptr, &rtv));
  if (!rtv)
    goto done;

  context->ClearRenderTargetView(rtv, clear_color);

  texture_desc.Usage = D3D11_USAGE_STAGING;
  texture_desc.BindFlags = 0;
  texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  check_hr(device->CreateTexture2D(&texture_desc, nullptr, &readback));
  if (!readback)
    goto done;

  context->CopyResource(readback, texture);

  check_hr(context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
  if (mapped.pData) {
    const uint8_t *pixel = static_cast<const uint8_t *>(mapped.pData);
    printf("dx11_clear_readback rgba=%u,%u,%u,%u\n",
           pixel[0], pixel[1], pixel[2], pixel[3]);
    check_true(pixel[0] >= 62 && pixel[0] <= 66);
    check_true(pixel[1] >= 126 && pixel[1] <= 130);
    check_true(pixel[2] >= 190 && pixel[2] <= 194);
    check_true(pixel[3] == 255);
    context->Unmap(readback, 0);
  }

done:
  release_object(&rtv);
  release_object(&readback);
  release_object(&texture);
  release_object(&context);
  release_object(&device);
}

static void test_dxgi_factory(void)
{
  IDXGIFactory2 *factory = nullptr;
  IDXGIAdapter1 *adapter = nullptr;
  DXGI_ADAPTER_DESC1 desc = {};

  check_hr(CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void **)&factory));
  if (!factory)
    goto done;

  check_hr(factory->EnumAdapters1(0, &adapter));
  if (!adapter)
    goto done;

  check_hr(adapter->GetDesc1(&desc));
  printf("dx11_adapter vendor=%#x device=%#x flags=%#x\n",
         desc.VendorId, desc.DeviceId, desc.Flags);

done:
  release_object(&adapter);
  release_object(&factory);
}

static void test_video_format_queries(void)
{
  static const DXGI_FORMAT formats[] = {
    DXGI_FORMAT_NV12,
    DXGI_FORMAT_P010,
    DXGI_FORMAT_P016,
  };

  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  ID3D11Texture2D *texture = nullptr;
  HRESULT hr = create_device(&device, &context);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  for (UINT i = 0; i < ARRAYSIZE(formats); ++i) {
    UINT support = 0xdeadbeef;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT feature_support = {};
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 feature_support2 = {};
    D3D11_TEXTURE2D_DESC desc = {};

    hr = device->CheckFormatSupport(formats[i], &support);
    check_hr(hr);
    check_true(support == 0);

    feature_support.InFormat = formats[i];
    feature_support.OutFormatSupport = 0xdeadbeef;
    hr = device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT,
                                     &feature_support, sizeof(feature_support));
    check_hr(hr);
    check_true(feature_support.OutFormatSupport == 0);

    feature_support2.InFormat = formats[i];
    feature_support2.OutFormatSupport2 = 0xdeadbeef;
    hr = device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2,
                                     &feature_support2, sizeof(feature_support2));
    check_hr(hr);
    check_true(feature_support2.OutFormatSupport2 == 0);

    desc.Width = 16;
    desc.Height = 16;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = formats[i];
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    check_hr_eq(device->CreateTexture2D(&desc, nullptr, &texture), E_INVALIDARG);
    check_true(texture == nullptr);
  }

done:
  release_object(&texture);
  release_object(&context);
  release_object(&device);
}

int main(int argc, char **argv)
{
  bool run_device = true;
  bool run_clear = true;
  bool run_dxgi = true;
  bool run_video_formats = true;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--list")) {
      puts("device-and-buffer");
      puts("clear-and-readback");
      puts("dxgi-factory");
      puts("video-format-queries");
      return 0;
    }
    if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
      const char *filter = argv[++i];
      run_device = strstr("device-and-buffer", filter) != nullptr;
      run_clear = strstr("clear-and-readback", filter) != nullptr;
      run_dxgi = strstr("dxgi-factory", filter) != nullptr;
      run_video_formats = strstr("video-format-queries", filter) != nullptr;
    }
  }

  if (run_device)
    test_device_and_buffer();
  if (run_clear)
    test_clear_and_readback();
  if (run_dxgi)
    test_dxgi_factory();
  if (run_video_formats)
    test_video_format_queries();

  printf("dx11_smoke: %s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
  return g_failures ? 1 : 0;
}
