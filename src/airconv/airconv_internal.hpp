#pragma once

#include "llvm/ADT/SmallVector.h"

class SM50CompiledBitcodeInternal {
public:
  llvm::SmallVector<char, 0> vec;
};

class SM50ErrorInternal {
public:
  llvm::SmallVector<char, 0> buf;
};
