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
subdir('d3d12')
subdir('d3d11')
subdir('d3d10')
subdir('d3d9')

if get_option('enable_nvapi')
subdir('nvapi')
endif

if get_option('enable_nvngx')
subdir('nvngx')
endif
