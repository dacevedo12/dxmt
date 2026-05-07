/*
 * Copyright 2016-2018 Józef Kucia for CodeWeavers
 * Copyright 2020-2021 Hans-Kristian Arntzen for Valve Corporation
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This file ports a small DXMT smoke-oriented subset from the upstream D3D12
 * test style. It intentionally keeps the cases compact so they can replace
 * the old external smoke loop without importing the whole suite.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

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

static D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type)
{
  D3D12_HEAP_PROPERTIES props = {};
  props.Type = type;
  return props;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size)
{
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return desc;
}

static D3D12_RESOURCE_DESC texture_desc(UINT width, UINT height, DXGI_FORMAT format,
                                        D3D12_RESOURCE_FLAGS flags)
{
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;
  return desc;
}

static D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource,
                                         D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after)
{
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  return barrier;
}

static HRESULT create_device(ID3D12Device **device)
{
  return D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                           __uuidof(ID3D12Device), (void **)device);
}

static HRESULT create_queue(ID3D12Device *device, ID3D12CommandQueue **queue)
{
  D3D12_COMMAND_QUEUE_DESC desc = {};
  desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  return device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), (void **)queue);
}

static HRESULT wait_queue_idle(ID3D12Device *device, ID3D12CommandQueue *queue)
{
  ID3D12Fence *fence = nullptr;
  HANDLE event = nullptr;
  HRESULT hr;

  hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&fence);
  if (FAILED(hr))
    goto done;

  event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!event) {
    hr = HRESULT_FROM_WIN32(GetLastError());
    goto done;
  }

  hr = queue->Signal(fence, 1);
  if (FAILED(hr))
    goto done;

  if (fence->GetCompletedValue() < 1) {
    hr = fence->SetEventOnCompletion(1, event);
    if (FAILED(hr))
      goto done;
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
      hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }

done:
  if (event)
    CloseHandle(event);
  release_object(&fence);
  return hr;
}

static void test_create_device(void)
{
  ID3D12Device *device = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  ID3D12Fence *fence = nullptr;
  ID3D12DescriptorHeap *heap = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};

  HRESULT hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  check_hr(create_queue(device, &queue));
  check_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          __uuidof(ID3D12CommandAllocator),
                                          (void **)&allocator));
  check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                     nullptr, __uuidof(ID3D12GraphicsCommandList),
                                     (void **)&list));
  if (list)
    check_hr(list->Close());

  check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                               (void **)&fence));

  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 1;
  check_hr(device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
                                        (void **)&heap));

done:
  release_object(&heap);
  release_object(&fence);
  release_object(&list);
  release_object(&allocator);
  release_object(&queue);
  release_object(&device);
}

static void test_create_device_arguments(void)
{
  ID3D12Device *device = nullptr;

  check_hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                             __uuidof(ID3D12Device), (void **)&device));
  release_object(&device);

  check_hr_eq(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                __uuidof(ID3D12Device), nullptr), S_FALSE);
  check_hr_eq(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                __uuidof(ID3D12DeviceChild), nullptr), S_FALSE);

  check_hr_eq(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_9_1,
                                __uuidof(ID3D12Device), (void **)&device), E_INVALIDARG);
  check_hr_eq(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_9_2,
                                __uuidof(ID3D12Device), (void **)&device), E_INVALIDARG);
  check_hr_eq(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_9_3,
                                __uuidof(ID3D12Device), (void **)&device), E_INVALIDARG);
  check_hr_eq(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_10_0,
                                __uuidof(ID3D12Device), (void **)&device), E_INVALIDARG);
  check_hr_eq(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_10_1,
                                __uuidof(ID3D12Device), (void **)&device), E_INVALIDARG);
  check_hr_eq(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL(0),
                                __uuidof(ID3D12Device), (void **)&device), E_INVALIDARG);
  check_hr_eq(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL(~0u),
                                __uuidof(ID3D12Device), (void **)&device), E_INVALIDARG);
}

static void test_device_properties(void)
{
  ID3D12Device *device = nullptr;
  D3D12_FEATURE_DATA_ARCHITECTURE architecture = {};
  D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels = {};
  D3D12_FEATURE_DATA_FORMAT_INFO format_info = {};
  static const D3D_FEATURE_LEVEL requested_levels[] = {
    D3D_FEATURE_LEVEL_12_2,
    D3D_FEATURE_LEVEL_12_1,
    D3D_FEATURE_LEVEL_12_0,
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
  };

  HRESULT hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  check_true(device->GetNodeCount() >= 1 && device->GetNodeCount() <= 32);

  hr = device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture,
                                   sizeof(architecture));
  check_hr(hr);
  if (SUCCEEDED(hr)) {
    check_true(!architecture.NodeIndex);
    check_true(!architecture.CacheCoherentUMA || architecture.UMA);
  }

  feature_levels.NumFeatureLevels = ARRAYSIZE(requested_levels);
  feature_levels.pFeatureLevelsRequested = requested_levels;
  hr = device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &feature_levels,
                                   sizeof(feature_levels));
  check_hr(hr);
  if (SUCCEEDED(hr)) {
    check_true(feature_levels.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_11_0);
  }

  format_info.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format_info,
                                   sizeof(format_info));
  check_hr(hr);
  if (SUCCEEDED(hr)) {
    check_true(format_info.PlaneCount == 1);
  }

  feature_levels.NumFeatureLevels = 0;
  feature_levels.pFeatureLevelsRequested = nullptr;
  check_hr_eq(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &feature_levels,
                                          sizeof(feature_levels)), E_INVALIDARG);

done:
  release_object(&device);
}

static void test_create_descriptor_heap(void)
{
  ID3D12Device *device = nullptr;
  ID3D12DescriptorHeap *heap = nullptr;
  ID3D12Device *heap_device = nullptr;
  ID3D12Object *object = nullptr;
  ID3D12DeviceChild *device_child = nullptr;
  ID3D12Pageable *pageable = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  HRESULT hr;

  hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = 16;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  check_hr(device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap),
                                        (void **)&heap));
  if (heap) {
    check_hr(heap->GetDevice(__uuidof(ID3D12Device), (void **)&heap_device));
    check_true(heap_device == device);
    check_hr(heap->QueryInterface(__uuidof(ID3D12Object), (void **)&object));
    check_hr(heap->QueryInterface(__uuidof(ID3D12DeviceChild), (void **)&device_child));
    check_hr(heap->QueryInterface(__uuidof(ID3D12Pageable), (void **)&pageable));
    release_object(&pageable);
    release_object(&device_child);
    release_object(&object);
    release_object(&heap_device);
    release_object(&heap);
  }

  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check_hr(device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap),
                                        (void **)&heap));
  release_object(&heap);

  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  check_hr(device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap),
                                        (void **)&heap));
  release_object(&heap);

  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  check_hr(device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap),
                                        (void **)&heap));
  release_object(&heap);

  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check_hr_eq(device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap),
                                           (void **)&heap), E_INVALIDARG);

  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  check_hr(device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap),
                                        (void **)&heap));
  release_object(&heap);

  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  check_hr_eq(device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap),
                                           (void **)&heap), E_INVALIDARG);

  check_true(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) > 0);
  check_true(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) > 0);
  check_true(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) > 0);
  check_true(device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV) > 0);

done:
  release_object(&heap);
  release_object(&heap_device);
  release_object(&pageable);
  release_object(&device_child);
  release_object(&object);
  release_object(&device);
}

static void test_create_query_heap(void)
{
  ID3D12Device *device = nullptr;
  ID3D12QueryHeap *query_heap = nullptr;
  D3D12_QUERY_HEAP_DESC desc = {};
  HRESULT hr;

  static const D3D12_QUERY_HEAP_TYPE types[] = {
    D3D12_QUERY_HEAP_TYPE_OCCLUSION,
    D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
    D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS,
  };

  hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  desc.Count = 1;
  for (UINT i = 0; i < ARRAYSIZE(types); ++i) {
    desc.Type = types[i];
    check_hr(device->CreateQueryHeap(&desc, __uuidof(ID3D12QueryHeap),
                                     (void **)&query_heap));
    release_object(&query_heap);
  }

  desc.Type = D3D12_QUERY_HEAP_TYPE_SO_STATISTICS;
  hr = device->CreateQueryHeap(&desc, __uuidof(ID3D12QueryHeap), (void **)&query_heap);
  if (hr == E_NOTIMPL) {
    printf("skip - stream output query heap is not implemented\n");
  } else {
    check_hr(hr);
    release_object(&query_heap);
  }

done:
  release_object(&query_heap);
  release_object(&device);
}

static void test_create_fence(void)
{
  ID3D12Device *device = nullptr;
  ID3D12Fence *fence = nullptr;
  ID3D12Device *fence_device = nullptr;
  ID3D12Object *object = nullptr;
  ID3D12DeviceChild *device_child = nullptr;
  ID3D12Pageable *pageable = nullptr;
  HRESULT hr;

  hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  check_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                               (void **)&fence));
  if (fence) {
    check_hr(fence->GetDevice(__uuidof(ID3D12Device), (void **)&fence_device));
    check_true(fence_device == device);
    check_hr(fence->QueryInterface(__uuidof(ID3D12Object), (void **)&object));
    check_hr(fence->QueryInterface(__uuidof(ID3D12DeviceChild), (void **)&device_child));
    check_hr(fence->QueryInterface(__uuidof(ID3D12Pageable), (void **)&pageable));
    check_true(fence->GetCompletedValue() == 0);
    release_object(&pageable);
    release_object(&device_child);
    release_object(&object);
    release_object(&fence_device);
    release_object(&fence);
  }

  check_hr(device->CreateFence(99, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                               (void **)&fence));
  if (fence)
    check_true(fence->GetCompletedValue() == 99);

done:
  release_object(&fence);
  release_object(&fence_device);
  release_object(&pageable);
  release_object(&device_child);
  release_object(&object);
  release_object(&device);
}

static void test_fence_values(void)
{
  ID3D12Device *device = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  ID3D12Fence *fence = nullptr;
  uint64_t next_value = UINT64_C(1) << 60;
  HRESULT hr;

  hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  check_hr(create_queue(device, &queue));
  check_hr(device->CreateFence(next_value, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                               (void **)&fence));
  if (!queue || !fence)
    goto done;

  check_true(fence->GetCompletedValue() == next_value);

  for (UINT i = 0; i < 8; ++i) {
    ++next_value;
    check_hr(queue->Signal(fence, next_value));
    check_hr(wait_queue_idle(device, queue));
    check_true(fence->GetCompletedValue() == next_value);
  }

  next_value += 10000;
  check_hr(fence->Signal(next_value));
  check_true(fence->GetCompletedValue() == next_value);
  check_hr(fence->Signal(0));
  check_true(fence->GetCompletedValue() == 0);

done:
  release_object(&fence);
  release_object(&queue);
  release_object(&device);
}

static void test_create_heap(void)
{
  ID3D12Device *device = nullptr;
  ID3D12Heap *heap = nullptr;
  ID3D12Device *heap_device = nullptr;
  ID3D12Object *object = nullptr;
  ID3D12DeviceChild *device_child = nullptr;
  ID3D12Pageable *pageable = nullptr;
  D3D12_HEAP_DESC desc = {};
  D3D12_HEAP_DESC got = {};
  HRESULT hr;

  hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  desc.SizeInBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  desc.Alignment = 0;
  desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
  check_hr(device->CreateHeap(&desc, __uuidof(ID3D12Heap), (void **)&heap));
  if (heap) {
    check_hr(heap->GetDevice(__uuidof(ID3D12Device), (void **)&heap_device));
    check_true(heap_device == device);
    check_hr(heap->QueryInterface(__uuidof(ID3D12Object), (void **)&object));
    check_hr(heap->QueryInterface(__uuidof(ID3D12DeviceChild), (void **)&device_child));
    check_hr(heap->QueryInterface(__uuidof(ID3D12Pageable), (void **)&pageable));
    got = heap->GetDesc();
    check_true(got.SizeInBytes == desc.SizeInBytes);
    check_true(got.Properties.Type == desc.Properties.Type);
    check_true(got.Flags == desc.Flags);
    release_object(&pageable);
    release_object(&device_child);
    release_object(&object);
    release_object(&heap_device);
    release_object(&heap);
  }

  desc.SizeInBytes = 0;
  check_hr_eq(device->CreateHeap(&desc, __uuidof(ID3D12Heap), (void **)&heap),
              E_INVALIDARG);

  desc.SizeInBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES | D3D12_HEAP_FLAG_ALLOW_DISPLAY;
  check_hr_eq(device->CreateHeap(&desc, __uuidof(ID3D12Heap), (void **)&heap),
              E_INVALIDARG);

  desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
  desc.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
  check_hr(device->CreateHeap(&desc, __uuidof(ID3D12Heap), (void **)&heap));
  release_object(&heap);

  desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT * 2;
  check_hr_eq(device->CreateHeap(&desc, __uuidof(ID3D12Heap), (void **)&heap),
              E_INVALIDARG);

done:
  release_object(&heap);
  release_object(&heap_device);
  release_object(&pageable);
  release_object(&device_child);
  release_object(&object);
  release_object(&device);
}

static void test_map_resource(void)
{
  ID3D12Device *device = nullptr;
  ID3D12Resource *resource = nullptr;
  D3D12_HEAP_PROPERTIES heap = {};
  D3D12_RESOURCE_DESC desc = {};
  void *data = nullptr;
  HRESULT hr;

  hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  desc = buffer_desc(256);
  check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                           __uuidof(ID3D12Resource), (void **)&resource));
  if (resource) {
    data = (void *)(uintptr_t)0xdeadbeef;
    check_hr_eq(resource->Map(0, nullptr, &data), E_INVALIDARG);
    check_true(data == (void *)(uintptr_t)0xdeadbeef);
    release_object(&resource);
  }

  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           __uuidof(ID3D12Resource), (void **)&resource));
  if (resource) {
    D3D12_RANGE read_range = { 0, 0 };
    data = nullptr;
    check_hr(resource->Map(0, &read_range, &data));
    check_true(data != nullptr);
    if (data)
      memset(data, 0x5a, 16);
    resource->Unmap(0, nullptr);
    release_object(&resource);
  }

  heap.Type = D3D12_HEAP_TYPE_READBACK;
  check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           __uuidof(ID3D12Resource), (void **)&resource));
  if (resource) {
    D3D12_RANGE read_range = { 0, 16 };
    data = nullptr;
    check_hr(resource->Map(0, &read_range, &data));
    check_true(data != nullptr);
    D3D12_RANGE write_range = { 0, 0 };
    resource->Unmap(0, &write_range);
    release_object(&resource);
  }

  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  desc = texture_desc(32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
  check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                           __uuidof(ID3D12Resource), (void **)&resource));
  if (resource) {
    data = (void *)(uintptr_t)0xdeadbeef;
    check_hr_eq(resource->Map(0, nullptr, &data), E_INVALIDARG);
    check_true(data == (void *)(uintptr_t)0xdeadbeef);
  }

done:
  release_object(&resource);
  release_object(&device);
}

static void test_get_copyable_footprints(void)
{
  ID3D12Device *device = nullptr;
  D3D12_RESOURCE_DESC desc = {};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[4] = {};
  UINT row_counts[4] = {};
  UINT64 row_sizes[4] = {};
  UINT64 total_size = 0;
  HRESULT hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  desc = buffer_desc(4);
  device->GetCopyableFootprints(&desc, 0, 1, 0, layouts, row_counts,
                                row_sizes, &total_size);
  check_true(layouts[0].Offset == 0);
  check_true(layouts[0].Footprint.Format == DXGI_FORMAT_UNKNOWN);
  check_true(layouts[0].Footprint.Width == 4);
  check_true(layouts[0].Footprint.Height == 1);
  check_true(layouts[0].Footprint.RowPitch >= 4);
  check_true(row_counts[0] == 1);
  check_true(row_sizes[0] == 4);
  check_true(total_size >= 4);

  desc = texture_desc(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
  device->GetCopyableFootprints(&desc, 0, 1, 1, layouts, row_counts,
                                row_sizes, &total_size);
  check_true(layouts[0].Offset == 1);
  check_true(layouts[0].Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
  check_true(layouts[0].Footprint.Width == 4);
  check_true(layouts[0].Footprint.Height == 4);
  check_true(layouts[0].Footprint.RowPitch == D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  check_true(row_counts[0] == 4);
  check_true(row_sizes[0] == 16);
  check_true(total_size == 3 * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT + 16);

  desc = texture_desc(4, 4, DXGI_FORMAT_BC1_UNORM, D3D12_RESOURCE_FLAG_NONE);
  device->GetCopyableFootprints(&desc, 0, 1, 0, layouts, row_counts,
                                row_sizes, &total_size);
  check_true(layouts[0].Footprint.Width == 4);
  check_true(layouts[0].Footprint.Height == 4);
  check_true(layouts[0].Footprint.RowPitch == D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  check_true(row_counts[0] == 1);
  check_true(row_sizes[0] == 8);
  check_true(total_size == 8);

done:
  release_object(&device);
}

static void test_resource_allocation_info(void)
{
  ID3D12Device *device = nullptr;
  ID3D12Device4 *device4 = nullptr;
  D3D12_RESOURCE_ALLOCATION_INFO info = {};
  D3D12_RESOURCE_ALLOCATION_INFO info1 = {};
  D3D12_RESOURCE_ALLOCATION_INFO1 resource_info[2] = {};
  D3D12_RESOURCE_DESC descs[2] = {};
  HRESULT hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  info = device->GetResourceAllocationInfo(0, 0, nullptr);
  check_true(info.SizeInBytes == 0);
  check_true(info.Alignment == 1);

  descs[0] = buffer_desc(32);
  descs[1] = buffer_desc(120000);
  info = device->GetResourceAllocationInfo(0, 2, descs);
  check_true(info.SizeInBytes >= descs[0].Width + descs[1].Width);
  check_true(info.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
  check_true((info.SizeInBytes % info.Alignment) == 0);

  descs[0] = texture_desc(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
  info = device->GetResourceAllocationInfo(0, 1, descs);
  check_true(info.SizeInBytes >= 8 * 8 * 4);
  check_true(info.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
  check_true((info.SizeInBytes % info.Alignment) == 0);

  hr = device->QueryInterface(__uuidof(ID3D12Device4), (void **)&device4);
  if (SUCCEEDED(hr) && device4) {
    descs[0] = buffer_desc(32);
    descs[1] = buffer_desc(120000);
    info = device->GetResourceAllocationInfo(0, 2, descs);
    info1 = device4->GetResourceAllocationInfo1(0, 2, descs, resource_info);
    check_true(info1.SizeInBytes == info.SizeInBytes);
    check_true(info1.Alignment == info.Alignment);
    check_true(resource_info[0].Offset == 0);
    check_true(resource_info[1].Offset >= resource_info[0].SizeInBytes);
  } else {
    printf("skip - ID3D12Device4 allocation info is not supported\n");
  }

done:
  release_object(&device4);
  release_object(&device);
}

static void test_create_committed_resource(void)
{
  ID3D12Device *device = nullptr;
  ID3D12Resource *resource = nullptr;
  D3D12_HEAP_PROPERTIES heap = {};
  D3D12_RESOURCE_DESC desc = {};
  D3D12_CLEAR_VALUE clear_value = {};
  HRESULT hr;

  hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  desc = texture_desc(32, 32, DXGI_FORMAT_R8G8B8A8_UNORM,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  clear_value.Color[0] = 1.0f;
  clear_value.Color[1] = 0.0f;
  clear_value.Color[2] = 0.0f;
  clear_value.Color[3] = 1.0f;

  check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           &clear_value, __uuidof(ID3D12Resource),
                                           (void **)&resource));
  if (resource) {
    ID3D12Object *object = nullptr;
    ID3D12DeviceChild *device_child = nullptr;
    ID3D12Pageable *pageable = nullptr;
    ID3D12Device *unexpected_device = nullptr;
    D3D12_RESOURCE_DESC got = resource->GetDesc();
    check_hr(resource->QueryInterface(__uuidof(ID3D12Object), (void **)&object));
    check_hr(resource->QueryInterface(__uuidof(ID3D12DeviceChild), (void **)&device_child));
    check_hr(resource->QueryInterface(__uuidof(ID3D12Pageable), (void **)&pageable));
    check_hr_eq(resource->QueryInterface(__uuidof(ID3D12Device), (void **)&unexpected_device),
                E_NOINTERFACE);
    check_true(got.MipLevels == 1);
    check_true(resource->GetGPUVirtualAddress() == 0);
    release_object(&unexpected_device);
    release_object(&pageable);
    release_object(&device_child);
    release_object(&object);
    release_object(&resource);
  }

  desc.MipLevels = 0;
  check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           &clear_value, __uuidof(ID3D12Resource),
                                           (void **)&resource));
  if (resource) {
    D3D12_RESOURCE_DESC got = resource->GetDesc();
    check_true(got.MipLevels == 6);
    release_object(&resource);
  }

  desc.MipLevels = 1;
  desc.SampleDesc.Count = 0;
  check_hr_eq(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              &clear_value, __uuidof(ID3D12Resource),
                                              (void **)&resource), E_INVALIDARG);
  desc.SampleDesc.Count = 1;

  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  check_hr_eq(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              nullptr, __uuidof(ID3D12Resource),
                                              (void **)&resource), E_INVALIDARG);
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  check_hr_eq(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET |
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                              &clear_value, __uuidof(ID3D12Resource),
                                              (void **)&resource), E_INVALIDARG);

  heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  check_hr_eq(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, __uuidof(ID3D12Resource),
                                              (void **)&resource), E_INVALIDARG);

  heap.Type = D3D12_HEAP_TYPE_READBACK;
  check_hr_eq(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, __uuidof(ID3D12Resource),
                                              (void **)&resource), E_INVALIDARG);

done:
  release_object(&resource);
  release_object(&device);
}

static void test_command_list_basics(void)
{
  ID3D12Device *device = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  ID3D12DescriptorHeap *rtv_heap = nullptr;
  ID3D12DescriptorHeap *dsv_heap = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
  ID3D12CommandList *lists[] = { nullptr };
  HRESULT hr;

  hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  check_hr(create_queue(device, &queue));
  check_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          __uuidof(ID3D12CommandAllocator),
                                          (void **)&allocator));
  check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                     nullptr, __uuidof(ID3D12GraphicsCommandList),
                                     (void **)&list));
  if (!list)
    goto done;

  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 4;
  check_hr(device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
                                        (void **)&rtv_heap));
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  check_hr(device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
                                        (void **)&dsv_heap));
  if (!rtv_heap || !dsv_heap)
    goto done;

  rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();

  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
  list->OMSetRenderTargets(1, &rtv, TRUE, &dsv);
  list->OMSetRenderTargets(0, &rtv, TRUE, &dsv);
  list->OMSetRenderTargets(0, &rtv, FALSE, &dsv);
  list->OMSetRenderTargets(0, nullptr, TRUE, &dsv);
  list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);

  check_hr(list->Close());
  check_hr_eq(list->Close(), E_FAIL);

  if (queue) {
    lists[0] = list;
    queue->ExecuteCommandLists(1, lists);
    check_hr(wait_queue_idle(device, queue));
  }

  check_hr(allocator->Reset());
  check_hr(list->Reset(allocator, nullptr));
  check_hr(list->Close());

done:
  release_object(&dsv_heap);
  release_object(&rtv_heap);
  release_object(&list);
  release_object(&allocator);
  release_object(&queue);
  release_object(&device);
}

static void test_copy_buffer_readback(void)
{
  ID3D12Device *device = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  ID3D12Resource *upload = nullptr;
  ID3D12Resource *default_buffer = nullptr;
  ID3D12Resource *readback = nullptr;
  D3D12_RESOURCE_DESC desc = {};
  D3D12_HEAP_PROPERTIES upload_heap = {};
  D3D12_HEAP_PROPERTIES default_heap = {};
  D3D12_HEAP_PROPERTIES readback_heap = {};
  void *mapped = nullptr;
  D3D12_RESOURCE_BARRIER barrier = {};
  ID3D12CommandList *lists[] = { nullptr };
  const uint32_t *readback_data = nullptr;
  D3D12_RANGE read_range = {};

  static const uint32_t expected[] = {
    0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00,
  };

  HRESULT hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  check_hr(create_queue(device, &queue));
  check_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          __uuidof(ID3D12CommandAllocator),
                                          (void **)&allocator));
  check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                     nullptr, __uuidof(ID3D12GraphicsCommandList),
                                     (void **)&list));

  desc = buffer_desc(sizeof(expected));
  upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
  default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
  readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);

  check_hr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           __uuidof(ID3D12Resource), (void **)&upload));
  check_hr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           __uuidof(ID3D12Resource), (void **)&default_buffer));
  check_hr(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           __uuidof(ID3D12Resource), (void **)&readback));
  if (!upload || !default_buffer || !readback || !list)
    goto done;

  check_hr(upload->Map(0, nullptr, &mapped));
  if (mapped) {
    memcpy(mapped, expected, sizeof(expected));
    upload->Unmap(0, nullptr);
  }

  list->CopyBufferRegion(default_buffer, 0, upload, 0, sizeof(expected));
  barrier = transition(default_buffer,
      D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &barrier);
  list->CopyBufferRegion(readback, 0, default_buffer, 0, sizeof(expected));
  check_hr(list->Close());

  lists[0] = list;
  queue->ExecuteCommandLists(1, lists);
  check_hr(wait_queue_idle(device, queue));

  read_range.Begin = 0;
  read_range.End = sizeof(expected);
  check_hr(readback->Map(0, &read_range, (void **)&readback_data));
  if (readback_data) {
    printf("dx12_copy_readback values=%08x,%08x,%08x,%08x\n",
           readback_data[0], readback_data[1], readback_data[2], readback_data[3]);
    check_true(!memcmp(readback_data, expected, sizeof(expected)));
    D3D12_RANGE write_range = { 0, 0 };
    readback->Unmap(0, &write_range);
  }

done:
  release_object(&readback);
  release_object(&default_buffer);
  release_object(&upload);
  release_object(&list);
  release_object(&allocator);
  release_object(&queue);
  release_object(&device);
}

static void fill_upload_texture(ID3D12Resource *upload, const uint32_t *pixels,
                                UINT width, UINT height)
{
  uint8_t *mapped = nullptr;
  if (FAILED(upload->Map(0, nullptr, (void **)&mapped)) || !mapped)
    return;

  memset(mapped, 0, 256 * height);
  for (UINT y = 0; y < height; ++y)
    memcpy(mapped + y * 256, pixels + y * width, width * sizeof(*pixels));

  upload->Unmap(0, nullptr);
}

static void test_copy_texture_region(void)
{
  ID3D12Device *device = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  ID3D12Resource *src_upload = nullptr;
  ID3D12Resource *dst_upload = nullptr;
  ID3D12Resource *src_texture = nullptr;
  ID3D12Resource *dst_texture = nullptr;
  ID3D12Resource *readback = nullptr;
  D3D12_HEAP_PROPERTIES upload_heap = {};
  D3D12_HEAP_PROPERTIES default_heap = {};
  D3D12_HEAP_PROPERTIES readback_heap = {};
  D3D12_RESOURCE_DESC upload_desc = {};
  D3D12_RESOURCE_DESC tex_desc = {};
  D3D12_TEXTURE_COPY_LOCATION src_location = {};
  D3D12_TEXTURE_COPY_LOCATION dst_location = {};
  D3D12_RESOURCE_BARRIER barrier = {};
  D3D12_BOX box = {};
  ID3D12CommandList *lists[] = { nullptr };
  const uint8_t *readback_data = nullptr;
  D3D12_RANGE read_range = {};

  static const uint32_t clear_data[] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
  };
  static const uint32_t bitmap_data[] = {
    0xff00ff00, 0xff00ff01, 0xff00ff02, 0xff00ff03,
    0xff00ff10, 0xff00ff11, 0xff00ff12, 0xff00ff13,
    0xff00ff20, 0xff00ff21, 0xff00ff22, 0xff00ff23,
    0xff00ff30, 0xff00ff31, 0xff00ff32, 0xff00ff33,
  };
  static const uint32_t expected[] = {
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0xff00ff00, 0xff00ff01, 0x00000000,
    0x00000000, 0xff00ff10, 0xff00ff11, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
  };

  HRESULT hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  check_hr(create_queue(device, &queue));
  check_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          __uuidof(ID3D12CommandAllocator),
                                          (void **)&allocator));
  check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                     nullptr, __uuidof(ID3D12GraphicsCommandList),
                                     (void **)&list));

  upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
  default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
  readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
  upload_desc = buffer_desc(256 * 4);
  tex_desc = texture_desc(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM,
                          D3D12_RESOURCE_FLAG_NONE);

  check_hr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
                                           &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                           nullptr, __uuidof(ID3D12Resource),
                                           (void **)&src_upload));
  check_hr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
                                           &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                           nullptr, __uuidof(ID3D12Resource),
                                           (void **)&dst_upload));
  check_hr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
                                           &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, __uuidof(ID3D12Resource),
                                           (void **)&src_texture));
  check_hr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
                                           &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, __uuidof(ID3D12Resource),
                                           (void **)&dst_texture));
  check_hr(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                           &upload_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, __uuidof(ID3D12Resource),
                                           (void **)&readback));
  if (!src_upload || !dst_upload || !src_texture || !dst_texture || !readback || !list)
    goto done;

  fill_upload_texture(src_upload, bitmap_data, 4, 4);
  fill_upload_texture(dst_upload, clear_data, 4, 4);

  src_location.pResource = src_upload;
  src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  src_location.PlacedFootprint.Footprint.Width = 4;
  src_location.PlacedFootprint.Footprint.Height = 4;
  src_location.PlacedFootprint.Footprint.Depth = 1;
  src_location.PlacedFootprint.Footprint.RowPitch = 256;

  dst_location.pResource = src_texture;
  dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst_location.SubresourceIndex = 0;
  list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);

  src_location.pResource = dst_upload;
  dst_location.pResource = dst_texture;
  list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);

  barrier = transition(src_texture, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &barrier);

  src_location.pResource = src_texture;
  src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src_location.SubresourceIndex = 0;
  dst_location.pResource = dst_texture;
  dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst_location.SubresourceIndex = 0;
  box.left = 0;
  box.top = 0;
  box.front = 0;
  box.right = 2;
  box.bottom = 2;
  box.back = 1;
  list->CopyTextureRegion(&dst_location, 1, 1, 0, &src_location, &box);

  barrier = transition(dst_texture, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &barrier);

  dst_location.pResource = readback;
  dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  dst_location.PlacedFootprint.Footprint.Width = 4;
  dst_location.PlacedFootprint.Footprint.Height = 4;
  dst_location.PlacedFootprint.Footprint.Depth = 1;
  dst_location.PlacedFootprint.Footprint.RowPitch = 256;
  src_location.pResource = dst_texture;
  src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src_location.SubresourceIndex = 0;
  list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
  check_hr(list->Close());

  lists[0] = list;
  queue->ExecuteCommandLists(1, lists);
  check_hr(wait_queue_idle(device, queue));

  read_range.Begin = 0;
  read_range.End = 256 * 4;
  check_hr(readback->Map(0, &read_range, (void **)&readback_data));
  if (readback_data) {
    for (UINT y = 0; y < 4; ++y) {
      const uint32_t *row = (const uint32_t *)(readback_data + y * 256);
      for (UINT x = 0; x < 4; ++x) {
        printf("dx12_copy_texture pixel[%u,%u]=%08x expected=%08x\n",
               x, y, row[x], expected[y * 4 + x]);
        check_true(row[x] == expected[y * 4 + x]);
      }
    }
    D3D12_RANGE write_range = { 0, 0 };
    readback->Unmap(0, &write_range);
  }

done:
  release_object(&readback);
  release_object(&dst_texture);
  release_object(&src_texture);
  release_object(&dst_upload);
  release_object(&src_upload);
  release_object(&list);
  release_object(&allocator);
  release_object(&queue);
  release_object(&device);
}

static void test_clear_texture_readback(void)
{
  ID3D12Device *device = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  ID3D12DescriptorHeap *rtv_heap = nullptr;
  ID3D12Resource *texture = nullptr;
  ID3D12Resource *readback = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  D3D12_RESOURCE_DESC tex_desc = {};
  D3D12_CLEAR_VALUE clear_value = {};
  D3D12_HEAP_PROPERTIES default_heap = {};
  D3D12_HEAP_PROPERTIES readback_heap = {};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
  D3D12_RESOURCE_BARRIER barrier = {};
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  D3D12_TEXTURE_COPY_LOCATION src = {};
  ID3D12CommandList *lists[] = { nullptr };
  const uint8_t *readback_data = nullptr;
  D3D12_RANGE read_range = {};
  D3D12_RESOURCE_DESC readback_desc = {};
  const float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };

  HRESULT hr = create_device(&device);
  check_hr(hr);
  if (FAILED(hr))
    goto done;

  check_hr(create_queue(device, &queue));
  check_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          __uuidof(ID3D12CommandAllocator),
                                          (void **)&allocator));
  check_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                     nullptr, __uuidof(ID3D12GraphicsCommandList),
                                     (void **)&list));

  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 1;
  check_hr(device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
                                        (void **)&rtv_heap));

  tex_desc = texture_desc(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM,
                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  clear_value.Color[0] = 0.25f;
  clear_value.Color[1] = 0.5f;
  clear_value.Color[2] = 0.75f;
  clear_value.Color[3] = 1.0f;
  default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
  check_hr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           &clear_value, __uuidof(ID3D12Resource),
                                           (void **)&texture));
  if (!texture || !rtv_heap || !list)
    goto done;

  rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(texture, nullptr, rtv);
  list->ClearRenderTargetView(rtv, color, 0, nullptr);

  readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
  readback_desc = buffer_desc(256 * 4);
  check_hr(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                           &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, __uuidof(ID3D12Resource),
                                           (void **)&readback));
  if (!readback)
    goto done;

  barrier = transition(texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &barrier);

  dst.pResource = readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  dst.PlacedFootprint.Footprint.Width = 4;
  dst.PlacedFootprint.Footprint.Height = 4;
  dst.PlacedFootprint.Footprint.Depth = 1;
  dst.PlacedFootprint.Footprint.RowPitch = 256;

  src.pResource = texture;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;

  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  check_hr(list->Close());

  lists[0] = list;
  queue->ExecuteCommandLists(1, lists);
  check_hr(wait_queue_idle(device, queue));

  read_range.Begin = 0;
  read_range.End = 4;
  check_hr(readback->Map(0, &read_range, (void **)&readback_data));
  if (readback_data) {
    printf("dx12_clear_readback rgba=%u,%u,%u,%u\n",
           readback_data[0], readback_data[1], readback_data[2], readback_data[3]);
    check_true(readback_data[0] >= 62 && readback_data[0] <= 66);
    check_true(readback_data[1] >= 126 && readback_data[1] <= 130);
    check_true(readback_data[2] >= 190 && readback_data[2] <= 194);
    check_true(readback_data[3] == 255);
    D3D12_RANGE write_range = { 0, 0 };
    readback->Unmap(0, &write_range);
  }

done:
  release_object(&readback);
  release_object(&texture);
  release_object(&rtv_heap);
  release_object(&list);
  release_object(&allocator);
  release_object(&queue);
  release_object(&device);
}

static void test_dxgi_factory(void)
{
  IDXGIFactory4 *factory = nullptr;
  IDXGIAdapter1 *adapter = nullptr;
  DXGI_ADAPTER_DESC1 desc = {};

  check_hr(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void **)&factory));
  if (!factory)
    goto done;

  check_hr(factory->EnumAdapters1(0, &adapter));
  if (!adapter)
    goto done;

  check_hr(adapter->GetDesc1(&desc));
  printf("dx12_adapter vendor=%#x device=%#x flags=%#x\n",
         desc.VendorId, desc.DeviceId, desc.Flags);

done:
  release_object(&adapter);
  release_object(&factory);
}

int main(int argc, char **argv)
{
  bool run_create_device = true;
  bool run_create_device_arguments = true;
  bool run_device_properties = true;
  bool run_descriptor_heap = true;
  bool run_query_heap = true;
  bool run_create_fence = true;
  bool run_fence_values = true;
  bool run_create_heap = true;
  bool run_map_resource = true;
  bool run_footprints = true;
  bool run_allocation_info = true;
  bool run_committed_resource = true;
  bool run_command_list = true;
  bool run_copy_buffer = true;
  bool run_copy_texture = true;
  bool run_clear = true;
  bool run_dxgi = true;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--list")) {
      puts("create-device");
      puts("create-device-arguments");
      puts("device-properties");
      puts("create-descriptor-heap");
      puts("create-query-heap");
      puts("create-fence");
      puts("fence-values");
      puts("create-heap");
      puts("map-resource");
      puts("get-copyable-footprints");
      puts("resource-allocation-info");
      puts("create-committed-resource");
      puts("command-list-basics");
      puts("copy-buffer-readback");
      puts("copy-texture-region");
      puts("clear-texture-readback");
      puts("dxgi-factory");
      return 0;
    }
    if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
      const char *filter = argv[++i];
      run_create_device = strstr("create-device", filter) != nullptr;
      run_create_device_arguments = strstr("create-device-arguments", filter) != nullptr;
      run_device_properties = strstr("device-properties", filter) != nullptr;
      run_descriptor_heap = strstr("create-descriptor-heap", filter) != nullptr;
      run_query_heap = strstr("create-query-heap", filter) != nullptr;
      run_create_fence = strstr("create-fence", filter) != nullptr;
      run_fence_values = strstr("fence-values", filter) != nullptr;
      run_create_heap = strstr("create-heap", filter) != nullptr;
      run_map_resource = strstr("map-resource", filter) != nullptr;
      run_footprints = strstr("get-copyable-footprints", filter) != nullptr;
      run_allocation_info = strstr("resource-allocation-info", filter) != nullptr;
      run_committed_resource = strstr("create-committed-resource", filter) != nullptr;
      run_command_list = strstr("command-list-basics", filter) != nullptr;
      run_copy_buffer = strstr("copy-buffer-readback", filter) != nullptr;
      run_copy_texture = strstr("copy-texture-region", filter) != nullptr;
      run_clear = strstr("clear-texture-readback", filter) != nullptr;
      run_dxgi = strstr("dxgi-factory", filter) != nullptr;
    }
  }

  if (run_create_device)
    test_create_device();
  if (run_create_device_arguments)
    test_create_device_arguments();
  if (run_device_properties)
    test_device_properties();
  if (run_descriptor_heap)
    test_create_descriptor_heap();
  if (run_query_heap)
    test_create_query_heap();
  if (run_create_fence)
    test_create_fence();
  if (run_fence_values)
    test_fence_values();
  if (run_create_heap)
    test_create_heap();
  if (run_map_resource)
    test_map_resource();
  if (run_footprints)
    test_get_copyable_footprints();
  if (run_allocation_info)
    test_resource_allocation_info();
  if (run_committed_resource)
    test_create_committed_resource();
  if (run_command_list)
    test_command_list_basics();
  if (run_copy_buffer)
    test_copy_buffer_readback();
  if (run_copy_texture)
    test_copy_texture_region();
  if (run_clear)
    test_clear_texture_readback();
  if (run_dxgi)
    test_dxgi_factory();

  printf("dx12_smoke: %s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
  return g_failures ? 1 : 0;
}
