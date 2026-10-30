#define _GNU_SOURCE
#include "decoder.h"
#include "h264_parse.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>

/* Native V4L2 stateless H264 decoder for Allwinner Cedrus (H616/H618 mainline).
 * Cedrus is "slice based, no start code": every slice NAL (header byte included, no
 * 00 00 01) goes in its own OUTPUT buffer together with parsed SPS/PPS/slice
 * controls bound to a media request. Output is NV12 on CAPTURE. */

#define OUT_N 3
#define CAP_N 8
#define MAX_REFS 16

typedef struct { void *map; size_t len; } mbuf;
typedef struct { int valid, cap, frame_num, wrap; int32_t poc; uint64_t ts; } ref;

typedef struct {
    int vfd, mfd;
    int capfd[CAP_N];
    mbuf out[OUT_N]; int req[OUT_N];
    mbuf cap[CAP_N]; int cap_busy[CAP_N];
    int stride, width, height, ready;
    h264_sps sps[32]; int sps_ok[32];
    h264_pps pps[256]; int pps_ok[256];
    int active_sps;
    ref refs[MAX_REFS];
    uint64_t next_ts;
    int out_i, prefix;            /* prefix = bytes of start code placed before each NAL */
    int prev_poc_msb, prev_poc_lsb, prev_fn, prev_fn_off;
    decoder *self;
    /* per-packet scratch */
    struct { const uint8_t *nal; size_t len; } slices[16]; int n_slices;
    int fatal;
} v4l2d;

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

static int find_media(void)
{
    for (int i = 0; i < 16; i++) {
        char path[32]; snprintf(path, sizeof path, "/dev/media%d", i);
        int fd = open(path, O_RDWR);
        if (fd < 0) continue;
        struct media_device_info info;
        if (!xioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) && !strcmp(info.driver, "cedrus")) return fd;
        close(fd);
    }
    return -1;
}

static int dev_setup(v4l2d *d, const h264_sps *sps)
{
    d->width = sps->width; d->height = sps->height;
    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
    f.fmt.pix.width = d->width; f.fmt.pix.height = d->height;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_H264_SLICE;
    f.fmt.pix.sizeimage = 0x80000;
    if (xioctl(d->vfd, VIDIOC_S_FMT, &f)) { perror("S_FMT output"); return -1; }
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = d->width; f.fmt.pix.height = d->height;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(d->vfd, VIDIOC_S_FMT, &f)) { perror("S_FMT capture"); return -1; }
    d->stride = f.fmt.pix.bytesperline;

    struct v4l2_ext_control c[2] = {
        { .id = V4L2_CID_STATELESS_H264_DECODE_MODE, .value = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED },
        { .id = V4L2_CID_STATELESS_H264_START_CODE, .value = V4L2_STATELESS_H264_START_CODE_NONE } };
    struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_CUR_VAL, .count = 2, .controls = c };
    if (xioctl(d->vfd, VIDIOC_S_EXT_CTRLS, &cs)) perror("S_EXT_CTRLS mode (ignored)");

    struct v4l2_requestbuffers rb = { .count = OUT_N, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(d->vfd, VIDIOC_REQBUFS, &rb)) { perror("REQBUFS out"); return -1; }
    for (int i = 0; i < OUT_N; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (xioctl(d->vfd, VIDIOC_QUERYBUF, &b)) { perror("QUERYBUF out"); return -1; }
        d->out[i].len = b.length;
        d->out[i].map = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, d->vfd, b.m.offset);
        if (d->out[i].map == MAP_FAILED) { perror("mmap out"); return -1; }
        if (xioctl(d->mfd, MEDIA_IOC_REQUEST_ALLOC, &d->req[i])) { perror("REQUEST_ALLOC"); return -1; }
    }
    rb = (struct v4l2_requestbuffers){ .count = CAP_N, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(d->vfd, VIDIOC_REQBUFS, &rb) || rb.count < 4) { perror("REQBUFS cap"); return -1; }
    for (int i = 0; i < CAP_N; i++) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (xioctl(d->vfd, VIDIOC_QUERYBUF, &b)) { perror("QUERYBUF cap"); return -1; }
        d->cap[i].len = b.length;
        d->cap[i].map = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, d->vfd, b.m.offset);
        if (d->cap[i].map == MAP_FAILED) { perror("mmap cap"); return -1; }
        struct v4l2_exportbuffer eb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = i, .flags = O_RDONLY | O_CLOEXEC };
        d->capfd[i] = xioctl(d->vfd, VIDIOC_EXPBUF, &eb) ? -1 : eb.fd;
    }
    int ty = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(d->vfd, VIDIOC_STREAMON, &ty)) { perror("STREAMON out"); return -1; }
    ty = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(d->vfd, VIDIOC_STREAMON, &ty)) { perror("STREAMON cap"); return -1; }
    d->ready = 1;
    fprintf(stderr, "cedrus: %dx%d NV12 stride %d, %d capture buffers\n", d->width, d->height, d->stride, CAP_N);
    return 0;
}

static int free_cap(v4l2d *d)
{
    for (int i = 0; i < CAP_N; i++) if (!d->cap_busy[i]) return i;
    return -1;
}

static void drop_ref(v4l2d *d, ref *r)
{
    if (r->valid) d->cap_busy[r->cap] = 0;
    r->valid = 0;
}

static void on_nal(void *ctx, const uint8_t *nal, size_t len)
{
    v4l2d *d = ctx;
    int type = nal[0] & 0x1F;
    if (type == 7) {
        h264_sps s; int r = h264_parse_sps(nal, len, &s);
        if (r) { fprintf(stderr, "cedrus: unsupported SPS (%d)\n", r); d->fatal = 1; return; }
        int id = s.v4l2.seq_parameter_set_id;
        if (id < 32) { d->sps[id] = s; d->sps_ok[id] = 1; }
        if (!d->ready && dev_setup(d, &s)) d->fatal = 1;
    } else if (type == 8) {
        h264_pps p; int r = h264_parse_pps(nal, len, &p);
        if (r) { fprintf(stderr, "cedrus: unsupported PPS (%d)\n", r); d->fatal = 1; return; }
        d->pps[p.pps_id] = p; d->pps_ok[p.pps_id] = 1;
    } else if (type == 1 || type == 5) {
        if (d->n_slices < 16) { d->slices[d->n_slices].nal = nal; d->slices[d->n_slices].len = len; d->n_slices++; }
    }
}

static int compute_poc(v4l2d *d, const h264_sps *sps, const h264_slice *s, int32_t *top, int32_t *bottom)
{
    int maxfn = 1 << sps->log2_max_frame_num;
    if (sps->poc_type == 0) {
        int maxlsb = 1 << sps->log2_max_poc_lsb, msb;
        if (s->is_idr) { d->prev_poc_msb = 0; d->prev_poc_lsb = 0; }
        if (s->poc_lsb < d->prev_poc_lsb && d->prev_poc_lsb - s->poc_lsb >= maxlsb / 2) msb = d->prev_poc_msb + maxlsb;
        else if (s->poc_lsb > d->prev_poc_lsb && s->poc_lsb - d->prev_poc_lsb > maxlsb / 2) msb = d->prev_poc_msb - maxlsb;
        else msb = d->prev_poc_msb;
        *top = msb + s->poc_lsb;
        *bottom = *top + s->delta_bottom;
        if (s->nal_ref_idc) { d->prev_poc_msb = msb; d->prev_poc_lsb = s->poc_lsb; }
        return 0;
    }
    if (sps->poc_type == 2) {
        int off;
        if (s->is_idr) off = 0;
        else off = d->prev_fn > s->frame_num ? d->prev_fn_off + maxfn : d->prev_fn_off;
        int t = s->is_idr ? 0 : 2 * (off + s->frame_num) - (s->nal_ref_idc ? 0 : 1);
        d->prev_fn = s->frame_num; d->prev_fn_off = off;
        *top = *bottom = t;
        return 0;
    }
    return -1;
}

static struct v4l2_ext_control mkctrl(unsigned id, const void *p, size_t size)
{
    struct v4l2_ext_control c = { .id = id, .size = size };
    c.ptr = (void *)p;
    return c;
}

static int wait_request(int rfd)
{
    struct pollfd p = { rfd, POLLPRI, 0 };
    int r = poll(&p, 1, 500);
    return r > 0 ? 0 : -1;
}

static int decode_picture(v4l2d *d, int ns)
{
    /* All slices of one picture share the ts and the capture buffer */
    h264_slice sl[16];
    for (int i = 0; i < ns; i++) {
        int pid = h264_peek_pps_id(d->slices[i].nal, d->slices[i].len);
        if (pid < 0 || !d->pps_ok[pid] || !d->sps_ok[d->pps[pid].sps_id]) { fprintf(stderr, "cedrus: slice without sps/pps\n"); return -1; }
        const h264_sps *sps = &d->sps[d->pps[pid].sps_id];
        int r = h264_parse_slice(d->slices[i].nal, d->slices[i].len, sps, d->pps, &sl[i]);
        if (r) { fprintf(stderr, "cedrus: slice parse failed (%d)\n", r); return -1; }
    }
    const h264_slice *s0 = &sl[0];
    const h264_pps *pps = &d->pps[s0->pps_id];
    const h264_sps *sps = &d->sps[pps->sps_id];
    if (s0->slice_type == V4L2_H264_SLICE_TYPE_B) { fprintf(stderr, "cedrus: B slices are not supported yet\n"); return -1; }

    if (s0->is_idr) for (int i = 0; i < MAX_REFS; i++) drop_ref(d, &d->refs[i]);
    if (s0->mmco_present) fprintf(stderr, "cedrus: MMCO is not supported, using sliding window\n");

    int maxfn = 1 << sps->log2_max_frame_num;
    int32_t top, bottom;
    if (compute_poc(d, sps, s0, &top, &bottom)) { fprintf(stderr, "cedrus: unsupported poc type\n"); return -1; }

    int cap = free_cap(d);
    if (cap < 0) { fprintf(stderr, "cedrus: no free capture buffer\n"); return -1; }
    uint64_t ts = ++d->next_ts;

    struct v4l2_ctrl_h264_decode_params dp;
    memset(&dp, 0, sizeof dp);
    int nrefs = 0, idx_of[MAX_REFS];
    ref sorted[MAX_REFS];
    for (int i = 0; i < MAX_REFS; i++) {
        idx_of[i] = -1;
        ref *r = &d->refs[i];
        if (!r->valid) continue;
        r->wrap = r->frame_num > s0->frame_num ? r->frame_num - maxfn : r->frame_num;
        dp.dpb[i] = (struct v4l2_h264_dpb_entry){ .reference_ts = r->ts * 1000000000ull, .pic_num = (uint32_t)r->wrap,
            .frame_num = r->frame_num, .fields = V4L2_H264_FRAME_REF, .top_field_order_cnt = r->poc,
            .bottom_field_order_cnt = r->poc, .flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE };
        sorted[nrefs] = *r; idx_of[nrefs] = i; nrefs++;
    }
    dp.nal_ref_idc = s0->nal_ref_idc; dp.frame_num = s0->frame_num;
    dp.top_field_order_cnt = top; dp.bottom_field_order_cnt = bottom;
    dp.idr_pic_id = s0->idr_pic_id; dp.pic_order_cnt_lsb = s0->poc_lsb;
    dp.delta_pic_order_cnt_bottom = s0->delta_bottom;
    dp.delta_pic_order_cnt0 = s0->delta0; dp.delta_pic_order_cnt1 = s0->delta1;
    dp.dec_ref_pic_marking_bit_size = s0->marking_bits; dp.pic_order_cnt_bit_size = s0->poc_bits;
    if (s0->is_idr) dp.flags |= V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
    if (s0->slice_type == V4L2_H264_SLICE_TYPE_P) dp.flags |= V4L2_H264_DECODE_PARAM_FLAG_PFRAME;

    /* default P list: descending FrameNumWrap, dpb index through idx_of */
    int order[MAX_REFS];
    {
        struct { ref r; int dpb; } tmp[MAX_REFS];
        for (int i = 0; i < nrefs; i++) { tmp[i].r = sorted[i]; tmp[i].dpb = idx_of[i]; }
        for (int i = 0; i < nrefs; i++) for (int j = i + 1; j < nrefs; j++)
            if (tmp[j].r.wrap > tmp[i].r.wrap) { typeof(tmp[0]) t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
        for (int i = 0; i < nrefs; i++) order[i] = tmp[i].dpb;
    }

    int fatal = 0;
    for (int k = 0; k < ns && !fatal; k++) {
        const h264_slice *s = &sl[k];
        struct v4l2_ctrl_h264_slice_params sp;
        memset(&sp, 0, sizeof sp);
        sp.header_bit_size = 8 + d->prefix * 8 + s->header_bits;
        sp.first_mb_in_slice = s->first_mb; sp.slice_type = s->slice_type;
        sp.redundant_pic_cnt = s->redundant_pic_cnt; sp.cabac_init_idc = s->cabac_init_idc;
        sp.slice_qp_delta = s->qp_delta; sp.slice_qs_delta = s->qs_delta;
        sp.disable_deblocking_filter_idc = s->disable_deblock;
        sp.slice_alpha_c0_offset_div2 = s->alpha_off; sp.slice_beta_offset_div2 = s->beta_off;
        sp.num_ref_idx_l0_active_minus1 = s->num_ref_l0 - 1; sp.num_ref_idx_l1_active_minus1 = s->num_ref_l1 - 1;
        if (s->direct_spatial) sp.flags |= V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED;
        if (s->sp_for_switch) sp.flags |= V4L2_H264_SLICE_FLAG_SP_FOR_SWITCH;

        if (s->slice_type == V4L2_H264_SLICE_TYPE_P || s->slice_type == V4L2_H264_SLICE_TYPE_SP) {
            int list[33], n = s->num_ref_l0;
            for (int i = 0; i < n; i++) list[i] = i < nrefs ? order[i] : (nrefs ? order[nrefs - 1] : 0);
            int pred = s->frame_num, ri = 0;
            for (int m = 0; m < s->n_mod[0]; m++) {
                const h264_mod *md = &s->mod[0][m];
                if (md->idc > 1) { fprintf(stderr, "cedrus: long-term ref reordering unsupported\n"); return -1; }
                int delta = (int)md->val + 1;
                pred = md->idc == 0 ? pred - delta : pred + delta;
                if (pred < 0) pred += maxfn;
                if (pred >= maxfn) pred -= maxfn;
                int pn = pred > s->frame_num ? pred - maxfn : pred, found = -1;
                for (int i = 0; i < MAX_REFS; i++) if (d->refs[i].valid && d->refs[i].wrap == pn) found = i;
                if (found < 0 || ri >= n) continue;
                for (int i = n; i > ri; i--) list[i] = list[i - 1];
                list[ri++] = found;
                int w = ri;
                for (int i = ri; i <= n; i++) if (list[i] != found) list[w++] = list[i];
            }
            for (int i = 0; i < n; i++) { sp.ref_pic_list0[i].fields = V4L2_H264_FRAME_REF; sp.ref_pic_list0[i].index = list[i]; }
        }

        /* controls bound to this slice's request */
        struct v4l2_ctrl_h264_scaling_matrix sm; memset(&sm, 0, sizeof sm);
        struct v4l2_ext_control c[6]; int nc = 0;
        c[nc++] = mkctrl(V4L2_CID_STATELESS_H264_SPS, &sps->v4l2, sizeof sps->v4l2);
        c[nc++] = mkctrl(V4L2_CID_STATELESS_H264_PPS, &pps->v4l2, sizeof pps->v4l2);
        c[nc++] = mkctrl(V4L2_CID_STATELESS_H264_SCALING_MATRIX, &sm, sizeof sm);
        c[nc++] = mkctrl(V4L2_CID_STATELESS_H264_DECODE_PARAMS, &dp, sizeof dp);
        c[nc++] = mkctrl(V4L2_CID_STATELESS_H264_SLICE_PARAMS, &sp, sizeof sp);
        if (s->has_pred_weights) c[nc++] = mkctrl(V4L2_CID_STATELESS_H264_PRED_WEIGHTS, &s->pw, sizeof s->pw);
        int oi = d->out_i; d->out_i = (d->out_i + 1) % OUT_N;
        struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_REQUEST_VAL, .count = nc, .controls = c, .request_fd = d->req[oi] };
        if (xioctl(d->vfd, VIDIOC_S_EXT_CTRLS, &cs)) { perror("S_EXT_CTRLS request"); return -1; }

        if (k == 0) {
            struct v4l2_buffer cb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = cap };
            if (xioctl(d->vfd, VIDIOC_QBUF, &cb)) { perror("QBUF cap"); return -1; }
        }
        size_t off = 0;
        uint8_t *dst = d->out[oi].map;
        if (d->prefix) { memcpy(dst, "\0\0\1", 3); off = 3; }
        memcpy(dst + off, d->slices[k].nal, d->slices[k].len);
        struct v4l2_buffer ob = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP, .index = oi,
            .bytesused = off + d->slices[k].len, .flags = V4L2_BUF_FLAG_REQUEST_FD | (k < ns - 1 ? V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF : 0),
            .request_fd = d->req[oi] };
        ob.timestamp.tv_sec = (long)ts;
        if (xioctl(d->vfd, VIDIOC_QBUF, &ob)) { perror("QBUF out"); return -1; }
        if (xioctl(d->req[oi], MEDIA_REQUEST_IOC_QUEUE, NULL)) { perror("REQUEST_QUEUE"); return -1; }
        if (wait_request(d->req[oi])) { fprintf(stderr, "cedrus: decode timeout\n"); fatal = 1; }
        struct v4l2_buffer db = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_MMAP };
        xioctl(d->vfd, VIDIOC_DQBUF, &db);
        xioctl(d->req[oi], MEDIA_REQUEST_IOC_REINIT, NULL);
    }
    if (fatal) return -1;

    struct v4l2_buffer cb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (xioctl(d->vfd, VIDIOC_DQBUF, &cb)) { perror("DQBUF cap"); return -1; }
    if (cb.flags & V4L2_BUF_FLAG_ERROR) fprintf(stderr, "cedrus: frame decoded with error flag\n");

    frame f = { .width = d->width, .height = d->height, .fmt = PIX_NV12, .dmabuf_fd = d->capfd[cb.index], .buf_id = cb.index, .uncached = 1, .ts = ts };
    f.data[0] = d->cap[cb.index].map; f.stride[0] = d->stride;
    f.data[1] = f.data[0] + (size_t)d->stride * d->height; f.stride[1] = d->stride;
    d->self->cb(d->self->cb_ctx, &f);

    /* reference marking: sliding window */
    if (s0->nal_ref_idc) {
        int count = 0, oldest = -1;
        for (int i = 0; i < MAX_REFS; i++) if (d->refs[i].valid) {
            count++;
            if (oldest < 0 || d->refs[i].wrap < d->refs[oldest].wrap) oldest = i;
        }
        int maxr = sps->v4l2.max_num_ref_frames ? sps->v4l2.max_num_ref_frames : 1;
        if (count >= maxr && oldest >= 0) drop_ref(d, &d->refs[oldest]);
        for (int i = 0; i < MAX_REFS; i++) if (!d->refs[i].valid) {
            d->refs[i] = (ref){ 1, cb.index, s0->frame_num, 0, top, ts };
            d->cap_busy[cb.index] = 1;
            break;
        }
    }
    return 0;
}

static int v4l2_decode(decoder *dd, const uint8_t *data, size_t len, uint64_t ts)
{
    v4l2d *d = dd->priv;
    if (d->fatal) return -1;
    d->n_slices = 0;
    h264_split_annexb(data, len, on_nal, d);
    if (d->fatal) return -1;
    if (d->n_slices && d->ready) return decode_picture(d, d->n_slices);
    return 0;
}

static void v4l2_close(decoder *dd)
{
    v4l2d *d = dd->priv;
    if (d->vfd >= 0) {
        int t = V4L2_BUF_TYPE_VIDEO_OUTPUT; xioctl(d->vfd, VIDIOC_STREAMOFF, &t);
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE; xioctl(d->vfd, VIDIOC_STREAMOFF, &t);
        close(d->vfd);
    }
    if (d->mfd >= 0) close(d->mfd);
    free(d);
}

decoder *decoder_v4l2_new(const decoder_opts *o)
{
    const char *path = o->device ? o->device : "/dev/video0";
    v4l2d *d = calloc(1, sizeof *d);
    d->vfd = open(path, O_RDWR | O_CLOEXEC);
    d->mfd = find_media();
    if (d->vfd < 0 || d->mfd < 0) {
        fprintf(stderr, "v4l2: cannot open %s / cedrus media device\n", path);
        if (d->vfd >= 0) close(d->vfd);
        free(d);
        return NULL;
    }
    d->prefix = getenv("SYSDVR_STARTCODE") ? 3 : 0;      /* experiment knob, see docs */
    decoder *dd = calloc(1, sizeof *dd);
    dd->decode = v4l2_decode; dd->close = v4l2_close; dd->priv = d; d->self = dd;
    return dd;
}
