#include "h264_parse.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *d; size_t n; size_t pos; int err; } br;

static int unescape(const uint8_t *src, size_t n, uint8_t **out, size_t *outn)
{
    uint8_t *d = malloc(n + 8);
    if (!d) return -1;
    size_t o = 0; int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && src[i] == 3) { zeros = 0; continue; }
        zeros = src[i] == 0 ? zeros + 1 : 0;
        d[o++] = src[i];
    }
    memset(d + o, 0, 8);
    *out = d; *outn = o;
    return 0;
}

static unsigned rbit(br *b)
{
    if ((b->pos >> 3) >= b->n) { b->err = 1; return 0; }
    unsigned v = (b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}
static uint32_t rbits(br *b, int n) { uint32_t v = 0; while (n--) v = v << 1 | rbit(b); return v; }
static uint32_t rue(br *b)
{
    int z = 0;
    while (!rbit(b) && !b->err && z < 32) z++;
    if (z >= 32) { b->err = 1; return 0; }      /* not a valid code, and 1u << 32 is undefined */
    return z ? ((1u << z) - 1 + rbits(b, z)) : 0;
}
static int32_t rse(br *b)
{
    uint32_t k = rue(b);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

int h264_parse_sps(const uint8_t *nal, size_t n, h264_sps *o)
{
    uint8_t *d; size_t dn;
    if (n < 4 || (nal[0] & 0x1F) != 7 || unescape(nal + 1, n - 1, &d, &dn)) return -1;
    br b = { d, dn, 0, 0 };
    memset(o, 0, sizeof *o);
    struct v4l2_ctrl_h264_sps *s = &o->v4l2;
    s->profile_idc = rbits(&b, 8);
    { uint32_t c = rbits(&b, 8); s->constraint_set_flags = 0;
      for (int k = 0; k < 6; k++) if (c & (0x80 >> k)) s->constraint_set_flags |= 1 << k; }
    s->level_idc = rbits(&b, 8);
    s->seq_parameter_set_id = rue(&b);
    s->chroma_format_idc = 1;
    int p = s->profile_idc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 || p == 138 || p == 139 || p == 134 || p == 135) {
        s->chroma_format_idc = rue(&b);
        if (s->chroma_format_idc == 3 && rbit(&b)) s->flags |= V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE;
        s->bit_depth_luma_minus8 = rue(&b);
        s->bit_depth_chroma_minus8 = rue(&b);
        if (rbit(&b)) s->flags |= V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS;
        if (rbit(&b)) {                       /* seq_scaling_matrix_present_flag: not supported */
            free(d); return -2;
        }
    }
    s->log2_max_frame_num_minus4 = rue(&b);
    o->log2_max_frame_num = s->log2_max_frame_num_minus4 + 4;
    s->pic_order_cnt_type = o->poc_type = rue(&b);
    if (o->poc_type == 0) {
        s->log2_max_pic_order_cnt_lsb_minus4 = rue(&b);
        o->log2_max_poc_lsb = s->log2_max_pic_order_cnt_lsb_minus4 + 4;
    } else if (o->poc_type == 1) {
        if (rbit(&b)) { s->flags |= V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO; o->delta_pic_order_always_zero = 1; }
        s->offset_for_non_ref_pic = rse(&b);
        s->offset_for_top_to_bottom_field = rse(&b);
        s->num_ref_frames_in_pic_order_cnt_cycle = rue(&b);
        for (int i = 0; i < s->num_ref_frames_in_pic_order_cnt_cycle; i++) s->offset_for_ref_frame[i] = rse(&b);
    }
    s->max_num_ref_frames = rue(&b);
    if (s->max_num_ref_frames > 16) b.err = 1;      /* the DPB holds 16 frames at most */
    if (rbit(&b)) s->flags |= V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED;
    s->pic_width_in_mbs_minus1 = rue(&b);
    s->pic_height_in_map_units_minus1 = rue(&b);
    o->frame_mbs_only = rbit(&b);
    if (o->frame_mbs_only) s->flags |= V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
    else if (rbit(&b)) s->flags |= V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
    if (rbit(&b)) s->flags |= V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;
    o->width = (s->pic_width_in_mbs_minus1 + 1) * 16;
    o->height = (s->pic_height_in_map_units_minus1 + 1) * 16 * (o->frame_mbs_only ? 1 : 2);
    int err = b.err; free(d);
    return err ? -1 : 0;
}

int h264_parse_pps(const uint8_t *nal, size_t n, h264_pps *o)
{
    uint8_t *d; size_t dn;
    if (n < 3 || (nal[0] & 0x1F) != 8 || unescape(nal + 1, n - 1, &d, &dn)) return -1;
    br b = { d, dn, 0, 0 };
    memset(o, 0, sizeof *o);
    struct v4l2_ctrl_h264_pps *p = &o->v4l2;
    o->pps_id = p->pic_parameter_set_id = rue(&b);
    o->sps_id = p->seq_parameter_set_id = rue(&b);
    if (rbit(&b)) p->flags |= V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
    if (rbit(&b)) p->flags |= V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT;
    p->num_slice_groups_minus1 = rue(&b);
    if (p->num_slice_groups_minus1) { free(d); return -2; }       /* FMO: not supported */
    p->num_ref_idx_l0_default_active_minus1 = rue(&b);
    p->num_ref_idx_l1_default_active_minus1 = rue(&b);
    if (rbit(&b)) p->flags |= V4L2_H264_PPS_FLAG_WEIGHTED_PRED;
    p->weighted_bipred_idc = rbits(&b, 2);
    p->pic_init_qp_minus26 = rse(&b);
    p->pic_init_qs_minus26 = rse(&b);
    p->chroma_qp_index_offset = rse(&b);
    if (rbit(&b)) p->flags |= V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
    if (rbit(&b)) p->flags |= V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED;
    if (rbit(&b)) p->flags |= V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT;
    p->second_chroma_qp_index_offset = p->chroma_qp_index_offset;
    /* optional High-profile tail */
    size_t more_bits = dn * 8 - b.pos;
    int has_more = 0;
    if (more_bits > 8) has_more = 1;
    else { /* rbsp trailing bits only if remaining bits are 1000... */
        uint32_t rest = 0; size_t save = b.pos;
        for (size_t i = 0; i < more_bits; i++) rest = rest << 1 | rbit(&b);
        b.pos = save;
        has_more = more_bits && rest != (1u << (more_bits - 1));
    }
    if (has_more) {
        if (rbit(&b)) p->flags |= V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
        if (rbit(&b)) { free(d); return -2; }                      /* pic scaling matrix: not supported */
        p->second_chroma_qp_index_offset = rse(&b);
    }
    int err = b.err; free(d);
    return err ? -1 : 0;
}

int h264_peek_pps_id(const uint8_t *nal, size_t n)
{
    uint8_t *d; size_t dn;
    if (n < 3 || unescape(nal + 1, n - 1, &d, &dn)) return -1;
    br b = { d, dn, 0, 0 };
    rue(&b); rue(&b);
    int id = rue(&b);
    free(d);
    return (b.err || id > 255) ? -1 : id;
}

static void pred_weights(br *b, const h264_sps *sps, const h264_slice *s, struct v4l2_ctrl_h264_pred_weights *pw)
{
    memset(pw, 0, sizeof *pw);
    int chroma = sps->v4l2.chroma_format_idc != 0;
    uint32_t ld = rue(b), cd = chroma ? rue(b) : 0;
    if (ld > 7 || cd > 7) { b->err = 1; return; }       /* the spec allows 0..7; 1 << 42 is undefined */
    pw->luma_log2_weight_denom = ld;
    pw->chroma_log2_weight_denom = cd;
    for (int l = 0; l < (s->slice_type == V4L2_H264_SLICE_TYPE_B ? 2 : 1); l++) {
        int cnt = l ? s->num_ref_l1 : s->num_ref_l0;
        struct v4l2_h264_weight_factors *f = &pw->weight_factors[l];
        for (int i = 0; i < cnt && i < 32; i++) {
            f->luma_weight[i] = 1 << pw->luma_log2_weight_denom;
            for (int j = 0; j < 2; j++) f->chroma_weight[i][j] = 1 << pw->chroma_log2_weight_denom;
            if (rbit(b)) { f->luma_weight[i] = rse(b); f->luma_offset[i] = rse(b); }
            if (chroma && rbit(b))
                for (int j = 0; j < 2; j++) { f->chroma_weight[i][j] = rse(b); f->chroma_offset[i][j] = rse(b); }
        }
    }
}

int h264_parse_slice(const uint8_t *nal, size_t n, const h264_sps *sps, const h264_pps *ppss, h264_slice *o)
{
    uint8_t *d; size_t dn;
    int type = nal[0] & 0x1F;
    if (n < 4 || (type != 1 && type != 5) || unescape(nal + 1, n - 1, &d, &dn)) return -1;
    br b = { d, dn, 0, 0 };
    memset(o, 0, sizeof *o);
    o->nal_ref_idc = (nal[0] >> 5) & 3;
    o->is_idr = type == 5;
    o->first_mb = rue(&b);
    o->slice_type = rue(&b) % 5;
    o->pps_id = rue(&b);
    if (o->pps_id > 255) { free(d); return -1; }
    const h264_pps *pps = &ppss[o->pps_id];
    o->frame_num = rbits(&b, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only) { if (rbit(&b)) { free(d); return -2; } }   /* field pictures: unsupported */
    if (o->is_idr) o->idr_pic_id = rue(&b);

    size_t poc_start = b.pos;
    if (sps->poc_type == 0) {
        o->poc_lsb = rbits(&b, sps->log2_max_poc_lsb);
        if (pps->v4l2.flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT)
            o->delta_bottom = rse(&b);
    } else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
        o->delta0 = rse(&b);
        if (pps->v4l2.flags & V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT)
            o->delta1 = rse(&b);
    }
    o->poc_bits = b.pos - poc_start;
    if (pps->v4l2.flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT) o->redundant_pic_cnt = rue(&b);

    int P = o->slice_type == V4L2_H264_SLICE_TYPE_P || o->slice_type == V4L2_H264_SLICE_TYPE_SP;
    int B = o->slice_type == V4L2_H264_SLICE_TYPE_B;
    if (B) o->direct_spatial = rbit(&b);
    o->num_ref_l0 = pps->v4l2.num_ref_idx_l0_default_active_minus1 + 1;
    o->num_ref_l1 = pps->v4l2.num_ref_idx_l1_default_active_minus1 + 1;
    if (P || B) {
        if (rbit(&b)) {
            o->num_ref_l0 = rue(&b) + 1;
            if (B) o->num_ref_l1 = rue(&b) + 1;
        }
        if (o->num_ref_l0 > 32 || o->num_ref_l1 > 32) { free(d); return -1; }
        for (int l = 0; l < (B ? 2 : 1); l++) {
            if (!rbit(&b)) continue;
            for (;;) {
                int idc = rue(&b);
                if (idc == 3) break;
                if (o->n_mod[l] >= 33 || b.err) { free(d); return -1; }
                o->mod[l][o->n_mod[l]].idc = idc;
                o->mod[l][o->n_mod[l]++].val = rue(&b);
            }
        }
    }
    if (((pps->v4l2.flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) && P) || (pps->v4l2.weighted_bipred_idc == 1 && B)) {
        o->has_pred_weights = 1;
        pred_weights(&b, sps, o, &o->pw);
    }
    if (o->nal_ref_idc) {
        size_t m0 = b.pos;
        if (o->is_idr) { rbit(&b); o->long_term_ref = rbit(&b); }
        else if ((o->adaptive_marking = rbit(&b))) {
            for (;;) {
                int op = rue(&b);
                if (op == 0 || b.err) break;
                o->mmco_present = 1;
                if (op == 1 || op == 3) rue(&b);
                if (op == 2) rue(&b);
                if (op == 3 || op == 6) rue(&b);
                if (op == 4) rue(&b);
            }
        }
        o->marking_bits = b.pos - m0;
    }
    if ((pps->v4l2.flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) && o->slice_type != V4L2_H264_SLICE_TYPE_I &&
        o->slice_type != V4L2_H264_SLICE_TYPE_SI)
        o->cabac_init_idc = rue(&b);
    o->qp_delta = rse(&b);
    if (o->slice_type == V4L2_H264_SLICE_TYPE_SP || o->slice_type == V4L2_H264_SLICE_TYPE_SI) {
        if (o->slice_type == V4L2_H264_SLICE_TYPE_SP) o->sp_for_switch = rbit(&b);
        o->qs_delta = rse(&b);
    }
    if (pps->v4l2.flags & V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT) {
        o->disable_deblock = rue(&b);
        if (o->disable_deblock != 1) { o->alpha_off = rse(&b); o->beta_off = rse(&b); }
    }
    o->header_bits = (uint32_t)b.pos;
    int err = b.err; free(d);
    return err ? -1 : 0;
}

void h264_split_annexb(const uint8_t *buf, size_t len, nal_cb cb, void *ctx)
{
    long start = -1;
    for (size_t i = 0; i + 2 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            if (start >= 0) {
                size_t end = i;
                while (end > (size_t)start && buf[end - 1] == 0) end--;   /* trailing zero / 4-byte start code */
                if (end > (size_t)start) cb(ctx, buf + start, end - start);
            }
            start = (long)i + 3;
            i += 2;
        }
    }
    if (start >= 0 && (size_t)start < len) {
        size_t end = len;
        while (end > (size_t)start && buf[end - 1] == 0) end--;
        if (end > (size_t)start) cb(ctx, buf + start, end - start);
    }
}
