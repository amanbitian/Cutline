#include "core/resource/QuotaManager.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cutline::resource {

QuotaManager::QuotaManager(QuotaConfig config) : config_(config) {
  if (config_.ram_bytes == 0 || config_.vram_bytes == 0 || config_.disk_bytes == 0) {
    throw std::invalid_argument("Resource quotas must be positive");
  }
}

std::string QuotaManager::Identity(const std::string& owner, const std::string& key, Tier tier) {
  return std::to_string(static_cast<int>(tier)) + ":" + owner + ":" + key;
}

std::size_t& QuotaManager::Usage(Tier tier) {
  if (tier == Tier::Ram) return ram_bytes_;
  if (tier == Tier::Vram) return vram_bytes_;
  return disk_bytes_;
}

std::size_t QuotaManager::Usage(Tier tier) const {
  if (tier == Tier::Ram) return ram_bytes_;
  if (tier == Tier::Vram) return vram_bytes_;
  return disk_bytes_;
}

std::size_t QuotaManager::Limit(Tier tier) const {
  const auto base = tier == Tier::Ram ? config_.ram_bytes : tier == Tier::Vram ? config_.vram_bytes : config_.disk_bytes;
  const auto ratio = pressure_ == Pressure::Normal ? 1.0 : pressure_ == Pressure::Elevated ? 0.75 : 0.5;
  return std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<double>(base) * ratio));
}

void QuotaManager::Track(std::string owner, std::string key, Tier tier, std::size_t bytes, double cost, Evict evict) {
  if (owner.empty() || key.empty() || bytes == 0 || !std::isfinite(cost) || cost < 0.0 || !evict) {
    throw std::invalid_argument("A quota entry needs identity, bytes, cost and an eviction callback");
  }
  const auto identity = Identity(owner, key, tier);
  const std::lock_guard<std::mutex> lock(mutex_);
  if (const auto found = entries_.find(identity); found != entries_.end()) {
    Usage(found->second.tier) -= found->second.bytes;
    entries_.erase(found);
  }
  Usage(tier) += bytes;
  entries_.emplace(identity, Entry{std::move(owner), std::move(key), tier, bytes, cost, ++clock_, std::move(evict)});
}

void QuotaManager::Touch(const std::string& owner, const std::string& key, Tier tier) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (const auto found = entries_.find(Identity(owner, key, tier)); found != entries_.end()) found->second.touched = ++clock_;
}

void QuotaManager::Forget(const std::string& owner, const std::string& key, Tier tier) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(Identity(owner, key, tier));
  if (found == entries_.end()) return;
  Usage(found->second.tier) -= found->second.bytes;
  entries_.erase(found);
}

void QuotaManager::ForgetOwner(const std::string& owner) {
  const std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.owner != owner) { ++it; continue; }
    Usage(it->second.tier) -= it->second.bytes;
    it = entries_.erase(it);
  }
}

void QuotaManager::SetPressure(Pressure pressure) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    pressure_ = pressure;
  }
  EnforceAll();
}

void QuotaManager::Enforce(Tier tier) {
  std::vector<Evict> callbacks;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    while (Usage(tier) > Limit(tier)) {
      auto victim = entries_.end();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.tier != tier) continue;
        if (victim == entries_.end() || it->second.cost < victim->second.cost ||
            (it->second.cost == victim->second.cost && it->second.touched < victim->second.touched) ||
            (it->second.cost == victim->second.cost && it->second.touched == victim->second.touched &&
             it->second.bytes > victim->second.bytes)) victim = it;
      }
      if (victim == entries_.end()) break;
      Usage(tier) -= victim->second.bytes;
      callbacks.push_back(std::move(victim->second.evict));
      entries_.erase(victim);
      ++evictions_;
    }
  }
  // Cache callbacks take their own locks, so they must never run under ours.
  for (auto& callback : callbacks) callback();
}

void QuotaManager::EnforceAll() {
  Enforce(Tier::Ram);
  Enforce(Tier::Vram);
  Enforce(Tier::Disk);
}

QuotaStatistics QuotaManager::statistics() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return {ram_bytes_, vram_bytes_, disk_bytes_, entries_.size(), evictions_, pressure_};
}

}  // namespace cutline::resource
