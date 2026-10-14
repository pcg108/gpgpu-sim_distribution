#include "l2_traffic_trace.h"

#include <climits>
#include <set>
#include <sstream>

#include "gpu-sim.h"
#include "mem_fetch.h"
#include "trace_file_manager.h"

namespace {
const char *source_name(const mem_fetch *mf) {
  switch (mf->get_l2_source()) {
    case L2_SOURCE_L1_READ: return "l1_miss";
    case L2_SOURCE_L1_WRITE: return "l1_write";
    case L2_SOURCE_L1_BYPASS_LOAD: return "l1_bypass_load";
    case L2_SOURCE_L1_BYPASS_STORE: return "l1_bypass_store";
    case L2_SOURCE_TMA_LOAD: return "tma_load";
    case L2_SOURCE_TMA_STORE: return "tma_store";
    default: return "other_read";
  }
}

std::string hex_value(unsigned long long value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

std::string byte_mask_hex(const mem_fetch *mf) {
  const auto mask = mf->get_access_byte_mask();
  std::string out = "0x";
  for (int i = (mask.size() + 3) / 4 - 1; i >= 0; --i) {
    unsigned digit = 0;
    for (unsigned bit = 0; bit < 4; ++bit) {
      unsigned index = i * 4 + bit;
      if (index < mask.size() && mask.test(index)) digit |= 1U << bit;
    }
    out += "0123456789abcdef"[digit];
  }
  return out;
}

struct attribution {
  unsigned kernel = UINT_MAX, scheduler = 0, warp = UINT_MAX,
           ordinal = UINT_MAX;
  unsigned long long pc = (unsigned long long)-1;
  std::string kernel_name = "unknown_kernel";
  explicit attribution(const mem_fetch *mf) {
    const auto *meta = mf->get_trace_metadata();
    if (meta && meta->valid) {
      kernel = meta->kernel_uid;
      scheduler = meta->scheduler_id;
      warp = meta->dynamic_wid;
      ordinal = meta->inst_ordinal;
      pc = meta->pc;
      if (!meta->kernel_name.empty()) kernel_name = meta->kernel_name;
    }
  }
};

std::string trace_filename(const mem_fetch *mf, const char *basename) {
  const attribution a(mf);
  std::ostringstream out;
  out << l2_trace_directory() << "/"
      << trace_kernel_dir_name(a.kernel, a.kernel_name)
      << "/shader_" << mf->get_sid() << "/scheduler_" << a.scheduler
      << "/" << basename;
  return out.str();
}

void write_trace(const std::string &filename, const std::string &line) {
  // Prepare parents lazily, including a nested caller-supplied trace root.
  static std::set<std::string> prepared_files;
  if (prepared_files.insert(filename).second) {
    for (size_t pos = filename.find('/', 1); pos != std::string::npos;
         pos = filename.find('/', pos + 1)) {
      ensure_directory_exists(filename.substr(0, pos).c_str());
    }
  }
  TraceFileManager::instance().write_line(filename, line.c_str());
}

std::string json_attribution(const mem_fetch *mf) {
  const attribution a(mf);
  std::ostringstream out;
  out << "\"kernel_uid\":" << a.kernel << ",\"sm\":" << mf->get_sid()
      << ",\"scheduler\":" << a.scheduler << ",\"dynamic_warp\":" << a.warp
      << ",\"inst_ordinal\":" << a.ordinal << ",\"PC\":\""
      << hex_value(a.pc) << "\"";
  return out.str();
}
}  // namespace

void log_l2_source_departure(const mem_fetch *mf, const memory_config *config) {
  if (l2_trace_directory() == nullptr) return;
  const char *filename = nullptr;
  switch (mf->get_l2_source()) {
    case L2_SOURCE_L1_BYPASS_LOAD:
    case L2_SOURCE_L1_BYPASS_STORE:
      filename = "l1_bypass_to_l2.txt";
      break;
    case L2_SOURCE_TMA_LOAD:
    case L2_SOURCE_TMA_STORE:
      filename = "tma_to_l2_requests.txt";
      break;
    default: return;
  }
  const attribution a(mf);
  const auto addr = mf->get_addr();
  std::ostringstream line;
  line << "request_uid=" << mf->get_l2_source_request_uid()
       << " PC=" << hex_value(a.pc) << " dynamic_warp=" << a.warp
       << " inst_ordinal=" << a.ordinal << " addr=" << hex_value(addr)
       << " subpartition=" << mf->get_sub_partition_id()
       << " set_index=" << (config->m_L2_config.disabled() ? UINT_MAX
                              : config->m_L2_config.set_index(addr))
       << " tag=" << hex_value(config->m_L2_config.disabled() ? ~0ULL
                                 : config->m_L2_config.tag(addr))
       << " sector_mask=" << hex_value(mf->get_access_sector_mask().to_ullong())
       << " size=" << mf->get_access_size()
       << " type=" << (mf->get_is_write() ? "WRITE" : "READ")
       << " source=" << source_name(mf)
       << " source_to_l2_cycle=" << mf->get_l2_source_cycle() << '\n';
  write_trace(trace_filename(mf, filename), line.str());
}

void audit_l2_traffic(const mem_fetch *mf, const char *event,
                      unsigned long long cycle, bool lrc_enabled) {
  static const char *path = std::getenv("L2_TRAFFIC_AUDIT_FILE");
  if (!path) return;
  const bool departure = std::string(event) == "departure";
  const auto type = mf->get_access_type();
  const bool data_request = type == GLOBAL_ACC_R || type == GLOBAL_ACC_W ||
      type == LOCAL_ACC_R || type == LOCAL_ACC_W || type == L1_WRBK_ACC ||
      type == L1_WR_ALLOC_R;
  // Do not use the production tag as the only eligibility test: otherwise a
  // missing source hook could disappear from both the log and its audit.
  if (departure && !mf->has_l2_source() && !data_request) return;
  const auto &access = mf->get_mem_access();
  unsigned destinations = access.is_tma_multicast()
      ? std::bitset<32>(access.get_tma_multicast_cta_mask()).count() : 1;
  std::ostringstream line;
  line << "{\"event\":\"" << event << "\",\"source_uid\":"
       << (departure && !mf->has_l2_source() ? mf->get_request_uid()
                                             : mf->get_l2_source_request_uid())
       << ",\"source\":\""
       << source_name(mf) << "\",\"cycle\":" << cycle
       << ",\"source_cycle\":" << mf->get_l2_source_cycle()
       << ",\"addr\":\"" << hex_value(mf->get_addr()) << "\",\"size\":"
       << mf->get_access_size() << ",\"data_size\":" << mf->get_data_size()
       << ",\"sector_mask\":\""
       << hex_value(mf->get_access_sector_mask().to_ullong())
       << "\",\"byte_mask\":\"" << byte_mask_hex(mf) << "\","
       << json_attribution(mf) << ",\"transport_uid\":" << mf->get_request_uid()
       << ",\"subpartition\":" << mf->get_sub_partition_id()
       << ",\"lrc_enabled\":" << (lrc_enabled ? "true" : "false")
       << ",\"is_write\":" << (mf->get_is_write() ? "true" : "false")
       << ",\"access_type\":\"" << mem_access_type_str(mf->get_access_type())
       << "\",\"multicast_destinations\":" << destinations << "}\n";
  TraceFileManager::instance().write_line(path, line.str().c_str());
}

void record_lrc_trace_member(lrc_trace_record &record, const mem_fetch *mf,
                             const memory_config *config,
                             unsigned long long cycle) {
  if (l2_trace_directory() == nullptr) return;
  if (record.contributors.empty()) {
    record.filename = trace_filename(mf, "lrc_to_l2_requests.txt");
    const auto addr = mf->get_addr();
    std::ostringstream prefix;
    prefix << "service_uid=" << mf->get_request_uid()
           << " addr=" << hex_value(addr)
           << " subpartition=" << mf->get_sub_partition_id()
           << " set_index=" << (config->m_L2_config.disabled() ? UINT_MAX
                                  : config->m_L2_config.set_index(addr))
           << " tag=" << hex_value(config->m_L2_config.disabled() ? ~0ULL
                                     : config->m_L2_config.tag(addr))
           << " sector_mask=" << hex_value(mf->get_access_sector_mask().to_ullong())
           << " size=" << mf->get_access_size()
           << " lrc_to_l2_cycle=" << cycle;
    record.prefix = prefix.str();
  }
  record.relevant |= mf->has_l2_source();
  std::ostringstream member;
  member << "{\"request_uid\":" << mf->get_l2_source_request_uid()
         << ",\"transport_uid\":" << mf->get_request_uid()
         << ",\"source\":\"" << source_name(mf)
         << "\",\"access_type\":\"" << mem_access_type_str(mf->get_access_type())
         << "\"," << json_attribution(mf)
         << ",\"source_cycle\":" << mf->get_l2_source_cycle() << "}";
  record.contributors.push_back(member.str());
}

void finalize_lrc_trace(lrc_trace_record &record) {
  if (record.emitted) return;
  record.emitted = true;
  if (!record.relevant) return;
  std::ostringstream line;
  line << record.prefix << " contributor_count=" << record.contributors.size()
       << " contributors=[";
  for (size_t i = 0; i < record.contributors.size(); ++i) {
    if (i) line << ',';
    line << record.contributors[i];
  }
  line << "]\n";
  write_trace(record.filename, line.str());
  // Replies may remain blocked; no need to retain serialized metadata.
  record.contributors.clear();
}
