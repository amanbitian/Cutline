#include "render/GpuDevice.h"

#include <algorithm>
#include <tuple>

namespace cutline::render::gpu {
namespace {

[[nodiscard]] int BitCount(Capabilities value) noexcept {
  int count = 0;
  while (value != 0) {
    count += static_cast<int>(value & 1ull);
    value >>= 1u;
  }
  return count;
}

[[nodiscard]] std::size_t AvailableMemory(const DeviceDescriptor& device) noexcept {
  return device.dedicated_memory_bytes + device.shared_memory_bytes;
}

[[nodiscard]] auto Rank(const DeviceDescriptor& device, const DeviceRequirements& requirements) {
  const auto preferred = BitCount(device.capabilities & requirements.preferred);
  const auto gpu = device.backend == Backend::Cpu ? 0 : 1;
  const auto dedicated = device.integrated ? 0 : 1;
  // The stable textual fields are final tie breakers, independent of discovery
  // order returned by an operating-system API.
  return std::tuple{gpu, preferred, dedicated, AvailableMemory(device), device.name, device.id};
}

}  // namespace

std::optional<DeviceSelection> SelectDevice(std::span<const DeviceDescriptor> devices,
                                            const DeviceRequirements& requirements) {
  const DeviceDescriptor* best = nullptr;
  for (const auto& device : devices) {
    if (!device.available || device.backend == Backend::Cpu) continue;
    if (!HasAll(device.capabilities, requirements.required)) continue;
    if (AvailableMemory(device) < requirements.minimum_memory_bytes) continue;
    if (best == nullptr || Rank(device, requirements) > Rank(*best, requirements)) best = &device;
  }
  if (best != nullptr) {
    return DeviceSelection{*best, false, requirements.preferred & ~best->capabilities};
  }

  if (!requirements.allow_cpu_fallback) return std::nullopt;
  auto cpu = CpuFallbackDevice();
  // The CPU path is the correctness reference for every graph operation. GPU-
  // only requirements still reject fallback (for example zero-copy decode).
  if (!HasAll(cpu.capabilities, requirements.required)) return std::nullopt;
  return DeviceSelection{std::move(cpu), true, requirements.preferred & ~cpu.capabilities};
}

DeviceDescriptor CpuFallbackDevice() {
  return {"cpu-reference", "CPU reference renderer", Backend::Cpu, Vendor::Software,
          Flag(Capability::Compositor) | Flag(Capability::OpticalFlow) |
              Flag(Capability::TemporalNoiseReduction) | Flag(Capability::AsyncScopes) |
              Flag(Capability::TenBit) | Flag(Capability::Float16),
          0, 0, true, true};
}

const char* ToString(Backend backend) {
  switch (backend) {
    case Backend::Cpu: return "CPU";
    case Backend::D3D11: return "D3D11";
    case Backend::D3D12: return "D3D12";
    case Backend::Metal: return "Metal";
    case Backend::Vulkan: return "Vulkan";
  }
  return "Unknown";
}

const char* ToString(Vendor vendor) {
  switch (vendor) {
    case Vendor::Unknown: return "Unknown";
    case Vendor::Amd: return "AMD";
    case Vendor::Nvidia: return "NVIDIA";
    case Vendor::Intel: return "Intel";
    case Vendor::Apple: return "Apple";
    case Vendor::Software: return "Software";
  }
  return "Unknown";
}

}  // namespace cutline::render::gpu
