python3 = find_program('python3')

# Wine-free static-source probes — part of the 'host' suite (see
# tests/dx9/unit) that CI runs as the required, deterministic gate.
test('static-buffer-rtv', python3, args: files('static_buffer_rtv.py'), suite: 'host')
test('winemetal-unixcall-table', python3, args: files('winemetal_unixcall_table.py'), suite: 'host')

subdir('dx9')
subdir('dx11')
subdir('dx12')
subdir('vkd3d-proton')
