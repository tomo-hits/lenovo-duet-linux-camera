/* SPDX-License-Identifier: GPL-2.0-only */
/* Host-only VB2/DMA providers; accounting is compiled from production code. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int spinlock_t;
struct device;
struct v4l2_device;
struct media_entity;
struct mutex { int unused; };
struct media_pad { int unused; };
struct video_device { int unused; };
struct dma_buf { int unused; };
struct vb2_queue { void *drv_priv; };
struct vb2_buffer {
    struct { struct dma_buf *dbuf; } planes[1];
    struct vb2_queue *vb2_queue;
    u64 timestamp;
    void *data;
    size_t payload;
    int completed;
};
struct vb2_v4l2_buffer {
    struct vb2_buffer vb2_buf;
    u32 sequence;
    int field;
};
struct list_head { struct list_head *next, *prev; };
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define list_first_entry(head, type, member) container_of((head)->next, type, member)
#define INIT_LIST_HEAD(head) ((head)->next = (head), (head)->prev = (head))
static bool list_empty(struct list_head *head) { return head->next == head; }
static void list_add_tail(struct list_head *entry, struct list_head *head)
{
    entry->next = head; entry->prev = head->prev;
    head->prev->next = entry; head->prev = entry;
}
static void list_del_init(struct list_head *entry)
{
    entry->next->prev = entry->prev; entry->prev->next = entry->next;
    INIT_LIST_HEAD(entry);
}
#define spin_lock_irqsave(lock, flags) do { assert(*(lock) == 0); *(lock) = 1; (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { assert(*(lock) == 1); *(lock) = 0; (void)(flags); } while (0)
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, new_value) ((value) = (new_value))
#define V4L2_FIELD_NONE 0
#define DMA_BIDIRECTIONAL 0
enum vb2_buffer_state { VB2_BUF_STATE_QUEUED = 1, VB2_BUF_STATE_ERROR, VB2_BUF_STATE_DONE };
static void *vb2_get_drv_priv(struct vb2_queue *queue) { return queue->drv_priv; }
static struct vb2_v4l2_buffer *to_vb2_v4l2_buffer(struct vb2_buffer *buffer)
{ return container_of(buffer, struct vb2_v4l2_buffer, vb2_buf); }
static void *vb2_plane_vaddr(struct vb2_buffer *buffer, int plane)
{ assert(plane == 0); return buffer->data; }
static void vb2_set_plane_payload(struct vb2_buffer *buffer, int plane, size_t payload)
{ assert(plane == 0); buffer->payload = payload; }
static void vb2_buffer_done(struct vb2_buffer *buffer, enum vb2_buffer_state state)
{ buffer->completed = state; }
static int begin_error, end_error, begin_calls, end_calls, unpack_error, unpack_calls;
static int pipeline_error, pipeline_stops;
static int dma_buf_begin_cpu_access(struct dma_buf *buffer, int direction)
{ assert(buffer && direction == DMA_BIDIRECTIONAL); begin_calls++; return begin_error; }
static int dma_buf_end_cpu_access(struct dma_buf *buffer, int direction)
{ assert(buffer && direction == DMA_BIDIRECTIONAL); end_calls++; return end_error; }
static int video_device_pipeline_alloc_start(struct video_device *video)
{ (void)video; return pipeline_error; }
static void video_device_pipeline_stop(struct video_device *video)
{ (void)video; pipeline_stops++; }
/* Tiny geometry keeps the harness focused on ownership and error paths. */
static int dcv_raw10_unpack(const u8 *in, size_t bytes, u8 *out, size_t size)
{
    assert(in && out && bytes == 5 && size == 8);
    unpack_calls++;
    if (unpack_error) return unpack_error;
    memset(out, 0x5a, size);
    return 0;
}
