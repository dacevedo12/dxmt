subdir('util')
subdir('airconv')

winemetal_runtime_targets = []
winemetal4_runtime_targets = []
dxgi_postprocess_targets = []
d3d10_postprocess_targets = []
d3d11_postprocess_targets = []
d3d9_postprocess_targets = []
d3d12_postprocess_targets = []
nvext_postprocess_targets = []

if not dxmt_crossbuild
subdir('nativemetal')
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

dxmt_metal3_runtime_targets = [
  dxgi_dll,
  d3d11_backend_dll,
  d3d11_dll,
  d3d10core_dll,
  d3d9_dll,
]
dxmt_metal3_runtime_targets += winemetal_runtime_targets
dxmt_metal3_runtime_targets += dxgi_postprocess_targets
dxmt_metal3_runtime_targets += d3d10_postprocess_targets
dxmt_metal3_runtime_targets += d3d11_postprocess_targets
dxmt_metal3_runtime_targets += d3d9_postprocess_targets

dxmt_metal4_runtime_targets = [
  dxgi_dll,
  d3d12_dll,
  d3d12core_dll,
]
dxmt_metal4_runtime_targets += winemetal4_runtime_targets
dxmt_metal4_runtime_targets += dxgi_postprocess_targets
dxmt_metal4_runtime_targets += d3d12_postprocess_targets

dxmt_runtime_targets = dxmt_metal3_runtime_targets + dxmt_metal4_runtime_targets
if get_option('enable_nvapi')
  dxmt_runtime_targets += [nvapi_dll] + nvext_postprocess_targets
endif
if get_option('enable_nvngx')
  dxmt_runtime_targets += [nvngx_dll] + nvext_postprocess_targets
endif

alias_target('dxmt-runtime', dxmt_runtime_targets)
alias_target('dxmt-d3d9', dxmt_metal3_runtime_targets)
alias_target('dxmt-d3d10', dxmt_metal3_runtime_targets)
alias_target('dxmt-d3d11', dxmt_metal3_runtime_targets)
alias_target('dxmt-d3d12', dxmt_metal4_runtime_targets)
