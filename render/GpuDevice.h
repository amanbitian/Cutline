#pragma once

// Capability-based GPU selection and execution contract. Render code does not
// branch on a vendor name: D3D12, Metal and Vulkan devices advertise the work
// they can execute and the selector chooses the best compatible adapter.

#include "render/RenderGraph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cutline::render::gpu {

enum class Backend { Cpu, D3D11, D3D12, Metal, Vulkan };
enum class Vendor { Unknown, Amd, Nvidia, Intel, Apple, Software };

enum class Capability : std::uint64_t {
  None = 0,
  Compositor = 1ull << 0,
  OpticalFlow = 1ull << 1,
  TemporalNoiseReduction = 1ull << 2,
  ZeroCopyDecode = 1ull << 3,
  ZeroCopyEncode = 1ull << 4,
  AsyncScopes = 1ull << 5,
  TenBit = 1ull << 6,
  Float16 = 1ull << 7,
};

using Capabilities = std::uint64_t;

[[nodiscard]] constexpr Capabilities Flag(Capability capability) noexcept {
  return static_cast<Capabilities>(capability);
}

[[nodiscard]] constexpr Capabilities operator|(Capability left, Capability right) noexcept {
  return Flag(left) | Flag(right);
}

[[nodiscard]] constexpr bool HasAll(Capabilities available, Capabilities required) noexcept {
  return (available & required) == required;
}

struct DeviceDescriptor final {
  std::string id;
  std::string name;
  Backend backend{Backend::Cpu};
  Vendor vendor{Vendor::Unknown};
  Capabilities capabilities{};
  std::size_t dedicated_memory_bytes{};
  std::size_t shared_memory_bytes{};
  bool integrated{};
  bool available{true};
};

struct DeviceRequirements final {
  Capabilities required{Flag(Capability::Compositor)};
  Capabilities preferred{};
  std::size_t minimum_memory_bytes{};
  bool allow_cpu_fallback{true};
};

struct DeviceSelection final {
  DeviceDescriptor device;
  bool used_cpu_fallback{};
  Capabilities missing_preferred{};
};

// Returns no value when no compatible device exists and CPU fallback was not
// permitted. Selection is deterministic so a saved preference can be audited.
[[nodiscard]] std::optional<DeviceSelection> SelectDevice(std::span<const DeviceDescriptor> devices,
                                                          const DeviceRequirements& requirements = {});

[[nodiscard]] DeviceDescriptor CpuFallbackDevice();
[[nodiscard]] const char* ToString(Backend backend);
[[nodiscard]] const char* ToString(Vendor vendor);

// Opaque native handles keep decoded pictures on-device. Their owner retains
// the native surface until the returned fence completes.
struct ExternalSurface final {
  Backend backend{Backend::Cpu};
  std::uintptr_t native_handle{};
  int width{};
  int height{};
  int format{};
  std::uint64_t generation{};
};

struct DeviceFence {
  virtual ~DeviceFence() = default;
  virtual void Wait() = 0;
  [[nodiscard]] virtual bool Ready() const = 0;
};

struct ExecutionResult final {
  ExternalSurface output;
  std::shared_ptr<DeviceFence> fence;
};

class RenderDevice {
 public:
  virtual ~RenderDevice() = default;
  [[nodiscard]] virtual const DeviceDescriptor& descriptor() const noexcept = 0;
  [[nodiscard]] virtual bool CanImport(const ExternalSurface& surface) const noexcept = 0;
  [[nodiscard]] virtual ExecutionResult Execute(const graph::CompiledGraph& graph,
                                                std::span<const ExternalSurface> inputs) = 0;
};

}  // namespace cutline::render::gpu
