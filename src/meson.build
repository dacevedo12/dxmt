subdir('util')
subdir('airconv')

if not dxmt_crossbuild
subdir('nativemetal')
winemetal4_dep = winemetal_dep
else
subdir('winemetal')
subdir('winemetal4')
endif

subdir('dxmt')
subdir('dxgi')
# d3d12 (Metal4 backend) is 64-bit only: it reinterpret_casts 64-bit
# handles to uintptr_t, which the i386 / WoW64 target rejects.
if host_machine.cpu_family() != 'x86'
subdir('d3d12')
endif
subdir('d3d11')
subdir('d3d10')
subdir('d3d9')

if get_option('enable_nvapi')
subdir('nvapi')
endif

if get_option('enable_nvngx')
subdir('nvngx')
endif
