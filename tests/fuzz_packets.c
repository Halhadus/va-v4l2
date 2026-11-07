/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Bounded structured parser input; no device fd or serialized pointer is used. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "stateful_fixture.h"
#include "codecs/bytestream.h"
static int no_device(int fd, unsigned long request, void *arg)
{
    (void)fd; (void)request; (void)arg; assert(!"packet parser issued a device ioctl"); return -1;
}
extern VAStatus VAV4L2_DRIVER_INIT(VADriverContextP);
int __wrap_vav4l2_scan_all_cached(char *h264, char *hevc, char *av1, char *vp8, char *mpeg2, char *jpeg, char *vpp, char *vp9, unsigned len)
{
    char *paths[] = {h264, hevc, av1, vp8, mpeg2, jpeg, vpp, vp9};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) if (paths[i] && len) paths[i][0] = 0;
    return 0;
}
static VAContextID api_context(struct vav4l2_driver_data *dd)
{
    struct vav4l2_context *c = calloc(1, sizeof(*c)); if (!c) return VA_INVALID_ID;
    c->context_id = ++dd->next_context_id; c->codec = VAV4L2_CODEC_VPP; c->entrypoint = VAEntrypointVideoProc;
    c->driver_data = dd; c->width = c->height = 8; c->v4l2_fd = c->media_fd = c->request_fd = -1;
    c->next = dd->contexts; dd->contexts = c; return c->context_id;
}
static void api_lifecycle(const uint8_t *input, size_t size)
{
    struct VADriverVTable v = {0}; struct VADriverContext driver = {.vtable = &v};
    if (VAV4L2_DRIVER_INIT(&driver)) return;
    struct vav4l2_driver_data *dd = driver.pDriverData;
    VAContextID contexts[2] = {api_context(dd), api_context(dd)};
    VASurfaceID surfaces[4]; VAImage images[4]; unsigned live[4] = {0};
    for (unsigned i = 0; i < 4; i++) { surfaces[i] = VA_INVALID_ID; images[i].image_id = VA_INVALID_ID; }
    for (size_t i = 1; i < size && i < 257; i++) {
        unsigned op = input[i] % 13, slot = input[i] / 13 % 4, context = input[i] / 52 % 2;
        if (op == 0 && surfaces[slot] == VA_INVALID_ID) (void)v.vaCreateSurfaces(&driver, 8, 8, VA_RT_FORMAT_YUV420, 1, surfaces + slot);
        if (op == 1 && !v.vaDestroySurfaces(&driver, surfaces + slot, 1)) surfaces[slot] = VA_INVALID_ID;
        if (op == 2) (void)v.vaSyncSurface2(&driver, surfaces[slot], 0);
        if (op == 3) (void)v.vaBeginPicture(&driver, contexts[context], surfaces[slot]);
        if (op == 4) (void)v.vaEndPicture(&driver, contexts[context]);
        if (op == 5 && !live[slot]) {
            const uint32_t formats[] = {VA_FOURCC_NV12, VA_FOURCC_P010, VA_FOURCC_YUY2, VA_FOURCC_RGBA};
            VAImageFormat format = {.fourcc = formats[input[i] / 13 % 4]};
            if (!v.vaCreateImage(&driver, &format, 8, 8, images + slot)) live[slot] = 1;
        }
        if (op == 6 && live[slot] && !v.vaDestroyImage(&driver, images[slot].image_id)) live[slot] = 0;
        if (op == 7 && live[slot]) {
            uint8_t *bytes = NULL;
            if (!v.vaMapBuffer(&driver, images[slot].buf, (void **)&bytes)) { if (bytes && images[slot].data_size) bytes[0] ^= input[i]; (void)v.vaUnmapBuffer(&driver, images[slot].buf); }
        }
        if (op == 8 && live[slot]) (void)v.vaPutImage(&driver, surfaces[slot], images[slot].image_id, 0, 0, 8, 8, 0, 0, 8, 8);
        if (op == 9 && live[slot]) (void)v.vaGetImage(&driver, surfaces[slot], 0, 0, 8, 8, images[slot].image_id);
        if (op == 10 && !live[slot] && !v.vaDeriveImage(&driver, surfaces[slot], images + slot)) live[slot] = 1;
        if (op == 11) { void *error = NULL; (void)v.vaQuerySurfaceError(&driver, surfaces[slot], VA_STATUS_ERROR_DECODING_ERROR, &error); }
        if (op == 12 && !v.vaDestroyContext(&driver, contexts[context])) contexts[context] = api_context(dd);
    }
    assert(!v.vaTerminate(&driver));
}
int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size)
{
    if (!size || size > 65536) return 0;
    if (input[0] % 4 == 3) { vav4l2_set_ioctl_hook(no_device); api_lifecycle(input, size); vav4l2_set_ioctl_hook(NULL); return 0; }
    struct stateful_fixture f; stateful_fixture(&f);
    VAPictureParameterBufferHEVC hevc = {.pic_width_in_luma_samples = 16, .pic_height_in_luma_samples = 16};
    VASliceParameterBufferHEVC hs = {.slice_data_size = 8, .slice_data_byte_offset = 3};
    VADecPictureParameterBufferAV1 av1 = {.frame_width_minus1 = 127, .frame_height_minus1 = 127, .tile_cols = 1, .tile_rows = 1, .primary_ref_frame = 7};
    VASliceParameterBufferAV1 tile = {.slice_data_size = 8};
    struct vav4l2_context ctx = {.codec = VAV4L2_CODEC_H264, .profile = VAProfileH264High, .width = 16, .height = 16};
    if (input[0] % 3 == 1) {
        ctx.codec = VAV4L2_CODEC_HEVC; ctx.profile = VAProfileHEVCMain;
        hevc.pic_fields.bits.chroma_format_idc = 1; hevc.log2_diff_max_min_luma_coding_block_size = 1;
        hevc.slice_parsing_fields.bits.IdrPicFlag = hevc.slice_parsing_fields.bits.RapPicFlag = 1; hs.LongSliceFlags.fields.slice_type = 2;
        for (unsigned i = 0; i < 15; i++) hevc.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
        f.buffers[0].data = &hevc; f.buffers[0].size = sizeof(hevc); f.buffers[1].data = &hs; f.buffers[1].size = sizeof(hs);
        memcpy(f.nal, (uint8_t[]){0,0,0,1,0x26,1,0xaf,0x80}, 8);
    } else if (input[0] % 3 == 2) {
        ctx.codec = VAV4L2_CODEC_AV1; ctx.profile = VAProfileAV1Profile0; ctx.width = ctx.height = 128;
        av1.seq_info_fields.fields.subsampling_x = av1.seq_info_fields.fields.subsampling_y = 1;
        av1.pic_info_fields.bits.show_frame = av1.pic_info_fields.bits.uniform_tile_spacing_flag = 1;
        f.buffers[0].data = &av1; f.buffers[0].size = sizeof(av1); f.buffers[1].data = &tile; f.buffers[1].size = sizeof(tile);
    }
    for (size_t i = 1; i + 2 < size; i += 3) {
        unsigned role = input[i] % 3;
        struct vav4l2_buffer *b = f.buffers + role;
        ((uint8_t *)b->data)[input[i + 1] % b->size] ^= input[i + 2];
    }
    uint8_t output[16384], poison[16384]; memset(output, 0xa5, sizeof(output)); memcpy(poison, output, sizeof(output));
    size_t bytes = 17; struct vav4l2_packet_state next = {0};
    vav4l2_set_ioctl_hook(no_device);
    VAStatus status = vav4l2_stateful_packet_prepare(&ctx, f.batch, 3, output, sizeof(output), &bytes, &next, NULL);
    if (status == VA_STATUS_SUCCESS) { assert(bytes && bytes <= sizeof(output)); vav4l2_packet_state_release(&next); }
    else assert(bytes == 17 && !next.av1 && !memcmp(output, poison, sizeof(output)));
    assert(!ctx.packet_state.h264_valid && !ctx.packet_state.hevc_started && !ctx.packet_state.av1);
    vav4l2_set_ioctl_hook(NULL); return 0;
}
#ifdef VAV4L2_FUZZ_SEEDS
int main(int argc, char **argv)
{
    if (argc == 1) {
        const uint8_t lifecycle[] = {3,0,5,7,8,6,10,3,1,4,12,9,6,1}; LLVMFuzzerTestOneInput(lifecycle, sizeof(lifecycle));
        for (unsigned codec = 0; codec < 4; codec++) {
            uint8_t seed[4] = {codec, 0, 0, 0}; LLVMFuzzerTestOneInput(seed, 1);
            for (unsigned i = 0; i < 256; i++) { seed[1] = i % 3; seed[2] = i; seed[3] = 1u << (i % 8); LLVMFuzzerTestOneInput(seed, sizeof(seed)); }
        }
    }
    for (int i = 1; i < argc; i++) {
        FILE *file = fopen(argv[i], "rb"); if (!file) { perror(argv[i]); return 1; }
        uint8_t data[65537]; size_t size = fread(data, 1, sizeof(data), file); int failed = ferror(file); if (fclose(file)) failed = 1;
        if (failed || size > 65536) return 1;
        LLVMFuzzerTestOneInput(data, size);
    }
    return 0;
}
#endif
