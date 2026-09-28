#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace rex::graphics::scaled_resolve_util {

constexpr uint32_t kGuestPageSizeLog2 = 12;
constexpr uint32_t kGuestPageSize = uint32_t(1) << kGuestPageSizeLog2;

// The page bit vector only says that a page has a scaled representation, but
// the byte layout of that representation also depends on bytes per block.
// Keep this compact summary independent of the backend so layout ownership can
// be checked before a texture or a later resolve consumes the page.
struct PageLayoutSummary {
  uint32_t unscaled_page_count = 0;
  uint32_t matching_page_count = 0;
  uint32_t mismatching_page_count = 0;
  uint32_t first_mismatching_page = UINT32_MAX;
  uint32_t first_mismatching_bytes_per_block_log2 = UINT32_MAX;
};

template <typename IsPageScaled, typename GetPageLayoutTag>
PageLayoutSummary SummarizePageLayouts(
    uint32_t page_first, uint32_t page_last,
    uint32_t expected_bytes_per_block_log2, IsPageScaled&& is_page_scaled,
    GetPageLayoutTag&& get_page_layout_tag) {
  PageLayoutSummary summary;
  const uint8_t expected_layout_tag =
      uint8_t(expected_bytes_per_block_log2 + 1);
  for (uint32_t page = page_first; page <= page_last; ++page) {
    if (!is_page_scaled(page)) {
      ++summary.unscaled_page_count;
      continue;
    }
    const uint8_t layout_tag = get_page_layout_tag(page);
    if (layout_tag == expected_layout_tag) {
      ++summary.matching_page_count;
      continue;
    }
    ++summary.mismatching_page_count;
    if (summary.first_mismatching_page == UINT32_MAX) {
      summary.first_mismatching_page = page;
      summary.first_mismatching_bytes_per_block_log2 =
          layout_tag ? uint32_t(layout_tag - 1) : UINT32_MAX;
    }
  }
  return summary;
}

// Collects complete guest pages that don't currently have a resolution-scaled
// representation. Complete pages are required because scaled resolve tracking
// has 4 KB granularity, while a resolve may touch only part of either boundary
// page. Adjacent missing pages are coalesced for one initialization dispatch.
template <typename IsPageScaled>
void CollectUnscaledPageRanges(
    uint32_t start, uint32_t length, uint32_t buffer_size,
    IsPageScaled&& is_page_scaled,
    std::vector<std::pair<uint32_t, uint32_t>>& ranges_out) {
  ranges_out.clear();
  start = std::min(start, buffer_size);
  length = std::min(length, buffer_size - start);
  if (!length) {
    return;
  }

  const uint32_t page_first = start >> kGuestPageSizeLog2;
  const uint32_t page_last = (start + length - 1) >> kGuestPageSizeLog2;
  uint32_t range_page_first = UINT32_MAX;
  for (uint32_t page = page_first; page <= page_last; ++page) {
    if (!is_page_scaled(page)) {
      if (range_page_first == UINT32_MAX) {
        range_page_first = page;
      }
      continue;
    }
    if (range_page_first != UINT32_MAX) {
      ranges_out.emplace_back(
          range_page_first << kGuestPageSizeLog2,
          (page - range_page_first) << kGuestPageSizeLog2);
      range_page_first = UINT32_MAX;
    }
  }
  if (range_page_first != UINT32_MAX) {
    ranges_out.emplace_back(
        range_page_first << kGuestPageSizeLog2,
        (page_last + 1 - range_page_first) << kGuestPageSizeLog2);
  }
}

constexpr uint32_t GetGroupElementsLog2X(
    uint32_t bytes_per_block_log2) {
  // Xenia 0f23f056: 16 elements for 1/2/4 B, 4 for 8 B, 2 for 16 B.
  return bytes_per_block_log2 >= 3 ? 5 - bytes_per_block_log2 : 4;
}

constexpr uint32_t GetGroupElementsLog2Y(
    uint32_t bytes_per_block_log2) {
  // Xenia 0f23f056: 8 elements for 1 B, 4 for 2 B, 2 otherwise.
  return 3 - std::min(bytes_per_block_log2, uint32_t(2));
}

constexpr uint32_t GetGroupSizeLog2(uint32_t bytes_per_block_log2) {
  return GetGroupElementsLog2X(bytes_per_block_log2) +
         GetGroupElementsLog2Y(bytes_per_block_log2) +
         bytes_per_block_log2;
}

// Within a resolution-scaled group, Xenia 0f23f056 stores host elements in
// row-major order. The authoritative guest representation remains Xenos tiled.
// These two helpers convert the low group-local byte bits without needing a
// texture pitch: the group sizes were intentionally chosen so all
// position-dependent tiled bits are above the group boundary.
constexpr uint32_t HostGroupByteToGuestTiledByte(
    uint32_t host_group_byte, uint32_t bytes_per_block_log2) {
  const uint32_t byte_mask =
      (uint32_t(1) << bytes_per_block_log2) - 1;
  const uint32_t byte_in_element = host_group_byte & byte_mask;
  const uint32_t element_index =
      host_group_byte >> bytes_per_block_log2;
  const uint32_t group_x_log2 =
      GetGroupElementsLog2X(bytes_per_block_log2);
  const uint32_t x = element_index & ((uint32_t(1) << group_x_log2) - 1);
  const uint32_t y = element_index >> group_x_log2;
  switch (bytes_per_block_log2) {
    case 0:
      return ((x & 0x7) | ((y & 0x2) << 2) | ((y & 0x1) << 4) |
              ((y & 0x4) << 3) | ((x & 0x8) << 3));
    case 1:
      return byte_in_element | ((x & 0x7) << 1) | ((y & 0x1) << 4) |
             ((y & 0x2) << 4) | ((x & 0x8) << 3);
    case 2:
      return byte_in_element | ((x & 0x3) << 2) | ((y & 0x1) << 4) |
             ((x & 0xC) << 3);
    case 3:
      return byte_in_element | ((x & 0x1) << 3) | ((y & 0x1) << 4) |
             ((x & 0x2) << 4);
    default:
      return byte_in_element | ((y & 0x1) << 4) | ((x & 0x1) << 5);
  }
}

constexpr uint32_t GuestTiledByteToHostGroupByte(
    uint32_t guest_tiled_byte, uint32_t bytes_per_block_log2) {
  switch (bytes_per_block_log2) {
    case 0: {
      const uint32_t x = (guest_tiled_byte & 0x7) |
                         ((guest_tiled_byte >> 3) & 0x8);
      const uint32_t y = ((guest_tiled_byte >> 4) & 0x1) |
                         ((guest_tiled_byte >> 2) & 0x2) |
                         ((guest_tiled_byte >> 3) & 0x4);
      return (y << 4) | x;
    }
    case 1: {
      const uint32_t byte_in_element = guest_tiled_byte & 0x1;
      const uint32_t x = ((guest_tiled_byte >> 1) & 0x7) |
                         ((guest_tiled_byte >> 3) & 0x8);
      const uint32_t y = ((guest_tiled_byte >> 4) & 0x1) |
                         ((guest_tiled_byte >> 4) & 0x2);
      return (((y << 4) | x) << 1) | byte_in_element;
    }
    case 2: {
      const uint32_t byte_in_element = guest_tiled_byte & 0x3;
      const uint32_t x = ((guest_tiled_byte >> 2) & 0x3) |
                         ((guest_tiled_byte >> 3) & 0xC);
      const uint32_t y = (guest_tiled_byte >> 4) & 0x1;
      return (((y << 4) | x) << 2) | byte_in_element;
    }
    case 3: {
      const uint32_t byte_in_element = guest_tiled_byte & 0x7;
      const uint32_t x = ((guest_tiled_byte >> 3) & 0x1) |
                         ((guest_tiled_byte >> 4) & 0x2);
      const uint32_t y = (guest_tiled_byte >> 4) & 0x1;
      return (((y << 2) | x) << 3) | byte_in_element;
    }
    default: {
      const uint32_t byte_in_element = guest_tiled_byte & 0xF;
      const uint32_t x = (guest_tiled_byte >> 5) & 0x1;
      const uint32_t y = (guest_tiled_byte >> 4) & 0x1;
      return (((y << 1) | x) << 4) | byte_in_element;
    }
  }
}

// Maps a byte in the Xenia 0f23f056 resolution-scaled representation back to
// the authoritative 1x Xenos-tiled byte. Rectangular groups describe STORAGE,
// not the replication unit: each guest texel expands to scale_x*scale_y host
// texels, distributed over column-major host groups.
constexpr uint32_t GetInitializationSourceByteOffset(
    uint32_t destination_byte_offset, uint32_t scale_x, uint32_t scale_y,
    uint32_t bytes_per_block_log2) {
  const uint32_t group_size_log2 =
      GetGroupSizeLog2(bytes_per_block_log2);
  const uint32_t group_size = uint32_t(1) << group_size_log2;
  const uint32_t scale_area = scale_x * scale_y;
  const uint32_t scaled_group_size = group_size * scale_area;
  const uint32_t guest_group = destination_byte_offset / scaled_group_size;
  const uint32_t host_group =
      (destination_byte_offset % scaled_group_size) >> group_size_log2;
  const uint32_t host_group_byte = destination_byte_offset & (group_size - 1);
  const uint32_t gx = GetGroupElementsLog2X(bytes_per_block_log2);
  const uint32_t gy = GetGroupElementsLog2Y(bytes_per_block_log2);
  const uint32_t element = host_group_byte >> bytes_per_block_log2;
  const uint32_t native_x =
      (((host_group / scale_y) << gx) + (element & ((1u << gx) - 1))) / scale_x;
  const uint32_t native_y =
      (((host_group % scale_y) << gy) + (element >> gx)) / scale_y;
  const uint32_t native_group_byte =
      (((native_y << gx) | native_x) << bytes_per_block_log2) |
      (host_group_byte & ((1u << bytes_per_block_log2) - 1));
  return guest_group * group_size + HostGroupByteToGuestTiledByte(
                                        native_group_byte,
                                        bytes_per_block_log2);
}

// Maps an authoritative 1x byte to a selected subpixel of that texel in the
// same pinned representation. This is the inverse used when a scaled resolve
// must be read back without changing the title's guest-visible layout.
constexpr uint32_t GetDownscaledSourceByteOffset(
    uint32_t destination_byte_offset, uint32_t scale_x, uint32_t scale_y,
    uint32_t bytes_per_block_log2, uint32_t subunit_x,
    uint32_t subunit_y) {
  const uint32_t group_size_log2 =
      GetGroupSizeLog2(bytes_per_block_log2);
  const uint32_t group_size = uint32_t(1) << group_size_log2;
  const uint32_t scale_area = scale_x * scale_y;
  const uint32_t guest_group = destination_byte_offset >> group_size_log2;
  const uint32_t guest_tiled_byte = destination_byte_offset & (group_size - 1);
  const uint32_t native_group_byte = GuestTiledByteToHostGroupByte(
      guest_tiled_byte, bytes_per_block_log2);
  const uint32_t gx = GetGroupElementsLog2X(bytes_per_block_log2);
  const uint32_t gy = GetGroupElementsLog2Y(bytes_per_block_log2);
  const uint32_t native_element = native_group_byte >> bytes_per_block_log2;
  const uint32_t host_x = (native_element & ((1u << gx) - 1)) * scale_x + subunit_x;
  const uint32_t host_y = (native_element >> gx) * scale_y + subunit_y;
  const uint32_t host_group = (host_x >> gx) * scale_y + (host_y >> gy);
  const uint32_t host_group_byte =
      ((((host_y & ((1u << gy) - 1)) << gx) | (host_x & ((1u << gx) - 1)))
       << bytes_per_block_log2) | (native_group_byte & ((1u << bytes_per_block_log2) - 1));
  return guest_group * group_size * scale_area + host_group * group_size +
         host_group_byte;
}

}  // namespace rex::graphics::scaled_resolve_util
