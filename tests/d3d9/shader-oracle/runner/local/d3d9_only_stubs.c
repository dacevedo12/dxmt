/*
 * shader_runner.c dispatches to a d3d9, d3d11 and d3d12 executor
 * unconditionally on a Windows cross build. This oracle only builds and links
 * the d3d9 executor (shader_runner_d3d9.c); the d3d11 and d3d12 executors would
 * pull in d3d11/d3d12 import libraries and device creation we do not exercise.
 * Satisfy their link references with no-ops so the dispatch is a runtime skip.
 */
void run_shader_tests_d3d11(void)
{
}

void run_shader_tests_d3d12(void *dxc_compiler)
{
    (void)dxc_compiler;
}
