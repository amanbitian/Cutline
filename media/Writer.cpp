#include "media/Writer.h"

#include <stdexcept>

namespace cutline::media {

WriterRegistry& WriterRegistry::Instance() {
  static WriterRegistry registry;
  return registry;
}

void WriterRegistry::Register(std::unique_ptr<WriterProvider> provider) {
  if (provider == nullptr) throw std::invalid_argument("Cannot register a null writer provider");
  providers_.push_back(std::move(provider));
}

std::unique_ptr<Writer> WriterRegistry::Open(const ExportSettings& settings) const {
  for (const auto& provider : providers_) {
    if (provider->CanWrite(settings)) return provider->Open(settings);
  }
  if (providers_.empty()) {
    throw std::runtime_error("This build cannot write media files: no encoder provider is registered");
  }
  std::string available;
  for (const auto& provider : providers_) {
    if (!available.empty()) available += ", ";
    available += provider->name();
  }
  throw std::runtime_error("No encoder provider can write " + settings.path + " (available: " + available + ")");
}

std::vector<std::string> WriterRegistry::ProviderNames() const {
  std::vector<std::string> names;
  names.reserve(providers_.size());
  for (const auto& provider : providers_) names.push_back(provider->name());
  return names;
}

}  // namespace cutline::media

#ifndef CUTLINE_HAVE_FFMPEG
namespace cutline::media {
// Without FFmpeg nothing can be encoded: say so rather than pretend.
std::vector<EncoderInfo> ListEncoders() { return {}; }
bool TestEncoder(const std::string&, const std::string&, std::string* why, const std::map<std::string, std::string>&) {
  if (why != nullptr) *why = "this build has no encoders";
  return false;
}
}  // namespace cutline::media
#endif
