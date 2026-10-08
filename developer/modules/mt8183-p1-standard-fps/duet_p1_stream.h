/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DUET_P1_STREAM_H
#define DUET_P1_STREAM_H
#include <linux/types.h>

/* Pure ownership model only: no kernel module, worker, queue, I/O or payload.
 * Every call requires the same external state lock. Caller-owned arguments
 * and outputs must not alias this object. Queue proofs are read-only views of
 * the ONE authoritative CQ/job queue; this helper never advances that queue.
 * A successful boolean proof is evidence supplied by the future HW adapter,
 * not a hardware guarantee established by this library. */
#define DPS_RECORDS 12U
#define DPS_SPANS 6U
#define DPS_SHADOWS 6U
#define DPS_CQS 3U
#define DPS_DESTINATIONS 8U
#define DPS_RETURNS 16U
#define DPS_FRAME_BYTES 2496960U
#define DPS_MAX_FW_SEQUENCE (0xffffffffU - 2U)

struct dps_key { u64 epoch, logical; u32 fw; };
struct dps_mapping { struct dps_key key; u64 span_generation, cq_generation; u32 span, cq; };
enum dps_cycle_state { DPS_OPEN, DPS_STARTING, DPS_RUNNING, DPS_STOPPING,
                      DPS_STOPPED, DPS_ERROR_HELD };
enum dps_destination_state { DPS_DEST_FREE, DPS_DEST_TICKET, DPS_DEST_QUEUED,
                             DPS_DEST_RESERVED, DPS_DEST_PUBLISHING };
enum dps_return_kind { DPS_RETURN_NONE, DPS_RETURN_DONE, DPS_RETURN_ERROR,
                       DPS_RETURN_QUEUED };
enum dps_read_kind { DPS_READ_NONE, DPS_READ_COPY, DPS_READ_COMPARE };

/* No pointer/index alone is an identity. A ticket is allocated in the QBUF
 * frontend BEFORE core acceptance; arrive() is the actual buf_queue callback.
 * Checked-cookie/return-capacity failure can therefore reject before driver
 * ownership. ticket_cancel() is used if core rejects before that callback. */
struct dps_ticket { u64 cycle, serial, cookie; u32 index; };
struct dps_return_token {
    struct dps_ticket ticket;
    struct dps_key frame;
    enum dps_return_kind kind;
};
struct dps_read_token {
    u64 serial;
    struct dps_mapping frame;
    enum dps_read_kind kind;
};
struct dps_publish_token { u64 serial; struct dps_key frame; struct dps_ticket destination; };

/* Adapter builds this from the authoritative queue while holding the SAME
 * lock. retired_* applies to retired_key, never to the new FRAME. New FRAME
 * send_complete additionally requires completion_accepted (ACK AND send0).
 * No helper copy of CQ READY/ARMED/APPLIED or SOF/DONE state is maintained. */
struct dps_queue_proof {
    u64 epoch, next_logical;
    struct dps_key retired_key, completion_key;
    u64 retired_cq_generation;
    u32 retired_cq_slot;
    bool no_pending, retired_accepted, retired_done, retired_unreferenced;
    bool completion_accepted, stopping;
    int error;
};
/* Phase is a one-based FW ordinal. The adapter must validate fresh double
 * sequence / actual IMGO / expected CQ base and translate IMGO to current_frame.
 * guard_valid is NOT satisfied by an ACK or a CPU barrier alone. */
struct dps_observation {
    u64 epoch, time_ns, phase_ordinal, done_ordinal;
    u32 sequence_before, sequence_after;
    struct dps_mapping current_frame;
    bool guard_valid;
};

struct dps_record {
    bool used, send_attempted, accepted;
    struct dps_mapping map;
};
struct dps_span { bool owned; struct dps_key owner; u64 generation; u32 read_refs; };
struct dps_shadow {
    bool owned, copied, compared, match;
    struct dps_key owner;
    u64 generation, copy_end_ns, compare_end_ns;
    u32 capture_refs, publisher_refs;
};
struct dps_destination {
    enum dps_destination_state state;
    struct dps_ticket ticket;
    struct dps_key frame;
    u64 epoch, order;
};
struct dps_return_slot { bool used; struct dps_return_token token; };
struct dps_stream {
    bool initialized, start_inflight, start_committed, phase_active;
    enum dps_cycle_state state;
    int error;
    u32 frame_bytes;
    u64 cycle, epoch, last_epoch, serial, next_logical, frame_limit;
    u64 phase_ordinal, phase_new_logical, read_begin_ns, capture_observed_ns;
    u64 cookies[DPS_DESTINATIONS];
    bool pending;
    struct dps_key pending_key;
    struct dps_read_token read;
    struct dps_publish_token publisher;
    struct dps_record records[DPS_RECORDS];
    struct dps_span spans[DPS_SPANS];
    struct dps_shadow shadows[DPS_SHADOWS];
    struct dps_destination destinations[DPS_DESTINATIONS];
    struct dps_return_slot returns[DPS_RETURNS];
};

int dps_checked_next(u64 current_frame, u64 *next);
bool dps_key_equal(const struct dps_key *, const struct dps_key *);
int dps_mapping(u64 epoch, u64 logical, struct dps_mapping *);
int dps_init(struct dps_stream *);
int dps_hold(struct dps_stream *, int error);
void dps_request_stop(struct dps_stream *, int error);
int dps_buffer_ticket(struct dps_stream *, u32 index, struct dps_ticket *);
int dps_buffer_ticket_cancel(struct dps_stream *, const struct dps_ticket *);
/* returns 0 if admitted, 1 with a committed immediate rejection return.
 * Late arrival after stop uses endpoint state only; no freed session pointer.
 * The caller executes one real buffer_done outside the lock, then return_end. */
int dps_buffer_arrive(struct dps_stream *, const struct dps_ticket *, struct dps_return_token *);
/* Frontend guard runs BEFORE core may deliver queued buffers. epoch must
 * strictly increase. limit=0 removes finite limit; otherwise >=3 frames.
 * FW u32 wrap is NEVER admitted, and two wire values remain as stop margin. */
int dps_streamon_guard(struct dps_stream *, u64 epoch, u64 frame_limit);
int dps_start_begin(struct dps_stream *);
int dps_start_commit(struct dps_stream *);
void dps_start_abort(struct dps_stream *, int error);
/* Preflight reserves M's CPU destination/shadow BEFORE any compare/reuse.
 * It also requires a free frame record. No waiting across the next SOF. */
int dps_phase_begin(struct dps_stream *, const struct dps_queue_proof *, const struct dps_observation *);
/* Same-lock transaction: encode FIRST, then this reserve -> real queue
 * recompose/reserve/SENT -> mark_sent, followed by a single process sender.
 * Any failure holds BOTH owners, with NO rollback/retry. A matching negative
 * send_complete drains pending even after STOP/error. */
int dps_reserve(struct dps_stream *, const struct dps_queue_proof *, const struct dps_observation *, struct dps_mapping *);
int dps_mark_sent(struct dps_stream *, const struct dps_key *);
int dps_send_complete(struct dps_stream *, const struct dps_key *, const struct dps_queue_proof *, const struct dps_observation *, int result);
/* One capture read lease (serial initial design), independently from the
 * CPU-only publisher. Valid end MUST run even on STOP/error and drain refs.
 * COPY at phase logical+2, COMPARE at phase logical+5. Full length required.
 * These retain the strict serial profile; its 30fps performance gate FAILED. */
int dps_read_begin(struct dps_stream *, const struct dps_key *, enum dps_read_kind,
                   const struct dps_queue_proof *, const struct dps_observation *, struct dps_read_token *);
int dps_read_end(struct dps_stream *, const struct dps_read_token *, const struct dps_queue_proof *,
                 const struct dps_observation *, int result, size_t bytes, bool match);
int dps_publish_begin(struct dps_stream *, const struct dps_key *, struct dps_publish_token *);
/* Commit is the return linearization point: detach index under lock, emit
 * buffer_done outside lock, then end. Immediate re-QBUF gets a new cookie;
 * old token end never writes that destination index. STOP after DONE commit
 * cannot convert or double-return the already committed token. */
int dps_publish_finish(struct dps_stream *, const struct dps_publish_token *, int result,
                        size_t bytes, struct dps_return_token *);
int dps_return_end(struct dps_stream *, const struct dps_return_token *);
/* Explicit reclaim: CQ/job queue and diagnostic journal remain external.
 * Returns -EBUSY without changing ownership when any actual reference exists.
 * It never guesses queue retirement from an image generation or an ACK. */
int dps_reclaim(struct dps_stream *, const struct dps_key *, bool queue_refs_zero, bool journal_captured);
/* Mock coordinator boundaries only: no worker is started or joined here.
 * stop_return_one requires start handoff + sender + capture/publisher drain;
 * 1=token, 0=no remaining driver-owned destination. Ticket-only callbacks may
 * arrive later; endpoint closed-cycle state persists through core cancel. */
int dps_stop_return_one(struct dps_stream *, struct dps_return_token *);
int dps_stop_finish(struct dps_stream *, bool positive_gate, bool external_drained);
int dps_cycle_cancelled(struct dps_stream *, bool core_cancelled, bool stop_work_drained);
#endif
