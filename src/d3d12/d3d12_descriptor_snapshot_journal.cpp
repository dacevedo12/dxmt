#include "d3d12_descriptor_snapshot_journal.hpp"

#include <algorithm>

namespace dxmt::d3d12 {

bool
CompiledDescriptorSnapshotStillCurrent(GraphicsBindingSnapshot &snapshot) {
  for (auto &journal : snapshot.descriptor_journals) {
    if (!journal.mirror)
      continue;
    if (!journal.valid)
      return false;
    if (journal.mirror->changeJournalCursor() == journal.cursor)
      continue;
    const auto changes = journal.mirror->changesSince(journal.cursor);
    if (!changes.complete) {
      journal.valid = false;
      return false;
    }
    for (const auto &change : changes.changes) {
      if (std::binary_search(journal.used_slots.begin(),
                             journal.used_slots.end(), change.slot)) {
        journal.valid = false;
        return false;
      }
    }
    journal.cursor = changes.cursor;
  }
  return true;
}

void
FinalizeCompiledDescriptorSnapshot(GraphicsBindingSnapshot &snapshot) {
  for (auto &journal : snapshot.descriptor_journals) {
    if (!journal.mirror)
      continue;
    std::sort(journal.used_slots.begin(), journal.used_slots.end());
    journal.used_slots.erase(
        std::unique(journal.used_slots.begin(), journal.used_slots.end()),
        journal.used_slots.end());
    if (!journal.cursor_captured) {
      journal.cursor = journal.mirror->changeJournalCursor();
      journal.cursor_captured = true;
    }
  }
}

void
CaptureCompiledDescriptorJournalCursor(GraphicsBindingSnapshot &snapshot,
                                       DescriptorHeapMirror *mirror) {
  if (!mirror)
    return;
  for (auto &journal : snapshot.descriptor_journals) {
    if (journal.mirror == mirror)
      return;
    if (!journal.mirror) {
      journal.mirror = mirror;
      journal.cursor = mirror->changeJournalCursor();
      journal.cursor_captured = true;
      return;
    }
  }
}

void
RecordCompiledDescriptorJournalSlot(GraphicsBindingSnapshot &snapshot,
                                    const DescriptorRecord &descriptor) {
  if (!descriptor.mirror)
    return;
  for (auto &journal : snapshot.descriptor_journals) {
    if (journal.mirror == descriptor.mirror) {
      journal.used_slots.push_back(descriptor.heap_index);
      return;
    }
  }
}

void
RecordCompiledDescriptorJournalSpan(GraphicsBindingSnapshot &snapshot,
                                    const SubmittedNativeDescriptorSpan &span) {
  if (!span.mirror)
    return;
  for (auto &journal : snapshot.descriptor_journals) {
    if (journal.mirror != span.mirror)
      continue;
    journal.used_slots.insert(journal.used_slots.end(),
                              span.used_slots.begin(), span.used_slots.end());
    return;
  }
}

} // namespace dxmt::d3d12
