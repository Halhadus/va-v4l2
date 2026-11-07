/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdio.h>
#include "codecs/bytestream.h"
#include "codecs/rbsp.h"
static unsigned submits, pulls, controls, slice_mode, cabac, fail_submit;
static int hook(int fd, unsigned long request, void *arg)
{
    (void)fd;
    if (request == VIDIOC_S_EXT_CTRLS) {
        struct v4l2_ext_controls *batch = arg;
        for (unsigned i = 0; i < batch->count; i++) {
            struct v4l2_ext_control *c = &batch->controls[i];
            if (c->id == V4L2_CID_STATELESS_H264_SPS) {
                const struct v4l2_ctrl_h264_sps *s = c->ptr;
                assert(s->pic_order_cnt_type == 0 && s->log2_max_pic_order_cnt_lsb_minus4 == 12); controls++;
            }
            if (c->id == V4L2_CID_STATELESS_H264_PPS) assert(((const struct v4l2_ctrl_h264_pps *)c->ptr)->flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT);
            if (c->id == V4L2_CID_STATELESS_H264_DECODE_PARAMS) {
                const struct v4l2_ctrl_h264_decode_params *d = c->ptr;
                assert(d->pic_order_cnt_lsb == 0 && d->top_field_order_cnt == 0 && d->bottom_field_order_cnt == 0 && d->pic_order_cnt_bit_size == 17);
            }
        }
    }
    return 0;
}
int __wrap_vav4l2_ensure_capture(struct vav4l2_context *ctx, int w, int h, uint32_t format)
{
    (void)ctx; assert(w == 16 && h == 16 && format == V4L2_PIX_FMT_NV12); return 0;
}
static int submitted(struct vav4l2_context *ctx, int index, uint32_t bytes, uint64_t timestamp)
{
    assert(timestamp == 42 && bytes > 5);
    const uint8_t *data = ctx->output_buf_ptr[index]; unsigned prefix = slice_mode ? 0 : 4;
    assert(data[prefix] == 0x65);
    struct rbsp_reader r; assert(!rbsp_open(&r, data + prefix + 1, bytes - prefix - 1));
    assert(rbsp_ue(&r) == 0 && rbsp_ue(&r) == 2 && rbsp_ue(&r) == 3 && rbsp_get(&r, 4) == 0 && rbsp_ue(&r) == 0);
    assert(rbsp_get(&r, 16) == 0 && rbsp_ue(&r) == 0 && rbsp_get(&r, 2) == 0 && rbsp_ue(&r) == 0);
    if (cabac) while (r.bit % 8) assert(rbsp_get(&r, 1) == 1);
    assert(rbsp_get(&r, 8) == 0xa5 && !r.error); free(r.data);
    submits++; vav4l2_out_pool_push(ctx, index); return fail_submit ? -2 : 0;
}
int __wrap_vav4l2_decode_submit(struct vav4l2_context *ctx, int index, uint32_t bytes, uint64_t timestamp)
{
    assert(!slice_mode); return submitted(ctx, index, bytes, timestamp);
}
int __wrap_vav4l2_decode_submit_field(struct vav4l2_context *ctx, int index, uint32_t bytes, uint64_t timestamp, int capture, int hold)
{
    assert(slice_mode && capture == -1 && !hold); return submitted(ctx, index, bytes, timestamp);
}
int __wrap_vav4l2_surface_pull_capture(struct vav4l2_context *ctx, struct vav4l2_surface *s, int index)
{
    (void)ctx; assert(index == 0); s->has_pic = 1; pulls++; return 0;
}
int main(void)
{
    vav4l2_set_ioctl_hook(hook);
    for (slice_mode = 0; slice_mode < 2; slice_mode++) for (cabac = 0; cabac < 2; cabac++) {
        struct bits b = {0}; uint8_t data[64], output[128]; size_t bytes = 0;
        ue(&b, 0); ue(&b, 2); ue(&b, 3); put(&b, 4, 0); ue(&b, 0); put(&b, 2, 0); se(&b, 0);
        unsigned header = b.bit + 8;
        if (cabac) while (b.bit % 8) put(&b, 1, 1);
        put(&b, 8, 0xa5); assert(!nal(&b, 5, 0, data, sizeof(data), &bytes));
        VAPictureParameterBufferH264 pic = {0};
        pic.seq_fields.bits.pic_order_cnt_type = 1; pic.seq_fields.bits.delta_pic_order_always_zero_flag = 1;
        pic.seq_fields.bits.frame_mbs_only_flag = pic.seq_fields.bits.chroma_format_idc = pic.seq_fields.bits.direct_8x8_inference_flag = 1;
        pic.pic_fields.bits.reference_pic_flag = 1; pic.pic_fields.bits.entropy_coding_mode_flag = cabac;
        for (unsigned i = 0; i < 16; i++) pic.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
        VASliceParameterBufferH264 slice = {.slice_type = 2, .slice_data_size = bytes, .slice_data_bit_offset = header};
        struct vav4l2_buffer buffers[] = {{.type = VAPictureParameterBufferType, .data = &pic, .size = sizeof(pic)},
            {.type = VASliceParameterBufferType, .data = &slice, .size = sizeof(slice)}, {.type = VASliceDataBufferType, .data = data, .size = bytes}};
        struct vav4l2_buffer *batch[] = {buffers, buffers + 1, buffers + 2};
        struct vav4l2_surface surface = {.timestamp = 42}; struct vav4l2_driver_data dd = {.surfaces = {&surface}};
        struct vav4l2_context ctx = {.codec = VAV4L2_CODEC_H264, .profile = VAProfileH264High, .width = 16, .height = 16,
            .driver_data = &dd, .current_surface = &surface, .v4l2_fd = 99, .request_fd = 100, .media_fd = -1,
            .output_buf_ptr = {output}, .output_buf_size = sizeof(output), .output_bufs_allocd = 1, .n_free_out = 1,
            .request_slice_based = slice_mode, .request_no_start_code = slice_mode};
        unsigned before = submits; fail_submit = 1;
        assert(vav4l2_h264_translate(&ctx, batch, 3) == VA_STATUS_ERROR_DECODING_ERROR && !ctx.packet_state.h264_valid && submits == before + 1);
        fail_submit = 0;
        assert(!vav4l2_h264_translate(&ctx, batch, 3) && ctx.packet_state.h264_valid && submits == before + 2);
        assert(pic.seq_fields.bits.pic_order_cnt_type == 1 && slice.slice_data_size == bytes && !memcmp(buffers[2].data, data, bytes));
        /* An IDR cannot begin at a resolved POC with a different origin. */
        pic.CurrPic.TopFieldOrderCnt = 65536; before = submits;
        assert(vav4l2_h264_translate(&ctx, batch, 3) == VA_STATUS_ERROR_UNIMPLEMENTED && submits == before);
        vav4l2_packet_state_release(&ctx.packet_state);
    }
    assert(pulls == 4 && controls >= 8); vav4l2_set_ioctl_hook(NULL);
    puts("H264 POC1 frame/slice controls, CAVLC/CABAC entropy and transactional state: PASS"); return 0;
}
