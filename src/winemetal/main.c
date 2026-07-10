#include "windef.h"
#include "winbase.h"
#include "wineunixlib.h"
#include <stdint.h>

#if defined(__x86_64__) || defined(_M_X64)
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


BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  apply_fh4_bad_fiber_data_bypass();
  DisableThreadLibraryCalls(instance);
  return !__wine_init_unix_call();
}

extern BOOL WINAPI DllMainCRTStartup(HANDLE hDllHandle, DWORD dwReason, LPVOID lpreserved);
