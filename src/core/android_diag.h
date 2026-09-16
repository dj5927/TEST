#pragma once

#include <string_view>

#if defined(__ANDROID__)
#include <cstdlib>
#include <fstream>
#include <mutex>

namespace bd {

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
} // namespace bd

#endif
