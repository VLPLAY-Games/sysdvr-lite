#ifndef H264_PARSE_H
#define H264_PARSE_H
#include <linux/v4l2-controls.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal H264 header parser, just enough to fill the V4L2 stateless controls.
 * Progressive frames only (the console never emits interlaced video). */

typedef struct {
    struct v4l2_ctrl_h264_sps v4l2;
    int log2_max_frame_num, poc_type, log2_max_poc_lsb;
    int frame_mbs_only, delta_pic_order_always_zero;
    int width, height;
} h264_sps;

typedef struct {
    struct v4l2_ctrl_h264_pps v4l2;
    struct v4l2_ctrl_h264_scaling_matrix scaling;
    int sps_id, pps_id;
} h264_pps;

typedef struct { int idc; uint32_t val; } h264_mod;

typedef struct {
    int nal_ref_idc, is_idr;
    uint32_t first_mb;
    int slice_type, pps_id;
    int frame_num, idr_pic_id, poc_lsb;
    int32_t delta_bottom, delta0, delta1;
    int redundant_pic_cnt, direct_spatial;
    int num_ref_l0, num_ref_l1;             /* active counts (not minus1) */
    h264_mod mod[2][34]; int n_mod[2];
    int has_pred_weights;
    struct v4l2_ctrl_h264_pred_weights pw;
    int adaptive_marking, mmco_present, long_term_ref;
    int cabac_init_idc, qp_delta, qs_delta, sp_for_switch;
    int disable_deblock, alpha_off, beta_off;
    uint32_t header_bits;                   /* rbsp bits from after the NAL header byte */
    uint32_t poc_bits, marking_bits;
} h264_slice;

/* data points to the NAL unit including its header byte; no start code */
int h264_parse_sps(const uint8_t *nal, size_t n, h264_sps *out);
int h264_parse_pps(const uint8_t *nal, size_t n, h264_pps *out);
int h264_parse_slice(const uint8_t *nal, size_t n, const h264_sps *sps,
                     const h264_pps *pps /* [256] */, h264_slice *out);
/* needed before slice parsing to find the pps -> returns pps_id or -1 */
int h264_peek_pps_id(const uint8_t *nal, size_t n);

/* Splits an Annex-B buffer; calls cb for each NAL (without start code) */
typedef void (*nal_cb)(void *ctx, const uint8_t *nal, size_t len);
void h264_split_annexb(const uint8_t *buf, size_t len, nal_cb cb, void *ctx);
#endif
