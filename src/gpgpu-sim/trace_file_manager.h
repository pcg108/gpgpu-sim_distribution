// Copyright (c) 2009-2021, Tor M. Aamodt, Tayler Hetherington,
// Vijay Kandiah, Nikos Hardavellas, Mahmoud Khairy, Junrui Pan,
// Timothy G. Rogers
// The University of British Columbia, Northwestern University, Purdue
// University All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this
//    list of conditions and the following disclaimer;
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution;
// 3. Neither the names of The University of British Columbia, Northwestern
//    University nor the names of their contributors may be used to
//    endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef TRACE_FILE_MANAGER_H
#define TRACE_FILE_MANAGER_H

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>

// Prefer the canonical name while accepting existing launch environments.
inline const char *l2_trace_directory() {
  const char *path = std::getenv("L2_TRACE_DIR");
  return path != NULL ? path : std::getenv("L1_TRACE_DIR");
}

inline unsigned long long trace_name_hash(const std::string &name) {
  unsigned long long hash = 1469598103934665603ull;
  for (unsigned char c : name) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

inline std::string bounded_trace_component(const std::string &name,
                                           size_t max_name_len = 160) {
  std::string sanitized;
  sanitized.reserve(name.size());
  for (unsigned char c : name) {
    sanitized.push_back((c == '/' || c == '\0') ? '_' : c);
  }

  if (sanitized.size() <= max_name_len) return sanitized;

  char suffix[32];
  snprintf(suffix, sizeof(suffix), "_%016llx",
           (unsigned long long)trace_name_hash(name));
  const size_t suffix_len = strlen(suffix);
  if (max_name_len <= suffix_len) return std::string(suffix, suffix_len);
  return sanitized.substr(0, max_name_len - suffix_len) + suffix;
}

inline std::string trace_kernel_dir_name(unsigned kernel_uid,
                                         const std::string &kernel_name) {
  return "kernel_" + std::to_string(kernel_uid) + "_" +
         bounded_trace_component(kernel_name);
}

inline std::string trace_kernel_uid_dir_name(unsigned kernel_uid) {
  return "kernel_" + std::to_string(kernel_uid);
}

// Bounded, process-local cache of trace streams. Logging is performed by the
// simulator's single cycle thread; no caller may retain a borrowed FILE*.
class TraceFileManager {
 public:
  struct Statistics {
    size_t capacity;
    size_t open_files;
    size_t peak_open_files;
    unsigned long long evictions;
    unsigned long long reopens;
  };

  static TraceFileManager &instance() {
    static TraceFileManager inst;
    return inst;
  }

  Statistics statistics() const {
    return {m_capacity, m_files.size(), m_peak_open_files, m_evictions, m_reopens};
  }

  void close_kernel_files(unsigned kernel_uid, const std::string &kernel_name) {
    const std::string kernel_dir = trace_kernel_dir_name(kernel_uid, kernel_name);
    const std::string kernel_uid_dir = trace_kernel_uid_dir_name(kernel_uid);
    for (auto it = m_files.begin(); it != m_files.end();) {
      if (is_kernel_file(it->first, kernel_dir) ||
          is_kernel_file(it->first, kernel_uid_dir)) {
        close_file(it++);
      } else {
        ++it;
      }
    }
  }

  void write_line(const std::string &filename, const char *line) {
    FILE *f = get_file(filename);
    errno = 0;
    if (fputs(line, f) == EOF) fail("write", filename, errno);
    record_write(filename, f);
  }

  void writef(const std::string &filename, const char *format, ...) {
    FILE *f = get_file(filename);
    va_list args;
    va_start(args, format);
    errno = 0;
    const int result = vfprintf(f, format, args);
    const int error = errno;
    va_end(args);
    if (result < 0) fail("write", filename, error);
    record_write(filename, f);
  }

  void flush_all() {
    for (const auto &entry : m_files) flush(entry.first, entry.second.file);
  }

  void close_all() {
    while (!m_files.empty()) close_file(m_files.begin());
    // Keep first-open history: late completions must append, even after a drain.
  }

  ~TraceFileManager() { close_all(); }

 private:
  struct OpenFile {
    FILE *file;
    unsigned long long writes;
    std::list<std::string>::iterator recency;
  };
  using Files = std::map<std::string, OpenFile>;

  TraceFileManager() : m_capacity(resolve_capacity()) {}
  TraceFileManager(const TraceFileManager &) = delete;
  TraceFileManager &operator=(const TraceFileManager &) = delete;

  [[noreturn]] static void fail(const char *operation, const std::string &path,
                               int error) {
    fprintf(stderr, "TraceFileManager: %s failed for %s: %s\n", operation,
            path.c_str(), strerror(error ? error : EIO));
    fflush(stderr);
    // Do not run this singleton's destructor recursively after an I/O failure.
    // An incomplete log must fail the run instead of silently losing records.
    std::_Exit(EXIT_FAILURE);
  }

  static size_t resolve_capacity() {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
      fail("getrlimit", "RLIMIT_NOFILE", errno);
    size_t requested = 4096;
    if (const char *value = std::getenv("ACCELSIM_TRACE_MAX_OPEN_FILES")) {
      errno = 0;
      char *end = nullptr;
      const unsigned long long parsed = strtoull(value, &end, 10);
      if (!*value || *value < '0' || *value > '9' || !end || *end ||
          errno == ERANGE || parsed == 0 ||
          parsed > std::numeric_limits<size_t>::max())
        fail("invalid capacity", "ACCELSIM_TRACE_MAX_OPEN_FILES", EINVAL);
      requested = static_cast<size_t>(parsed);
    }
    if (limit.rlim_cur == RLIM_INFINITY) return requested;
    // Reserve room for stdin/stdout/stderr, SASS readers, and simulator assets.
    // EMFILE handling below also covers limits lowered after initialization and
    // unrelated descriptors consuming more than this reserve.
    const rlim_t budget = limit.rlim_cur > 64 ? limit.rlim_cur - 64
                                            : std::max<rlim_t>(1, limit.rlim_cur / 2);
    return static_cast<size_t>(std::min<rlim_t>(requested, budget));
  }

  FILE *get_file(const std::string &filename) {
    auto it = m_files.find(filename);
    if (it != m_files.end()) {
      m_recency.splice(m_recency.begin(), m_recency, it->second.recency);
      return it->second.file;
    }
    if (m_files.size() >= m_capacity) evict_one();
    const bool first_open = m_opened_files.count(filename) == 0;
    FILE *f = nullptr;
    for (;;) {
      errno = 0;
      f = fopen(filename.c_str(), first_open ? "w" : "a");
      if (f) break;
      const int error = errno;
      if ((error == EMFILE || error == ENFILE) && !m_files.empty()) {
        evict_one();
        continue;
      }
      fail("open", filename, error);
    }
    errno = 0;
    if (setvbuf(f, nullptr, _IOFBF, 64 * 1024) != 0)
      fail("setvbuf", filename, errno);
    // Only a successful first open may replace stale contents. A failed first
    // attempt followed by EMFILE eviction must still retry with mode "w".
    m_opened_files.insert(filename);
    if (!first_open) ++m_reopens;
    m_recency.push_front(filename);
    m_files.emplace(filename, OpenFile{f, 0, m_recency.begin()});
    m_peak_open_files = std::max(m_peak_open_files, m_files.size());
    return f;
  }

  static void flush(const std::string &filename, FILE *f) {
    errno = 0;
    if (fflush(f) != 0) fail("flush", filename, errno);
  }

  void close_file(Files::iterator it) {
    flush(it->first, it->second.file);
    errno = 0;
    if (fclose(it->second.file) != 0) fail("close", it->first, errno);
    m_recency.erase(it->second.recency);
    m_files.erase(it);
  }

  void evict_one() {
    close_file(m_files.find(m_recency.back()));
    ++m_evictions;
  }

  void record_write(const std::string &filename, FILE *f) {
    const unsigned long long count = ++m_files.find(filename)->second.writes;
    if (count % 4096 == 0) flush(filename, f);
  }

  bool is_kernel_file(const std::string &filename,
                      const std::string &kernel_dir) const {
    for (size_t pos = filename.find(kernel_dir); pos != std::string::npos;
         pos = filename.find(kernel_dir, pos + 1)) {
      const size_t after = pos + kernel_dir.size();
      if ((pos == 0 || filename[pos - 1] == '/') &&
          (after == filename.size() || filename[after] == '/'))
        return true;
    }
    return false;
  }

  const size_t m_capacity;
  size_t m_peak_open_files = 0;
  unsigned long long m_evictions = 0;
  unsigned long long m_reopens = 0;
  Files m_files;
  std::list<std::string> m_recency;
  std::set<std::string> m_opened_files;
};

// Helper to ensure directory exists (creates if missing)
inline void ensure_directory_exists(const char *path) {
  struct stat st = {0};
  if (stat(path, &st) == -1) {
    mkdir(path, 0755);
  }
}

#endif  // TRACE_FILE_MANAGER_H
