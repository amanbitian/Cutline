#pragma once

// Reader and sampler for the widely supported IRIDAS / Adobe .cube look-up tables, 3D and 1D.
// Parsing is deliberately independent of the compositor so import validation,
// thumbnails, CPU rendering, and the GPU upload path share one contract.
//
// A 3D table maps a colour to a colour through a lattice of size^3 points with the red index changing fastest, and is
// read with trilinear interpolation. A 1D table is three curves in one: each channel is looked up in its own column
// (this is how a shaper or a tone curve is shipped as a .cube). The domain says what input range the table covers
// (DOMAIN_MIN / DOMAIN_MAX, or the Adobe spelling LUT_*_INPUT_RANGE); inputs outside it are held at its edges.

#include <array>
#include <filesystem>
#include <istream>
#include <vector>

namespace cutline::render {

class CubeLut final {
 public:
  enum class Kind { Cube3D, Curves1D };

  [[nodiscard]] static CubeLut Parse(std::istream& input);
  [[nodiscard]] static CubeLut Load(const std::filesystem::path& path);

  // The colour the table makes of (red, green, blue).
  [[nodiscard]] std::array<float, 3> Sample(float red, float green, float blue) const;
  // The same for a pixel loop: no allocation, nothing re-derived per call that does not depend on the colour.
  // Bit for bit what Sample returns.
  void Map(const float input[3], float output[3]) const noexcept;

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  // Lattice points per side of a 3D table, or the number of rows of a 1D one.
  [[nodiscard]] int size() const noexcept { return size_; }
  [[nodiscard]] const std::array<float, 3>& domain_min() const noexcept { return domain_min_; }
  [[nodiscard]] const std::array<float, 3>& domain_max() const noexcept { return domain_max_; }
  // The entries as red, green, blue triples in file order (red index fastest for a 3D table).
  [[nodiscard]] const float* data() const noexcept { return entries_.data(); }
  [[nodiscard]] std::size_t entry_count() const noexcept { return entries_.size() / 3; }

 private:
  Kind kind_{Kind::Cube3D};
  int size_{0};
  std::array<float, 3> domain_min_{0.0f, 0.0f, 0.0f};
  std::array<float, 3> domain_max_{1.0f, 1.0f, 1.0f};
  bool default_domain_{true};
  std::vector<float> entries_;
};

}  // namespace cutline::render
