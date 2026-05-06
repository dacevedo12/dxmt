/*
 * Copyright 2018 Jozef Kucia for CodeWeavers
 * Copyright 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#define VKD3D_TEST_DECLARE_MAIN
#include "d3d12_crosstest.h"

static void test_create_device(void)
{
    ID3D12Device *device = NULL;
    IUnknown *adapter;
    HRESULT hr;
    ULONG refcount;

    hr = pfn_D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_9_1, &IID_ID3D12Device, (void **)&device);
    ok(hr == E_INVALIDARG, "Got unexpected hr %#x for feature level 9_1.\n", hr);
    ok(!device, "Got unexpected device %p for feature level 9_1.\n", device);
    if (device)
    {
        ID3D12Device_Release(device);
        device = NULL;
    }

    hr = pfn_D3D12CreateDevice(NULL, vkd3d_device_feature_level, &IID_ID3D12CommandQueue, (void **)&device);
    ok(hr == E_NOINTERFACE, "Got unexpected hr %#x for ID3D12CommandQueue.\n", hr);
    ok(!device, "Got unexpected object %p for ID3D12CommandQueue.\n", device);
    if (device)
    {
        IUnknown_Release((IUnknown *)device);
        device = NULL;
    }

    adapter = create_adapter();
    ok(!!adapter, "Failed to create DXGI adapter.\n");

    hr = pfn_D3D12CreateDevice(adapter, vkd3d_device_feature_level, &IID_ID3D12Device, (void **)&device);
    ok(hr == S_OK, "Failed to create D3D12 device, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
    {
        check_interface((IUnknown *)device, &IID_ID3D12Device, true);
        check_interface((IUnknown *)device, &IID_ID3D12Object, true);
        refcount = ID3D12Device_Release(device);
        ok(!refcount, "ID3D12Device has %u references left.\n", (unsigned int)refcount);
    }

    if (adapter)
        IUnknown_Release(adapter);
}

static bool luid_equal(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static void test_adapter_luid(void)
{
    IDXGIFactory4 *factory;
    IDXGIAdapter *adapter;
    ID3D12Device *device;
    DXGI_ADAPTER_DESC desc;
    HRESULT hr;
    LUID luid;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    luid = ID3D12Device_GetAdapterLuid(device);
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory);
    ok(hr == S_OK, "Failed to create IDXGIFactory4, hr %#x.\n", hr);
    if (FAILED(hr))
    {
        ID3D12Device_Release(device);
        return;
    }

    hr = IDXGIFactory4_EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter, (void **)&adapter);
    ok(hr == S_OK, "Failed to find adapter by LUID, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IDXGIAdapter_GetDesc(adapter, &desc);
        ok(hr == S_OK, "Failed to get adapter desc, hr %#x.\n", hr);
        ok(luid_equal(luid, desc.AdapterLuid), "Got unexpected adapter LUID %#x:%#x, expected %#x:%#x.\n",
                desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart, luid.HighPart, luid.LowPart);
        IDXGIAdapter_Release(adapter);
    }

    IDXGIFactory4_Release(factory);
    ID3D12Device_Release(device);
}

static void test_device_parent(void)
{
    ID3D12DeviceChild *device_child;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    IUnknown *parent;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    queue = create_command_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
    hr = ID3D12CommandQueue_QueryInterface(queue, &IID_ID3D12DeviceChild, (void **)&device_child);
    ok(hr == S_OK, "Failed to query ID3D12DeviceChild, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
    {
        parent = NULL;
        hr = ID3D12DeviceChild_GetDevice(device_child, &IID_ID3D12Device, (void **)&parent);
        ok(hr == S_OK, "Failed to get parent device, hr %#x.\n", hr);
        ok(parent == (IUnknown *)device, "Got unexpected parent device %p, expected %p.\n", parent, device);
        if (parent)
            IUnknown_Release(parent);

        parent = NULL;
        hr = ID3D12DeviceChild_GetDevice(device_child, &IID_ID3D12CommandQueue, (void **)&parent);
        ok(hr == E_NOINTERFACE, "Got unexpected hr %#x for unsupported parent interface.\n", hr);
        ok(!parent, "Got unexpected unsupported parent pointer %p.\n", parent);

        ID3D12DeviceChild_Release(device_child);
    }

    ID3D12CommandQueue_Release(queue);
    ID3D12Device_Release(device);
}

static void test_command_queue_and_fence(void)
{
    D3D12_COMMAND_QUEUE_DESC queue_desc;
    ID3D12CommandQueue *queue;
    ID3D12Fence *fence;
    ID3D12Device *device;
    unsigned int i;
    HRESULT hr;

    static const D3D12_COMMAND_LIST_TYPE queue_types[] =
    {
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        D3D12_COMMAND_LIST_TYPE_COPY,
    };

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ok(hr == S_OK, "Failed to create fence, hr %#x.\n", hr);
    if (FAILED(hr))
    {
        ID3D12Device_Release(device);
        return;
    }

    for (i = 0; i < ARRAY_SIZE(queue_types); ++i)
    {
        queue = create_command_queue(device, queue_types[i], D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
        queue_desc = ID3D12CommandQueue_GetDesc(queue);
        ok(queue_desc.Type == queue_types[i], "Got queue type %#x, expected %#x.\n",
                queue_desc.Type, queue_types[i]);
        ok(queue_desc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
                "Got queue priority %#x.\n", queue_desc.Priority);
        ok(queue_desc.Flags == D3D12_COMMAND_QUEUE_FLAG_NONE,
                "Got queue flags %#x.\n", queue_desc.Flags);

        hr = ID3D12CommandQueue_Signal(queue, fence, i + 1);
        ok(hr == S_OK, "Failed to signal queue, hr %#x.\n", hr);
        wait_queue_idle(device, queue);
        ok(ID3D12Fence_GetCompletedValue(fence) >= i + 1,
                "Got completed value %#"PRIx64", expected at least %u.\n",
                ID3D12Fence_GetCompletedValue(fence), i + 1);

        ID3D12CommandQueue_Release(queue);
    }

    ID3D12Fence_Release(fence);
    ID3D12Device_Release(device);
}

static void init_buffer_desc(D3D12_RESOURCE_DESC *desc, UINT64 size)
{
    memset(desc, 0, sizeof(*desc));
    desc->Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc->Width = size;
    desc->Height = 1;
    desc->DepthOrArraySize = 1;
    desc->MipLevels = 1;
    desc->Format = DXGI_FORMAT_UNKNOWN;
    desc->SampleDesc.Count = 1;
    desc->Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
}

static void init_texture_desc(D3D12_RESOURCE_DESC *desc, DXGI_FORMAT format)
{
    memset(desc, 0, sizeof(*desc));
    desc->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc->Width = 32;
    desc->Height = 32;
    desc->DepthOrArraySize = 1;
    desc->MipLevels = 1;
    desc->Format = format;
    desc->SampleDesc.Count = 1;
    desc->Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
}

static void init_heap_properties(D3D12_HEAP_PROPERTIES *properties, D3D12_HEAP_TYPE type)
{
    memset(properties, 0, sizeof(*properties));
    properties->Type = type;
    properties->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties->MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties->CreationNodeMask = 1;
    properties->VisibleNodeMask = 1;
}

static void test_resource_refcount_and_desc(void)
{
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc, desc;
    ID3D12Resource *buffer, *texture;
    ID3D12Device *device;
    ULONG refcount, queried_refcount;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    init_heap_properties(&heap_properties, D3D12_HEAP_TYPE_UPLOAD);
    init_buffer_desc(&resource_desc, 4096);
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&buffer);
    ok(hr == S_OK, "Failed to create upload buffer, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
    {
        desc = ID3D12Resource_GetDesc(buffer);
        ok(desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER, "Got dimension %#x.\n", desc.Dimension);
        ok(desc.Width == resource_desc.Width, "Got width %#"PRIu64", expected %#"PRIu64".\n",
                desc.Width, resource_desc.Width);
        ok(desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR, "Got layout %#x.\n", desc.Layout);
        queried_refcount = get_refcount(buffer);
        ok(queried_refcount == 1, "Got unexpected resource refcount %u.\n",
                (unsigned int)queried_refcount);
        ID3D12Resource_AddRef(buffer);
        queried_refcount = get_refcount(buffer);
        ok(queried_refcount == 2, "Got unexpected resource refcount %u after AddRef.\n",
                (unsigned int)queried_refcount);
        ID3D12Resource_Release(buffer);
        refcount = ID3D12Resource_Release(buffer);
        ok(!refcount, "ID3D12Resource has %u references left.\n", (unsigned int)refcount);
    }

    init_heap_properties(&heap_properties, D3D12_HEAP_TYPE_DEFAULT);
    init_texture_desc(&resource_desc, DXGI_FORMAT_R8G8B8A8_UNORM);
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
            &IID_ID3D12Resource, (void **)&texture);
    ok(hr == S_OK, "Failed to create default texture, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
    {
        desc = ID3D12Resource_GetDesc(texture);
        ok(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D, "Got dimension %#x.\n", desc.Dimension);
        ok(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Got format %#x.\n", desc.Format);
        ok(desc.Width == 32 && desc.Height == 32, "Got size %#"PRIu64"x%u.\n", desc.Width, desc.Height);
        ID3D12Resource_Release(texture);
    }

    ID3D12Device_Release(device);
}

static void test_formats(void)
{
    D3D12_FEATURE_DATA_FORMAT_INFO format_info;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12Device *device;
    unsigned int i;
    HRESULT hr;

    static const DXGI_FORMAT formats[] =
    {
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_UINT,
        DXGI_FORMAT_R32G32B32A32_SINT,
        DXGI_FORMAT_R32G32B32_FLOAT,
        DXGI_FORMAT_R32G32B32_UINT,
        DXGI_FORMAT_R32G32B32_SINT,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_UNORM,
        DXGI_FORMAT_R16G16B16A16_UINT,
        DXGI_FORMAT_R16G16B16A16_SNORM,
        DXGI_FORMAT_R16G16B16A16_SINT,
        DXGI_FORMAT_R32G32_FLOAT,
        DXGI_FORMAT_R32G32_UINT,
        DXGI_FORMAT_R32G32_SINT,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R11G11B10_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UINT,
        DXGI_FORMAT_R8G8B8A8_SNORM,
        DXGI_FORMAT_R8G8B8A8_SINT,
        DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R16G16_UNORM,
        DXGI_FORMAT_R16G16_UINT,
        DXGI_FORMAT_R16G16_SNORM,
        DXGI_FORMAT_R16G16_SINT,
        DXGI_FORMAT_D32_FLOAT,
        DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R32_UINT,
        DXGI_FORMAT_R32_SINT,
        DXGI_FORMAT_R8G8_UNORM,
        DXGI_FORMAT_R8G8_UINT,
        DXGI_FORMAT_R8G8_SNORM,
        DXGI_FORMAT_R8G8_SINT,
        DXGI_FORMAT_R16_FLOAT,
        DXGI_FORMAT_D16_UNORM,
        DXGI_FORMAT_R16_UNORM,
        DXGI_FORMAT_R16_UINT,
        DXGI_FORMAT_R16_SNORM,
        DXGI_FORMAT_R16_SINT,
        DXGI_FORMAT_R8_UNORM,
        DXGI_FORMAT_R8_UINT,
        DXGI_FORMAT_R8_SNORM,
        DXGI_FORMAT_R8_SINT,
    };

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(formats); ++i)
    {
        memset(&format_info, 0, sizeof(format_info));
        format_info.Format = formats[i];
        hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_INFO,
                &format_info, sizeof(format_info));
        ok(hr == S_OK, "Failed to query format %#x info, hr %#x.\n", formats[i], hr);
        if (SUCCEEDED(hr))
            ok(format_info.PlaneCount == 1, "Format %#x has unexpected plane count %u.\n",
                    formats[i], format_info.PlaneCount);

        memset(&format_support, 0, sizeof(format_support));
        format_support.Format = formats[i];
        hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_SUPPORT,
                &format_support, sizeof(format_support));
        ok(hr == S_OK, "Failed to query format %#x support, hr %#x.\n", formats[i], hr);
        if (FAILED(hr))
            continue;

        ok(format_support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D,
                "Format %#x missing TEXTURE2D support, Support1 %#x.\n",
                formats[i], format_support.Support1);

        if (format_support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)
        {
            init_texture_desc(&resource_desc, formats[i]);
            resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ID3D12Device_GetResourceAllocationInfo(device, 0, 1, &resource_desc);
        }
    }

    ID3D12Device_Release(device);
}

static bool feature_level_in_list(D3D_FEATURE_LEVEL level, const D3D_FEATURE_LEVEL *levels, unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; ++i)
    {
        if (levels[i] == level)
            return true;
    }

    return false;
}

static void check_object_name(ID3D12Object *object, const WCHAR *expected_name, UINT expected_size)
{
    WCHAR buffer[64];
    UINT size = sizeof(buffer);
    HRESULT hr;

    hr = ID3D12Object_GetPrivateData(object, &WKPDID_D3DDebugObjectNameW, &size, buffer);
    ok(hr == S_OK, "Failed to get object name, hr %#x.\n", hr);
    ok(size == expected_size, "Got name size %u, expected %u.\n", size, expected_size);
    ok(!memcmp(buffer, expected_name, expected_size), "Got unexpected object name.\n");
}

static void test_feature_support(void)
{
    static const D3D_FEATURE_LEVEL feature_levels[] =
    {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D12_FEATURE_DATA_ARCHITECTURE architecture;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options;
    D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels_desc;
    D3D12_FEATURE_DATA_FORMAT_INFO format_info;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support;
    D3D12_FEATURE_DATA_ROOT_SIGNATURE root_signature;
    ID3D12Device *device;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    memset(&feature_levels_desc, 0, sizeof(feature_levels_desc));
    feature_levels_desc.NumFeatureLevels = ARRAY_SIZE(feature_levels);
    feature_levels_desc.pFeatureLevelsRequested = feature_levels;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FEATURE_LEVELS,
            &feature_levels_desc, sizeof(feature_levels_desc));
    ok(hr == S_OK, "Failed to query feature levels, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
        ok(feature_level_in_list(feature_levels_desc.MaxSupportedFeatureLevel, feature_levels, ARRAY_SIZE(feature_levels)),
                "Got unexpected max feature level %#x.\n", feature_levels_desc.MaxSupportedFeatureLevel);

    memset(&options, 0, sizeof(options));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    ok(hr == S_OK, "Failed to query D3D12 options, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_1,
                "Got resource binding tier %#x.\n", options.ResourceBindingTier);
        ok(options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_1,
                "Got resource heap tier %#x.\n", options.ResourceHeapTier);
    }

    memset(&architecture, 0, sizeof(architecture));
    architecture.NodeIndex = 0;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_ARCHITECTURE,
            &architecture, sizeof(architecture));
    ok(hr == S_OK, "Failed to query architecture support, hr %#x.\n", hr);

    memset(&root_signature, 0, sizeof(root_signature));
    root_signature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_ROOT_SIGNATURE,
            &root_signature, sizeof(root_signature));
    ok(hr == S_OK, "Failed to query root signature support, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
    {
        ok(root_signature.HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_0
                || root_signature.HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_1,
                "Got unexpected root signature version %#x.\n", root_signature.HighestVersion);
    }

    memset(&format_support, 0, sizeof(format_support));
    format_support.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_SUPPORT,
            &format_support, sizeof(format_support));
    ok(hr == S_OK, "Failed to query format support, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
        ok(format_support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D,
                "R8G8B8A8_UNORM is missing TEXTURE2D support, Support1 %#x.\n", format_support.Support1);

    memset(&format_info, 0, sizeof(format_info));
    format_info.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_INFO,
            &format_info, sizeof(format_info));
    ok(hr == S_OK, "Failed to query format info, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
        ok(format_info.PlaneCount == 1, "Got unexpected plane count %u.\n", format_info.PlaneCount);

    ID3D12Device_Release(device);
}

static void test_private_data_and_name(void)
{
    static const GUID private_data_guid =
    { 0x92fc4c7d, 0x2d1e, 0x44bb, { 0xb6, 0x91, 0xa7, 0xb3, 0x50, 0x63, 0x1d, 0x35 } };
    static const WCHAR device_name[] = L"dxmt-d3d12-device";
    static const WCHAR queue_name[] = L"dxmt-d3d12-queue";
    static const WCHAR resource_name[] = L"dxmt-d3d12-resource";

    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_DESC resource_desc;
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    ID3D12Object *object;
    ID3D12Resource *resource;
    UINT64 private_value = 0x1122334455667788ull;
    UINT64 queried_value = 0;
    UINT private_size;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    object = (ID3D12Object *)device;
    hr = ID3D12Object_SetPrivateData(object, &private_data_guid, sizeof(private_value), &private_value);
    ok(hr == S_OK, "Failed to set device private data, hr %#x.\n", hr);
    private_size = sizeof(queried_value);
    queried_value = 0;
    hr = ID3D12Object_GetPrivateData(object, &private_data_guid, &private_size, &queried_value);
    ok(hr == S_OK, "Failed to get device private data, hr %#x.\n", hr);
    ok(private_size == sizeof(private_value) && queried_value == private_value,
            "Got private data size %u value %#"PRIx64", expected size %u value %#"PRIx64".\n",
            private_size, queried_value, (unsigned int)sizeof(private_value), private_value);
    hr = ID3D12Object_SetName(object, device_name);
    ok(hr == S_OK, "Failed to set device name, hr %#x.\n", hr);
    check_object_name(object, device_name, sizeof(device_name));

    queue = create_command_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
    if (!queue)
    {
        ID3D12Device_Release(device);
        return;
    }
    object = (ID3D12Object *)queue;
    private_value = 0x8877665544332211ull;
    hr = ID3D12Object_SetPrivateData(object, &private_data_guid, sizeof(private_value), &private_value);
    ok(hr == S_OK, "Failed to set queue private data, hr %#x.\n", hr);
    private_size = sizeof(queried_value);
    queried_value = 0;
    hr = ID3D12Object_GetPrivateData(object, &private_data_guid, &private_size, &queried_value);
    ok(hr == S_OK, "Failed to get queue private data, hr %#x.\n", hr);
    ok(private_size == sizeof(private_value) && queried_value == private_value,
            "Got queue private data size %u value %#"PRIx64", expected size %u value %#"PRIx64".\n",
            private_size, queried_value, (unsigned int)sizeof(private_value), private_value);
    hr = ID3D12Object_SetName(object, queue_name);
    ok(hr == S_OK, "Failed to set queue name, hr %#x.\n", hr);
    check_object_name(object, queue_name, sizeof(queue_name));

    init_heap_properties(&heap_properties, D3D12_HEAP_TYPE_UPLOAD);
    init_buffer_desc(&resource_desc, 4096);
    hr = ID3D12Device_CreateCommittedResource(device, &heap_properties, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void **)&resource);
    ok(hr == S_OK, "Failed to create resource, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
    {
        object = (ID3D12Object *)resource;
        private_value = 0x0102030405060708ull;
        hr = ID3D12Object_SetPrivateData(object, &private_data_guid, sizeof(private_value), &private_value);
        ok(hr == S_OK, "Failed to set resource private data, hr %#x.\n", hr);
        private_size = sizeof(queried_value);
        queried_value = 0;
        hr = ID3D12Object_GetPrivateData(object, &private_data_guid, &private_size, &queried_value);
        ok(hr == S_OK, "Failed to get resource private data, hr %#x.\n", hr);
        ok(private_size == sizeof(private_value) && queried_value == private_value,
                "Got resource private data size %u value %#"PRIx64", expected size %u value %#"PRIx64".\n",
                private_size, queried_value, (unsigned int)sizeof(private_value), private_value);
        hr = ID3D12Object_SetName(object, resource_name);
        ok(hr == S_OK, "Failed to set resource name, hr %#x.\n", hr);
        check_object_name(object, resource_name, sizeof(resource_name));
        ID3D12Resource_Release(resource);
    }

    ID3D12CommandQueue_Release(queue);
    ID3D12Device_Release(device);
}

static void test_root_signature(void)
{
    D3D12_FEATURE_DATA_ROOT_SIGNATURE root_signature_support;
    D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned_desc;
    ID3D12Object *object;
    ID3D12Device *device;
    ID3D12RootSignature *root_signature;
    HRESULT hr;

    static const WCHAR root_signature_name[] = L"dxmt-d3d12-root-signature";

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    memset(&root_signature_support, 0, sizeof(root_signature_support));
    root_signature_support.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_ROOT_SIGNATURE,
            &root_signature_support, sizeof(root_signature_support));
    ok(hr == S_OK, "Failed to query root signature support, hr %#x.\n", hr);

    memset(&root_signature_desc, 0, sizeof(root_signature_desc));
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    hr = create_root_signature(device, &root_signature_desc, &root_signature);
    ok(hr == S_OK, "Failed to create 1.0 root signature, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
    {
        object = (ID3D12Object *)root_signature;
        hr = ID3D12Object_SetName(object, root_signature_name);
        ok(hr == S_OK, "Failed to set root signature name, hr %#x.\n", hr);
        check_object_name(object, root_signature_name, sizeof(root_signature_name));
        ID3D12RootSignature_Release(root_signature);
    }

    if (root_signature_support.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1)
    {
        memset(&versioned_desc, 0, sizeof(versioned_desc));
        versioned_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        versioned_desc.Desc_1_1.NumParameters = 0;
        versioned_desc.Desc_1_1.pParameters = NULL;
        versioned_desc.Desc_1_1.NumStaticSamplers = 0;
        versioned_desc.Desc_1_1.pStaticSamplers = NULL;
        versioned_desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        hr = create_versioned_root_signature(device, &versioned_desc, &root_signature);
        ok(hr == S_OK, "Failed to create 1.1 root signature, hr %#x.\n", hr);
        if (SUCCEEDED(hr))
            ID3D12RootSignature_Release(root_signature);
    }

    ID3D12Device_Release(device);
}

static void test_heap_and_allocation_info(void)
{
    D3D12_HEAP_DESC heap_desc, queried_heap_desc;
    D3D12_HEAP_PROPERTIES heap_properties;
    D3D12_RESOURCE_ALLOCATION_INFO allocation_info;
    D3D12_RESOURCE_DESC resource_desc, queried_desc;
    ID3D12Device *device;
    ID3D12Heap *heap;
    ID3D12Resource *resource;
    HRESULT hr;

    if (!(device = create_device()))
    {
        skip("Failed to create device.\n");
        return;
    }

    init_heap_properties(&heap_properties, D3D12_HEAP_TYPE_DEFAULT);
    init_texture_desc(&resource_desc, DXGI_FORMAT_R8G8B8A8_UNORM);
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    allocation_info = ID3D12Device_GetResourceAllocationInfo(device, 0, 1, &resource_desc);
    ok(allocation_info.SizeInBytes > 0, "Got zero allocation size.\n");
    ok(allocation_info.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
            "Got unexpected allocation alignment %#"PRIu64".\n", allocation_info.Alignment);

    memset(&heap_desc, 0, sizeof(heap_desc));
    heap_desc.SizeInBytes = allocation_info.SizeInBytes;
    heap_desc.Properties = heap_properties;
    heap_desc.Alignment = allocation_info.Alignment;
    heap_desc.Flags = D3D12_HEAP_FLAG_NONE;
    hr = ID3D12Device_CreateHeap(device, &heap_desc, &IID_ID3D12Heap, (void **)&heap);
    ok(hr == S_OK, "Failed to create heap, hr %#x.\n", hr);
    if (SUCCEEDED(hr))
    {
        queried_heap_desc = ID3D12Heap_GetDesc(heap);
        ok(queried_heap_desc.SizeInBytes == heap_desc.SizeInBytes,
                "Got heap size %#"PRIu64", expected %#"PRIu64".\n",
                queried_heap_desc.SizeInBytes, heap_desc.SizeInBytes);
        ok(queried_heap_desc.Alignment == heap_desc.Alignment,
                "Got heap alignment %#"PRIu64", expected %#"PRIu64".\n",
                queried_heap_desc.Alignment, heap_desc.Alignment);
        ok(queried_heap_desc.Properties.Type == heap_desc.Properties.Type,
                "Got heap type %#x, expected %#x.\n",
                queried_heap_desc.Properties.Type, heap_desc.Properties.Type);

        hr = ID3D12Device_CreatePlacedResource(device, heap, 0, &resource_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&resource);
        ok(hr == S_OK, "Failed to create placed resource, hr %#x.\n", hr);
        if (SUCCEEDED(hr))
        {
            queried_desc = ID3D12Resource_GetDesc(resource);
            ok(queried_desc.Dimension == resource_desc.Dimension, "Got unexpected resource dimension %#x.\n",
                    queried_desc.Dimension);
            ok(queried_desc.Format == resource_desc.Format, "Got unexpected resource format %#x.\n",
                    queried_desc.Format);
            ok(queried_desc.Flags == resource_desc.Flags, "Got unexpected resource flags %#x.\n",
                    queried_desc.Flags);
            ID3D12Resource_Release(resource);
        }

        ID3D12Heap_Release(heap);
    }

    ID3D12Device_Release(device);
}

static bool list_tests_requested(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--list-tests"))
            return true;
    }

    return false;
}

static bool have_d3d12_device(void)
{
    ID3D12Device *device;

    if ((device = create_device()))
        ID3D12Device_Release(device);
    return device;
}

START_TEST(d3d12_api_d3d)
{
    pfn_D3D12CreateDevice = get_d3d12_pfn(D3D12CreateDevice);
    pfn_D3D12EnableExperimentalFeatures = get_d3d12_pfn(D3D12EnableExperimentalFeatures);
    pfn_D3D12GetDebugInterface = get_d3d12_pfn(D3D12GetDebugInterface);
    pfn_D3D12GetInterface = get_d3d12_pfn(D3D12GetInterface);

    if (list_tests_requested(argc, argv))
    {
        printf("test_create_device\n");
        printf("test_feature_support\n");
        printf("test_adapter_luid\n");
        printf("test_device_parent\n");
        printf("test_command_queue_and_fence\n");
        printf("test_private_data_and_name\n");
        printf("test_root_signature\n");
        printf("test_resource_refcount_and_desc\n");
        printf("test_heap_and_allocation_info\n");
        printf("test_formats\n");
        exit(0);
    }

    parse_args(argc, argv);
    enable_d3d12_debug_layer(argc, argv);
    enable_feature_level_override(argc, argv);
    vkd3d_set_running_in_test_suite();
    init_adapter_info();

    if (!have_d3d12_device())
    {
        skip("D3D12 device cannot be created.\n");
        return;
    }

    run_test(test_create_device);
    run_test(test_feature_support);
    run_test(test_adapter_luid);
    run_test(test_device_parent);
    run_test(test_command_queue_and_fence);
    run_test(test_private_data_and_name);
    run_test(test_root_signature);
    run_test(test_resource_refcount_and_desc);
    run_test(test_heap_and_allocation_info);
    run_test(test_formats);
}
