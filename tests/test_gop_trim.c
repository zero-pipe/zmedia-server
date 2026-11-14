/**
 * @file test_gop_trim.c
 * @brief GOP 缓存回收：时间窗裁剪、字节上限裁剪、字节记账与清空。
 */
#include "zms/core/gop/queue.h"
#include "zms/core/frame.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* 构造一个"视频帧"：H.264 Annex-B 形态，keyframe 控制 GOP 边界。 */
static void push_video(zms_gop_queue *q, uint64_t dts_ms, int keyframe, const uint8_t *payload,
                       size_t len)
{
    zms_frame f;

    memset(&f, 0, sizeof(f));
    f.data = (uint8_t *)payload;
    f.size = len;
    f.dts_ms = dts_ms;
    f.pts_ms = dts_ms;
    f.codec = ZMS_CODEC_H264;
    f.track = ZMS_TRACK_VIDEO;
    f.keyframe = keyframe ? 1 : 0;
    (void)zms_gop_queue_write(q, &f);
}

static size_t test_time_window(void)
{
    static uint8_t frame[1000];
    zms_gop_queue *q = zms_gop_queue_create();
    size_t bytes;

    CHECK(q != NULL);
    memset(frame, 0x42, sizeof(frame));

    zms_gop_queue_set_cache_ms(q, 1000); /* 1 秒窗口 */
    zms_gop_queue_set_byte_budget(q, 0); /* 只测时间 */

    /* 10fps、每 10 帧一个关键帧（GOP=1s），共 6 秒 */
    for (int i = 0; i < 60; ++i) {
        push_video(q, (uint64_t)i * 100u, (i % 10) == 0, frame, sizeof(frame));
    }
    bytes = zms_gop_queue_bytes(q);
    /* 窗口 1s + 当前 GOP：最多 ~2~3 个 GOP，绝不是 60 帧全留 */
    CHECK(zms_gop_queue_gop_count(q) <= 3);
    CHECK(zms_gop_queue_gop_count(q) >= 1);
    CHECK(zms_gop_queue_pending_count(q) <= 40);
    CHECK(bytes > 0 && bytes <= 40u * sizeof(frame));
    zms_gop_queue_destroy(q);
    return 0;
}

static int test_byte_budget(void)
{
    static uint8_t frame[1000];
    zms_gop_queue *q = zms_gop_queue_create();
    size_t bytes;

    CHECK(q != NULL);
    memset(frame, 0x11, sizeof(frame));

    zms_gop_queue_set_cache_ms(q, 0);              /* 关时间窗 */
    zms_gop_queue_set_byte_budget(q, 4u * 1024u);  /* 4KB 上限 */

    for (int i = 0; i < 60; ++i) {
        push_video(q, (uint64_t)i * 40u, (i % 10) == 0, frame, sizeof(frame));
    }
    bytes = zms_gop_queue_bytes(q);
    /* 至少保留当前 GOP（10 帧 = 10KB），但不会保留全部 60KB */
    CHECK(bytes >= 1);
    CHECK(bytes < 60u * sizeof(frame));
    CHECK(zms_gop_queue_gop_count(q) >= 1);
    zms_gop_queue_destroy(q);
    return 0;
}

static int test_bytes_accounting(void)
{
    static uint8_t frame[500];
    zms_gop_queue *q = zms_gop_queue_create();
    size_t before;
    size_t after;

    CHECK(q != NULL);
    memset(frame, 0x22, sizeof(frame));
    zms_gop_queue_set_cache_ms(q, 0);
    zms_gop_queue_set_byte_budget(q, 0);

    CHECK(zms_gop_queue_bytes(q) == 0);
    push_video(q, 0, 1, frame, sizeof(frame));
    before = zms_gop_queue_bytes(q);
    CHECK(before == sizeof(frame));
    push_video(q, 40, 0, frame, sizeof(frame));
    after = zms_gop_queue_bytes(q);
    CHECK(after == 2u * sizeof(frame));

    /* clear 后记账归零 */
    zms_gop_queue_clear(q);
    CHECK(zms_gop_queue_bytes(q) == 0);
    CHECK(zms_gop_queue_pending_count(q) == 0);
    zms_gop_queue_destroy(q);
    return 0;
}

int main(void)
{
    int r;

    if ((r = (int)test_time_window()) != 0) {
        return r;
    }
    if ((r = test_byte_budget()) != 0) {
        return r;
    }
    if ((r = test_bytes_accounting()) != 0) {
        return r;
    }
    printf("test_gop_trim: ok\n");
    return 0;
}
