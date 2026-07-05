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

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>

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

// Singleton class to manage persistent file handles for trace logging.
// This avoids the overhead of opening/closing files on every log call.
class TraceFileManager {
 public:
  static TraceFileManager &instance() {
    static TraceFileManager inst;
    return inst;
  }

  bool is_critical_trace_file(const std::string &filename) const {
    return filename.find("l2_to_icnt_timing.txt") != std::string::npos ||
           filename.find("l1_to_l2_requests.txt") != std::string::npos ||
           filename.find("ldst_unit_entries.txt") != std::string::npos ||
           filename.find("icache_access.txt") != std::string::npos ||
           filename.find("dcache_access.txt") != std::string::npos ||
           filename.find("warp_assignments/") != std::string::npos;
  }

  FILE *get_file(const std::string &filename) {
    auto it = m_files.find(filename);
    if (it != m_files.end()) {
      return it->second;
    }
    // Open new file (truncate on first open)
    FILE *f = fopen(filename.c_str(), "w");
    if (f) {
      if (is_critical_trace_file(filename)) {
        setvbuf(f, NULL, _IOLBF, 0);
      }
      m_files[filename] = f;
      m_write_count[filename] = 0;
    } else {
      fprintf(stderr, "TraceFileManager: failed to open %s: %s\n",
              filename.c_str(), strerror(errno));
    }
    return f;
  }

  void close_kernel_files(unsigned kernel_uid, const std::string &kernel_name) {
    const std::string kernel_dir =
        trace_kernel_dir_name(kernel_uid, kernel_name);

    for (auto it = m_files.begin(); it != m_files.end();) {
      if (is_kernel_file(it->first, kernel_dir)) {
        if (it->second) {
          fflush(it->second);
          fclose(it->second);
        }
        m_write_count.erase(it->first);
        it = m_files.erase(it);
      } else {
        ++it;
      }
    }
  }

  void write_line(const std::string &filename, const char *line) {
    FILE *f = get_file(filename);
    if (f) {
      fputs(line, f);
      m_write_count[filename]++;
      if (is_critical_trace_file(filename)) {
        fflush(f);
        return;
      }
      // Flush every N writes to balance performance and data safety
      if (m_write_count[filename] % 10000 == 0) {
        fflush(f);
      }
    }
  }

  void flush_all() {
    for (auto &pair : m_files) {
      if (pair.second) fflush(pair.second);
    }
  }

  void close_all() {
    for (auto &pair : m_files) {
      if (pair.second) {
        fflush(pair.second);
        fclose(pair.second);
      }
    }
    m_files.clear();
    m_write_count.clear();
  }

  ~TraceFileManager() {
    close_all();
  }

 private:
	  TraceFileManager() {}
	  TraceFileManager(const TraceFileManager &) = delete;
	  TraceFileManager &operator=(const TraceFileManager &) = delete;

  bool is_kernel_file(const std::string &filename,
                      const std::string &kernel_dir) const {
    size_t pos = filename.find(kernel_dir);
    if (pos == std::string::npos) return false;
    const bool component_start = (pos == 0 || filename[pos - 1] == '/');
    const size_t after = pos + kernel_dir.size();
    const bool component_end =
        (after == filename.size() || filename[after] == '/');
    return component_start && component_end;
  }

	  std::map<std::string, FILE *> m_files;
	  std::map<std::string, unsigned long long> m_write_count;
};

// Helper to ensure directory exists (creates if missing)
inline void ensure_directory_exists(const char *path) {
  struct stat st = {0};
  if (stat(path, &st) == -1) {
    mkdir(path, 0755);
  }
}

#endif  // TRACE_FILE_MANAGER_H
