#ifndef L2_TRAFFIC_TRACE_H
#define L2_TRAFFIC_TRACE_H

#include <string>
#include <vector>

class mem_fetch;
class memory_config;

// Only snapshots live here: requests may be mutated by chiplet forwarding and
// released independently during LRC reply distribution.
struct lrc_trace_record {
  std::string filename;
  std::string prefix;
  std::vector<std::string> contributors;
  bool relevant = false;
  bool emitted = false;
};

void log_l2_source_departure(const mem_fetch *mf, const memory_config *config);
void audit_l2_traffic(const mem_fetch *mf, const char *event,
                      unsigned long long cycle, bool lrc_enabled);
void record_lrc_trace_member(lrc_trace_record &record, const mem_fetch *mf,
                             const memory_config *config,
                             unsigned long long cycle);
void finalize_lrc_trace(lrc_trace_record &record);

#endif
