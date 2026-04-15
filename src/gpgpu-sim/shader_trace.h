// Copyright (c) 2009-2011, Tor M. Aamodt, Tim Rogers
// George L. Yuan, Andrew Turner, Inderpreet Singh
// The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution. Neither the name of
// The University of British Columbia nor the names of its contributors may be
// used to endorse or promote products derived from this software without
// specific prior written permission.
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

#ifndef __SHADER_TRACE_H__
#define __SHADER_TRACE_H__

#include "../trace.h"
#include <sys/stat.h>
#include <string>

#if TRACING_ON

#define SHADER_PRINT_STR SIM_PRINT_STR "Core %d - "
#define SCHED_PRINT_STR SHADER_PRINT_STR "Scheduler %d - "
#define SHADER_DTRACE(x) \
  (DTRACE(x) &&          \
   (Trace::sampling_core == (int)get_sid() || Trace::sampling_core == -1))

// Intended to be called from inside components of a shader core.
// Depends on a get_sid() function
#define SHADER_DPRINTF(x, ...)                                \
  do {                                                        \
    if (SHADER_DTRACE(x)) {                                   \
      printf(SHADER_PRINT_STR,                                \
             m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle, \
             Trace::trace_streams_str[Trace::x], get_sid());  \
      printf(__VA_ARGS__);                                    \
    }                                                         \
  } while (0)

/*
// Intended to be called from inside a scheduler_unit.
// Depends on a m_id member
#define SCHED_DPRINTF(...)                                               \
  do {                                                                   \
    if (SHADER_DTRACE(WARP_SCHEDULER)) {                                 \
      printf(SCHED_PRINT_STR,                                            \
             m_shader->get_gpu()->gpu_sim_cycle +                        \
                 m_shader->get_gpu()->gpu_tot_sim_cycle,                 \
             Trace::trace_streams_str[Trace::WARP_SCHEDULER], get_sid(), \
             m_id);                                                      \
      printf(__VA_ARGS__);                                               \
    }                                                                    \
  } while (0)
*/
#define SCHED_DPRINTF(...)                                               \
  do {                                                                   \
    if (SHADER_DTRACE(WARP_SCHEDULER)) {                                 \
      static FILE* sched_trace_file = NULL;                              \
      static unsigned int last_kernel_uid = (unsigned int)(-1);          \
      static int last_cluster_id = -1;                                   \
      static int last_core_id = -1;                                      \
      static int last_scheduler_id = -1;                                 \
      unsigned int current_kernel_uid = (unsigned int)(-1);              \
      kernel_info_t *current_kernel = m_shader->get_kernel();            \
      if (current_kernel) {                                              \
        current_kernel_uid = current_kernel->get_trace_kernel_id();      \
      } else {                                                            \
        current_kernel_uid = m_shader->get_gpu()->last_uid;              \
      }                                                                   \
      int current_cluster_id = m_shader->get_cluster_id();               \
      int current_core_id = get_sid();                                   \
      int current_scheduler_id = m_id;                                   \
      \
      /* Check if kernel, cluster, core, or scheduler changed */ \
      if (current_kernel_uid != last_kernel_uid ||                       \
          current_cluster_id != last_cluster_id ||                       \
          current_core_id != last_core_id ||                             \
          current_scheduler_id != last_scheduler_id) {                   \
        if (sched_trace_file != NULL) {                                  \
          fclose(sched_trace_file);                                      \
          sched_trace_file = NULL;                                       \
        }                                                                 \
        last_kernel_uid = current_kernel_uid;                            \
        last_cluster_id = current_cluster_id;                            \
        last_core_id = current_core_id;                                  \
        last_scheduler_id = current_scheduler_id;                        \
      }                                                                   \
      \
      if (sched_trace_file == NULL) {                                    \
        const char* trace_dir = getenv("SCHED_TRACE_DIR");              \
        if (trace_dir == NULL) trace_dir = "scheduler_traces";         \
        \
        /* Create base trace directory if it doesn't exist */ \
        mkdir(trace_dir, 0755);                                          \
        \
        /* Create kernel subdirectory */ \
        char kernel_dir[256];                                            \
        snprintf(kernel_dir, sizeof(kernel_dir),                         \
                 "%s/kernel_%u/", trace_dir, current_kernel_uid);        \
        mkdir(kernel_dir, 0755);                                         \
        \
        /* Create cluster subdirectory */ \
        char cluster_dir[256];                                           \
        snprintf(cluster_dir, sizeof(cluster_dir),                       \
                 "%scluster_%d/", kernel_dir, current_cluster_id);       \
        mkdir(cluster_dir, 0755);                                        \
        \
        /* Create filename with core and scheduler ID */ \
        char filename[256];                                              \
        snprintf(filename, sizeof(filename),                             \
                 "%score_%d_scheduler_%d.txt", cluster_dir, current_core_id, current_scheduler_id); \
        sched_trace_file = fopen(filename, "a");                         \
      }                                                                   \
      \
      if (sched_trace_file != NULL) {                                    \
        fprintf(sched_trace_file, SCHED_PRINT_STR,                       \
               m_shader->get_gpu()->gpu_sim_cycle +                      \
                   m_shader->get_gpu()->gpu_tot_sim_cycle,               \
               Trace::trace_streams_str[Trace::WARP_SCHEDULER], get_sid(), \
               m_id);                                                    \
        fprintf(sched_trace_file, __VA_ARGS__);                          \
        fflush(sched_trace_file);                                        \
      }                                                                   \
    }                                                                    \
  } while (0)


#else

#define SHADER_DTRACE(x) (false)
#define SHADER_DPRINTF(x, ...) \
  do {                         \
  } while (0)
#define SCHED_DPRINTF(x, ...) \
  do {                        \
  } while (0)

#endif

#endif
