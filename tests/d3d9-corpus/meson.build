# The d3d9 suite resolves its corpus relative to the test image so the same
# layout serves a build tree and a packaged drop. configure_file writes into the
# current build directory, so the copies live in their own subdirectory here
# rather than beside the vendored originals.
foreach case : [
  'dp2add', 'loop', 'lrp', 'mnxn', 'nrm', 'pow', 'rep', 'texdepth', 'texreg',
]
  configure_file(
    input: '..' / 'd3d9' / 'shader-oracle' / 'corpus' / (case + '.shader_test'),
    output: case + '.shader_test',
    copy: true,
  )
endforeach
