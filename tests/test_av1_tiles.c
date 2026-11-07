/* Exercise the shipped AV1 geometry and payload validation without a VPU. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <linux/media.h>
#include "va_v4l2.h"

static int failures, ioctls;
static int submitted, pull_error, grain_missing, grain_sent, normalize_capture, control_error;
static struct v4l2_ctrl_av1_frame submitted_frame;
static struct v4l2_ctrl_av1_film_grain submitted_grain;
static void *capture_pixels;
static int replay_fixture, replay_fault, replayed, resets, request_allocs, expected_replay_byte, expected_replay_seed;
static uint64_t cdf_ts[8], capture_ts[VAV4L2_MAX_CAPTURE_BUFS];
static void expect(int ok, const char *tag);
static void kernel_picture(const struct v4l2_ctrl_av1_frame *frame, int capture, uint64_t timestamp)
{
    if (frame->frame_type == V4L2_AV1_INTER_FRAME) for (unsigned i = 0; i < 7; i++) {
        unsigned slot = frame->ref_frame_idx[i];
        expect(slot < 8 && cdf_ts[slot] == frame->reference_frame_ts[slot], "every-reference-retains-its-original-CDF");
        int found = 0;
        for (unsigned j = 0; j < VAV4L2_MAX_CAPTURE_BUFS; j++) found |= capture_ts[j] == frame->reference_frame_ts[slot];
        expect(found, "every-reference-retains-its-native-CAPTURE-timestamp");
    }
    for (unsigned i = 0; i < 8; i++) if (frame->refresh_frame_flags & (1u << i)) cdf_ts[i] = timestamp;
    capture_ts[capture] = timestamp;
}
void __wrap_vav4l2_decode_reset(struct vav4l2_context *c)
{ resets++; vav4l2_av1_dpb_model_reset(c); }
int __wrap_vav4l2_decode_replay(struct vav4l2_context *c, int output, uint32_t bytes, uint64_t timestamp, int capture)
{
    replayed++;
    expect(bytes == 4 && ((uint8_t *)c->output_buf_ptr[output])[0] == expected_replay_byte, "replay-owns-unchanged-original-tile-bytes");
    expect(submitted_grain.grain_seed == expected_replay_seed, "replay-owns-original-film-grain-controls");
    if (replay_fault == 10) { __wrap_vav4l2_decode_reset(c); return -1; }
    if (replay_fault == 1) return -1;
    if (replay_fault == 3) return capture + 1;
    kernel_picture(&submitted_frame, capture, timestamp); vav4l2_out_pool_push(c, output);
    if (replay_fault == 7 || replay_fault == 11) c->request_fd = -1;
    return capture;
}
extern VAStatus VAV4L2_DRIVER_INIT(VADriverContextP ctx);
int __wrap_vav4l2_ensure_capture(struct vav4l2_context *c, int w, int h, uint32_t f)
{ (void)c; (void)w; (void)h; (void)f; return 0; }
int __wrap_vav4l2_decode_submit(struct vav4l2_context *c, int i, uint32_t bytes, uint64_t timestamp)
{
    (void)bytes; submitted++;
    if (!replay_fixture) return 0;
    int capture = c->current_surface->buf_index;
    if (capture < 0) capture = c->free_cap_bufs[--c->n_free_cap];
    kernel_picture(&submitted_frame, capture, timestamp); vav4l2_out_pool_push(c, i); return capture;
}
int __wrap_vav4l2_surface_pull_capture(struct vav4l2_context *c, struct vav4l2_surface *s, int i)
{
    if (pull_error) return -1;
    if (capture_pixels) return vav4l2_surface_store_capture(c, s, i, capture_pixels, c->capture_buf_size, 1);
    if (normalize_capture) { s->buf_index = i; s->capture_ctx = c; s->status = VASurfaceReady; s->has_pic = 1; }
    return 0;
}
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", tag); failures++; }
}
static int reject_ioctl(int fd, unsigned long req, void *arg)
{
    (void)fd; (void)req; (void)arg;
    ioctls++;
    return -1;
}
static VADecPictureParameterBufferAV1 picture(unsigned w, unsigned h, unsigned cols, unsigned rows)
{
    VADecPictureParameterBufferAV1 p = {0};
    p.frame_width_minus1 = w - 1;
    p.frame_height_minus1 = h - 1;
    p.tile_cols = cols;
    p.tile_rows = rows;
    p.pic_info_fields.bits.uniform_tile_spacing_flag = 1;
    return p;
}
static void geometry(void)
{
    struct v4l2_av1_tile_info t;
    VADecPictureParameterBufferAV1 p;
    const unsigned grids[][2] = {{1,1}, {2,1}, {4,1}, {8,1}, {2,2}};
    for (unsigned g = 0; g < sizeof(grids) / sizeof(grids[0]); g++) {
        p = picture(3840, 2160, grids[g][0], grids[g][1]);
        expect(vav4l2_av1_fill_tile_info(&t, &p) == VA_STATUS_SUCCESS, "uniform-grid");
        expect(t.mi_col_starts[p.tile_cols] == 960 && t.mi_row_starts[p.tile_rows] == 540, "MI-sentinels");
        for (unsigned i = 1; i < p.tile_cols; i++)
            expect(t.mi_col_starts[i] % 16 == 0, "SB-column-alignment");
    }
    p = picture(3840, 2160, 8, 1);
    vav4l2_av1_fill_tile_info(&t, &p);
    for (unsigned i = 0; i < 8; i++)
        expect(t.mi_col_starts[i] == 128 * i && t.width_in_sbs_minus_1[i] == (i == 7 ? 3u : 7u), "4k-eight-columns");
    p.seq_info_fields.fields.use_128x128_superblock = 1;
    expect(vav4l2_av1_fill_tile_info(&t, &p) == VA_STATUS_SUCCESS && t.mi_col_starts[7] == 896, "SB128");
    p = picture(1921, 1081, 1, 1);
    expect(vav4l2_av1_fill_tile_info(&t, &p) == VA_STATUS_SUCCESS && t.mi_col_starts[1] == 482 && t.mi_row_starts[1] == 272, "odd-size-MI-rounding");
    p = picture(3840, 2160, 8, 1);
    p.superres_scale_denominator = 16;
    expect(vav4l2_av1_fill_tile_info(&t, &p) == VA_STATUS_SUCCESS && t.mi_col_starts[8] == 480 && t.mi_col_starts[1] == 64, "superres-coded-grid");
    p = picture(1920, 1080, 3, 2);
    p.pic_info_fields.bits.uniform_tile_spacing_flag = 0;
    p.width_in_sbs_minus_1[0] = 4;
    p.width_in_sbs_minus_1[1] = 9;
    p.width_in_sbs_minus_1[2] = 65535; /* Last width is inferred, per libva. */
    p.height_in_sbs_minus_1[0] = 7;
    expect(vav4l2_av1_fill_tile_info(&t, &p) == VA_STATUS_SUCCESS && t.mi_col_starts[2] == 240 && t.width_in_sbs_minus_1[2] == 14 && t.mi_row_starts[2] == 270, "nonuniform-last-SB-crop");
    p.width_in_sbs_minus_1[0] = 29;
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "nonuniform-overrun");
    p = picture(1280, 720, 0, 1);
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "zero-grid");
    p.tile_cols = 8; p.tile_rows = 8;
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "table-overflow");
    p = picture(64, 64, 2, 1);
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "empty-tile");
    p = picture(1280, 720, 2, 1); p.context_update_tile_id = 2;
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "bad-context-tile");
    p.context_update_tile_id = 0; p.order_hint_bits_minus_1 = 31;
    expect(vav4l2_av1_fill_tile_info(&t, &p) != VA_STATUS_SUCCESS, "bad-order-hint-bits");
}
static void payload(void)
{
    VADecPictureParameterBufferAV1 p = picture(1280, 720, 2, 2);
    VASliceParameterBufferAV1 t[4] = {0};
    const VASliceParameterBufferAV1 *tiles[4] = {&t[0], &t[1], &t[2], &t[3]};
    uint8_t data[14] = {2,0, 10,11,12, 1,0, 13,14, 1,0, 15,16, 17};
    const unsigned offsets[] = {2,7,11,13}, sizes[] = {3,2,2,1};
    struct v4l2_av1_tile_info info;
    struct v4l2_ctrl_av1_tile_group_entry e[32];
    uint32_t start, length;
    for (unsigned i = 0; i < 4; i++) {
        t[i].tile_row = i / 2; t[i].tile_column = i % 2;
        t[i].slice_data_offset = offsets[i]; t[i].slice_data_size = sizes[i];
        t[i].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    }
#define PREP(n, cap) vav4l2_av1_prepare_tiles(&p, tiles, n, data, sizeof(data), cap, &info, e, &start, &length)
    expect(PREP(4, 64) == VA_STATUS_SUCCESS && info.tile_size_bytes == 2 && start == 2 && length == 12 && e[1].tile_offset == 5, "tile-size-fields-preserved");
    expect(PREP(3, 64) != VA_STATUS_SUCCESS, "missing-tile");
    expect(PREP(4, 11) != VA_STATUS_SUCCESS, "output-too-small");
    t[1].tile_column = 0;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "duplicate-tile");
    t[1].tile_column = 1; t[1].slice_data_offset = 4;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "overlap");
    t[1].slice_data_offset = 7; t[3].slice_data_size = 0;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "empty-payload");
    t[3].slice_data_size = 0xffffffff;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "range-overflow");
    t[3].slice_data_size = 1; data[5] = 0;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "invalid-size-prefix");
    data[5] = 1; t[0].slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN;
    expect(PREP(4, 64) != VA_STATUS_SUCCESS, "partial-tile");
    p = picture(1280, 720, 2, 1);
    t[0].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    t[1].slice_data_offset = 5;
    expect(PREP(2, 64) == VA_STATUS_SUCCESS && info.tile_size_bytes == 2, "two-raw-tiles");
    t[0].slice_data_offset = 0; t[1].slice_data_offset = 3;
    expect(PREP(2, 64) != VA_STATUS_SUCCESS, "missing-size-metadata");
    p = picture(1280, 720, 1, 1);
    expect(PREP(0, 64) == VA_STATUS_SUCCESS && e[0].tile_size == sizeof(data), "single-raw-tile");
#undef PREP
}
static void reject_before_ioctl(void)
{
    struct vav4l2_context ctx = {0};
    VADecPictureParameterBufferAV1 p = picture(1280, 720, 2, 1);
    VASliceParameterBufferAV1 t[2] = {0};
    uint8_t data[16] = {0};
    struct vav4l2_buffer b[3] = {
        {.type = VAPictureParameterBufferType, .size = sizeof(p), .num_elements = 1, .data = &p},
        {.type = VASliceParameterBufferType, .size = sizeof(t[0]), .num_elements = 2, .data = t},
        {.type = VASliceDataBufferType, .size = sizeof(data), .num_elements = 1, .data = data},
    };
    struct vav4l2_buffer *buffers[] = {&b[0], &b[1], &b[2]};
    ctx.output_buf_size = 64;
    vav4l2_set_ioctl_hook(reject_ioctl);
    expect(vav4l2_av1_translate(&ctx, buffers, 3) == VA_STATUS_ERROR_INVALID_PARAMETER && ioctls == 0, "bad-payload-no-ioctl");
    b[0].size = 1;
    expect(vav4l2_av1_translate(&ctx, buffers, 3) == VA_STATUS_ERROR_INVALID_PARAMETER && ioctls == 0, "short-picture-no-ioctl");
    vav4l2_set_ioctl_hook(NULL);
}
static int controls(int fd, unsigned long request, void *arg)
{
    (void)fd; ioctls++;
    if (request == MEDIA_IOC_REQUEST_ALLOC) {
        request_allocs++; if (replay_fault == 11) { errno = EIO; return -1; }
        *(int *)arg = 42; return 0;
    }
    if (request == VIDIOC_QUERY_EXT_CTRL && grain_missing) { errno = EINVAL; return -1; }
    if (request == VIDIOC_S_EXT_CTRLS) {
        struct v4l2_ext_controls *batch = arg;
        for (unsigned i = 0; i < batch->count; i++) {
            grain_sent += batch->controls[i].id == V4L2_CID_STATELESS_AV1_FILM_GRAIN;
            if (batch->controls[i].id == V4L2_CID_STATELESS_AV1_FILM_GRAIN) submitted_grain = *(struct v4l2_ctrl_av1_film_grain *)batch->controls[i].ptr;
            if (batch->controls[i].id == V4L2_CID_STATELESS_AV1_FRAME) {
                submitted_frame = *batch->controls[i].p_av1_frame;
                if (control_error) { errno = EIO; return -1; }
            }
        }
    }
    return 0;
}
static void completion(void)
{
    uint8_t output[64], data[] = {1,2,3,4};
    VADecPictureParameterBufferAV1 p = picture(16, 16, 1, 1);
    p.primary_ref_frame = 7; p.pic_info_fields.bits.show_frame = 1;
    p.seq_info_fields.fields.subsampling_x = p.seq_info_fields.fields.subsampling_y = 1;
    struct vav4l2_buffer b[] = {{.type = VAPictureParameterBufferType, .size = sizeof(p), .data = &p},
        {.type = VASliceDataBufferType, .size = sizeof(data), .data = data}};
    struct vav4l2_buffer *batch[] = {b, b + 1};
    for (unsigned mode = 0; mode < 4; mode++) {
        struct vav4l2_surface s = {.status = VASurfaceRendering, .buf_index = -1};
        struct vav4l2_context c = {.current_surface = &s, .v4l2_fd = 8, .request_fd = 9, .streamed = 1,
            .n_free_out = 1, .output_buf_ptr = {output}, .output_buf_size = sizeof(output), .capture_bufs_allocd = 4};
        submitted = grain_sent = 0; grain_missing = mode != 2; pull_error = mode == 0;
        p.film_grain_info.film_grain_info_fields.bits.apply_grain = mode == 3;
        vav4l2_set_ioctl_hook(controls);
        VAStatus status = vav4l2_av1_translate(&c, batch, 2);
        if (!mode) expect(status == VA_STATUS_ERROR_OPERATION_FAILED && submitted == 1 && !s.has_pic && c.n_free_cap == 1, "capture-transfer-failure-is-not-success");
        if (mode == 1) expect(status == VA_STATUS_SUCCESS && submitted == 1 && !grain_sent, "grainless-node-omits-optional-control");
        if (mode == 2) expect(status == VA_STATUS_SUCCESS && submitted == 1 && grain_sent == 1, "available-grain-control-is-submitted");
        if (mode == 3) expect(status == VA_STATUS_ERROR_UNIMPLEMENTED && !submitted, "missing-grain-feature-rejects-before-submit");
    }
    vav4l2_set_ioctl_hook(NULL);
    struct vav4l2_av1_header header = {0}; uint8_t refresh;
    p.pic_info_fields.bits.frame_type = 1;
    expect(vav4l2_av1_refresh_flags(&header, &p, data, sizeof(data), &refresh) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "raw-inter-tiles-never-guess-encoder-GOP-refresh");
    p.pic_info_fields.bits.frame_type = 3;
    expect(vav4l2_av1_refresh_flags(&header, &p, data, sizeof(data), &refresh) == VA_STATUS_SUCCESS && refresh == 255,
        "switch-frame-refresh-is-defined-by-specification");
}
static void normalized_references(void)
{
    struct vav4l2_driver_data *dd = calloc(1, sizeof(*dd));
    if (!dd) { expect(0, "reference-fixture-allocation"); return; }
    uint8_t output[64], data[] = {1,2,3,4};
    VADecPictureParameterBufferAV1 p = picture(16, 16, 1, 1);
    p.primary_ref_frame = 0; p.pic_info_fields.bits.frame_type = V4L2_AV1_INTER_FRAME; p.pic_info_fields.bits.show_frame = 1;
    p.seq_info_fields.fields.subsampling_x = p.seq_info_fields.fields.subsampling_y = 1;
    VASliceParameterBufferAV1 tile = {.slice_data_size = sizeof(data), .slice_data_flag = VA_SLICE_DATA_FLAG_ALL};
    struct vav4l2_buffer b[] = {{.type = VAPictureParameterBufferType, .size = sizeof(p), .data = &p},
        {.type = VASliceParameterBufferType, .size = sizeof(tile), .num_elements = 1, .data = &tile},
        {.type = VASliceDataBufferType, .size = sizeof(data), .num_elements = 1, .data = data}};
    struct vav4l2_buffer *batch[] = {b, b + 1, b + 2};
    struct vav4l2_surface surfaces[10] = {0};
    struct vav4l2_context c = {.driver_data = dd, .current_surface = &surfaces[0], .v4l2_fd = 8, .request_fd = 9, .streamed = 1,
        .n_free_out = 1, .output_buf_ptr = {output}, .output_buf_size = sizeof(output), .capture_bufs_allocd = 10};
    for (unsigned mode = 0; mode < 8; mode++) {
        vav4l2_av1_dpb_model_reset(&c); c.av1.model_active = 1; c.av1.grain_checked = 1; c.av1.has_grain = 0;
        c.n_free_out = 1; c.free_out_bufs[0] = 0; c.n_free_cap = 0; submitted = ioctls = 0; normalize_capture = 1;
        pull_error = mode == 4; control_error = mode == 5;
        for (unsigned i = 0; i < 10; i++) {
            surfaces[i] = (struct vav4l2_surface){.surface_id = i, .status = VASurfaceReady, .has_pic = 1,
                .buf_index = i, .capture_ctx = &c, .timestamp = i ? i - 1 : 99};
            dd->surfaces[i] = &surfaces[i]; c.av1.buf_owner[i] = i;
        }
        surfaces[1].timestamp = 0; surfaces[0].status = VASurfaceRendering;
        for (unsigned i = 0; i < 8; i++) { p.ref_frame_map[i] = 1; c.av1.slot_buf[i] = 1; }
        memset(p.ref_frame_idx, 0, sizeof(p.ref_frame_idx));
        if (mode == 1) {
            c.av1.normalized = 1;
            for (unsigned i = 0; i < 8; i++) { p.ref_frame_map[i] = i == 7 ? 1 : 2; c.av1.slot_buf[i] = i == 4 ? 1 : 2; }
            for (unsigned i = 0; i < 7; i++) p.ref_frame_idx[i] = i == 3 ? 7 : 0;
            p.primary_ref_frame = 3;
        } else p.primary_ref_frame = 0;
        if (mode == 2) {
            for (unsigned i = 0; i < 8; i++) { p.ref_frame_map[i] = i + 1; c.av1.slot_buf[i] = i + 1; }
        }
        if (mode == 3) { c.av1.normalized = 1; c.av1.buf_owner[1] = VA_INVALID_SURFACE; }
        if (mode == 6) c.av1.header.valid = 1;
        if (mode == 7) {
            c.av1.normalized = 1;
            for (unsigned i = 0; i < 8; i++) { p.ref_frame_map[i] = i < 7 ? i + 1 : 1; c.av1.slot_buf[i] = i < 7 ? 7 - i : 1; }
            for (unsigned i = 0; i < 7; i++) p.ref_frame_idx[i] = 6 - i;
        }
        int saved_slots[8], saved_normalized = c.av1.normalized; memcpy(saved_slots, c.av1.slot_buf, sizeof(saved_slots));
        vav4l2_set_ioctl_hook(controls);
        VAStatus status = vav4l2_av1_translate(&c, batch, 3);
        if (mode == 2) {
            expect(status == VA_STATUS_SUCCESS && submitted == 1 && !submitted_frame.refresh_frame_flags && c.av1.pending &&
                !c.n_free_cap && c.av1.buf_owner[0] == 0 && !memcmp(saved_slots, c.av1.slot_buf, sizeof(saved_slots)),
                "full-map-decodes-without-overwriting-live-CDF-or-releasing-current-CAPTURE");
        } else if (mode == 7) {
            expect(status == VA_STATUS_SUCCESS && submitted == 1 && submitted_frame.refresh_frame_flags == 128 &&
                c.av1.slot_buf[7] == 0, "seven-live-CDFs-leave-room-for-the-eighth-picture");
            for (unsigned i = 0; i < 7; i++) expect(submitted_frame.ref_frame_idx[i] == (int)i &&
                submitted_frame.reference_frame_ts[i] == 6 - i && c.av1.slot_buf[i] == 7 - (int)i,
                "all-seven-reference-types-preserve-their-native-state");
        } else if (mode < 2) {
            expect(status == VA_STATUS_SUCCESS && submitted == 1 && c.av1.normalized, "raw-reference-normalization-submits");
            expect(submitted_frame.refresh_frame_flags == 2, "only-a-duplicate-CDF-slot-is-replaced");
            expect(submitted_frame.reference_frame_ts[mode == 1 ? 4 : 0] == 0 &&
                submitted_frame.ref_frame_idx[mode == 1 ? 3 : 0] == (mode == 1 ? 4 : 0), "CDF-reference-remap-keeps-zero-timestamp");
            expect(submitted_frame.primary_ref_frame == p.primary_ref_frame && c.av1.slot_buf[1] == 0 &&
                c.av1.slot_buf[mode == 1 ? 4 : 0] == 1, "primary-reference-type-and-old-CDF-remain-live");
        } else {
            expect(status == (mode == 2 || mode == 6 ? VA_STATUS_ERROR_UNIMPLEMENTED : mode == 3 ? VA_STATUS_ERROR_INVALID_SURFACE : VA_STATUS_ERROR_OPERATION_FAILED),
                "full-stale-or-failed-normalization-is-rejected");
            if (mode == 2 || mode == 3 || mode == 6) expect(!ioctls, "unsupported-or-stale-slots-do-not-reach-ioctl");
            expect(submitted == (mode == 4) && !memcmp(saved_slots, c.av1.slot_buf, sizeof(saved_slots)) && c.av1.normalized == saved_normalized,
                "normalization-failure-does-not-commit-slot-state");
        }
    }
    vav4l2_av1_dpb_model_reset(&c);
    normalize_capture = control_error = pull_error = 0; vav4l2_set_ioctl_hook(NULL); free(dd);
}
static void deferred_references(void)
{
    struct vav4l2_driver_data *dd = calloc(1, sizeof(*dd));
    if (!dd) { expect(0, "deferred-reference-fixture"); return; }
    struct vav4l2_surface surfaces[12]; uint8_t output[64], data[4];
    VADecPictureParameterBufferAV1 p = picture(16, 16, 1, 1);
    VASliceParameterBufferAV1 tile = {.slice_data_size = sizeof(data), .slice_data_flag = VA_SLICE_DATA_FLAG_ALL};
    struct vav4l2_buffer b[] = {{.type = VAPictureParameterBufferType, .size = sizeof(p), .data = &p},
        {.type = VASliceParameterBufferType, .size = sizeof(tile), .num_elements = 1, .data = &tile},
        {.type = VASliceDataBufferType, .size = sizeof(data), .data = data}};
    struct vav4l2_buffer *batch[] = {b, b + 1, b + 2};
    for (unsigned mode = 0; mode < 12; mode++) {
        struct vav4l2_context c = {.codec = VAV4L2_CODEC_AV1, .driver_data = dd, .current_surface = surfaces,
            .v4l2_fd = 8, .media_fd = 8, .request_fd = 9, .streamed = 1, .n_free_out = 1,
            .output_buf_ptr = {output}, .output_buf_size = sizeof(output), .capture_bufs_allocd = 12};
        vav4l2_av1_dpb_model_reset(&c); c.av1.model_active = c.av1.normalized = c.av1.grain_checked = c.av1.has_grain = 1;
        replay_fixture = normalize_capture = 1; replay_fault = replayed = resets = control_error = request_allocs = 0;
        memset(capture_ts, 0, sizeof(capture_ts)); data[0] = expected_replay_byte = 1;
        p.pic_info_fields.bits.frame_type = V4L2_AV1_INTER_FRAME; p.primary_ref_frame = 0;
        p.film_grain_info.film_grain_info_fields.bits.apply_grain = 1;
        p.film_grain_info.grain_seed = expected_replay_seed = 137;
        p.seq_info_fields.fields.subsampling_x = p.seq_info_fields.fields.subsampling_y = 1;
        for (unsigned i = 0; i < 12; i++) {
            surfaces[i] = (struct vav4l2_surface){.surface_id = i, .has_pic = 1, .status = VASurfaceReady,
                .buf_index = i, .capture_ctx = &c, .timestamp = 1000 + i};
            dd->surfaces[i] = &surfaces[i];
        }
        surfaces[0].buf_index = -1; surfaces[0].status = VASurfaceRendering;
        if (mode == 9) surfaces[0].timestamp = 0;
        for (unsigned i = 0; i < 8; i++) {
            p.ref_frame_map[i] = i + 1; c.av1.slot_buf[i] = i + 1; c.av1.buf_owner[i + 1] = i + 1;
            cdf_ts[i] = capture_ts[i + 1] = surfaces[i + 1].timestamp;
        }
        for (unsigned i = 0; i < 7; i++) p.ref_frame_idx[i] = i;
        vav4l2_cap_pool_push(&c, 11); vav4l2_cap_pool_push(&c, 10); vav4l2_cap_pool_push(&c, 9); vav4l2_cap_pool_push(&c, 0);
        vav4l2_set_ioctl_hook(controls);
        expect(vav4l2_av1_translate(&c, batch, 3) == VA_STATUS_SUCCESS && c.av1.pending && c.n_free_cap == 3,
            "full-DPB-picture-is-ready-before-the-next-VA-map");
        data[0] = 0xa5; p.film_grain_info.grain_seed = 999; p.ref_frame_map[3] = 0; c.current_surface = &surfaces[9]; surfaces[9].buf_index = -1;
        surfaces[9].status = VASurfaceRendering;
        replay_fault = mode; control_error = mode == 2;
        if (mode == 4) p.ref_frame_map[7] = VA_INVALID_ID - 1;
        if (mode == 5) surfaces[0].timestamp++;
        if (mode == 6) c.capture_exports[0].count = 1;
        if (mode == 8) p.ref_frame_map[3] = 4;
        int before = ioctls;
        VAStatus status = vav4l2_av1_translate(&c, batch, 3);
        if (mode == 1 || mode == 2 || mode == 3 || mode == 10) {
            expect(status == VA_STATUS_ERROR_DECODING_ERROR && resets && !c.av1.pending && !c.av1.model_active,
                "replay-submit-control-or-completion-failure-invalidates-CDF-state");
        } else if (mode == 11) {
            expect(status == VA_STATUS_ERROR_OPERATION_FAILED && replayed == 1 && request_allocs == 1 && !resets &&
                !c.av1.pending && c.av1.slot_buf[3] == 0 && c.n_free_cap == 4,
                "current-request-allocation-failure-preserves-the-completed-previous-CDF-install");
        } else if (mode >= 4 && mode <= 6) {
            expect(status == (mode == 6 ? VA_STATUS_ERROR_UNIMPLEMENTED : VA_STATUS_ERROR_INVALID_SURFACE) &&
                !replayed && !resets && c.av1.pending && ioctls == before,
                "stale-invalid-or-externally-exported-reference-is-rejected-before-replay");
        } else {
            expect(status == VA_STATUS_SUCCESS && c.av1.pending && replayed == (mode == 8 ? 0 : 1) && c.n_free_cap == 3,
                "next-map-resolves-or-discards-pending-picture-with-a-bounded-nine-picture-DPB");
            if (mode == 7) expect(request_allocs == 1, "replay-REINIT-failure-allocates-a-new-request-for-current-picture");
            if (mode != 8) {
                expect(c.av1.slot_buf[3] == 0 && cdf_ts[3] == surfaces[0].timestamp && surfaces[4].buf_index == -1,
                    "only-the-map-proven-retired-CDF-slot-is-replaced");
                expected_replay_byte = 0xa5; expected_replay_seed = 999;
                /* Reuse retired VA IDs while every new map still has eight distinct references. */
                for (unsigned n = 0; n < 16; n++) {
                    unsigned slot = n % 8; VASurfaceID target = p.ref_frame_map[slot];
                    p.ref_frame_map[slot] = c.current_surface->surface_id; c.current_surface = &surfaces[target];
                    c.current_surface->timestamp = 2000 + n; c.current_surface->status = VASurfaceRendering;
                    data[0] = 1; p.film_grain_info.grain_seed = 137; replay_fault = 0;
                    expect(vav4l2_av1_translate(&c, batch, 3) == VA_STATUS_SUCCESS && c.av1.pending && c.n_free_cap == 3,
                        "consecutive-full-maps-preserve-CDF-and-CAPTURE-across-surface-reuse");
                    expected_replay_byte = 1; expected_replay_seed = 137;
                }
            }
            p.pic_info_fields.bits.frame_type = V4L2_AV1_KEY_FRAME; p.pic_info_fields.bits.show_frame = 1; p.primary_ref_frame = 7;
            c.current_surface = &surfaces[11]; c.current_surface->buf_index = -1; c.current_surface->timestamp = 9000;
            expect(vav4l2_av1_translate(&c, batch, 3) == VA_STATUS_SUCCESS && !c.av1.pending && c.n_free_cap == 11,
                "key-frame-discards-pending-payload-and-releases-old-references");
        }
        vav4l2_av1_dpb_model_reset(&c);
    }
    replay_fixture = replay_fault = normalize_capture = control_error = 0; vav4l2_set_ioctl_hook(NULL); free(dd);
}
static void delayed_readback(void)
{
    struct VADriverVTable v = {0}; struct VADriverContext va = {.vtable = &v};
    setenv("VAV4L2_PROBE_NOCACHE", "1", 1); setenv("VAV4L2_EXPBUF_EXPORT", "1", 1);
    if (VAV4L2_DRIVER_INIT(&va)) { expect(0, "readback-driver-setup"); return; }
    struct vav4l2_driver_data *dd = va.pDriverData;
    for (unsigned deep = 0; deep < 2; deep++) {
        VASurfaceID ids[2]; VAImage image; VAImageFormat fmt = {.fourcc = deep ? VA_FOURCC_P010 : VA_FOURCC_NV12};
        if (v.vaCreateSurfaces(&va, 16, 16, deep ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420, 2, ids) ||
            v.vaCreateImage(&va, &fmt, 16, 16, &image)) { expect(0, "readback-surface-setup"); continue; }
        uint16_t capture[2][1152]; uint8_t output[64], data[] = {1,2,3,4};
        unsigned stride = deep ? 64 : 32, height = 24, size = stride * height * 3 / 2;
        for (unsigned i = 0; i < 2; i++) for (unsigned j = 0; j < size / (deep ? 2 : 1); j++) {
            unsigned value = (j < stride * height / (deep ? 2 : 1) ? 17 : 73) + i * 31;
            if (deep) capture[i][j] = value << 6; else ((uint8_t *)capture[i])[j] = value;
        }
        struct vav4l2_surface *old = dd->surfaces[ids[0]], *next = dd->surfaces[ids[1]];
        struct vav4l2_context c = {.codec = VAV4L2_CODEC_AV1, .driver_data = dd, .current_surface = next,
            .v4l2_fd = 8, .request_fd = 9, .streamed = 1, .n_free_out = 1, .output_buf_ptr = {output},
            .output_buf_size = sizeof(output), .capture_bufs_allocd = 2, .capture_buf_size = size,
            .cap_stride = stride, .cap_height = height, .cap_pixelformat = deep ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12,
            .capture_buf_ptr = {capture[1], capture[0]}};
        vav4l2_av1_dpb_model_reset(&c); c.av1.model_active = c.av1.grain_checked = 1;
        for (unsigned i = 0; i < 8; i++) c.av1.slot_buf[i] = 1;
        c.av1.buf_owner[1] = ids[0]; old->status = VASurfaceReady;
        expect(!vav4l2_surface_store_capture(&c, old, 1, capture[0], size, 1), "readback-retained-capture-setup");
        VADecPictureParameterBufferAV1 p = picture(16, 16, 1, 1);
        p.primary_ref_frame = 7; p.pic_info_fields.bits.show_frame = 1; p.bit_depth_idx = deep;
        p.seq_info_fields.fields.subsampling_x = p.seq_info_fields.fields.subsampling_y = 1;
        struct vav4l2_buffer b[] = {{.type = VAPictureParameterBufferType, .size = sizeof(p), .data = &p},
            {.type = VASliceDataBufferType, .size = sizeof(data), .data = data}};
        struct vav4l2_buffer *batch[] = {b, b + 1}; capture_pixels = capture[1]; vav4l2_set_ioctl_hook(controls);
        expect(vav4l2_av1_translate(&c, batch, 2) == VA_STATUS_SUCCESS && old->buf_index == -1 && !old->cap_view &&
            c.n_free_cap == 1 && c.free_cap_bufs[0] == 1, "key-refresh-releases-the-old-CAPTURE-slot");
        capture_pixels = NULL; vav4l2_set_ioctl_hook(NULL); memset(capture[0], 0xa5, size);
        expect(v.vaGetImage(&va, ids[0], 0, 0, 16, 16, image.image_id) == VA_STATUS_SUCCESS, "delayed-readback-after-CAPTURE-reuse");
        uint8_t *pixels = NULL;
        if (v.vaMapBuffer(&va, image.buf, (void **)&pixels) == VA_STATUS_SUCCESS) {
            for (unsigned plane = 0; plane < 2; plane++) {
                int same = 1;
                for (unsigned y = 0; y < (plane ? 8u : 16u); y++) for (unsigned x = 0; x < 16; x++) {
                    uint8_t *row = pixels + image.offsets[plane] + y * image.pitches[plane];
                    same &= (deep ? ((uint16_t *)row)[x] : row[x]) == (plane ? 73u : 17u) << (deep ? 6 : 0);
                }
                expect(same, "released-surface-keeps-both-original-pixel-planes");
            }
            v.vaUnmapBuffer(&va, image.buf);
        } else expect(0, "readback-image-map");
        next->buf_index = -1; next->cap_view = NULL; next->capture_ctx = NULL;
        v.vaDestroyImage(&va, image.image_id); v.vaDestroySurfaces(&va, ids, 2);
    }
    v.vaTerminate(&va);
}
struct bits { uint8_t data[128]; unsigned bit; };
static void put(struct bits *b, unsigned value, unsigned n)
{
    while (n--) { b->data[b->bit / 8] |= (value >> n & 1) << (7 - b->bit % 8); b->bit++; }
}
static void metadata(void)
{
    for (unsigned equal = 0; equal < 2; equal++) for (unsigned included = 0; included < 2; included++) {
        struct bits sequence = {0}, frame = {0};
        put(&sequence, 0, 3); put(&sequence, 0, 1); put(&sequence, 0, 1);
        put(&sequence, 1, 1); put(&sequence, 1, 32); put(&sequence, 30, 32);
        put(&sequence, equal, 1); if (equal) put(&sequence, 1, 1); /* UVLC zero */
        put(&sequence, 1, 1); put(&sequence, 3, 5); put(&sequence, 1, 32); put(&sequence, 5, 5); put(&sequence, 4, 5);
        put(&sequence, 0, 1); put(&sequence, 1, 5); /* two operating points */
        for (unsigned i = 0; i < 2; i++) {
            put(&sequence, i ? (1u << (included ? 2 : 1)) | (1u << 9) : 0, 12); put(&sequence, 0, 5);
            put(&sequence, 1, 1); put(&sequence, 7, 4); put(&sequence, 3, 4); put(&sequence, 0, 1);
        }
        put(&sequence, 0, 4); put(&sequence, 0, 4); put(&sequence, 0, 1); put(&sequence, 0, 1);
        put(&sequence, 1, 1); put(&sequence, 1, 4); put(&sequence, 1, 3); /* five frame-id bits */
        put(&sequence, 0, 3); put(&sequence, 0, 4); put(&sequence, 1, 1); put(&sequence, 0, 2);
        put(&sequence, 0, 1); put(&sequence, 0, 1); put(&sequence, 2, 3); /* fixed screen tools off, order hints */
        put(&frame, 0, 1); put(&frame, 1, 2); put(&frame, 1, 1);
        if (!equal) put(&frame, 17, 5);
        put(&frame, 0, 1); put(&frame, 0, 1); put(&frame, 19, 5); put(&frame, 0, 1);
        put(&frame, 3, 3); put(&frame, 2, 3); put(&frame, 1, 1); put(&frame, 37, 6);
        if (included) put(&frame, 12, 6);
        put(&frame, 0x52, 8);
        uint8_t obu[256]; unsigned seq_size = (sequence.bit + 7) / 8, frame_size = (frame.bit + 7) / 8;
        obu[0] = 0x0a; obu[1] = seq_size; memcpy(obu + 2, sequence.data, seq_size);
        unsigned at = seq_size + 2; obu[at++] = 0x1e; obu[at++] = (2 << 5) | (1 << 3); obu[at++] = frame_size;
        memcpy(obu + at, frame.data, frame_size); unsigned size = at + frame_size;
        VADecPictureParameterBufferAV1 p = {0}; p.pic_info_fields.bits.frame_type = 1; p.pic_info_fields.bits.show_frame = 1;
        p.seq_info_fields.fields.enable_order_hint = 1; p.order_hint_bits_minus_1 = 2; p.order_hint = 3; p.primary_ref_frame = 2;
        struct vav4l2_av1_header header = {0}; uint8_t refresh = 0;
        expect(vav4l2_av1_refresh_flags(&header, &p, obu, size, &refresh) == VA_STATUS_SUCCESS && refresh == 0x52 &&
            header.frame_id == 19 && header.removal_present && header.removal_time[0] == 37 && header.removal_time[1] == (included ? 12u : 0u),
            "original-OBU-timing-frame-id-and-operating-point-membership");
        struct vav4l2_av1_header saved = header;
        expect(vav4l2_av1_refresh_flags(&header, &p, obu, size - 1, &refresh) == VA_STATUS_ERROR_UNIMPLEMENTED && !memcmp(&header, &saved, sizeof(header)),
            "truncated-original-metadata-does-not-commit");
        p.primary_ref_frame++;
        expect(vav4l2_av1_refresh_flags(&header, &p, obu, size, &refresh) == VA_STATUS_ERROR_UNIMPLEMENTED && !memcmp(&header, &saved, sizeof(header)),
            "VA-and-original-header-mismatch-does-not-commit");
    }
}
int main(void)
{
    geometry(); payload(); reject_before_ioctl(); completion(); normalized_references(); metadata(); delayed_readback(); deferred_references();
    printf("AV1 tile failures: %d\n", failures);
    return failures != 0;
}
