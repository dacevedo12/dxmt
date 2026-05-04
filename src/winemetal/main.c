#include "windef.h"
#include "winbase.h"
#include "wineunixlib.h"
#include <stdint.h>

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__aarch64__) &&       \
    !defined(__arm64ec__) && !defined(_M_ARM64) && !defined(_M_ARM64EC)
#define DXMT_FH4_BYPASS_HAS_X64_GS 1
#endif

static uintptr_t read_gs_qword(unsigned int offset) {
#if DXMT_FH4_BYPASS_HAS_X64_GS
  uintptr_t value = 0;
#if defined(__GNUC__) || defined(__clang__)
  if (offset == 0x20)
    __asm__ volatile("movq %%gs:0x20,%0" : "=r"(value));
#endif
  return value;
#else
  (void)offset;
  return 0;
#endif
}

static void write_gs_qword(unsigned int offset, uintptr_t value) {
#if DXMT_FH4_BYPASS_HAS_X64_GS
#if defined(__GNUC__) || defined(__clang__)
  if (offset == 0x20)
    __asm__ volatile("movq %0,%%gs:0x20" : : "r"(value) : "memory");
#else
  (void)offset;
  (void)value;
#endif
#else
  (void)offset;
  (void)value;
#endif
}

static void apply_fh4_bad_fiber_data_bypass(void) {
  WCHAR path[MAX_PATH + 1] = {0};
  DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
  const WCHAR *base = path;
  uintptr_t fiber_data;

  if (!len)
    return;

  for (DWORD i = 0; i < len; i++) {
    if (path[i] == L'\\' || path[i] == L'/')
      base = path + i + 1;
  }

  if (lstrcmpiW(base, L"ForzaHorizon4.exe") != 0)
    return;

  /* Temporary downstream workaround. FH4 can inherit a bogus low-address
   * FiberData value under Wine and crash before the D3D runtime is initialized.
   * Remove this once Wine provides the proper loader/TEB behavior. */
  fiber_data = read_gs_qword(0x20);
  if (fiber_data && fiber_data < 0x10000)
    write_gs_qword(0x20, 0);
}

#include <stdatomic.h>

/*
 * dxmt cross-boundary call counters, one slot per wine_unix_call code.
 * Bumped from the WINE_UNIX_CALL macro in wineunixlib.h. Read by
 * d3d9_trace.cpp's per-second hotcounters tick(): the breakdown lets
 * us see which specific codes dominate the cross-WoW64 traffic
 * (newBuffer? signaledValue? presentDrawable?), turning the aggregate
 * unixCalls=N number into actionable per-call-site data.
 */
_Atomic unsigned long long dxmt_unix_call_counters[256] = {0};

__declspec(dllexport) unsigned long long winemetal_consume_unix_call_count(void) {
  /* Aggregate snapshot-and-zero across every slot. Callers that want
   * just the total don't need the per-code breakdown. */
  unsigned long long total = 0;
  for (unsigned i = 0; i < 256; ++i) {
    total += atomic_exchange_explicit(&dxmt_unix_call_counters[i], 0ULL, memory_order_relaxed);
  }
  return total;
}

/* Snapshot-and-zero every per-code slot into the caller's buffer.
 * counts_out must point at a 256-element uint64_t array. Returns the
 * sum across all slots so the caller doesn't have to add them up
 * again. d3d9_trace.cpp uses this to log the top-N hottest codes per
 * second alongside the aggregate. */
__declspec(dllexport) unsigned long long winemetal_consume_unix_call_breakdown(unsigned long long *counts_out) {
  unsigned long long total = 0;
  for (unsigned i = 0; i < 256; ++i) {
    unsigned long long c = atomic_exchange_explicit(&dxmt_unix_call_counters[i], 0ULL, memory_order_relaxed);
    counts_out[i] = c;
    total += c;
  }
  return total;
}

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  apply_fh4_bad_fiber_data_bypass();
  DisableThreadLibraryCalls(instance);
  return !__wine_init_unix_call();
}

extern BOOL WINAPI DllMainCRTStartup(HANDLE hDllHandle, DWORD dwReason, LPVOID lpreserved);
