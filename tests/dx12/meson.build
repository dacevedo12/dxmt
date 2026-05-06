lib_d3d12 = cpp.find_library('d3d12')
lib_dxgi = cpp.find_library('dxgi')

dx12_smoke = executable('dx12_smoke', ['dx12_smoke.cpp'],
  dependencies: [ lib_d3d12, lib_dxgi ]
)

test('dx12-smoke-list', dx12_smoke, args: ['--list'])
