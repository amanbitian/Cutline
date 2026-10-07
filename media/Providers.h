#pragma once

// Provider registration.
//
// One call sets up every media backend this build contains. FFmpeg is optional:
// a build without it still opens synthetic media, so the core, the compositor,
// and their tests remain buildable and runnable with no third-party media
// dependency at all.

#include <string>
#include <vector>

namespace cutline::media {

void RegisterSyntheticProvider();

#ifdef CUTLINE_HAVE_FFMPEG
void RegisterFFmpegProvider();
void RegisterFFmpegWriter();
#endif

// Registers everything available. Safe to call more than once.
inline void RegisterAllProviders() {
  RegisterSyntheticProvider();
#ifdef CUTLINE_HAVE_FFMPEG
  RegisterFFmpegProvider();
  RegisterFFmpegWriter();
#endif
}

// True when this build can write media files. Reading and writing arrive
// together today, but they are separate questions and a caller should ask the
// one it means.
[[nodiscard]] inline bool HasFileEncoding() {
#ifdef CUTLINE_HAVE_FFMPEG
  return true;
#else
  return false;
#endif
}

// True when this build can read real media files rather than only synthetic
// ones. Callers use it to explain the limitation instead of failing opaquely.
[[nodiscard]] inline bool HasFileDecoding() {
#ifdef CUTLINE_HAVE_FFMPEG
  return true;
#else
  return false;
#endif
}

}  // namespace cutline::media
