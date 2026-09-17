#pragma once

#include <string_view>

#if defined(__ANDROID__)
#include <cstdlib>
#include <fstream>
#include <mutex>

extern "C" void ReblueAndroidCrashStage(int stage);
extern "C" int ReblueAndroidCrashStageGet();
extern "C" int ReblueAndroidCrashStageThreadGet();

namespace bd {

inline void AndroidCrashStage(int stage) { ReblueAndroidCrashStage(stage); }
inline int AndroidCrashStageGet() { return ReblueAndroidCrashStageGet(); }
inline int AndroidCrashStageThreadGet() { return ReblueAndroidCrashStageThreadGet(); }

inline void AndroidDiag(std::string_view line) {
  const char *path = std::getenv("REBLUE_DIAG_FILE");
  if (!path || !*path)
    return;

  static std::mutex mutex;
  std::lock_guard lock(mutex);
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out)
    return;
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
  out.put('\n');
  out.flush();
}

} // namespace bd

#else

namespace bd {
inline void AndroidDiag(std::string_view) {}
inline void AndroidCrashStage(int) {}
inline int AndroidCrashStageGet() { return 0; }
inline int AndroidCrashStageThreadGet() { return 0; }
} // namespace bd

#endif
