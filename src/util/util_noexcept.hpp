/*
 * This file is part of DXMT, Copyright (c) 2026 DXMT Project
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <cstdio>
#include <exception>
#include <utility>

namespace dxmt {

/**
 * Runs teardown or completion work without allowing an exception to cross a
 * noexcept boundary. The caller receives false so it can continue independent
 * cleanup after a failed callback.
 */
template <typename Fn>
bool
invokeNoexcept(const char *context, Fn &&fn) noexcept {
  try {
    std::forward<Fn>(fn)();
    return true;
  } catch (const std::exception &exception) {
    std::fprintf(stderr, "err:   DXMT %s threw an exception: %s\n", context,
                 exception.what());
  } catch (...) {
    std::fprintf(stderr, "err:   DXMT %s threw an unknown exception\n",
                 context);
  }
  return false;
}

} // namespace dxmt
