/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Initializes a page-aligned portion of resolution-scaled resolve memory from
// the authoritative 1x guest shared-memory buffer. A later partial resolve may
// then safely replace only the texels it actually covers without exposing
// undefined data elsewhere in the same 4 KB tracked page. The mapping matches
// Xenia 0f23f056 resolution-scaled rectangular storage groups. Replication is
// per TEXEL, not per storage group (ABCD -> AABBCCDD, never ABCDABCD).

cbuffer XeScaledResolveInitializeConstants : register(b0) {
  uint xe_scaled_resolve_source_offset;
  uint xe_scaled_resolve_source_length;
  uint xe_scaled_resolve_scale_x;
  uint xe_scaled_resolve_scale_y;
  uint xe_scaled_resolve_bytes_per_block_log2;
};

ByteAddressBuffer xe_scaled_resolve_source : register(t0);
RWByteAddressBuffer xe_scaled_resolve_destination : register(u0);

uint XeHostGroupByteToGuestTiledByte(uint host_group_byte,
                                     uint bytes_per_element_log2) {
  uint byte_mask = (1u << bytes_per_element_log2) - 1u;
  uint byte_in_element = host_group_byte & byte_mask;
  uint group_x_log2 =
      bytes_per_element_log2 >= 3u ? 5u - bytes_per_element_log2 : 4u;
  uint element_index = host_group_byte >> bytes_per_element_log2;
  uint x = element_index & ((1u << group_x_log2) - 1u);
  uint y = element_index >> group_x_log2;
  [branch] switch (bytes_per_element_log2) {
    case 0u:
      return (x & 0x7u) | ((y & 0x2u) << 2u) | ((y & 0x1u) << 4u) |
             ((y & 0x4u) << 3u) | ((x & 0x8u) << 3u);
    case 1u:
      return byte_in_element | ((x & 0x7u) << 1u) |
             ((y & 0x1u) << 4u) | ((y & 0x2u) << 4u) |
             ((x & 0x8u) << 3u);
    case 2u:
      return byte_in_element | ((x & 0x3u) << 2u) |
             ((y & 0x1u) << 4u) | ((x & 0xCu) << 3u);
    case 3u:
      return byte_in_element | ((x & 0x1u) << 3u) |
             ((y & 0x1u) << 4u) | ((x & 0x2u) << 4u);
    default:
      return byte_in_element | ((y & 0x1u) << 4u) |
             ((x & 0x1u) << 5u);
  }
}

[numthreads(256, 1, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  uint destination_byte_base = xe_thread_id.x << 2u;
  uint scale_area =
      xe_scaled_resolve_scale_x * xe_scaled_resolve_scale_y;
  uint destination_length =
      xe_scaled_resolve_source_length * scale_area;
  if (destination_byte_base >= destination_length) {
    return;
  }

  uint value = 0u;
  [unroll]
  for (uint byte_index = 0u; byte_index < 4u; ++byte_index) {
    uint destination_byte = destination_byte_base + byte_index;
    if (destination_byte >= destination_length) {
      break;
    }
    uint group_x_log2 = xe_scaled_resolve_bytes_per_block_log2 >= 3u
                           ? 5u - xe_scaled_resolve_bytes_per_block_log2
                           : 4u;
    uint group_y_log2 =
        3u - min(xe_scaled_resolve_bytes_per_block_log2, 2u);
    uint group_size_log2 = group_x_log2 + group_y_log2 +
                           xe_scaled_resolve_bytes_per_block_log2;
    uint group_size = 1u << group_size_log2;
    uint scaled_group_size = group_size * scale_area;
    uint guest_group = destination_byte / scaled_group_size;
    uint host_group = (destination_byte % scaled_group_size) >> group_size_log2;
    uint host_group_byte = destination_byte & (group_size - 1u);
    uint element = host_group_byte >> xe_scaled_resolve_bytes_per_block_log2;
    uint native_x = (((host_group / xe_scaled_resolve_scale_y) << group_x_log2) +
                     (element & ((1u << group_x_log2) - 1u))) / xe_scaled_resolve_scale_x;
    uint native_y = (((host_group % xe_scaled_resolve_scale_y) << group_y_log2) +
                     (element >> group_x_log2)) / xe_scaled_resolve_scale_y;
    uint native_group_byte = (((native_y << group_x_log2) | native_x)
                             << xe_scaled_resolve_bytes_per_block_log2) |
        (host_group_byte & ((1u << xe_scaled_resolve_bytes_per_block_log2) - 1u));
    uint source_byte = xe_scaled_resolve_source_offset +
                       guest_group * group_size +
                       XeHostGroupByteToGuestTiledByte(
                           native_group_byte,
                           xe_scaled_resolve_bytes_per_block_log2);
    uint source_word = xe_scaled_resolve_source.Load(source_byte & ~3u);
    uint source_value =
        (source_word >> ((source_byte & 3u) << 3u)) & 0xFFu;
    value |= source_value << (byte_index << 3u);
  }
  xe_scaled_resolve_destination.Store(destination_byte_base, value);
}
