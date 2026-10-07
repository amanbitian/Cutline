#include "core/util/Sha256.h"

#include <cstring>
#include <stdexcept>

namespace cutline::util {
namespace {

// First 32 bits of the fractional parts of the cube roots of the first 64
// primes, as the standard specifies.
constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

[[nodiscard]] constexpr std::uint32_t RotateRight(std::uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

}  // namespace

Sha256::Sha256() {
  // First 32 bits of the fractional parts of the square roots of the first
  // eight primes.
  state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
}

void Sha256::Compress(const std::byte* block) {
  std::array<std::uint32_t, 64> schedule{};
  for (int index = 0; index < 16; ++index) {
    // Big-endian, as the standard requires.
    schedule[static_cast<std::size_t>(index)] =
        (static_cast<std::uint32_t>(block[index * 4 + 0]) << 24) |
        (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
        (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
        (static_cast<std::uint32_t>(block[index * 4 + 3]));
  }
  for (int index = 16; index < 64; ++index) {
    const auto previous = schedule[static_cast<std::size_t>(index) - 15];
    const auto recent = schedule[static_cast<std::size_t>(index) - 2];
    const auto s0 = RotateRight(previous, 7) ^ RotateRight(previous, 18) ^ (previous >> 3);
    const auto s1 = RotateRight(recent, 17) ^ RotateRight(recent, 19) ^ (recent >> 10);
    schedule[static_cast<std::size_t>(index)] = schedule[static_cast<std::size_t>(index) - 16] + s0 +
                                                schedule[static_cast<std::size_t>(index) - 7] + s1;
  }

  auto a = state_[0];
  auto b = state_[1];
  auto c = state_[2];
  auto d = state_[3];
  auto e = state_[4];
  auto f = state_[5];
  auto g = state_[6];
  auto h = state_[7];

  for (int index = 0; index < 64; ++index) {
    const auto s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const auto choice = (e & f) ^ (~e & g);
    const auto temp1 = h + s1 + choice + kRoundConstants[static_cast<std::size_t>(index)] +
                       schedule[static_cast<std::size_t>(index)];
    const auto s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const auto majority = (a & b) ^ (a & c) ^ (b & c);
    const auto temp2 = s0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::Update(const std::byte* data, std::size_t size) {
  if (finished_) throw std::logic_error("Sha256 has already produced its digest");
  total_bits_ += static_cast<std::uint64_t>(size) * 8;
  while (size > 0) {
    const auto take = std::min(size, buffer_.size() - buffered_);
    std::memcpy(buffer_.data() + buffered_, data, take);
    buffered_ += take;
    data += take;
    size -= take;
    if (buffered_ == buffer_.size()) {
      Compress(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Sha256::Update(std::string_view text) {
  Update(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

std::string Sha256::HexDigest() {
  if (finished_) throw std::logic_error("Sha256 has already produced its digest");
  finished_ = true;

  // Append 0x80, pad with zeros, then the message length as a big-endian 64-bit
  // count of bits.
  const auto bits = total_bits_;
  buffer_[buffered_++] = std::byte{0x80};
  if (buffered_ > 56) {
    std::memset(buffer_.data() + buffered_, 0, buffer_.size() - buffered_);
    Compress(buffer_.data());
    buffered_ = 0;
  }
  std::memset(buffer_.data() + buffered_, 0, 56 - buffered_);
  for (int index = 0; index < 8; ++index) {
    buffer_[56 + static_cast<std::size_t>(index)] =
        static_cast<std::byte>((bits >> (56 - index * 8)) & 0xFF);
  }
  Compress(buffer_.data());

  static constexpr char kHex[] = "0123456789abcdef";
  std::string digest;
  digest.reserve(64);
  for (const auto word : state_) {
    for (int shift = 28; shift >= 0; shift -= 4) digest += kHex[(word >> shift) & 0xF];
  }
  return digest;
}

std::string Sha256::Of(std::string_view text) {
  Sha256 hash;
  hash.Update(text);
  return hash.HexDigest();
}

}  // namespace cutline::util
