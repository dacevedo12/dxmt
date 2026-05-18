#include "util_fh4_bypass.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>

namespace dxmt::fh4bypass {
namespace {

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64)) &&             \
    !defined(__aarch64__) && !defined(__arm64ec__) &&                          \
    !defined(_M_ARM64) && !defined(_M_ARM64EC)
#define DXMT_FH4_BYPASS_HAS_X64_GS 1
#endif

uintptr_t ReadGsQword(uint32_t offset) {
#if DXMT_FH4_BYPASS_HAS_X64_GS
  uintptr_t value = 0;
#if defined(__GNUC__) || defined(__clang__)
  if (offset == 0x20)
    __asm__ volatile("movq %%gs:0x20,%0" : "=r"(value));
#elif defined(_MSC_VER)
  value = static_cast<uintptr_t>(__readgsqword(offset));
#else
  (void)offset;
#endif
  return value;
#else
  (void)offset;
  return 0;
#endif
}

void WriteGsQword(uint32_t offset, uintptr_t value) {
#if DXMT_FH4_BYPASS_HAS_X64_GS
#if defined(__GNUC__) || defined(__clang__)
  if (offset == 0x20)
    __asm__ volatile("movq %0,%%gs:0x20" : : "r"(value) : "memory");
#elif defined(_MSC_VER)
  __writegsqword(offset, value);
#else
  (void)offset;
  (void)value;
#endif
#else
  (void)offset;
  (void)value;
#endif
}

} // namespace

void ApplyBadFiberDataBypass() {
#ifdef _WIN32
  WCHAR path[MAX_PATH + 1] = {};
  const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (!len)
    return;

  const WCHAR *base = path;
  for (DWORD i = 0; i < len; i++) {
    if (path[i] == L'\\' || path[i] == L'/')
      base = path + i + 1;
  }

  if (lstrcmpiW(base, L"ForzaHorizon4.exe") != 0)
    return;

  // Temporary downstream workaround. FH4 can inherit a bogus low-address
  // FiberData value under Wine and crash before the D3D runtime is initialized.
  // Remove this once Wine provides the proper loader/TEB behavior.
  const uintptr_t fiber_data = ReadGsQword(0x20);
  if (fiber_data && fiber_data < 0x10000)
    WriteGsQword(0x20, 0);
#endif
}

} // namespace dxmt::fh4bypass
