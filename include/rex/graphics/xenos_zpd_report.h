/**
 ******************************************************************************
 * Xenos Z-pass-done report layout helpers.                                  *
 ******************************************************************************
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

// D3D uses pairs of 0x20-byte records for a hardware occlusion query. The END
// record is at the 0x40-byte slot base and the BEGIN record follows it. The
// address programmed in RB_SAMPLE_COUNT_ADDR may point anywhere in a record,
// so report ownership must be derived from the aligned address rather than
// from the current contents alone.
struct XenosZPDReport {
  static constexpr uint32_t kRecordSizeBytes = 0x20;
  static constexpr uint32_t kRecordAlignMask = ~(kRecordSizeBytes - 1);
  static constexpr uint32_t kSlotSizeBytes = 0x40;
  static constexpr uint32_t kSlotAlignMask = ~(kSlotSizeBytes - 1);

  static constexpr uint32_t GetRecordBase(uint32_t address) {
    return address & kRecordAlignMask;
  }

  static constexpr uint32_t GetSlotBase(uint32_t address) {
    return address & kSlotAlignMask;
  }

  static constexpr uint32_t GetBeginRecordBase(uint32_t address) {
    return GetSlotBase(address) + kRecordSizeBytes;
  }

  static constexpr uint32_t GetEndRecordBase(uint32_t address) {
    return GetSlotBase(address);
  }

  static constexpr bool IsBeginRecord(uint32_t address) {
    const uint32_t record_base = GetRecordBase(address);
    return record_base && record_base == GetBeginRecordBase(record_base);
  }

  static constexpr bool IsEndRecord(uint32_t address) {
    const uint32_t record_base = GetRecordBase(address);
    return record_base && record_base == GetEndRecordBase(record_base);
  }

  static bool HasPendingSentinel(const xenos::xe_gpu_depth_sample_counts* report) {
    if (!report) {
      return false;
    }
    constexpr uint32_t kSentinelLE = 0xEDFEFFFFu;
    constexpr uint32_t kSentinelBE = 0xFFFFFEEDu;
    return report->ZPass_A == kSentinelLE || report->ZPass_A == kSentinelBE ||
           report->ZFail_A == kSentinelLE || report->ZFail_A == kSentinelBE;
  }

  // The pending state guest D3D gives an END record when it issues a query:
  // both ZPass lanes hold 0xFFFFFEED as the guest stores it (big-endian), the
  // other lanes are untouched. Written as one aligned 64-bit store.
  static void WritePendingSentinel(xenos::xe_gpu_depth_sample_counts* report) {
    if (!report) {
      return;
    }
    static constexpr uint8_t kLane[4] = {0xFF, 0xFF, 0xFE, 0xED};
    uint8_t pair[8];
    std::memcpy(pair, kLane, 4);
    std::memcpy(pair + 4, kLane, 4);
    uint64_t zpass_pair;
    std::memcpy(&zpass_pair, pair, sizeof(zpass_pair));
    auto* zpass_target = reinterpret_cast<uint64_t*>(&report->ZPass_A);
    if (!(reinterpret_cast<uintptr_t>(zpass_target) &
          (std::atomic_ref<uint64_t>::required_alignment - 1))) {
      std::atomic_ref<uint64_t>(*zpass_target).store(zpass_pair, std::memory_order_release);
    } else {
      std::memcpy(&report->ZPass_A, pair, sizeof(pair));
    }
  }
  static void WriteSampleCount(xenos::xe_gpu_depth_sample_counts* report,
                               uint32_t sample_count) {
    if (!report) {
      return;
    }
    std::memset(report, 0, sizeof(*report));
    report->Total_A = sample_count;
    report->ZPass_A = sample_count;
  }

  static uint32_t ClampSampleCount(uint64_t sample_count) {
    return sample_count > uint64_t(UINT32_MAX) ? UINT32_MAX
                                               : uint32_t(sample_count);
  }

  static void WriteReportDelta(xenos::xe_gpu_depth_sample_counts* begin_report,
                               xenos::xe_gpu_depth_sample_counts* end_report,
                               uint32_t begin_value, uint64_t delta_value) {
    const uint32_t delta = ClampSampleCount(delta_value);
    const uint32_t end_value =
        begin_value > UINT32_MAX - delta ? UINT32_MAX : begin_value + delta;
    if (begin_report && begin_report != end_report) {
      WriteSampleCount(begin_report, begin_value);
    }
    WriteSampleCount(end_report, end_value);
  }

  // Same final bytes as WriteReportDelta, for a report the guest may be
  // polling concurrently. Guest D3D treats the END record as pending while
  // both ZPass lanes hold the sentinel, so every other lane (and the BEGIN
  // record) is final before the ZPass pair is replaced by one aligned 64-bit
  // release store. The guest never observes a completed report whose other
  // lanes still belong to an earlier state.
  static void PublishReportDelta(xenos::xe_gpu_depth_sample_counts* begin_report,
                                 xenos::xe_gpu_depth_sample_counts* end_report,
                                 uint32_t begin_value, uint64_t delta_value) {
    const uint32_t delta = ClampSampleCount(delta_value);
    const uint32_t end_value =
        begin_value > UINT32_MAX - delta ? UINT32_MAX : begin_value + delta;
    if (begin_report && begin_report != end_report) {
      WriteSampleCount(begin_report, begin_value);
    }
    if (!end_report) {
      return;
    }
    xenos::xe_gpu_depth_sample_counts final_report{};
    WriteSampleCount(&final_report, end_value);
    end_report->Total_A = final_report.Total_A;
    end_report->Total_B = final_report.Total_B;
    end_report->ZFail_A = final_report.ZFail_A;
    end_report->ZFail_B = final_report.ZFail_B;
    end_report->StencilFail_A = final_report.StencilFail_A;
    end_report->StencilFail_B = final_report.StencilFail_B;
    uint64_t zpass_pair;
    std::memcpy(&zpass_pair, &final_report.ZPass_A, sizeof(zpass_pair));
    auto* zpass_target = reinterpret_cast<uint64_t*>(&end_report->ZPass_A);
    if (!(reinterpret_cast<uintptr_t>(zpass_target) &
          (std::atomic_ref<uint64_t>::required_alignment - 1))) {
      std::atomic_ref<uint64_t>(*zpass_target)
          .store(zpass_pair, std::memory_order_release);
    } else {
      // Records are 0x20-aligned in guest memory, so this is unreachable for
      // real reports; keep the lanes correct for any other caller.
      std::atomic_thread_fence(std::memory_order_release);
      end_report->ZPass_B = final_report.ZPass_B;
      end_report->ZPass_A = final_report.ZPass_A;
    }
  }
};

// Host APIs require occlusion queries to be split at command-list / render-pass
// boundaries, while the Xenos report is one logical lifetime. Keeping this
// accumulator independent of a host graphics API makes the no-lost-segment
// invariant directly regression-testable.
struct XenosZPDReportAccumulator {
  uint32_t slot_base = 0;
  uint32_t begin_record = 0;
  uint32_t end_record = 0;
  uint32_t begin_value = 0;
  uint64_t accumulated_samples = 0;
  bool valid = false;

  bool Begin(uint32_t report_address, uint32_t running_value) {
    if (!XenosZPDReport::IsBeginRecord(report_address)) {
      return false;
    }
    slot_base = XenosZPDReport::GetSlotBase(report_address);
    begin_record = XenosZPDReport::GetBeginRecordBase(slot_base);
    end_record = XenosZPDReport::GetEndRecordBase(slot_base);
    begin_value = running_value;
    accumulated_samples = 0;
    valid = true;
    return true;
  }

  bool CanEnd(uint32_t report_address) const {
    return valid && XenosZPDReport::IsEndRecord(report_address) &&
           XenosZPDReport::GetRecordBase(report_address) == end_record;
  }

  void AccumulateSegment(uint64_t samples) {
    accumulated_samples = samples > UINT64_MAX - accumulated_samples
                              ? UINT64_MAX
                              : accumulated_samples + samples;
  }

  void Reset() { *this = {}; }
};

// Xenos writes a ZPD report when the GPU reaches the event, not when the
// command processor reads it, and guest D3D polls for it without blocking:
// GetData reports "not ready" while both END ZPass lanes still hold the
// 0xFFFFFEED sentinel written at issue time. The Darkness consumes its
// occlusion queries only through such polls, from a five-entry ring. So the
// command processor does not wait for the host GPU at guest END. The exact
// accumulated result is published once every host segment of the report has
// completed, in END order. Callers await a specific report only where a later
// command-processor action on the same guest memory must observe it first.
struct XenosZPDDeferredReports {
  struct Segment {
    uint32_t host_index = UINT32_MAX;
    uint32_t scale_area = 1;
    uint64_t submission = 0;
    uint64_t report_id = 0;
  };
  struct EndedReport {
    XenosZPDReportAccumulator report;
    uint64_t id = 0;
    uint64_t last_submission = 0;
    uint32_t pending_segments = 0;
    uint64_t tag = 0;  // caller-defined, e.g. the guest swap at END
    // The guest began a new lifetime of this report's slot before the result
    // was published: the result belongs to a query the guest has reissued and
    // must never reach guest memory (see SupersedeSlot).
    bool superseded = false;
  };

  // Host segments in submission order, including those of the open report.
  std::deque<Segment> segments;
  // Reports whose guest END has been processed, in END order.
  std::deque<EndedReport> ended;
  uint64_t open_id = 0;
  uint64_t open_last_submission = 0;
  uint32_t open_pending_segments = 0;
  uint64_t next_id = 1;

  void Open() {
    open_id = next_id++;
    open_last_submission = 0;
    open_pending_segments = 0;
  }

  void AddSegment(uint32_t host_index, uint32_t scale_area,
                  uint64_t submission) {
    segments.push_back({host_index, scale_area, submission, open_id});
    ++open_pending_segments;
    open_last_submission = std::max(open_last_submission, submission);
  }

  void End(const XenosZPDReportAccumulator& report, uint64_t tag = 0) {
    ended.push_back(
        {report, open_id, open_last_submission, open_pending_segments, tag});
    open_id = 0;
    open_last_submission = 0;
    open_pending_segments = 0;
  }

  // Forget the open report without publishing it. Its segments still retire
  // (their host indices must not be reused early) but accumulate nowhere.
  void DiscardOpen() {
    open_id = 0;
    open_last_submission = 0;
    open_pending_segments = 0;
  }

  void Clear() { *this = {}; }

  bool empty() const { return segments.empty() && ended.empty(); }

  // Accumulates every segment whose submission has completed into its report,
  // then publishes the leading ended reports that have no segment in flight.
  template <typename ReadSamples, typename Publish>
  void Retire(uint64_t completed_submission,
              XenosZPDReportAccumulator& open_report,
              ReadSamples&& read_samples, Publish&& publish) {
    while (!segments.empty() &&
           segments.front().submission <= completed_submission) {
      const Segment segment = segments.front();
      segments.pop_front();
      const uint64_t samples = read_samples(segment);
      if (open_id && segment.report_id == open_id) {
        open_report.AccumulateSegment(samples);
        if (open_pending_segments) {
          --open_pending_segments;
        }
        continue;
      }
      for (EndedReport& ended_report : ended) {
        if (ended_report.id == segment.report_id) {
          ended_report.report.AccumulateSegment(samples);
          if (ended_report.pending_segments) {
            --ended_report.pending_segments;
          }
          break;
        }
      }
    }
    while (!ended.empty() && !ended.front().pending_segments) {
      // A superseded result retires silently (its host indices are free now).
      if (!ended.front().superseded) {
        publish(ended.front());
      }
      ended.pop_front();
    }
  }

  // A new BEGIN of the guest's report in this slot: every still unpublished
  // result of an earlier lifetime of the slot belongs to a query the guest has
  // reissued. On Xenos that result reached memory when the GPU passed its END,
  // before this BEGIN; published now it would land on the new lifetime (over
  // the sentinel the guest wrote when issuing it, next to this BEGIN's zeroed
  // record), and the guest would read an older query's END against the new
  // BEGIN. Returns the number of results dropped.
  uint32_t SupersedeSlot(uint32_t slot_base) {
    uint32_t superseded = 0;
    for (EndedReport& ended_report : ended) {
      if (!ended_report.superseded &&
          SlotOverlaps(ended_report.report.slot_base, slot_base,
                       XenosZPDReport::kSlotSizeBytes)) {
        ended_report.superseded = true;
        ++superseded;
      }
    }
    return superseded;
  }

  // Submission that must complete before every ended report overlapping the
  // guest range is published, or 0 if none. Reports publish in END order, so
  // this includes the segments of every earlier ended report.
  uint64_t AwaitSubmissionForRange(uint32_t address, uint32_t bytes) const {
    uint64_t await_submission = 0;
    uint64_t preceding_submission = 0;
    for (const EndedReport& ended_report : ended) {
      preceding_submission =
          std::max(preceding_submission, ended_report.last_submission);
      if (!ended_report.superseded &&
          SlotOverlaps(ended_report.report.slot_base, address, bytes)) {
        await_submission = preceding_submission;
      }
    }
    return await_submission;
  }

  // Submission that must complete before every report ended with a tag at or
  // below `tag_limit` is published (with every earlier report, in END order),
  // or 0 if none.
  uint64_t AwaitSubmissionForTag(uint64_t tag_limit) const {
    uint64_t await_submission = 0;
    uint64_t preceding_submission = 0;
    for (const EndedReport& ended_report : ended) {
      preceding_submission =
          std::max(preceding_submission, ended_report.last_submission);
      if (!ended_report.superseded && ended_report.tag <= tag_limit) {
        await_submission = preceding_submission;
      }
    }
    return await_submission;
  }

  bool HasEndedReportInRange(uint32_t address, uint32_t bytes) const {
    for (const EndedReport& ended_report : ended) {
      if (!ended_report.superseded &&
          SlotOverlaps(ended_report.report.slot_base, address, bytes)) {
        return true;
      }
    }
    return false;
  }

  // Compares GPU physical addresses, as the command processor translates them.
  static bool SlotOverlaps(uint32_t slot_base, uint32_t address,
                           uint32_t bytes) {
    constexpr uint32_t kPhysicalAddressMask = 0x1FFFFFFFu;
    const uint64_t slot = slot_base & kPhysicalAddressMask;
    const uint64_t begin = address & kPhysicalAddressMask;
    const uint64_t end = begin + std::max(bytes, 1u);
    return slot < end && begin < slot + XenosZPDReport::kSlotSizeBytes;
  }

  // Submission that must complete before a host query index can be reused,
  // or 0 if no segment in flight owns it.
  uint64_t AwaitSubmissionForHostIndex(uint32_t host_index) const {
    uint64_t await_submission = 0;
    for (const Segment& segment : segments) {
      if (segment.host_index == host_index) {
        await_submission = std::max(await_submission, segment.submission);
      }
    }
    return await_submission;
  }

  bool HasSegmentInSubmission(uint64_t submission) const {
    return !segments.empty() && segments.back().submission >= submission;
  }
};

}  // namespace rex::graphics
