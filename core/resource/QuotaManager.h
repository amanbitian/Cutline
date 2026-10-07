#pragma once

// Shared, pressure-aware budgets for caches and frame pools. Entries remain
// owned by their cache; the manager only chooses what should be evicted.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cutline::resource {

enum class Tier { Ram, Vram, Disk };
enum class Pressure { Normal, Elevated, Critical };

struct QuotaConfig final {
  std::size_t ram_bytes{1024ull << 20};
  std::size_t vram_bytes{1024ull << 20};
  std::size_t disk_bytes{16ull << 30};
};

struct QuotaStatistics final {
  std::size_t ram_bytes{};
  std::size_t vram_bytes{};
  std::size_t disk_bytes{};
  std::size_t entries{};
  std::uint64_t evictions{};
  Pressure pressure{Pressure::Normal};
};

class QuotaManager final {
 public:
  using Evict = std::function<void()>;

  explicit QuotaManager(QuotaConfig config = {});

  // `cost` is relative recomputation cost. Under equal age, cheap entries are
  // evicted before expensive analyses; larger entries win ties to release
  // pressure with fewer callbacks.
  void Track(std::string owner, std::string key, Tier tier, std::size_t bytes, double cost, Evict evict);
  void Touch(const std::string& owner, const std::string& key, Tier tier);
  void Forget(const std::string& owner, const std::string& key, Tier tier);
  void ForgetOwner(const std::string& owner);

  void SetPressure(Pressure pressure);
  void Enforce(Tier tier);
  void EnforceAll();

  [[nodiscard]] QuotaStatistics statistics() const;
  [[nodiscard]] const QuotaConfig& config() const noexcept { return config_; }

 private:
  struct Entry final {
    std::string owner;
    std::string key;
    Tier tier{Tier::Ram};
    std::size_t bytes{};
    double cost{1.0};
    std::uint64_t touched{};
    Evict evict;
  };

  [[nodiscard]] static std::string Identity(const std::string& owner, const std::string& key, Tier tier);
  [[nodiscard]] std::size_t Limit(Tier tier) const;
  [[nodiscard]] std::size_t& Usage(Tier tier);
  [[nodiscard]] std::size_t Usage(Tier tier) const;

  QuotaConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::uint64_t clock_{};
  std::uint64_t evictions_{};
  std::size_t ram_bytes_{};
  std::size_t vram_bytes_{};
  std::size_t disk_bytes_{};
  Pressure pressure_{Pressure::Normal};
};

}  // namespace cutline::resource
