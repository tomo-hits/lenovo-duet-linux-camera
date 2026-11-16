/* SPDX-License-Identifier: GPL-2.0-only */
static const u8 raw[5] = { 1, 2, 3, 4, 5 };
static u8 pixels[8];
static int start_error, stop_error;
static bool synchronous_frame;
static int on(void *context)
{
    struct dcv_video *video = context;
    struct dcv_video_stats stats;
    dcv_get_stats(video, &stats);
    assert(stats.running && stats.stream_id > 0);
    assert(!stats.delivered && !stats.no_buffer && !stats.stopping && !stats.errors);
    if (synchronous_frame)
        assert(dcv_deliver(video, raw, sizeof(raw), 7, 123456) == 1);
    return start_error;
}
static int off(void *context)
{
    struct dcv_video *video = context;
    struct dcv_video_stats stats;
    dcv_get_stats(video, &stats);
    assert(!stats.running);
    /* A final producer callback racing ordinary STOP is benign. */
    assert(dcv_deliver(video, raw, sizeof(raw), 8, 123789) == 0);
    return stop_error;
}
static void fresh(struct dcv_video *video, struct dcv_buffer *buffer, bool imported)
{
    static struct dma_buf dma;
    memset(video, 0, sizeof(*video));
    memset(buffer, 0, sizeof(*buffer));
    INIT_LIST_HEAD(&video->buffers);
    INIT_LIST_HEAD(&buffer->link);
    video->width = 4; video->height = 1;
    video->queue.drv_priv = video;
    video->context = video; video->start = on; video->stop = off;
    buffer->vb.vb2_buf.vb2_queue = &video->queue;
    buffer->vb.vb2_buf.data = pixels;
    buffer->vb.vb2_buf.planes[0].dbuf = imported ? &dma : NULL;
    begin_error = end_error = unpack_error = pipeline_error = start_error = stop_error = 0;
    begin_calls = end_calls = unpack_calls = pipeline_stops = 0;
    synchronous_frame = false;
}
static struct dcv_video_stats snapshot(struct dcv_video *video)
{
    struct dcv_video_stats stats;
    dcv_get_stats(video, &stats);
    assert(video->buffers_lock == 0);
    return stats;
}
int main(void)
{
    struct dcv_video video;
    struct dcv_buffer buffer;
    struct dcv_video_stats stats;

    /* A delivery during STREAMON must see a fresh session and open admission. */
    fresh(&video, &buffer, true);
    queue_buffer(&buffer.vb.vb2_buf);
    synchronous_frame = true;
    assert(start(&video.queue, 1) == 0);
    stats = snapshot(&video);
    assert(stats.stream_id == 1 && stats.delivered == 1 && stats.total_delivered == 1);
    assert(buffer.vb.sequence == 7 && buffer.vb.vb2_buf.timestamp == 123456);
    assert(buffer.vb.vb2_buf.completed == VB2_BUF_STATE_DONE && buffer.vb.vb2_buf.payload == 8);
    assert(begin_calls == 1 && end_calls == 1 && unpack_calls == 1);

    /* Starvation keeps capture alive and can recover after requeue. */
    assert(dcv_deliver(&video, raw, sizeof(raw), 8, 124000) == 0);
    stats = snapshot(&video);
    assert(stats.no_buffer == 1 && !stats.stopping && !stats.errors && stats.total_dropped == 1);
    queue_buffer(&buffer.vb.vb2_buf);
    assert(dcv_deliver(&video, raw, sizeof(raw), 9, 124500) == 1);
    assert(buffer.vb.sequence == 9 && buffer.vb.vb2_buf.timestamp == 124500);
    assert(snapshot(&video).delivered == 2);

    /* Ordinary STOP's discard is distinct from starvation, and remains nonfatal. */
    stop(&video.queue);
    stats = snapshot(&video);
    assert(!video.blocked && !stats.running && stats.no_buffer == 1 && stats.stopping == 1);
    assert(stats.delivered == 2 && stats.total_dropped == 2 && pipeline_stops == 1);

    /* A second stream resets only session values, retaining lifetime totals. */
    synchronous_frame = false;
    assert(start(&video.queue, 0) == 0);
    stats = snapshot(&video);
    assert(stats.stream_id == 2 && !stats.delivered && !stats.no_buffer && !stats.stopping && !stats.errors);
    assert(stats.total_delivered == 2 && stats.total_dropped == 2);
    queue_buffer(&buffer.vb.vb2_buf);
    assert(dcv_deliver(&video, raw, sizeof(raw), 1, 99) == 1);
    stats = snapshot(&video);
    assert(stats.delivered == 1 && stats.total_delivered == 3);

    /* Invalid input does not consume a queued buffer. */
    queue_buffer(&buffer.vb.vb2_buf);
    assert(dcv_deliver(&video, NULL, sizeof(raw), 2, 100) == -EINVAL);
    assert(dcv_deliver(&video, raw, sizeof(raw) - 1, 2, 100) == -EINVAL);
    stats = snapshot(&video);
    assert(stats.errors == 2 && !list_empty(&video.buffers));
    assert(dcv_deliver(NULL, raw, sizeof(raw), 2, 100) == -EINVAL);

    /* Import synchronization errors return an ERROR buffer and preserve errno. */
    begin_error = -EACCES;
    int ends_before = end_calls, unpacks_before = unpack_calls;
    assert(dcv_deliver(&video, raw, sizeof(raw), 2, 100) == -EACCES);
    assert(end_calls == ends_before && unpack_calls == unpacks_before);
    assert(buffer.vb.vb2_buf.completed == VB2_BUF_STATE_ERROR && !buffer.vb.vb2_buf.payload);
    assert(snapshot(&video).errors == 3 && snapshot(&video).delivered == 1);
    begin_error = 0;
    queue_buffer(&buffer.vb.vb2_buf);
    unpack_error = -ERANGE; end_error = -EIO;
    assert(dcv_deliver(&video, raw, sizeof(raw), 3, 101) == -ERANGE);
    assert(end_calls == ends_before + 1 && snapshot(&video).errors == 4);
    unpack_error = 0;
    queue_buffer(&buffer.vb.vb2_buf);
    assert(dcv_deliver(&video, raw, sizeof(raw), 4, 102) == -EIO);
    assert(snapshot(&video).errors == 5 && snapshot(&video).total_delivered == 3);
    end_error = 0;
    queue_buffer(&buffer.vb.vb2_buf);
    buffer.vb.vb2_buf.data = NULL;
    assert(dcv_deliver(&video, raw, sizeof(raw), 5, 103) == -EFAULT);
    assert(snapshot(&video).errors == 6);

    /* MMAP buffers do not invoke DMA-BUF synchronization. */
    fresh(&video, &buffer, false);
    assert(start(&video.queue, 0) == 0);
    queue_buffer(&buffer.vb.vb2_buf);
    assert(dcv_deliver(&video, raw, sizeof(raw), 1, 1) == 1);
    assert(!begin_calls && !end_calls && unpack_calls == 1);

    /* Rejected admission leaves counters intact; failed start closes admission. */
    fresh(&video, &buffer, false);
    pipeline_error = -EBUSY;
    assert(start(&video.queue, 0) == -EBUSY);
    assert(!snapshot(&video).stream_id && !snapshot(&video).running);
    pipeline_error = 0; start_error = -EIO;
    queue_buffer(&buffer.vb.vb2_buf);
    assert(start(&video.queue, 1) == -EIO);
    assert(video.blocked && !snapshot(&video).running && snapshot(&video).stream_id == 1);
    assert(buffer.vb.vb2_buf.completed == VB2_BUF_STATE_QUEUED);
    assert(start(&video.queue, 0) == -EIO && snapshot(&video).stream_id == 1);

    /* Closing admission takes precedence over a still-queued buffer. */
    fresh(&video, &buffer, false);
    assert(start(&video.queue, 0) == 0);
    queue_buffer(&buffer.vb.vb2_buf);
    stop_error = -ETIMEDOUT;
    stop(&video.queue);
    stats = snapshot(&video);
    assert(stats.stopping == 1 && !stats.no_buffer && !stats.delivered && !unpack_calls);
    assert(buffer.vb.vb2_buf.completed == VB2_BUF_STATE_ERROR && list_empty(&video.buffers));
    assert(video.blocked && start(&video.queue, 0) == -ETIMEDOUT && stats.stream_id == 1);
    puts("PASS delivery accounting, timestamps, DMA errors, starvation recovery and stream lifecycle");
    return 0;
}
