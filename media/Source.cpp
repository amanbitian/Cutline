#include "media/Source.h"

#include <algorithm>
#include <stdexcept>

namespace cutline::media {

const commands::MediaStream* Probe::PrimaryVideo() const {
  const auto found = std::find_if(streams.begin(), streams.end(), [](const commands::MediaStream& stream) {
    return stream.kind == model::StreamKind::Video;
  });
  return found == streams.end() ? nullptr : &*found;
}

const commands::MediaStream* Probe::PrimaryAudio() const {
  const auto found = std::find_if(streams.begin(), streams.end(), [](const commands::MediaStream& stream) {
    return stream.kind == model::StreamKind::Audio;
  });
  return found == streams.end() ? nullptr : &*found;
}

SourceRegistry& SourceRegistry::Instance() {
  static SourceRegistry registry;
  return registry;
}

void SourceRegistry::Register(std::unique_ptr<SourceProvider> provider) {
  if (provider == nullptr) throw std::invalid_argument("Cannot register a null source provider");
  providers_.push_back(std::move(provider));
}

std::unique_ptr<Source> SourceRegistry::Open(const std::string& path) const {
  for (const auto& provider : providers_) {
    if (provider->CanOpen(path)) return provider->Open(path);
  }
  return nullptr;
}

std::unique_ptr<Source> SourceRegistry::Open(const std::string& path, const OpenOptions& options) const {
  for (const auto& provider : providers_) {
    if (provider->CanOpen(path)) return provider->Open(path, options);
  }
  return nullptr;
}

Probe SourceRegistry::ProbeFile(const std::string& path) const {
  for (const auto& provider : providers_) {
    if (provider->CanOpen(path)) return provider->ProbeFile(path);
  }
  throw std::runtime_error("No media provider can read " + path +
                           (providers_.empty() ? " (no providers are registered in this build)" : ""));
}

std::vector<std::string> SourceRegistry::ProviderNames() const {
  std::vector<std::string> names;
  names.reserve(providers_.size());
  for (const auto& provider : providers_) names.push_back(provider->name());
  return names;
}

}  // namespace cutline::media
