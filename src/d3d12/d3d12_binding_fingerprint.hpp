#pragma once

#include "d3d12_descriptor_heap.hpp"

#include <cstddef>
#include <cstdint>

namespace dxmt::d3d12 {

// FNV-1a (64-bit) parameters shared by every graphics-binding fingerprint.
// Seed a fingerprint with kGraphicsBindingFingerprintOffset, then feed it
// through the HashGraphicsBinding* helpers below.
inline constexpr uint64_t kGraphicsBindingFingerprintOffset =
    14695981039346656037ull;
inline constexpr uint64_t kGraphicsBindingFingerprintPrime = 1099511628211ull;

// The byte/value/pointer mixers stay inline: they are called dozens of times
// per draw on the binding-snapshot hot path, and the project ships without LTO,
// so an out-of-line definition would turn each mix into a real call.
inline void
HashGraphicsBindingBytes(uint64_t &hash, const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= kGraphicsBindingFingerprintPrime;
  }
}

template <typename T>
void
HashGraphicsBindingValue(uint64_t &hash, const T &value) {
  HashGraphicsBindingBytes(hash, &value, sizeof(value));
}

inline void
HashGraphicsBindingPointer(uint64_t &hash, const void *value) {
  HashGraphicsBindingValue(hash, reinterpret_cast<uintptr_t>(value));
}

// Mixes the identity-relevant fields of a descriptor slot snapshot: record
// type, whether a desc payload is present, the referenced resources, and the
// type-selected union member.
void HashGraphicsBindingDescriptor(uint64_t &hash,
                                   const DescriptorRecord &descriptor);

} // namespace dxmt::d3d12
