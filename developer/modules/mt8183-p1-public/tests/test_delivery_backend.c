/* SPDX-License-Identifier: GPL-2.0-only */
static int deliver_callback(void *context, const u8 *bytes, size_t count, u64 logical, u64 timestamp)
{ return dcv_deliver(context, bytes, count, logical, timestamp); }
static struct mt8183_p1_hw fresh_hw(struct dcv_video *video)
{
    static u8 input[5] = { 1, 2, 3, 4, 5 }, shadow[5], compare[5];
    protocol_error = reclaim_calls = publish_calls = publish_result = 0;
    return (struct mt8183_p1_hw){
        .output_cpu = input, .copy_cpu = shadow, .compare_cpu = compare,
        .frame_bytes = 5, .frame_stride = 5,
        .deliver = deliver_callback, .deliver_context = video,
    };
}
int main(void)
{
    frontend_main();
    struct dcv_video video;
    struct dcv_buffer buffer;
    struct mt8183_p1_hw hw;

    /* Exercise actual backend -> actual frontend with no queued VB2 buffer. */
    fresh(&video, &buffer, false);
    assert(start(&video.queue, 0) == 0);
    hw = fresh_hw(&video);
    hw.frames[1].sof_ns = 900;
    assert(read_image(&hw, 1, false) == 0);
    assert(!hw.error && !hw.copy_pending && !hw.frames[1].delivered);
    assert(hw.frames[1].delivery_completed && snapshot(&video).no_buffer == 1);
    /* A benign drop still completes compare/publication/reclamation. */
    assert(read_image(&hw, 1, true) == 0);
    assert(mt8183_p1_hw_publish_image(&hw, 1) == 0);
    assert(reclaim_calls == 1 && publish_calls == 1 && !publish_result);
    assert(hw.frames[1].archived && hw.frames[1].reclaimed && !hw.frames[1].delivered);

    /* A later buffer delivers normally; positive success must not hold capture. */
    hw = fresh_hw(&video);
    hw.frames[2].sof_ns = 987654;
    queue_buffer(&buffer.vb.vb2_buf);
    assert(read_image(&hw, 2, false) == 0);
    assert(!hw.error && hw.frames[2].delivered && hw.frames[2].delivery_completed);
    assert(buffer.vb.sequence == 2 && buffer.vb.vb2_buf.timestamp == 987654);
    assert(read_image(&hw, 2, true) == 0);
    assert(mt8183_p1_hw_publish_image(&hw, 2) == 0 && reclaim_calls == 1);

    /* STOP callbacks also discard nonfatally without claiming delivery. */
    stop(&video.queue);
    hw = fresh_hw(&video);
    assert(read_image(&hw, 3, false) == 0);
    assert(hw.frames[3].delivery_completed && !hw.frames[3].delivered && !hw.error);

    /* A conversion failure is not normalized into a successful handoff. */
    fresh(&video, &buffer, false);
    assert(start(&video.queue, 0) == 0);
    queue_buffer(&buffer.vb.vb2_buf);
    unpack_error = -EIO;
    hw = fresh_hw(&video);
    assert(read_image(&hw, 4, false) == -EIO && hw.error == -EIO);
    assert(!hw.frames[4].delivery_completed && !hw.frames[4].delivered);
    assert(mt8183_p1_hw_publish_image(&hw, 4) == -EPROTO && !reclaim_calls);

    /* Protocol errors before delivery continue to retain error ownership. */
    hw = fresh_hw(&video);
    protocol_error = -ETIME;
    assert(read_image(&hw, 5, false) == -ETIME && hw.error == -ETIME);
    assert(!hw.frames[5].delivery_completed && !reclaim_calls);

    /* The actual compare path still passes mismatches into the protocol guard. */
    fresh(&video, &buffer, false);
    assert(start(&video.queue, 0) == 0);
    hw = fresh_hw(&video);
    assert(read_image(&hw, 6, false) == 0);
    ((u8 *)hw.copy_cpu)[0] ^= 1;
    assert(read_image(&hw, 6, true) == -EPROTO && hw.error == -EPROTO);
    assert(!hw.frames[6].compared && !hw.frames[6].match && !reclaim_calls);
    puts("PASS actual backend handoff/drop distinction and publication error propagation");
    return 0;
}
