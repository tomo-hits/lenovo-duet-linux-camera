/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DUET_P1_ADAPTER_H
#define DUET_P1_ADAPTER_H
#include "duet_p1_bqueue.h"

/* External SAME HW lock for ALL calls. No I/O, callbacks, time acquisition,
 * allocation, wait, payload access, or third frame-state array. Inputs/outputs
 * must not alias either owner or adapter. Objects outlive every token. */
struct dpa_layout { u64 image_iova, span_stride, allocation_bytes; };
/* Caller freshly reads SEQ -> IMGO -> CQ -> SEQ and monotonic time. RAW_STATUS
 * is read-clear and MUST only be read by the external IRQ owner, never here. */
struct dpa_sample { u32 sequence_before, sequence_after; u64 imgo_iova, cq_iova, time_ns; };
struct dpa_adapter {
    struct dps_stream *stream;
    struct bq_queue *queue;
    struct dpa_layout layout;
    bool bound, inputs_live;
};

/* Bind already initialized owners after dps_streamon_guard and before either
 * start/publication. Queue epoch/limit must equal pure; allocation is merely a
 * range contract here, not an allocation or mapping established by this code. */
int dpa_bind(struct dpa_adapter *, struct dps_stream *, struct bq_queue *, const struct dpa_layout *);
int dpa_start_begin(struct dpa_adapter *);
int dpa_inputs_begin(struct dpa_adapter *);
int dpa_start_commit(struct dpa_adapter *);
void dpa_start_abort(struct dpa_adapter *, int error);
int dpa_hold(struct dpa_adapter *, int error);
void dpa_stop(struct dpa_adapter *);
int dpa_inputs_stopped(struct dpa_adapter *, bool sensor_and_seninf_idle);
int dpa_iova(const struct dpa_adapter *, const struct dps_mapping *, u32 *);
int dpa_observe(const struct dpa_adapter *, const struct dpa_sample *, struct bq_guard *);
/* Caller encodes/validates the wire packet using next_map BEFORE prepare.
 * Same-lock prepare reserves both owners and commits the one sender token.
 * It never invokes IPI. Failed partial reservation drains pure pending and
 * holds both owners with no rollback; no caller token is emitted on error. */
int dpa_next_map(const struct dpa_adapter *, struct dps_mapping *, u32 *iova);
int dpa_send_prepare(struct dpa_adapter *, const struct dpa_sample *, struct bq_send_token *, u32 *iova);
int dpa_send_result(struct dpa_adapter *, const struct bq_send_token *, int result);
int dpa_ack(struct dpa_adapter *, u32 channel, const void *, size_t, const struct dpa_sample *);
int dpa_send_complete(struct dpa_adapter *, const struct bq_send_token *,
                       const struct dpa_sample *, int wait_result, bool sender_drained);
int dpa_phase_begin(struct dpa_adapter *, const struct dpa_sample *);
int dpa_read_begin(struct dpa_adapter *, const struct dps_key *, enum dps_read_kind,
                   const struct dpa_sample *, struct dps_read_token *);
int dpa_read_end(struct dpa_adapter *, const struct dps_read_token *, const struct dpa_sample *,
                 int result, size_t bytes, bool match);
int dpa_publish_begin(struct dpa_adapter *, const struct dps_key *, struct dps_publish_token *);
int dpa_publish_end(struct dpa_adapter *, const struct dps_publish_token *, int result,
                    size_t bytes, struct dps_return_token *);
int dpa_return_end(struct dpa_adapter *, const struct dps_return_token *);
int dpa_reclaim(struct dpa_adapter *, const struct dps_key *, bool journal_captured);
/* IRQ plan and finish stay within ONE critical section; real MMIO is between
 * them. Plan rejects an unaccepted pure next/current_frame BEFORE caller can write.
 * Finish MUST run for an emitted arm action, even if write/after-sample fails. */
int dpa_irq_plan(struct dpa_adapter *, u32 raw_status, const struct dpa_sample *, struct bq_irq_action *);
int dpa_irq_finish(struct dpa_adapter *, const struct bq_irq_action *, const struct dpa_sample *, int write_result);
int dpa_enter_tail(struct dpa_adapter *, const struct dpa_sample *);
bool dpa_terminal(const struct dpa_adapter *);
/* Outside: inputoff retaining CAM PM/SCP/memory, start/sender/read/publisher
 * join, buffer returns, IRQ/IPI/SCP drain and positive gate. Save all numeric
 * journal/tail diagnostics BEFORE finish. False gate retains both owners. */
int dpa_stop_return_one(struct dpa_adapter *, struct dps_return_token *);
int dpa_finish_stop(struct dpa_adapter *, bool positive_gate, bool external_drained, bool diagnostics_saved);
#endif
