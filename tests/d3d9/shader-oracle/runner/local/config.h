/*
 * vkd3d builds this header from configure.ac to advertise optional host
 * features (an OpenGL/Metal executor, a libdxcompiler soname, profiling).
 * The d3d9-only cross build needs none of them: the d3d9 executor loads
 * d3d9.dll and dxcompiler.dll dynamically, and every optional block in
 * vkd3d_common.h / utils.h is guarded by #ifdef, so an empty config.h is
 * the correct configuration here.
 */
