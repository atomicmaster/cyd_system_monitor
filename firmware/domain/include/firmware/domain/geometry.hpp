// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace firmware::domain {

// Landscape screen geometry in logical (post-rotation) pixels.
struct Geometry {
  int logical_width;
  int logical_height;
};

// Rejects a geometry that does not match the one supported MVP layout.
// A future profile with different geometry extends this check rather than
// branching application code on a board name.
constexpr bool IsSupportedGeometry(Geometry geometry) {
  return geometry.logical_width == 320 && geometry.logical_height == 240;
}

// A profile's physical panel dimensions and its post-rotation logical
// dimensions must be the same pair, in either order (0/180, or swapped for
// 90/270). Anything else means the profile's rotation and logical geometry
// disagree with each other.
constexpr bool IsConsistentRotation(int physical_width, int physical_height,
                                     int logical_width, int logical_height) {
  return (physical_width == logical_width && physical_height == logical_height) ||
         (physical_width == logical_height && physical_height == logical_width);
}

}  // namespace firmware::domain
