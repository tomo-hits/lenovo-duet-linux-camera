/* Modified for the MT8183 camera release on 2026-10-05.
 * See docs/MODIFICATIONS.json in the source-kit root; original licenses retained.
 */
/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/string.h>
#include "mt8183_p1_stream.h"

int dps_checked_next(u64 current_frame, u64 *next)
{
    if (!next) return -EINVAL;
    if (current_frame == ~(u64)0) return -EOVERFLOW;
    *next = current_frame + 1;
    return 0;
}
bool dps_key_equal(const struct dps_key *a, const struct dps_key *b)
{
    return a && b && a->epoch == b->epoch && a->logical == b->logical && a->fw == b->fw;
}
static bool ticket_equal(const struct dps_ticket *a, const struct dps_ticket *b)
{
    return a->cycle == b->cycle && a->serial == b->serial &&
           a->cookie == b->cookie && a->index == b->index;
}
static bool mapping_equal(const struct dps_mapping *a, const struct dps_mapping *b)
{
    return dps_key_equal(&a->key, &b->key) && a->span == b->span &&
           a->cq == b->cq && a->span_generation == b->span_generation &&
           a->cq_generation == b->cq_generation;
}
int dps_mapping(u64 epoch, u64 logical, struct dps_mapping *m)
{
    if (!epoch || !m) return -EINVAL;
    if (logical >= 0xffffffffULL) return -EOVERFLOW;
    memset(m, 0, sizeof(*m));
    m->key.epoch = epoch; m->key.logical = logical; m->key.fw = (u32)(logical + 1);
    m->span = (u32)(logical % DPS_SPANS); m->span_generation = logical / DPS_SPANS + 1;
    m->cq = (u32)(logical % DPS_CQS); m->cq_generation = logical / DPS_CQS + 1;
    return 0;
}
static bool live(const struct dps_stream *s)
{
    return s && s->initialized && !s->error &&
           (s->state == DPS_STARTING || s->state == DPS_RUNNING);
}
void dps_request_stop(struct dps_stream *s, int error)
{
    if (!s || !s->initialized) return;
    if (error && !s->error) s->error = error < 0 ? error : -EINVAL;
    if (s->state != DPS_STOPPED && s->state != DPS_ERROR_HELD) s->state = DPS_STOPPING;
}
int dps_hold(struct dps_stream *s, int error)
{
    if (!s || !s->initialized) return -EINVAL;
    dps_request_stop(s, error < 0 ? error : -EINVAL);
    return s->error;
}
static int fail(struct dps_stream *s, int error) { return dps_hold(s, error); }
static struct dps_record *record(struct dps_stream *s, const struct dps_key *key)
{
    u32 i;
    for (i = 0; i < DPS_RECORDS; i++)
        if (s->records[i].used && dps_key_equal(&s->records[i].map.key, key)) return &s->records[i];
    return NULL;
}
static struct dps_record *free_record(struct dps_stream *s)
{
    u32 i;
    for (i = 0; i < DPS_RECORDS; i++) if (!s->records[i].used) return &s->records[i];
    return NULL;
}
static struct dps_destination *destination(struct dps_stream *s, const struct dps_key *key)
{
    u32 i;
    for (i = 0; i < DPS_DESTINATIONS; i++) {
        struct dps_destination *d = &s->destinations[i];
        if ((d->state == DPS_DEST_RESERVED || d->state == DPS_DEST_PUBLISHING) &&
            dps_key_equal(&d->frame, key)) return d;
    }
    return NULL;
}
static int new_serial(struct dps_stream *s, u64 *serial)
{
    if (dps_checked_next(s->serial, serial)) return fail(s, -EOVERFLOW);
    s->serial = *serial;
    return 0;
}
static bool queue_ok(const struct dps_stream *s, const struct dps_queue_proof *q)
{
    return q && q->epoch == s->epoch && !q->stopping && !q->error && q->no_pending;
}
static bool phase_ok(struct dps_stream *s, const struct dps_observation *o, u64 phase)
{
    struct dps_mapping m;
    struct dps_record *r;
    struct dps_span *span;
    if (!o || phase < 2 || phase > DPS_MAX_FW_SEQUENCE ||
        o->epoch != s->epoch || !o->time_ns || o->time_ns < s->capture_observed_ns || !o->guard_valid ||
        o->phase_ordinal != phase || o->done_ordinal != phase - 1 ||
        o->sequence_before != phase || o->sequence_after != phase ||
        dps_mapping(s->epoch, phase - 1, &m) || !mapping_equal(&o->current_frame, &m)) return false;
    r = record(s, &m.key); span = &s->spans[m.span];
    return r && r->accepted && span->owned && dps_key_equal(&span->owner, &m.key) &&
           span->generation == m.span_generation;
}
static bool retired_ok(const struct dps_queue_proof *q, const struct dps_key *key)
{
    struct dps_mapping m;
    if (dps_mapping(key->epoch, key->logical, &m)) return false;
    return dps_key_equal(&q->retired_key, key) && q->retired_accepted &&
           q->retired_cq_slot == m.cq && q->retired_cq_generation == m.cq_generation &&
           q->retired_done && q->retired_unreferenced;
}
static bool internal_drained(const struct dps_stream *s)
{
    u32 i;
    if (s->start_inflight || s->pending || s->read.serial || s->publisher.serial) return false;
    for (i = 0; i < DPS_SPANS; i++) if (s->spans[i].read_refs) return false;
    for (i = 0; i < DPS_SHADOWS; i++)
        if (s->shadows[i].capture_refs || s->shadows[i].publisher_refs) return false;
    return true;
}
int dps_init(struct dps_stream *s)
{
    const unsigned char *bytes = (const unsigned char *)s;
    size_t i;
    if (!s) return -EINVAL;
    for (i = 0; i < sizeof(*s); i++) if (bytes[i]) return -EINVAL;
    s->frame_bytes = DPS_FRAME_BYTES;
    s->initialized = true; s->state = DPS_OPEN; s->cycle = 1;
    return 0;
}
static u32 occupancy(const struct dps_stream *s)
{
    u32 i, count = 0;
    for (i = 0; i < DPS_DESTINATIONS; i++) if (s->destinations[i].state != DPS_DEST_FREE) count++;
    for (i = 0; i < DPS_RETURNS; i++) if (s->returns[i].used) count++;
    return count;
}
int dps_buffer_ticket(struct dps_stream *s, u32 index, struct dps_ticket *out)
{
    struct dps_destination *d;
    u64 serial, cookie;
    if (!s || !s->initialized || !out || index >= DPS_DESTINATIONS) return -EINVAL;
    if (s->state != DPS_OPEN && s->state != DPS_STARTING && s->state != DPS_RUNNING)
        return s->state == DPS_ERROR_HELD ? -EIO : -EBUSY;
    d = &s->destinations[index];
    if (d->state != DPS_DEST_FREE) return -EBUSY;
    if (occupancy(s) >= DPS_RETURNS) return -EAGAIN;
    if (dps_checked_next(s->cookies[index], &cookie)) return fail(s, -EOVERFLOW);
    if (new_serial(s, &serial)) return s->error;
    s->cookies[index] = cookie;
    memset(d, 0, sizeof(*d));
    d->state = DPS_DEST_TICKET;
    d->ticket = (struct dps_ticket){ .cycle = s->cycle, .serial = serial, .cookie = cookie, .index = index };
    *out = d->ticket;
    return 0;
}
int dps_buffer_ticket_cancel(struct dps_stream *s, const struct dps_ticket *t)
{
    struct dps_destination *d;
    if (!s || !s->initialized || !t || t->index >= DPS_DESTINATIONS) return -EINVAL;
    d = &s->destinations[t->index];
    if (d->state != DPS_DEST_TICKET || !ticket_equal(&d->ticket, t)) return fail(s, -ESTALE);
    memset(d, 0, sizeof(*d));
    return 0;
}
static int commit_return(struct dps_stream *s, struct dps_destination *d,
                         enum dps_return_kind kind, struct dps_return_token *out)
{
    u32 i;
    /* Each admitted ticket reserved return capacity. This path must not fail
     * for a valid object, including late callbacks after the last stop drain. */
    for (i = 0; i < DPS_RETURNS; i++) if (!s->returns[i].used) {
        struct dps_return_token token = { .ticket = d->ticket, .frame = d->frame, .kind = kind };
        s->returns[i].used = true; s->returns[i].token = token;
        memset(d, 0, sizeof(*d));
        *out = token;
        return 0;
    }
    return fail(s, -ENOSPC);
}
int dps_buffer_arrive(struct dps_stream *s, const struct dps_ticket *t, struct dps_return_token *out)
{
    struct dps_destination *d;
    int ret;
    if (!s || !s->initialized || !t || !out || t->index >= DPS_DESTINATIONS) return -EINVAL;
    memset(out, 0, sizeof(*out));
    d = &s->destinations[t->index];
    if (d->state != DPS_DEST_TICKET || !ticket_equal(&d->ticket, t) || t->cycle != s->cycle)
        return fail(s, -ESTALE);
    if (s->state == DPS_OPEN || s->state == DPS_STARTING || s->state == DPS_RUNNING) {
        d->state = DPS_DEST_QUEUED;
        d->epoch = s->state == DPS_OPEN ? 0 : s->epoch;
        d->order = t->serial;
        return 0;
    }
    ret = commit_return(s, d, s->start_committed ? DPS_RETURN_ERROR : DPS_RETURN_QUEUED, out);
    return ret ? ret : 1;
}
int dps_streamon_guard(struct dps_stream *s, u64 epoch, u64 limit)
{
    if (!s || !s->initialized) return -EINVAL;
    if (s->state == DPS_ERROR_HELD) return -EIO;
    if (s->state != DPS_OPEN) return -EBUSY;
    if (!epoch || epoch <= s->last_epoch) return -ESTALE;
    if (limit && (limit < 3 || limit > DPS_MAX_FW_SEQUENCE)) return -EINVAL;
    s->epoch = epoch; s->last_epoch = epoch; s->frame_limit = limit;
    s->state = DPS_STARTING; s->start_committed = false;
    return 0;
}
int dps_start_begin(struct dps_stream *s)
{
    u32 i, queued = 0;
    if (!live(s) || s->state != DPS_STARTING || s->start_inflight || s->next_logical)
        return fail(s, -EINVAL);
    for (i = 0; i < DPS_DESTINATIONS; i++)
        if (s->destinations[i].state == DPS_DEST_QUEUED) queued++;
    if (queued < 6) return fail(s, -ENOBUFS);
    for (i = 0; i < DPS_DESTINATIONS; i++)
        if (s->destinations[i].state == DPS_DEST_QUEUED) s->destinations[i].epoch = s->epoch;
    s->start_inflight = true;
    return 0;
}
int dps_start_commit(struct dps_stream *s)
{
    u32 i;
    if (!live(s) || s->state != DPS_STARTING || !s->start_inflight ||
        s->next_logical != 3 || s->pending) return fail(s, -EINVAL);
    for (i = 0; i < 3; i++) {
        struct dps_mapping m; struct dps_record *r;
        dps_mapping(s->epoch, i, &m); r = record(s, &m.key);
        if (!r || !r->accepted) return fail(s, -EPROTO);
    }
    s->start_inflight = false; s->start_committed = true; s->state = DPS_RUNNING;
    return 0;
}
void dps_start_abort(struct dps_stream *s, int error)
{
    if (!s || !s->initialized) return;
    dps_request_stop(s, error ? error : -ECANCELED);
    s->start_inflight = false;
}
int dps_phase_begin(struct dps_stream *s, const struct dps_queue_proof *q, const struct dps_observation *o)
{
    struct dps_mapping copy;
    struct dps_record *r;
    struct dps_shadow *shadow;
    struct dps_destination *d = NULL;
    u32 i;
    if (!live(s) || s->state != DPS_RUNNING || s->phase_active || s->pending || s->read.serial)
        return fail(s, -EBUSY);
    if ((s->frame_limit && s->next_logical >= s->frame_limit) ||
        s->next_logical >= DPS_MAX_FW_SEQUENCE) {
        dps_request_stop(s, 0); return -ENODATA;
    }
    if (s->next_logical < 3 || !queue_ok(s, q) || q->next_logical != s->next_logical ||
        !phase_ok(s, o, s->next_logical - 1)) return fail(s, -EPROTO);
    if (dps_mapping(s->epoch, s->next_logical - 3, &copy)) return fail(s, -EOVERFLOW);
    r = record(s, &copy.key); shadow = &s->shadows[copy.span];
    if (!r || !r->accepted || shadow->owned || !free_record(s)) return fail(s, -ENOSPC);
    for (i = 0; i < DPS_DESTINATIONS; i++) {
        struct dps_destination *candidate = &s->destinations[i];
        if (candidate->state == DPS_DEST_QUEUED && candidate->epoch == s->epoch &&
            (!d || candidate->order < d->order)) d = candidate;
    }
    if (!d) return fail(s, -ENOBUFS);
    d->state = DPS_DEST_RESERVED; d->frame = copy.key;
    memset(shadow, 0, sizeof(*shadow));
    shadow->owned = true; shadow->owner = copy.key; shadow->generation = copy.span_generation;
    s->phase_active = true; s->phase_ordinal = o->phase_ordinal; s->phase_new_logical = s->next_logical;
    s->capture_observed_ns = o->time_ns;
    return 0;
}
int dps_reserve(struct dps_stream *s, const struct dps_queue_proof *q,
                 const struct dps_observation *o, struct dps_mapping *out)
{
    struct dps_mapping m, oldcq, oldimage;
    struct dps_record *r;
    struct dps_span *span;
    u64 n;
    if (!live(s) || !out || s->pending || !queue_ok(s, q) || q->next_logical != s->next_logical)
        return fail(s, -EPROTO);
    n = s->next_logical;
    if (n >= DPS_MAX_FW_SEQUENCE || (s->frame_limit && n >= s->frame_limit)) return fail(s, -EOVERFLOW);
    if (dps_mapping(s->epoch, n, &m)) return fail(s, -EOVERFLOW);
    r = free_record(s); if (!r) return fail(s, -ENOSPC);
    span = &s->spans[m.span];
    if (n < 3) {
        if (s->state != DPS_STARTING || !s->start_inflight || o) return fail(s, -EPROTO);
    } else {
        struct dps_record *old;
        if (s->state != DPS_RUNNING || !s->phase_active || s->phase_new_logical != n ||
            !phase_ok(s, o, n - 1) || s->read.serial) return fail(s, -EPROTO);
        dps_mapping(s->epoch, n - 3, &oldcq); old = record(s, &oldcq.key);
        if (!old || !old->accepted || !retired_ok(q, &oldcq.key)) return fail(s, -EPROTO);
    }
    if (n < DPS_SPANS) {
        if (span->owned || span->read_refs) return fail(s, -EPROTO);
    } else {
        struct dps_shadow *shadow = &s->shadows[m.span];
        dps_mapping(s->epoch, n - DPS_SPANS, &oldimage);
        if (!span->owned || !dps_key_equal(&span->owner, &oldimage.key) ||
            span->generation != oldimage.span_generation || span->read_refs ||
            !shadow->owned || !dps_key_equal(&shadow->owner, &oldimage.key) ||
            shadow->generation != oldimage.span_generation || !shadow->copied ||
            !shadow->compared || !shadow->match || shadow->capture_refs || shadow->publisher_refs ||
            !shadow->compare_end_ns || shadow->compare_end_ns > o->time_ns) return fail(s, -EPROTO);
    }
    /* From here the old image owner is irrevocably replaced, even if the
     * subsequent real queue transaction or IPI never succeeds. */
    memset(r, 0, sizeof(*r)); r->used = true; r->map = m;
    span->owned = true; span->owner = m.key; span->generation = m.span_generation;
    s->pending = true; s->pending_key = m.key; s->next_logical++;
    if (n >= 3) s->capture_observed_ns = o->time_ns;
    *out = m;
    return 0;
}
int dps_mark_sent(struct dps_stream *s, const struct dps_key *key)
{
    struct dps_record *r;
    if (!live(s) || !key || !s->pending || !dps_key_equal(&s->pending_key, key)) return fail(s, -EPROTO);
    r = record(s, key);
    if (!r || r->send_attempted || r->accepted) return fail(s, -EPROTO);
    r->send_attempted = true;
    return 0;
}
int dps_send_complete(struct dps_stream *s, const struct dps_key *key,
                       const struct dps_queue_proof *q, const struct dps_observation *o, int result)
{
    struct dps_record *r;
    bool valid;
    if (!s || !s->initialized || !key || !s->pending || !dps_key_equal(&s->pending_key, key))
        return fail(s, -ESTALE);
    r = record(s, key);
    valid = r && r->send_attempted && live(s) && !result && queue_ok(s, q) &&
            q->next_logical == s->next_logical && q->completion_accepted &&
            dps_key_equal(&q->completion_key, key);
    if (valid && key->logical >= 3) valid = phase_ok(s, o, key->logical - 1);
    if (valid && key->logical < 3) valid = !o;
    s->pending = false; memset(&s->pending_key, 0, sizeof(s->pending_key));
    if (!valid) return fail(s, result < 0 ? result : -EPROTO);
    r->accepted = true;
    if (key->logical >= 3) s->capture_observed_ns = o->time_ns;
    return 0;
}
static bool read_guard(struct dps_stream *s, const struct dps_mapping *m, enum dps_read_kind kind,
                        const struct dps_queue_proof *q, const struct dps_observation *o)
{
    struct dps_span *span = &s->spans[m->span];
    u64 phase = m->key.logical + (kind == DPS_READ_COPY ? 2 : 5);
    u64 next = m->key.logical + (kind == DPS_READ_COPY ? 4 : 6);
    struct dps_record *r = record(s, &m->key);
    if (!live(s) || s->state != DPS_RUNNING || !s->phase_active || s->pending ||
        !r || !r->accepted || !queue_ok(s, q) || q->next_logical != next || s->next_logical != next ||
        !retired_ok(q, &m->key) || !phase_ok(s, o, phase) || s->phase_ordinal != phase ||
        !span->owned || !dps_key_equal(&span->owner, &m->key) || span->generation != m->span_generation)
        return false;
    if (kind == DPS_READ_COPY) {
        struct dps_mapping submitted; struct dps_record *newframe;
        dps_mapping(s->epoch, m->key.logical + 3, &submitted); newframe = record(s, &submitted.key);
        if (!newframe || !newframe->accepted) return false;
    }
    return true;
}
int dps_read_begin(struct dps_stream *s, const struct dps_key *key, enum dps_read_kind kind,
                    const struct dps_queue_proof *q, const struct dps_observation *o, struct dps_read_token *out)
{
    struct dps_record *r;
    struct dps_shadow *shadow;
    u64 serial;
    if (!s || !key || !out || (kind != DPS_READ_COPY && kind != DPS_READ_COMPARE)) return fail(s, -EINVAL);
    r = record(s, key);
    if (!r || s->read.serial || !read_guard(s, &r->map, kind, q, o)) return fail(s, -EPROTO);
    shadow = &s->shadows[r->map.span];
    if (!shadow->owned || !dps_key_equal(&shadow->owner, key) ||
        shadow->generation != r->map.span_generation || shadow->capture_refs || shadow->publisher_refs ||
        s->spans[r->map.span].read_refs ||
        (kind == DPS_READ_COPY && shadow->copied) ||
        (kind == DPS_READ_COMPARE && (!shadow->copied || shadow->compared || o->time_ns < shadow->copy_end_ns)))
        return fail(s, -EPROTO);
    if (new_serial(s, &serial)) return s->error;
    s->read = (struct dps_read_token){ .serial = serial, .frame = r->map, .kind = kind };
    s->read_begin_ns = o->time_ns;
    s->capture_observed_ns = o->time_ns;
    s->spans[r->map.span].read_refs = 1; shadow->capture_refs = 1;
    *out = s->read;
    return 0;
}
int dps_read_end(struct dps_stream *s, const struct dps_read_token *token,
                  const struct dps_queue_proof *q, const struct dps_observation *o,
                  int result, size_t bytes, bool match)
{
    struct dps_shadow *shadow;
    bool valid;
    if (!s || !token || !token->serial || token->serial != s->read.serial ||
        token->kind != s->read.kind || !mapping_equal(&token->frame, &s->read.frame)) return fail(s, -ESTALE);
    shadow = &s->shadows[token->frame.span];
    valid = !result && bytes == s->frame_bytes &&
            (token->kind == DPS_READ_COPY || match) &&
            read_guard(s, &token->frame, token->kind, q, o) && o->time_ns > s->read_begin_ns;
    /* Exact valid-token end always drains, including error/STOP/phase expiry. */
    s->spans[token->frame.span].read_refs = 0; shadow->capture_refs = 0;
    memset(&s->read, 0, sizeof(s->read)); s->read_begin_ns = 0;
    if (!valid) return fail(s, result < 0 ? result : -EPROTO);
    s->capture_observed_ns = o->time_ns;
    if (token->kind == DPS_READ_COPY) {
        shadow->copied = true; shadow->copy_end_ns = o->time_ns; s->phase_active = false;
    } else {
        shadow->compared = true; shadow->match = true; shadow->compare_end_ns = o->time_ns;
    }
    return 0;
}
int dps_publish_begin(struct dps_stream *s, const struct dps_key *key, struct dps_publish_token *out)
{
    struct dps_record *r, *newframe;
    struct dps_shadow *shadow;
    struct dps_destination *d;
    struct dps_span *span;
    struct dps_mapping replaced;
    u64 serial;
    if (!live(s) || s->state != DPS_RUNNING || !s->start_committed || s->publisher.serial || !key || !out)
        return fail(s, -EBUSY);
    r = record(s, key); d = destination(s, key);
    if (!r || !d || d->state != DPS_DEST_RESERVED) return fail(s, -EPROTO);
    shadow = &s->shadows[r->map.span]; span = &s->spans[r->map.span];
    if (dps_mapping(s->epoch, key->logical + 6, &replaced)) return fail(s, -EOVERFLOW);
    newframe = record(s, &replaced.key);
    if (!shadow->owned || !dps_key_equal(&shadow->owner, key) ||
        shadow->generation != r->map.span_generation || !shadow->copied ||
        !shadow->compared || !shadow->match || shadow->capture_refs || shadow->publisher_refs ||
        !newframe || !newframe->accepted || !span->owned || !dps_key_equal(&span->owner, &replaced.key) ||
        span->generation != replaced.span_generation) return fail(s, -EPROTO);
    if (new_serial(s, &serial)) return s->error;
    d->state = DPS_DEST_PUBLISHING; shadow->publisher_refs = 1;
    s->publisher = (struct dps_publish_token){ .serial = serial, .frame = *key, .destination = d->ticket };
    *out = s->publisher;
    return 0;
}
int dps_publish_finish(struct dps_stream *s, const struct dps_publish_token *token,
                        int result, size_t bytes, struct dps_return_token *out)
{
    struct dps_record *r;
    struct dps_destination *d;
    struct dps_shadow *shadow;
    bool done;
    int ret;
    if (!s || !token || !out || !token->serial || token->serial != s->publisher.serial ||
        !dps_key_equal(&token->frame, &s->publisher.frame) ||
        !ticket_equal(&token->destination, &s->publisher.destination)) return fail(s, -ESTALE);
    r = record(s, &token->frame); d = destination(s, &token->frame);
    if (!r || !d || d->state != DPS_DEST_PUBLISHING || !ticket_equal(&d->ticket, &token->destination))
        return fail(s, -EPROTO);
    shadow = &s->shadows[r->map.span];
    if (result || bytes != s->frame_bytes) fail(s, result < 0 ? result : -EPROTO);
    done = live(s) && s->state == DPS_RUNNING && s->start_committed;
    shadow->publisher_refs = 0;
    memset(&s->publisher, 0, sizeof(s->publisher));
    ret = commit_return(s, d, done ? DPS_RETURN_DONE : DPS_RETURN_ERROR, out);
    if (ret) return ret;
    /* CPU copy has ended and no further comparison needs this old shadow.
     * Its frame record remains pinned by the independent return token. */
    memset(shadow, 0, sizeof(*shadow));
    return 0;
}
int dps_return_end(struct dps_stream *s, const struct dps_return_token *token)
{
    u32 i;
    if (!s || !s->initialized || !token || token->kind == DPS_RETURN_NONE) return -EINVAL;
    for (i = 0; i < DPS_RETURNS; i++) {
        struct dps_return_slot *slot = &s->returns[i];
        if (slot->used && ticket_equal(&slot->token.ticket, &token->ticket) &&
            dps_key_equal(&slot->token.frame, &token->frame) && slot->token.kind == token->kind) {
            memset(slot, 0, sizeof(*slot));
            return 0;
        }
    }
    return fail(s, -ESTALE);
}
int dps_reclaim(struct dps_stream *s, const struct dps_key *key, bool queue_refs_zero, bool journal_captured)
{
    struct dps_record *r;
    u32 i;
    if (!s || !s->initialized || !key) return -EINVAL;
    r = record(s, key); if (!r) return -ENOENT;
    if (!queue_refs_zero || !journal_captured ||
        (s->pending && dps_key_equal(&s->pending_key, key))) return -EBUSY;
    for (i = 0; i < DPS_SPANS; i++)
        if (s->spans[i].owned && dps_key_equal(&s->spans[i].owner, key)) return -EBUSY;
    for (i = 0; i < DPS_SHADOWS; i++)
        if (s->shadows[i].owned && dps_key_equal(&s->shadows[i].owner, key)) return -EBUSY;
    for (i = 0; i < DPS_DESTINATIONS; i++)
        if (s->destinations[i].state != DPS_DEST_FREE && dps_key_equal(&s->destinations[i].frame, key)) return -EBUSY;
    for (i = 0; i < DPS_RETURNS; i++)
        if (s->returns[i].used && dps_key_equal(&s->returns[i].token.frame, key)) return -EBUSY;
    memset(r, 0, sizeof(*r));
    return 0;
}
int dps_stop_return_one(struct dps_stream *s, struct dps_return_token *out)
{
    u32 i;
    if (!s || !s->initialized || !out ||
        (s->state != DPS_STOPPING && s->state != DPS_ERROR_HELD && s->state != DPS_STOPPED)) return -EINVAL;
    if (!internal_drained(s)) return -EBUSY;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < DPS_DESTINATIONS; i++) {
        struct dps_destination *d = &s->destinations[i];
        int ret;
        if (d->state == DPS_DEST_FREE || d->state == DPS_DEST_TICKET) continue;
        if (d->state == DPS_DEST_PUBLISHING) return fail(s, -EPROTO);
        ret = commit_return(s, d, s->start_committed ? DPS_RETURN_ERROR : DPS_RETURN_QUEUED, out);
        return ret ? ret : 1;
    }
    return 0;
}
int dps_stop_finish(struct dps_stream *s, bool positive_gate, bool external_drained)
{
    u32 i;
    if (!s || !s->initialized || (s->state != DPS_STOPPING && s->state != DPS_ERROR_HELD)) return -EINVAL;
    if (!internal_drained(s) || !external_drained) return -EBUSY;
    for (i = 0; i < DPS_DESTINATIONS; i++)
        if (s->destinations[i].state != DPS_DEST_FREE && s->destinations[i].state != DPS_DEST_TICKET) return -EBUSY;
    for (i = 0; i < DPS_RETURNS; i++) if (s->returns[i].used) return -EBUSY;
    if (!positive_gate) { s->state = DPS_ERROR_HELD; if (!s->error) s->error = -EIO; return -EIO; }
    memset(s->records, 0, sizeof(s->records));
    memset(s->spans, 0, sizeof(s->spans)); memset(s->shadows, 0, sizeof(s->shadows));
    s->phase_active = false; s->state = DPS_STOPPED;
    return 0;
}
int dps_cycle_cancelled(struct dps_stream *s, bool core_cancelled, bool stop_work_drained)
{
    u32 i;
    u64 cycle;
    if (!s || !s->initialized) return -EINVAL;
    if (s->state == DPS_ERROR_HELD) return -EIO;
    if (s->state != DPS_STOPPED || !core_cancelled || !stop_work_drained || !internal_drained(s)) return -EBUSY;
    for (i = 0; i < DPS_DESTINATIONS; i++) if (s->destinations[i].state != DPS_DEST_FREE) return -EBUSY;
    for (i = 0; i < DPS_RETURNS; i++) if (s->returns[i].used) return -EBUSY;
    if (dps_checked_next(s->cycle, &cycle)) return fail(s, -EOVERFLOW);
    s->cycle = cycle; s->epoch = 0; s->error = 0; s->state = DPS_OPEN;
    s->next_logical = 0; s->frame_limit = 0; s->phase_ordinal = 0; s->phase_new_logical = 0;
    s->capture_observed_ns = 0;
    s->start_committed = false;
    return 0;
}
