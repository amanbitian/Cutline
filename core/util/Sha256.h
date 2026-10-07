#pragma once

// SHA-256, per FIPS 180-4.
//
// Implemented here rather than pulled in as a dependency: it is a hundred lines
// of well-specified arithmetic, it is the only hash the project needs, and the
// alternative is linking a crypto library for one function.
//
// It identifies media, not secrets. Collision resistance is what matters, so
// that two different files cannot be mistaken for one another during relink.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cutline::util {

class Sha256 final {
 public:
  Sha256();
  void Update(const std::byte* data, std::size_t size);
  void Update(std::string_view text);
  // Finishes the hash and returns it as lowercase hex. The object must not be
  // updated afterwards.
  [[nodiscard]] std::string HexDigest();

  [[nodiscard]] static std::string Of(std::string_view text);

 private:
  void Compress(const std::byte* block);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::byte, 64> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t total_bits_{0};
  bool finished_{false};
};

}  // namespace cutline::util
