subdir('util')
subdir('airconv')

if not dxmt_crossbuild
subdir('nativemetal')
else
subdir('winemetal')
endif

subdir('dxmt')
subdir('dxgi')
subdir('d3d11')
subdir('d3d10')

# The Direct3D 9 frontend manages the app's window itself, which the API's
# focus and device window rules require, so it is built only against wine.
if dxmt_crossbuild
subdir('d3d9')
endif

if get_option('enable_d3d12')
subdir('d3d12')
endif

if get_option('enable_nvapi')
subdir('nvapi')
endif

if get_option('enable_nvngx')
subdir('nvngx')
endif