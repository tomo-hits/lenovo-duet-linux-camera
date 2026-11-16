/* SPDX-License-Identifier: GPL-2.0-only */
/* Only protocol providers are simulated; read_image/publication are extracted. */
#define MT8183_P1_LOG_FRAMES 12U
#define DPS_MAX_FW_SEQUENCE 1000U
#define DPS_READ_COPY 1
#define DPS_READ_COMPARE 2
#define DPS_RETURN_DONE 1
struct dps_key { u64 logical; };
struct dps_mapping { struct dps_key key; unsigned int span; };
struct dps_read_token { int unused; };
struct dps_publish_token { int unused; };
struct dps_ticket { unsigned int index; };
struct dps_return_token { int kind; struct dps_ticket ticket; };
struct dpa_sample { u64 time_ns; };
struct dpa_adapter { int unused; };
struct dps_stream { u64 epoch; };
struct mt8183_p1_hw {
    spinlock_t lock;
    struct dps_stream stream;
    struct dpa_adapter adapter;
    struct mt8183_p1_frame_log frames[MT8183_P1_LOG_FRAMES];
    struct { unsigned int archived_frames; } observed;
    void *output_cpu, *copy_cpu, *compare_cpu;
    size_t frame_bytes, frame_stride;
    bool copy_pending, publish_pending;
    int error;
    int (*deliver)(void *, const u8 *, size_t, u64, u64);
    void *deliver_context;
};
static int protocol_error, reclaim_calls, publish_calls, publish_result;
static u64 clock_ns;
static u64 ktime_get_ns(void) { return ++clock_ns; }
static void dma_rmb(void) { }
static int hold_locked(struct mt8183_p1_hw *hw, int error)
{ assert(hw->lock); hw->error = error; return error; }
static void sample_locked(struct mt8183_p1_hw *hw, struct dpa_sample *sample)
{ assert(hw->lock); sample->time_ns = ktime_get_ns(); }
static void dps_mapping(u64 epoch, u64 logical, struct dps_mapping *mapping)
{ (void)epoch; mapping->key.logical = logical; mapping->span = 0; }
static int dpa_read_begin(struct dpa_adapter *adapter, const struct dps_key *key,
                         int kind, struct dpa_sample *sample, struct dps_read_token *token)
{ (void)adapter; (void)key; (void)kind; (void)sample; (void)token; return protocol_error; }
static int dpa_read_end(struct dpa_adapter *adapter, struct dps_read_token *token,
                       struct dpa_sample *sample, int result, size_t bytes, bool match)
{
    (void)adapter; (void)token; (void)sample;
    assert(bytes == 5 && result == 0);
    return match ? protocol_error : -EPROTO;
}
static int dpa_publish_begin(struct dpa_adapter *adapter, const struct dps_key *key,
                             struct dps_publish_token *token)
{ (void)adapter; (void)key; (void)token; return protocol_error; }
static int dpa_publish_end(struct dpa_adapter *adapter, struct dps_publish_token *token,
                          int result, size_t bytes, struct dps_return_token *returned)
{
    (void)adapter; (void)token;
    publish_calls++; publish_result = result;
    assert(bytes == (result ? 0U : 5U));
    returned->kind = DPS_RETURN_DONE; returned->ticket.index = 0;
    return result;
}
static int dpa_return_end(struct dpa_adapter *adapter, struct dps_return_token *returned)
{ (void)adapter; (void)returned; return 0; }
static int dps_buffer_ticket(struct dps_stream *stream, unsigned int index, struct dps_ticket *ticket)
{ (void)stream; ticket->index = index; return 0; }
static int dps_buffer_arrive(struct dps_stream *stream, struct dps_ticket *ticket,
                            struct dps_return_token *returned)
{ (void)stream; (void)ticket; (void)returned; return 0; }
static int dpa_reclaim(struct dpa_adapter *adapter, const struct dps_key *key, bool complete)
{ (void)adapter; (void)key; assert(complete); reclaim_calls++; return 0; }
