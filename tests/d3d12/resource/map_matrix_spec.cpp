#include <dxmt_test.hpp>

#include "d3d12_test_context.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Plan §14.6: Map / Unmap ranges on upload/readback; default heap map fails.
// Public D3D12 API only.

namespace {

using dxmt::test::ComPtr;
using dxmt::test::D3D12TestContext;

struct MapRangeCase {
  UINT64 buffer_size;
  UINT64 begin;
  UINT64 end; // exclusive; 0 means nullptr range (full)
  bool null_range;
  const char *tag;
};

std::vector<MapRangeCase> BuildMapRangeCases() {
  std::vector<MapRangeCase> cases;
  const UINT64 sizes[] = {16, 64, 256, 1024, 4096, 16384};
  for (const UINT64 size : sizes) {
    cases.push_back({size, 0, 0, true, "NullRange"});
    cases.push_back({size, 0, size, false, "Full"});
  }
  cases.insert(cases.end(), {{4096, 0, 0, false, "EmptyRead"},
                             {4096, 0, 16, false, "Head16"},
                             {4096, 4080, 4096, false, "Tail16"},
                             {4096, 16, 4080, false, "Interior"},
                             {4096, 64, 128, false, "Mid64"},
                             {4096, 1, 4095, false, "AlmostAll"}});
  return cases;
}

class MapMatrixSpec : public ::testing::TestWithParam<MapRangeCase> {
protected:
  void SetUp() override { ASSERT_EQ(context_.Initialize(), S_OK); }

  D3D12TestContext context_;
};

TEST_P(MapMatrixSpec, UploadMapWriteUnmapRoundTripsBytes) {
  const auto &test = GetParam();
  auto upload = context_.CreateBuffer(test.buffer_size, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
  ASSERT_TRUE(upload);

  void *mapped = nullptr;
  if (test.null_range) {
    ASSERT_EQ(upload->Map(0, nullptr, &mapped), S_OK);
  } else {
    D3D12_RANGE range = {static_cast<SIZE_T>(test.begin),
                         static_cast<SIZE_T>(test.end)};
    ASSERT_EQ(upload->Map(0, &range, &mapped), S_OK);
  }
  ASSERT_NE(mapped, nullptr);

  auto *bytes = static_cast<std::uint8_t *>(mapped);
  const UINT64 write_begin = test.null_range ? 0 : test.begin;
  const UINT64 write_end =
      test.null_range ? test.buffer_size
                      : (test.end > test.begin ? test.end : test.begin);
  for (UINT64 i = write_begin; i < write_end; ++i)
    bytes[i] = static_cast<std::uint8_t>(0xa5 ^ (i & 0xff));

  if (test.null_range) {
    upload->Unmap(0, nullptr);
  } else {
    D3D12_RANGE written = {static_cast<SIZE_T>(write_begin),
                           static_cast<SIZE_T>(write_end)};
    upload->Unmap(0, &written);
  }

  // Re-map full range and verify written region; unspecified bytes may be
  // anything, so only assert the range we wrote.
  void *verify = nullptr;
  ASSERT_EQ(upload->Map(0, nullptr, &verify), S_OK);
  ASSERT_NE(verify, nullptr);
  auto *vbytes = static_cast<const std::uint8_t *>(verify);
  for (UINT64 i = write_begin; i < write_end; ++i) {
    EXPECT_EQ(vbytes[i], static_cast<std::uint8_t>(0xa5 ^ (i & 0xff)))
        << "offset " << i;
  }
  upload->Unmap(0, nullptr);
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

TEST_P(MapMatrixSpec, ReadbackMapAfterGpuCopySeesUploadBytes) {
  const auto &test = GetParam();
  if (test.buffer_size > 4096)
    GTEST_SKIP() << "keep GPU copy map matrix to 4KiB";

  std::vector<std::uint8_t> pattern(static_cast<size_t>(test.buffer_size));
  for (size_t i = 0; i < pattern.size(); ++i)
    pattern[i] = static_cast<std::uint8_t>(i * 3u + 7u);

  auto upload = context_.CreateUploadBuffer(test.buffer_size, pattern.data(),
                                            pattern.size());
  auto default_buf = context_.CreateBuffer(
      test.buffer_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
      D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback = context_.CreateBuffer(test.buffer_size,
                                        D3D12_HEAP_TYPE_READBACK,
                                        D3D12_RESOURCE_FLAG_NONE,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
  ASSERT_TRUE(upload);
  ASSERT_TRUE(default_buf);
  ASSERT_TRUE(readback);

  context_.list()->CopyBufferRegion(default_buf.get(), 0, upload.get(), 0,
                                    test.buffer_size);
  D3D12TestContext::Transition(context_.list(), default_buf.get(),
                               D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
  context_.list()->CopyBufferRegion(readback.get(), 0, default_buf.get(), 0,
                                    test.buffer_size);
  ASSERT_EQ(context_.ExecuteAndWait(), S_OK);

  void *mapped = nullptr;
  D3D12_RANGE range = {0, static_cast<SIZE_T>(test.buffer_size)};
  ASSERT_EQ(readback->Map(0, &range, &mapped), S_OK);
  ASSERT_NE(mapped, nullptr);
  EXPECT_EQ(std::memcmp(mapped, pattern.data(), pattern.size()), 0);
  readback->Unmap(0, nullptr);
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

TEST_F(MapMatrixSpec, DefaultHeapMapFails) {
  auto buffer = context_.CreateBuffer(256, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_COMMON);
  ASSERT_TRUE(buffer);
  void *mapped = reinterpret_cast<void *>(uintptr_t{1});
  const HRESULT hr = buffer->Map(0, nullptr, &mapped);
  EXPECT_HRESULT_FAILED(hr);
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

TEST_F(MapMatrixSpec, MapSubresourceOneOnBufferFails) {
  auto upload = context_.CreateBuffer(64, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
  ASSERT_TRUE(upload);
  void *mapped = nullptr;
  EXPECT_EQ(upload->Map(1, nullptr, &mapped), E_INVALIDARG);
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

// GPTK D3DMResource::Map: null ppData returns S_OK and does not open a map.
TEST_F(MapMatrixSpec, NullPpDataMapSucceedsWithoutPointer) {
  auto upload = context_.CreateBuffer(128, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
  ASSERT_TRUE(upload);
  ASSERT_EQ(upload->Map(0, nullptr, nullptr), S_OK);
  // Real map still works after null-ppData call (no leftover pin).
  void *mapped = nullptr;
  ASSERT_EQ(upload->Map(0, nullptr, &mapped), S_OK);
  ASSERT_NE(mapped, nullptr);
  upload->Unmap(0, nullptr);
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

// Nested Map returns the same host pointer (GPTK cached contents + refcount).
TEST_F(MapMatrixSpec, NestedMapReturnsStablePointer) {
  auto upload = context_.CreateBuffer(256, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
  ASSERT_TRUE(upload);
  void *first = nullptr;
  void *second = nullptr;
  ASSERT_EQ(upload->Map(0, nullptr, &first), S_OK);
  ASSERT_EQ(upload->Map(0, nullptr, &second), S_OK);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(second, first);
  auto *bytes = static_cast<std::uint8_t *>(first);
  bytes[0] = 0x5a;
  bytes[1] = 0xa5;
  D3D12_RANGE written = {0, 2};
  upload->Unmap(0, &written);
  upload->Unmap(0, nullptr);
  void *again = nullptr;
  ASSERT_EQ(upload->Map(0, nullptr, &again), S_OK);
  EXPECT_EQ(static_cast<std::uint8_t *>(again)[0], 0x5a);
  EXPECT_EQ(static_cast<std::uint8_t *>(again)[1], 0xa5);
  upload->Unmap(0, nullptr);
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

// App may Release without Unmap; GPTK silently abandons map slots.
TEST_F(MapMatrixSpec, MapWithoutUnmapThenReleaseKeepsDeviceHealthy) {
  {
    auto upload = context_.CreateBuffer(64, D3D12_HEAP_TYPE_UPLOAD,
                                        D3D12_RESOURCE_FLAG_NONE,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
    ASSERT_TRUE(upload);
    void *mapped = nullptr;
    ASSERT_EQ(upload->Map(0, nullptr, &mapped), S_OK);
    ASSERT_NE(mapped, nullptr);
    *static_cast<std::uint32_t *>(mapped) = 0xdeadbeefu;
    // Drop last ref while mapped — no Unmap.
  }
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);

  // Subsequent map on a fresh resource still works after abandon path.
  auto next = context_.CreateBuffer(64, D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
  ASSERT_TRUE(next);
  void *mapped = nullptr;
  ASSERT_EQ(next->Map(0, nullptr, &mapped), S_OK);
  ASSERT_NE(mapped, nullptr);
  next->Unmap(0, nullptr);
  EXPECT_EQ(context_.device()->GetDeviceRemovedReason(), S_OK);
}

std::string MapRangeName(const ::testing::TestParamInfo<MapRangeCase> &info) {
  return std::string(info.param.tag) + "S" +
         std::to_string(info.param.buffer_size) + "B" +
         std::to_string(info.param.begin) + "E" +
         std::to_string(info.param.end) + "N" +
         std::to_string(info.index);
}

INSTANTIATE_TEST_SUITE_P(RangeMatrix, MapMatrixSpec,
                         ::testing::ValuesIn(BuildMapRangeCases()),
                         MapRangeName);

} // namespace
