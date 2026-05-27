#pragma once

#include <cstdint>

namespace dxmt::apitrace {

bool enabled();
void ensure_session_open();
void shutdown();
void set_current_d3d_sequence(uint64_t d3d_sequence);
void on_command_buffer_begin(uint64_t command_buffer_id, uint64_t frame_id);
void on_command_buffer_commit(uint64_t command_buffer_id);
void on_present_drawable(
    uint64_t command_buffer_id,
    uint64_t drawable_id,
    uint64_t frame_index,
    uint32_t sync_interval,
    uint32_t flags);

} // namespace dxmt::apitrace
