/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MT8183_P1_BQUEUE_H
#define MT8183_P1_BQUEUE_H
#include "mt8183_p1_stream.h"

/* Bounded transport state only. SAME external HW lock for every call, input
 * and output must not alias q. No MMIO/IPI/allocation/threads or DMA ownership.
 * Fresh physical guard and pure accepted checkpoints remain adapter duties. */
#define BQ_JOBS 12U
#define BQ_SLOTS 3U
#define BQ_CQ_STRIDE 0x640U
#define BQ_COMPOSER_BYTES 0x200000U
#define BQ_IRQ_SOF (1U << 12)
#define BQ_IRQ_DONE (1U << 30)
#define BQ_IRQ_ERROR 0x23ff81f0U

enum bq_job_state { BQ_FREE, BQ_RESERVED, BQ_SENT, BQ_COMPOSED,
                    BQ_ARMING, BQ_ARMED, BQ_ACTIVE, BQ_DONE };
enum bq_slot_state { BQ_CQ_FREE, BQ_CQ_RESERVED, BQ_CQ_WRITING, BQ_CQ_READY,
                     BQ_CQ_ARMING, BQ_CQ_ARMED, BQ_CQ_APPLIED };
struct bq_job {
    struct dps_mapping map;
    enum bq_job_state state;
    bool send_returned, ack, cancelled, fw_unknown, retired;
    int send_errno;
    u32 ack_channel;
    u64 sof_ns, done_ns, retired_phase, retired_ns;
    struct dps_key retired_by;
};
struct bq_slot { struct dps_key owner; u64 generation; enum bq_slot_state state; };
struct bq_send_token { u64 serial; struct dps_mapping map; };
struct bq_arm_token { u64 serial; struct dps_mapping map; u32 iova; };
struct bq_guard { struct dps_observation observation; u64 cq_iova; };
struct bq_irq_action {
    bool done, sof, arm, terminal, cancelled;
    struct dps_key done_key, sof_key;
    struct bq_arm_token token;
};
struct bq_queue {
    bool initialized, published, stopping, tail, stopped;
    int error;
    u64 epoch, limit, next_logical, next_submit, serial, last_event_ns;
    u64 send_commits, acks, completions, sofs, dones, cq_arms_completed;
    u32 last_sof, last_done, composer_iova;
    struct dps_key active;
    struct bq_send_token sender;
    struct bq_arm_token arm;
    struct bq_job jobs[BQ_JOBS];
    struct bq_slot slots[BQ_SLOTS];
};

/* init requires an all-zero NEW allocation. limit=0 or 6..MAX; finite tail
 * entry is separate and cannot turn starvation into terminal success. */
int bq_init(struct bq_queue *, u64 epoch, u64 limit, u64 composer_iova, u64 bytes);
int bq_publish(struct bq_queue *);
int bq_hold(struct bq_queue *, int error);
void bq_stop(struct bq_queue *);
const struct bq_job *bq_lookup(const struct bq_queue *, const struct dps_key *);
/* guard NULL only for initial FRAME1..3 with inputs off externally verified.
 * Call after dps_reserve under the same lock; any error then holds BOTH owners. */
int bq_reserve(struct bq_queue *, const struct dps_mapping *, const struct bq_guard *);
int bq_send_commit(struct bq_queue *, const struct dps_key *, struct bq_send_token *);
int bq_send_result(struct bq_queue *, const struct bq_send_token *, int result);
/* Raw FRAME_ACK decoder; exact wire identity within the current_frame epoch/token.
 * Real callback lifetime/old epoch drain cannot be proved by wire bytes. */
int bq_ack(struct bq_queue *, u32 channel, const void *bytes, size_t len,
           const struct bq_guard *);
/* Successful proof while a completed sender token is still pinned is legal.
 * End once AFTER pure completion (including matching negative completion).
 * On cancellation caller must join the sender; no ACK is required to drain
 * its HOST token. Unknown FW references stay in job until positive stop. */
int bq_send_end(struct bq_queue *, const struct bq_send_token *, bool sender_drained);
int bq_make_proof(const struct bq_queue *, const struct dps_key *retired,
                  const struct bq_send_token *completion, const struct bq_guard *,
                  struct dps_queue_proof *);
/* Plan/finish remain in ONE critical section. Adapter checks fresh MMIO and
 * pure accepted current_frame/next before writing. Plan pins exact arm identity;
 * finish(error) holds without rolling back an attempted action. */
int bq_irq(struct bq_queue *, u32 status, u32 sequence, u64 time_ns,
           struct bq_irq_action *);
int bq_arm_finish(struct bq_queue *, const struct bq_arm_token *, int result);
int bq_enter_tail(struct bq_queue *, const struct bq_guard *);
bool bq_terminal(const struct bq_queue *);
bool bq_refs_zero(const struct bq_queue *, const struct dps_key *);
/* Pair preflight/record journal/dps_reclaim first, then forget under same lock.
 * journal=true is caller evidence, not proof produced by this library. */
int bq_forget(struct bq_queue *, const struct dps_key *, bool journal_captured);
/* Caller proves producer/IRQ/IPI/worker/token drain + fresh positive CAM gate.
 * No all-zero reset/reopen API: reuse requires a genuinely new session object. */
int bq_finish_stop(struct bq_queue *, bool positive_gate, bool external_drained);
#endif
