/*****************************************************************************
 * This file is part of uvg266 VVC encoder.
 *
 * Copyright (c) 2021, Tampere University, ITU/ISO/IEC, project contributors
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 * 
 * * Neither the name of the Tampere University or ITU/ISO/IEC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * INCLUDING NEGLIGENCE OR OTHERWISE ARISING IN ANY WAY OUT OF THE USE OF THIS
 ****************************************************************************/

#include "search.h"

#include <limits.h>
#include <string.h>

#include "cabac.h"
#include "encoder.h"
#include "imagelist.h"
#include "inter.h"
#include "intra.h"
#include "kvazaar.h"
#include "rdo.h"
#include "search_inter.h"
#include "search_intra.h"
#include "threadqueue.h"
#include "transform.h"
#include "videoframe.h"
#include "strategies/strategies-picture.h"
#include "strategies/strategies-quant.h"
#include "reshape.h"

#define IN_FRAME(x, y, width, height, block_width, block_height) \
  ((x) >= 0 && (y) >= 0 \
  && (x) + (block_width) <= (width) \
  && (y) + (block_height) <= (height))

// Cost threshold for doing intra search in inter frames with --rd=0.
static const int INTRA_THRESHOLD = 8;

// Modify weight of luma SSD.
#ifndef LUMA_MULT
# define LUMA_MULT 0.8
#endif
// Modify weight of chroma SSD.
#ifndef CHROMA_MULT
# define CHROMA_MULT 1.5
#endif

static INLINE void copy_cu_info(int x_local, int y_local, int width, lcu_t *from, lcu_t *to)
{
  for   (int y = y_local; y < y_local + width; y += SCU_WIDTH) {
    for (int x = x_local; x < x_local + width; x += SCU_WIDTH) {
      *LCU_GET_CU_AT_PX(to, x, y) = *LCU_GET_CU_AT_PX(from, x, y);
    }
  }
}

static INLINE void copy_cu_pixels(int x_local, int y_local, int width, lcu_t *from, lcu_t *to)
{
  const int luma_index = x_local + y_local * LCU_WIDTH;
  const int chroma_index = (x_local / 2) + (y_local / 2) * (LCU_WIDTH / 2);

  uvg_pixels_blit(&from->rec.y[luma_index], &to->rec.y[luma_index],
                  width, width, LCU_WIDTH, LCU_WIDTH);
  if (from->rec.chroma_format != UVG_CSP_400) {
    uvg_pixels_blit(&from->rec.u[chroma_index], &to->rec.u[chroma_index],
                    width / 2, width / 2, LCU_WIDTH / 2, LCU_WIDTH / 2);
    uvg_pixels_blit(&from->rec.v[chroma_index], &to->rec.v[chroma_index],
                    width / 2, width / 2, LCU_WIDTH / 2, LCU_WIDTH / 2);
  }
}

static INLINE void copy_cu_coeffs(int x_local, int y_local, int width, lcu_t *from, lcu_t *to, bool joint)
{
  const int luma_z = xy_to_zorder(LCU_WIDTH, x_local, y_local);
  copy_coeffs(&from->coeff.y[luma_z], &to->coeff.y[luma_z], width);

  if (from->rec.chroma_format != UVG_CSP_400) {
    const int chroma_z = xy_to_zorder(LCU_WIDTH_C, x_local >> 1, y_local >> 1);
    copy_coeffs(&from->coeff.u[chroma_z], &to->coeff.u[chroma_z], width >> 1);
    copy_coeffs(&from->coeff.v[chroma_z], &to->coeff.v[chroma_z], width >> 1);
    if (joint) {
      copy_coeffs(&from->coeff.joint_uv[chroma_z], &to->coeff.joint_uv[chroma_z], width >> 1);
    }
  }
}

/**
 * Copy all non-reference CU data from next level to current level.
 */
static void work_tree_copy_up(int x_local, int y_local, int depth, lcu_t *work_tree, bool joint)
{
  const int width = LCU_WIDTH >> depth;
  copy_cu_info  (x_local, y_local, width, &work_tree[depth + 1], &work_tree[depth]);
  copy_cu_pixels(x_local, y_local, width, &work_tree[depth + 1], &work_tree[depth]);
  copy_cu_coeffs(x_local, y_local, width, &work_tree[depth + 1], &work_tree[depth], joint);
  
}


/**
 * Copy all non-reference CU data from current level to all lower levels.
 */
static void work_tree_copy_down(int x_local, int y_local, int depth, lcu_t *work_tree)
{
  const int width = LCU_WIDTH >> depth;
  for (int i = depth + 1; i <= MAX_PU_DEPTH; i++) {
    copy_cu_info  (x_local, y_local, width, &work_tree[depth], &work_tree[i]);
    copy_cu_pixels(x_local, y_local, width, &work_tree[depth], &work_tree[i]);
  }
}

void uvg_lcu_fill_trdepth(lcu_t *lcu, int x_px, int y_px, int depth, int tr_depth)
{
  const int x_local = SUB_SCU(x_px);
  const int y_local = SUB_SCU(y_px);
  const int width = LCU_WIDTH >> depth;

  for (unsigned y = 0; y < width; y += SCU_WIDTH) {
    for (unsigned x = 0; x < width; x += SCU_WIDTH) {
      LCU_GET_CU_AT_PX(lcu, x_local + x, y_local + y)->tr_depth = tr_depth;
    }
  }
}

static void lcu_fill_cu_info(lcu_t *lcu, int x_local, int y_local, int width, int height, cu_info_t *cu)
{
  // Set mode in every CU covered by part_mode in this depth.
  for (int y = y_local; y < y_local + height; y += SCU_WIDTH) {
    for (int x = x_local; x < x_local + width; x += SCU_WIDTH) {
      cu_info_t *to = LCU_GET_CU_AT_PX(lcu, x, y);
      to->type      = cu->type;
      to->depth     = cu->depth;
      to->part_size = cu->part_size;
      to->qp        = cu->qp;
      //to->tr_idx    = cu->tr_idx;

      if (cu->type == CU_INTRA) {
        to->intra.mode        = cu->intra.mode;
        to->intra.mode_chroma = cu->intra.mode_chroma;
        to->intra.multi_ref_idx = cu->intra.multi_ref_idx;
        to->intra.mip_flag = cu->intra.mip_flag;
        to->intra.mip_is_transposed = cu->intra.mip_is_transposed;
      } else {
        to->skipped   = cu->skipped;
        to->merged    = cu->merged;
        to->merge_idx = cu->merge_idx;
        to->inter     = cu->inter;
      }
    }
  }
}

static void lcu_fill_inter(lcu_t *lcu, int x_local, int y_local, int cu_width)
{
  const part_mode_t part_mode = LCU_GET_CU_AT_PX(lcu, x_local, y_local)->part_size;
  const int num_pu = uvg_part_mode_num_parts[part_mode];

  for (int i = 0; i < num_pu; ++i) {
    const int x_pu      = PU_GET_X(part_mode, cu_width, x_local, i);
    const int y_pu      = PU_GET_Y(part_mode, cu_width, y_local, i);
    const int width_pu  = PU_GET_W(part_mode, cu_width, i);
    const int height_pu = PU_GET_H(part_mode, cu_width, i);

    cu_info_t *pu  = LCU_GET_CU_AT_PX(lcu, x_pu, y_pu);
    pu->type = CU_INTER;
    lcu_fill_cu_info(lcu, x_pu, y_pu, width_pu, height_pu, pu);
  }
}

static void lcu_fill_cbf(lcu_t *lcu, int x_local, int y_local, int width, cu_info_t *cur_cu)
{
  const uint32_t tr_split = cur_cu->tr_depth - cur_cu->depth;
  const uint32_t mask = ~((width >> tr_split)-1);

  // Set coeff flags in every CU covered by part_mode in this depth.
  for (uint32_t y = y_local; y < y_local + width; y += SCU_WIDTH) {
    for (uint32_t x = x_local; x < x_local + width; x += SCU_WIDTH) {
      // Use TU top-left CU to propagate coeff flags
      cu_info_t *cu_from = LCU_GET_CU_AT_PX(lcu, x & mask, y & mask);
      cu_info_t *cu_to   = LCU_GET_CU_AT_PX(lcu, x, y);
      if (cu_from != cu_to) {
        // Chroma and luma coeff data is needed for deblocking
        cbf_copy(&cu_to->cbf, cu_from->cbf, COLOR_Y);
        cbf_copy(&cu_to->cbf, cu_from->cbf, COLOR_U);
        cbf_copy(&cu_to->cbf, cu_from->cbf, COLOR_V);
      }
    }
  }
}


//Calculates cost for all zero coeffs
static double cu_zero_coeff_cost(const encoder_state_t *state, lcu_t *work_tree, const int x, const int y,
  const int depth)
{
  int x_local = SUB_SCU(x);
  int y_local = SUB_SCU(y);
  int cu_width = LCU_WIDTH >> depth;
  lcu_t *const lcu = &work_tree[depth];

  const int luma_index = y_local * LCU_WIDTH + x_local;
  const int chroma_index = (y_local / 2) * LCU_WIDTH_C + (x_local / 2);

  double ssd = 0.0;
  ssd += LUMA_MULT * uvg_pixels_calc_ssd(
    &lcu->ref.y[luma_index], &lcu->rec.y[luma_index],
    LCU_WIDTH, LCU_WIDTH, cu_width
    );
  if (x % 8 == 0 && y % 8 == 0 && state->encoder_control->chroma_format != UVG_CSP_400) {
    ssd += CHROMA_MULT * uvg_pixels_calc_ssd(
      &lcu->ref.u[chroma_index], &lcu->rec.u[chroma_index],
      LCU_WIDTH_C, LCU_WIDTH_C, cu_width / 2
      );
    ssd += CHROMA_MULT * uvg_pixels_calc_ssd(
      &lcu->ref.v[chroma_index], &lcu->rec.v[chroma_index],
      LCU_WIDTH_C, LCU_WIDTH_C, cu_width / 2
      );
  }
  // Save the pixels at a lower level of the working tree.
  copy_cu_pixels(x_local, y_local, cu_width, lcu, &work_tree[depth + 1]);

  return ssd;
}


static void downsample_cclm_rec(encoder_state_t *state, int x, int y, int width, int height, uvg_pixel *y_rec, uvg_pixel extra_pixel) {
  if (!state->encoder_control->cfg.cclm) return;
  int x_scu = SUB_SCU(x);
  int y_scu = SUB_SCU(y);
  y_rec += x_scu + y_scu * LCU_WIDTH;
  int stride = state->tile->frame->source->stride;

  for (int y_ = 0; y_ < height && y_ * 2 + y < state->encoder_control->cfg.height; y_++) {
    for (int x_ = 0; x_ < width; x_++) {
      int s = 4;
      s += y_rec[2 * x_] * 2;
      s += y_rec[2 * x_ + 1];
      // If we are at the edge of the CTU read the pixel from the frame reconstruct buffer,
      // *except* when we are also at the edge of the frame, in which case we want to duplicate
      // the edge pixel
      s += !x_scu && !x_ && x ? state->tile->frame->rec->y[x - 1 + (y + y_ * 2) * stride] : y_rec[2 * x_ - ((x_ + x) > 0)];
      s += y_rec[2 * x_ + LCU_WIDTH] * 2;
      s += y_rec[2 * x_ + 1 + LCU_WIDTH];
      s += !x_scu && !x_ && x ? state->tile->frame->rec->y[x - 1 + (y + y_ * 2 + 1) * stride] : y_rec[2 * x_ - ((x_ + x) > 0) + LCU_WIDTH];
      int index = x / 2 + x_ + (y / 2 + y_ )* stride / 2;
      state->tile->frame->cclm_luma_rec[index] = s >> 3;
    }
    y_rec += LCU_WIDTH * 2;
  }
  if((y + height * 2) % 64 == 0) {
    int line = y / 64 * stride / 2;
    y_rec -= LCU_WIDTH;
    for (int i = 0; i < width; ++i) {
      int s = 2;
      s += y_rec[i * 2] * 2;
      s += y_rec[i * 2 + 1];
      s += !x_scu && !i && x ? extra_pixel : y_rec[i * 2 - ((i + x) > 0)] ;
      state->tile->frame->cclm_luma_rec_top_line[i + x / 2 + line] = s >> 2;
    }
  }
}


/**
* Calculate RD cost for a Coding Unit.
* \return Cost of block
* \param ref_cu  CU used for prediction parameters.
*
* Calculates the RDO cost of a single CU that will not be split further.
* Takes into account SSD of reconstruction and the cost of encoding whatever
* prediction unit data needs to be coded.
*/
double uvg_cu_rd_cost_luma(const encoder_state_t *const state,
                       const int x_px, const int y_px, const int depth,
                       const cu_info_t *const pred_cu,
                       lcu_t *const lcu)
{
  const int width = LCU_WIDTH >> depth;

  // cur_cu is used for TU parameters.
  cu_info_t *const tr_cu = LCU_GET_CU_AT_PX(lcu, x_px, y_px);

  double coeff_bits = 0;
  double tr_tree_bits = 0;

  // Check that lcu is not in 
  assert(x_px >= 0 && x_px < LCU_WIDTH);
  assert(y_px >= 0 && y_px < LCU_WIDTH);

  const uint8_t tr_depth = tr_cu->tr_depth - depth;

  if (tr_depth > 0) {
    int offset = width / 2;
    double sum = 0;

    sum += uvg_cu_rd_cost_luma(state, x_px, y_px, depth + 1, pred_cu, lcu);
    sum += uvg_cu_rd_cost_luma(state, x_px + offset, y_px, depth + 1, pred_cu, lcu);
    sum += uvg_cu_rd_cost_luma(state, x_px, y_px + offset, depth + 1, pred_cu, lcu);
    sum += uvg_cu_rd_cost_luma(state, x_px + offset, y_px + offset, depth + 1, pred_cu, lcu);

    return sum + tr_tree_bits * state->lambda;
  }

  // Add transform_tree cbf_luma bit cost.
  if (pred_cu->type == CU_INTRA ||
      tr_depth > 0 ||
      cbf_is_set(tr_cu->cbf, depth, COLOR_U) ||
      cbf_is_set(tr_cu->cbf, depth, COLOR_V))
  {
    const cabac_ctx_t *ctx = &(state->cabac.ctx.qt_cbf_model_luma[0]);
    tr_tree_bits += CTX_ENTROPY_FBITS(ctx, cbf_is_set(pred_cu->cbf, depth, COLOR_Y));
  }

  // SSD between reconstruction and original
  int ssd = 0;
  if (!state->encoder_control->cfg.lossless) {
    int index = y_px * LCU_WIDTH + x_px;
    ssd = uvg_pixels_calc_ssd(&lcu->ref.y[index], &lcu->rec.y[index],
                                        LCU_WIDTH,          LCU_WIDTH,
                                        width);
  }

  {
    int8_t luma_scan_mode = uvg_get_scan_order(pred_cu->type, pred_cu->intra.mode, depth);
    const coeff_t *coeffs = &lcu->coeff.y[xy_to_zorder(LCU_WIDTH, x_px, y_px)];

    coeff_bits += uvg_get_coeff_cost(state, coeffs, width, 0, luma_scan_mode, pred_cu->tr_idx == MTS_SKIP);
  }

  double bits = tr_tree_bits + coeff_bits;
  return (double)ssd * LUMA_MULT + bits * state->lambda;
}


double uvg_cu_rd_cost_chroma(const encoder_state_t *const state,
                         const int x_px, const int y_px, const int depth,
                         cu_info_t * pred_cu,
                         lcu_t *const lcu)
{
  const vector2d_t lcu_px = { (x_px & ~7) / 2, (y_px & ~7) / 2 };
  const int width = (depth < MAX_DEPTH) ? LCU_WIDTH >> (depth + 1) : LCU_WIDTH >> depth;
  cu_info_t *const tr_cu = LCU_GET_CU_AT_PX(lcu, x_px, y_px);

  double tr_tree_bits = 0;
  double joint_cbcr_tr_tree_bits = 0;
  double coeff_bits = 0;
  double joint_coeff_bits = 0;

  assert(x_px >= 0 && x_px < LCU_WIDTH);
  assert(y_px >= 0 && y_px < LCU_WIDTH);

  if (depth == 4 && (x_px % 8 == 0 || y_px % 8 == 0)) {
    // For MAX_PU_DEPTH calculate chroma for previous depth for the first
    // block and return 0 cost for all others.
    return 0;
  }

  if (depth < MAX_PU_DEPTH) {
    const int tr_depth = depth - pred_cu->depth;
    const cabac_ctx_t *ctx = &(state->cabac.ctx.qt_cbf_model_cb[0]);
    if (tr_depth == 0 || cbf_is_set(pred_cu->cbf, depth - 1, COLOR_U)) {
      tr_tree_bits += CTX_ENTROPY_FBITS(ctx, cbf_is_set(pred_cu->cbf, depth, COLOR_U));
    }
    if(state->encoder_control->cfg.jccr) {
      joint_cbcr_tr_tree_bits += CTX_ENTROPY_FBITS(ctx, pred_cu->joint_cb_cr & 1);
    }
    int is_set = cbf_is_set(pred_cu->cbf, depth, COLOR_U);
    ctx = &(state->cabac.ctx.qt_cbf_model_cr[is_set]);
    if (tr_depth == 0 || cbf_is_set(pred_cu->cbf, depth - 1, COLOR_V)) {
      tr_tree_bits += CTX_ENTROPY_FBITS(ctx, cbf_is_set(pred_cu->cbf, depth, COLOR_V));
    }
    if(state->encoder_control->cfg.jccr) {
      ctx = &(state->cabac.ctx.qt_cbf_model_cr[pred_cu->joint_cb_cr & 1]);
      joint_cbcr_tr_tree_bits += CTX_ENTROPY_FBITS(ctx, (pred_cu->joint_cb_cr & 2) >> 1);
    }
  }


  if (tr_cu->tr_depth > depth) {
    int offset = LCU_WIDTH >> (depth + 1);
    int sum = 0;

    sum += uvg_cu_rd_cost_chroma(state, x_px, y_px, depth + 1, pred_cu, lcu);
    sum += uvg_cu_rd_cost_chroma(state, x_px + offset, y_px, depth + 1, pred_cu, lcu);
    sum += uvg_cu_rd_cost_chroma(state, x_px, y_px + offset, depth + 1, pred_cu, lcu);
    sum += uvg_cu_rd_cost_chroma(state, x_px + offset, y_px + offset, depth + 1, pred_cu, lcu);

    return sum + tr_tree_bits * state->lambda;
  }

  if (state->encoder_control->cfg.jccr) {
    int cbf_mask = cbf_is_set(pred_cu->cbf, depth, COLOR_U) * 2 + cbf_is_set(pred_cu->cbf, depth, COLOR_V) - 1;
    const cabac_ctx_t* ctx = NULL;
    if (cbf_mask != -1) {
      ctx = &(state->cabac.ctx.joint_cb_cr[cbf_mask]);
      tr_tree_bits += CTX_ENTROPY_FBITS(ctx, 0);      
    }
    if(pred_cu->joint_cb_cr) {
      ctx = &(state->cabac.ctx.joint_cb_cr[(pred_cu->joint_cb_cr & 1) * 2 + ((pred_cu->joint_cb_cr & 2) >> 1) - 1]);
      joint_cbcr_tr_tree_bits += CTX_ENTROPY_FBITS(ctx, 1);
    }
  }

  // Chroma SSD
  int ssd = 0;
  int joint_ssd = 0;
  if (!state->encoder_control->cfg.lossless) {
    int index = lcu_px.y * LCU_WIDTH_C + lcu_px.x;
    int ssd_u = uvg_pixels_calc_ssd(&lcu->ref.u[index], &lcu->rec.u[index],
                                    LCU_WIDTH_C,         LCU_WIDTH_C,
                                    width);
    int ssd_v = uvg_pixels_calc_ssd(&lcu->ref.v[index], &lcu->rec.v[index],
                                    LCU_WIDTH_C,        LCU_WIDTH_C,
                                    width);
    ssd = ssd_u + ssd_v;

    if(state->encoder_control->cfg.jccr) {
      int ssd_u_joint = uvg_pixels_calc_ssd(&lcu->ref.u[index], &lcu->rec.joint_u[index],
        LCU_WIDTH_C, LCU_WIDTH_C,
        width);
      int ssd_v_joint = uvg_pixels_calc_ssd(&lcu->ref.v[index], &lcu->rec.joint_v[index],
        LCU_WIDTH_C, LCU_WIDTH_C,
        width);
      joint_ssd = ssd_u_joint + ssd_v_joint;
    }
  }

  {
    int8_t scan_order = uvg_get_scan_order(pred_cu->type, pred_cu->intra.mode_chroma, depth);
    const int index = xy_to_zorder(LCU_WIDTH_C, lcu_px.x, lcu_px.y);

    coeff_bits += uvg_get_coeff_cost(state, &lcu->coeff.u[index], width, 2, scan_order, 0);
    coeff_bits += uvg_get_coeff_cost(state, &lcu->coeff.v[index], width, 2, scan_order, 0);

    if(state->encoder_control->cfg.jccr) {
      joint_coeff_bits += uvg_get_coeff_cost(state, &lcu->coeff.joint_uv[index], width, 2, scan_order, 0);
    }
  }


  double bits = tr_tree_bits + coeff_bits;
  double joint_bits = joint_cbcr_tr_tree_bits + joint_coeff_bits;

  double cost = (double)ssd + bits * state->c_lambda;
  double joint_cost = (double)joint_ssd + joint_bits * state->c_lambda;
  if ((cost < joint_cost || !pred_cu->joint_cb_cr) || !state->encoder_control->cfg.jccr) {
    pred_cu->joint_cb_cr = 0;
    return cost;    
  }
  cbf_clear(&pred_cu->cbf, depth, COLOR_U);
  cbf_clear(&pred_cu->cbf, depth, COLOR_V);
  if (pred_cu->joint_cb_cr & 1) {
    cbf_set(&pred_cu->cbf, depth, COLOR_U);
  }
  if (pred_cu->joint_cb_cr & 2) {
    cbf_set(&pred_cu->cbf, depth, COLOR_V);
  }
  int lcu_width = LCU_WIDTH_C;
  const int index = lcu_px.x + lcu_px.y * lcu_width;
  uvg_pixels_blit(&lcu->rec.joint_u[index], &lcu->rec.u[index], width, width, lcu_width, lcu_width);
  uvg_pixels_blit(&lcu->rec.joint_v[index], &lcu->rec.v[index], width, width, lcu_width, lcu_width);
  return joint_cost;
}


// Return estimate of bits used to code prediction mode of cur_cu.
static double calc_mode_bits(const encoder_state_t *state,
                             const lcu_t *lcu,
                             const cu_info_t * cur_cu,
                             int x, int y, int depth)
{
  int x_local = SUB_SCU(x);
  int y_local = SUB_SCU(y);

  assert(cur_cu->type == CU_INTRA);

  int8_t candidate_modes[INTRA_MPM_COUNT];
  {
    const cu_info_t *left_cu  = ((x >= SCU_WIDTH) ? LCU_GET_CU_AT_PX(lcu, x_local - SCU_WIDTH, y_local) : NULL);
    const cu_info_t *above_cu = ((y >= SCU_WIDTH) ? LCU_GET_CU_AT_PX(lcu, x_local, y_local - SCU_WIDTH) : NULL);
    uvg_intra_get_dir_luma_predictor(x, y, candidate_modes, cur_cu, left_cu, above_cu);
  }

  int width = LCU_WIDTH >> depth;
  int height = width; // TODO: height for non-square blocks
  int num_mip_modes_half = NUM_MIP_MODES_HALF(width, height);
  int mip_flag_ctx_id = uvg_get_mip_flag_context(x, y, width, height, lcu, NULL);
  double mode_bits = uvg_luma_mode_bits(state, cur_cu->intra.mode, candidate_modes, cur_cu->intra.multi_ref_idx, num_mip_modes_half, mip_flag_ctx_id);

  if (((depth == 4 && x % 8 && y % 8) || (depth != 4)) && state->encoder_control->chroma_format != UVG_CSP_400) {
    mode_bits += uvg_chroma_mode_bits(state, cur_cu->intra.mode_chroma, cur_cu->intra.mode);
  }

  return mode_bits;
}


/**
 * \brief Sort modes and costs to ascending order according to costs.
 */
void uvg_sort_modes(int8_t *__restrict modes, double *__restrict costs, uint8_t length)
{
  // Length for intra is always between 5 and 23, and is either 21, 17, 9 or 8 about
  // 60% of the time, so there should be no need for anything more complex
  // than insertion sort.
  // Length for merge is 5 or less.
  for (uint8_t i = 1; i < length; ++i) {
    const double cur_cost = costs[i];
    const int8_t cur_mode = modes[i];
    uint8_t j = i;
    while (j > 0 && cur_cost < costs[j - 1]) {
      costs[j] = costs[j - 1];
      modes[j] = modes[j - 1];
      --j;
    }
    costs[j] = cur_cost;
    modes[j] = cur_mode;
  }
}

/**
 * \brief Sort modes and costs to ascending order according to costs.
 */
void uvg_sort_modes_intra_luma(int8_t *__restrict modes, int8_t *__restrict trafo, double *__restrict costs, uint8_t length)
{
  // Length for intra is always between 5 and 23, and is either 21, 17, 9 or 8 about
  // 60% of the time, so there should be no need for anything more complex
  // than insertion sort.
  // Length for merge is 5 or less.
  for (uint8_t i = 1; i < length; ++i) {
    const double cur_cost = costs[i];
    const int8_t cur_mode = modes[i];
    const int8_t cur_tr = trafo[i];
    uint8_t j = i;
    while (j > 0 && cur_cost < costs[j - 1]) {
      costs[j] = costs[j - 1];
      modes[j] = modes[j - 1];
      trafo[j] = trafo[j - 1];
      --j;
    }
    costs[j] = cur_cost;
    modes[j] = cur_mode;
    trafo[j] = cur_tr;
  }
}



static uint8_t get_ctx_cu_split_model(const lcu_t *lcu, int x, int y, int depth)
{
  vector2d_t lcu_cu = { SUB_SCU(x), SUB_SCU(y) };
  bool condA = x >= 8 && LCU_GET_CU_AT_PX(lcu, lcu_cu.x - 1, lcu_cu.y    )->depth > depth;
  bool condL = y >= 8 && LCU_GET_CU_AT_PX(lcu, lcu_cu.x,     lcu_cu.y - 1)->depth > depth;
  return condA + condL;
}

/**
 * Search every mode from 0 to MAX_PU_DEPTH and return cost of best mode.
 * - The recursion is started at depth 0 and goes in Z-order to MAX_PU_DEPTH.
 * - Data structure work_tree is maintained such that the neighbouring SCUs
 *   and pixels to the left and up of current CU are the final CUs decided
 *   via the search. This is done by copying the relevant data to all
 *   relevant levels whenever a decision is made whether to split or not.
 * - All the final data for the LCU gets eventually copied to depth 0, which
 *   will be the final output of the recursion.
 */
static double search_cu(encoder_state_t * const state, int x, int y, int depth, lcu_t *work_tree)
{
  const encoder_control_t* ctrl = state->encoder_control;
  const videoframe_t * const frame = state->tile->frame;
  int cu_width = LCU_WIDTH >> depth;
  double cost = MAX_INT;
  double inter_zero_coeff_cost = MAX_INT;
  uint32_t inter_bitcost = MAX_INT;
  cu_info_t *cur_cu;

  const uint32_t ctu_row = (y >> LOG2_LCU_WIDTH);
  const uint32_t ctu_row_mul_five = ctu_row * MAX_NUM_HMVP_CANDS;

  cu_info_t hmvp_lut[MAX_NUM_HMVP_CANDS];
  uint8_t hmvp_lut_size = state->tile->frame->hmvp_size[ctu_row];

  // Store original HMVP lut before search and restore after, since it's modified
  if (state->frame->slicetype != UVG_SLICE_I) memcpy(hmvp_lut, &state->tile->frame->hmvp_lut[ctu_row_mul_five], sizeof(cu_info_t) * MAX_NUM_HMVP_CANDS);

  struct {
    int32_t min;
    int32_t max;
  } pu_depth_inter, pu_depth_intra;

  lcu_t *const lcu = &work_tree[depth];

  int x_local = SUB_SCU(x);
  int y_local = SUB_SCU(y);

  // Stop recursion if the CU is completely outside the frame.
  if (x >= frame->width || y >= frame->height) {
    // Return zero cost because this CU does not have to be coded.
    return 0;
  }

  int gop_layer = ctrl->cfg.gop_len != 0 ? ctrl->cfg.gop[state->frame->gop_offset].layer - 1 : 0;

  // Assign correct depth limit
  constraint_t* constr = state->constraint;
 if(constr->ml_intra_depth_ctu) {
    pu_depth_intra.min = constr->ml_intra_depth_ctu->_mat_upper_depth[(x_local >> 3) + (y_local >> 3) * 8];
    pu_depth_intra.max = constr->ml_intra_depth_ctu->_mat_lower_depth[(x_local >> 3) + (y_local >> 3) * 8];
  }
  else {
    pu_depth_intra.min = ctrl->cfg.pu_depth_intra.min[gop_layer] >= 0 ? ctrl->cfg.pu_depth_intra.min[gop_layer] : ctrl->cfg.pu_depth_intra.min[0];
    pu_depth_intra.max = ctrl->cfg.pu_depth_intra.max[gop_layer] >= 0 ? ctrl->cfg.pu_depth_intra.max[gop_layer] : ctrl->cfg.pu_depth_intra.max[0];
  }
  pu_depth_inter.min = ctrl->cfg.pu_depth_inter.min[gop_layer] >= 0 ? ctrl->cfg.pu_depth_inter.min[gop_layer] : ctrl->cfg.pu_depth_inter.min[0];
  pu_depth_inter.max = ctrl->cfg.pu_depth_inter.max[gop_layer] >= 0 ? ctrl->cfg.pu_depth_inter.max[gop_layer] : ctrl->cfg.pu_depth_inter.max[0];

  cur_cu = LCU_GET_CU_AT_PX(lcu, x_local, y_local);
  // Assign correct depth
  cur_cu->depth = (depth > MAX_DEPTH) ? MAX_DEPTH : depth;
  cur_cu->tr_depth = (depth > 0) ? depth : 1;
  cur_cu->type = CU_NOTSET;
  cur_cu->part_size = SIZE_2Nx2N;
  cur_cu->qp = state->qp;
  cur_cu->bdpcmMode = 0;
  cur_cu->tr_idx = 0;
  cur_cu->violates_mts_coeff_constraint = 0;
  cur_cu->mts_last_scan_pos = 0;
  cur_cu->joint_cb_cr = 0;

  // If the CU is completely inside the frame at this depth, search for
  // prediction modes at this depth.
  if (x + cu_width <= frame->width &&
      y + cu_width <= frame->height)
  {
    int cu_width_inter_min = LCU_WIDTH >> pu_depth_inter.max;
    bool can_use_inter =
      state->frame->slicetype != UVG_SLICE_I &&
      depth <= MAX_DEPTH &&
      (
        WITHIN(depth, pu_depth_inter.min, pu_depth_inter.max) ||
        // When the split was forced because the CTU is partially outside the
        // frame, we permit inter coding even if pu_depth_inter would
        // otherwise forbid it.
        (x & ~(cu_width_inter_min - 1)) + cu_width_inter_min > frame->width ||
        (y & ~(cu_width_inter_min - 1)) + cu_width_inter_min > frame->height
      );

    if (can_use_inter) {
      double mode_cost;
      uint32_t mode_bitcost;
      uvg_search_cu_inter(state,
                          x, y,
                          depth,
                          lcu,
                          &mode_cost, &mode_bitcost);
      if (mode_cost < cost) {
        cost = mode_cost;
        inter_bitcost = mode_bitcost;
        cur_cu->type = CU_INTER;
      }

      if (!(ctrl->cfg.early_skip && cur_cu->skipped)) {
        // Try SMP and AMP partitioning.
        static const part_mode_t mp_modes[] = {
          // SMP
          SIZE_2NxN, SIZE_Nx2N,
          // AMP
          SIZE_2NxnU, SIZE_2NxnD,
          SIZE_nLx2N, SIZE_nRx2N,
        };

        const int first_mode = ctrl->cfg.smp_enable ? 0 : 2;
        const int last_mode = (ctrl->cfg.amp_enable && cu_width >= 16) ? 5 : 1;
        for (int i = first_mode; i <= last_mode; ++i) {
          uvg_search_cu_smp(state,
                            x, y,
                            depth,
                            mp_modes[i],
                            &work_tree[depth + 1],
                            &mode_cost, &mode_bitcost);
          if (mode_cost < cost) {
            cost = mode_cost;
            inter_bitcost = mode_bitcost;
            // Copy inter prediction info to current level.
            copy_cu_info(x_local, y_local, cu_width, &work_tree[depth + 1], lcu);
          }
        }
      }
    }

    // Try to skip intra search in rd==0 mode.
    // This can be quite severe on bdrate. It might be better to do this
    // decision after reconstructing the inter frame.
    bool skip_intra = (state->encoder_control->cfg.rdo == 0
                      && cur_cu->type != CU_NOTSET
                      && cost / (cu_width * cu_width) < INTRA_THRESHOLD)
                      || (ctrl->cfg.early_skip && cur_cu->skipped);

    int32_t cu_width_intra_min = LCU_WIDTH >> pu_depth_intra.max;
    bool can_use_intra =
        WITHIN(depth, pu_depth_intra.min, pu_depth_intra.max) ||
        // When the split was forced because the CTU is partially outside
        // the frame, we permit intra coding even if pu_depth_intra would
        // otherwise forbid it.
        (x & ~(cu_width_intra_min - 1)) + cu_width_intra_min > frame->width ||
        (y & ~(cu_width_intra_min - 1)) + cu_width_intra_min > frame->height;

    if (can_use_intra && !skip_intra) {
      int8_t intra_mode;
      int8_t intra_trafo;
      double intra_cost;
      uint8_t multi_ref_index = 0;
      bool mip_flag = false;
      bool mip_transposed = false;
      uvg_search_cu_intra(state, x, y, depth, lcu,
                          &intra_mode, &intra_trafo, &intra_cost, &multi_ref_index, &mip_flag, &mip_transposed);
      if (intra_cost < cost) {
        cost = intra_cost;
        cur_cu->type = CU_INTRA;
        cur_cu->part_size = depth > MAX_DEPTH ? SIZE_NxN : SIZE_2Nx2N;
        cur_cu->intra.mode = intra_mode;
        cur_cu->intra.multi_ref_idx = multi_ref_index;
        cur_cu->intra.mip_flag = mip_flag;
        cur_cu->intra.mip_is_transposed = mip_transposed;

        //If the CU is not split from 64x64 block, the MTS is disabled for that CU.
        cur_cu->tr_idx = (depth > 0) ? intra_trafo : 0;
      }
    }

    // Reconstruct best mode because we need the reconstructed pixels for
    // mode search of adjacent CUs.
    if (cur_cu->type == CU_INTRA) {
      assert(cur_cu->part_size == SIZE_2Nx2N || cur_cu->part_size == SIZE_NxN);
      cur_cu->intra.mode_chroma = cur_cu->intra.mode;
      
      lcu_fill_cu_info(lcu, x_local, y_local, cu_width, cu_width, cur_cu);
      uvg_intra_recon_cu(state,
                         x, y,
                         depth,
                         cur_cu->intra.mode, -1, // skip chroma
                         NULL, NULL, cur_cu->intra.multi_ref_idx, 
                         cur_cu->intra.mip_flag, cur_cu->intra.mip_is_transposed, 
                         lcu);

      downsample_cclm_rec(
        state, x, y, cu_width / 2, cu_width / 2, lcu->rec.y, lcu->left_ref.y[64]
      );

      // TODO: This heavily relies to square CUs
      if ((depth != 4 || (x % 8 && y % 8)) && state->encoder_control->chroma_format != UVG_CSP_400) {
        // There is almost no benefit to doing the chroma mode search for
        // rd2. Possibly because the luma mode search already takes chroma
        // into account, so there is less of a chanse of luma mode being
        // really bad for chroma.
        cclm_parameters_t cclm_params[2];
        if (ctrl->cfg.rdo >= 3 && !cur_cu->intra.mip_flag) {
          cur_cu->intra.mode_chroma = uvg_search_cu_intra_chroma(state, x, y, depth, lcu, cclm_params);
          lcu_fill_cu_info(lcu, x_local, y_local, cu_width, cu_width, cur_cu);
        }

        uvg_intra_recon_cu(state,
                           x & ~7, y & ~7, // TODO: as does this
                           depth,
                           -1, cur_cu->intra.mode_chroma, // skip luma
                           NULL, cclm_params, 0, 
                           cur_cu->intra.mip_flag, cur_cu->intra.mip_is_transposed,
                           lcu);
      }
    } else if (cur_cu->type == CU_INTER) {

      if (!cur_cu->skipped) {

        if (!cur_cu->merged) {
            if (cur_cu->inter.mv_dir & 1) uvg_round_precision(INTERNAL_MV_PREC, 2, &cur_cu->inter.mv[0][0], &cur_cu->inter.mv[0][1]);
            if (cur_cu->inter.mv_dir & 2) uvg_round_precision(INTERNAL_MV_PREC, 2, &cur_cu->inter.mv[1][0], &cur_cu->inter.mv[1][1]);
        }
        // Reset transform depth because intra messes with them.
        // This will no longer be necessary if the transform depths are not shared.
        int tr_depth = MAX(1, depth);
        if (cur_cu->part_size != SIZE_2Nx2N) {
          tr_depth = depth + 1;
        }
        uvg_lcu_fill_trdepth(lcu, x, y, depth, tr_depth);

        const bool has_chroma = state->encoder_control->chroma_format != UVG_CSP_400;
        uvg_inter_recon_cu(state, lcu, x, y, cu_width, true, has_chroma);

        if (ctrl->cfg.zero_coeff_rdo && !ctrl->cfg.lossless && !ctrl->cfg.rdoq_enable) {
          //Calculate cost for zero coeffs
          inter_zero_coeff_cost = cu_zero_coeff_cost(state, work_tree, x, y, depth) + inter_bitcost * state->lambda;

        }

        uvg_quantize_lcu_residual(state,
          true, has_chroma,
          x, y, depth,
          NULL,
          lcu,
          false);

        int cbf = cbf_is_set_any(cur_cu->cbf, depth);

        if (cur_cu->merged && !cbf && cur_cu->part_size == SIZE_2Nx2N) {
          cur_cu->merged = 0;
          cur_cu->skipped = 1;
          // Selecting skip reduces bits needed to code the CU
          if (inter_bitcost > 1) {
            inter_bitcost -= 1;
          }
        }
      }
      lcu_fill_inter(lcu, x_local, y_local, cu_width);
      lcu_fill_cbf(lcu, x_local, y_local, cu_width, cur_cu);
    }
  }

  if (cur_cu->type == CU_INTRA || cur_cu->type == CU_INTER) {
    cost = uvg_cu_rd_cost_luma(state, x_local, y_local, depth, cur_cu, lcu);
    if (state->encoder_control->chroma_format != UVG_CSP_400) {
      cost += uvg_cu_rd_cost_chroma(state, x_local, y_local, depth, cur_cu, lcu);
    }

    double mode_bits;
    if (cur_cu->type == CU_INTRA) {
      mode_bits = calc_mode_bits(state, lcu, cur_cu, x, y, depth);
    } else {
      mode_bits = inter_bitcost;
    }

    cost += mode_bits * state->lambda;

    if (ctrl->cfg.zero_coeff_rdo && inter_zero_coeff_cost <= cost) {
      cost = inter_zero_coeff_cost;

      // Restore saved pixels from lower level of the working tree.
      copy_cu_pixels(x_local, y_local, cu_width, &work_tree[depth + 1], lcu);

      if (cur_cu->merged && cur_cu->part_size == SIZE_2Nx2N) {
        cur_cu->merged = 0;
        cur_cu->skipped = 1;
        lcu_fill_cu_info(lcu, x_local, y_local, cu_width, cu_width, cur_cu);
      }

      if (cur_cu->tr_depth != depth) {
        // Reset transform depth since there are no coefficients. This
        // ensures that CBF is cleared for the whole area of the CU.
        uvg_lcu_fill_trdepth(lcu, x, y, depth, depth);
      }

      cur_cu->cbf = 0;
      lcu_fill_cbf(lcu, x_local, y_local, cu_width, cur_cu);
    }
  }

  bool can_split_cu =
    // If the CU is partially outside the frame, we need to split it even
    // if pu_depth_intra and pu_depth_inter would not permit it.
    cur_cu->type == CU_NOTSET ||
    depth < pu_depth_intra.max ||
    (state->frame->slicetype != UVG_SLICE_I &&
      depth < pu_depth_inter.max);

  // Recursively split all the way to max search depth.
  if (can_split_cu) {
    int half_cu = cu_width / 2;
    double split_cost = 0.0;
    int cbf = cbf_is_set_any(cur_cu->cbf, depth);

    if (depth < MAX_DEPTH) {
      // Add cost of cu_split_flag.
      uint8_t split_model = get_ctx_cu_split_model(lcu, x, y, depth);
      const cabac_ctx_t *ctx = &(state->cabac.ctx.split_flag_model[split_model]);
      cost += CTX_ENTROPY_FBITS(ctx, 0) * state->lambda;
      split_cost += CTX_ENTROPY_FBITS(ctx, 1) * state->lambda;
    }

    if (cur_cu->type == CU_INTRA && depth == MAX_DEPTH) {
      // Add cost of intra part_size.
      const cabac_ctx_t *ctx = &(state->cabac.ctx.part_size_model[0]);
      cost += CTX_ENTROPY_FBITS(ctx, 1) * state->lambda;  // 2Nx2N
      split_cost += CTX_ENTROPY_FBITS(ctx, 0) * state->lambda;  // NxN
    }

    // If skip mode was selected for the block, skip further search.
    // Skip mode means there's no coefficients in the block, so splitting
    // might not give any better results but takes more time to do.
    // It is ok to interrupt the search as soon as it is known that
    // the split costs at least as much as not splitting.
    if (cur_cu->type == CU_NOTSET || cbf || state->encoder_control->cfg.cu_split_termination == UVG_CU_SPLIT_TERMINATION_OFF) {
      if (split_cost < cost) split_cost += search_cu(state, x,           y,           depth + 1, work_tree);
      if (split_cost < cost) split_cost += search_cu(state, x + half_cu, y,           depth + 1, work_tree);
      if (split_cost < cost) split_cost += search_cu(state, x,           y + half_cu, depth + 1, work_tree);
      if (split_cost < cost) split_cost += search_cu(state, x + half_cu, y + half_cu, depth + 1, work_tree);
    } else {
      split_cost = INT_MAX;
    }

    // If no search is not performed for this depth, try just the best mode
    // of the top left CU from the next depth. This should ensure that 64x64
    // gets used, at least in the most obvious cases, while avoiding any
    // searching.
    
    if (cur_cu->type == CU_NOTSET && depth < MAX_PU_DEPTH
        && x + cu_width <= frame->width && y + cu_width <= frame->height && 0)
    {
      cu_info_t *cu_d1 = LCU_GET_CU_AT_PX(&work_tree[depth + 1], x_local, y_local);

      // If the best CU in depth+1 is intra and the biggest it can be, try it.
      if (cu_d1->type == CU_INTRA && cu_d1->depth == depth + 1) {
        cost = 0;

        cur_cu->intra = cu_d1->intra;
        cur_cu->type = CU_INTRA;
        cur_cu->part_size = SIZE_2Nx2N;

        // Disable MRL in this case
        cur_cu->intra.multi_ref_idx = 0;

        uvg_lcu_fill_trdepth(lcu, x, y, depth, cur_cu->tr_depth);
        lcu_fill_cu_info(lcu, x_local, y_local, cu_width, cu_width, cur_cu);

        const bool has_chroma = state->encoder_control->chroma_format != UVG_CSP_400;
        const int8_t mode_chroma = has_chroma ? cur_cu->intra.mode_chroma : -1;
        uvg_intra_recon_cu(state,
                           x, y,
                           depth,
                           cur_cu->intra.mode, mode_chroma,
                           NULL,NULL, 0, cur_cu->intra.mip_flag, cur_cu->intra.mip_is_transposed,
                           lcu);

        cost += uvg_cu_rd_cost_luma(state, x_local, y_local, depth, cur_cu, lcu);
        if (has_chroma) {
          cost += uvg_cu_rd_cost_chroma(state, x_local, y_local, depth, cur_cu, lcu);
        }

        // Add the cost of coding no-split.
        uint8_t split_model = get_ctx_cu_split_model(lcu, x, y, depth);
        const cabac_ctx_t *ctx = &(state->cabac.ctx.split_flag_model[split_model]);
        cost += CTX_ENTROPY_FBITS(ctx, 0) * state->lambda;

        // Add the cost of coding intra mode only once.
        double mode_bits = calc_mode_bits(state, lcu, cur_cu, x, y, depth);
        cost += mode_bits * state->lambda;
      }
    }

    if (split_cost < cost) {
      // Copy split modes to this depth.
      cost = split_cost;
      work_tree_copy_up(x_local, y_local, depth, work_tree, state->encoder_control->cfg.jccr);
#if UVG_DEBUG
      //debug_split = 1;
#endif
    } else if (depth > 0) {
      // Copy this CU's mode all the way down for use in adjacent CUs mode
      // search.
      work_tree_copy_down(x_local, y_local, depth, work_tree);
      downsample_cclm_rec(
        state, x, y, cu_width / 2, cu_width / 2, lcu->rec.y, lcu->left_ref.y[64]
      );

      if (state->frame->slicetype != UVG_SLICE_I) {
        // Reset HMVP to the beginning of this CU level search and add this CU as the mvp
        memcpy(&state->tile->frame->hmvp_lut[ctu_row_mul_five], hmvp_lut, sizeof(cu_info_t) * MAX_NUM_HMVP_CANDS);
        state->tile->frame->hmvp_size[ctu_row] = hmvp_lut_size;
        uvg_hmvp_add_mv(state, x, y, cu_width, cu_width, cur_cu);
      }
    }
  } else if (depth >= 0 && depth < MAX_PU_DEPTH) {
    // Need to copy modes down since the lower level of the work tree is used
    // when searching SMP and AMP blocks.
    work_tree_copy_down(x_local, y_local, depth, work_tree);
    downsample_cclm_rec(
      state, x, y, cu_width / 2, cu_width / 2, lcu->rec.y, lcu->left_ref.y[64]
    );

    if (state->frame->slicetype != UVG_SLICE_I) {
      // Reset HMVP to the beginning of this CU level search and add this CU as the mvp
      memcpy(&state->tile->frame->hmvp_lut[ctu_row_mul_five], hmvp_lut, sizeof(cu_info_t) * MAX_NUM_HMVP_CANDS);
      state->tile->frame->hmvp_size[ctu_row] = hmvp_lut_size;
      uvg_hmvp_add_mv(state, x, y, cu_width, cu_width, cur_cu);
    }
  }

  assert(cur_cu->type != CU_NOTSET);

  return cost;
}


/**
 * Initialize lcu_t for search.
 * - Copy reference CUs.
 * - Copy reference pixels from neighbouring LCUs.
 * - Copy reference pixels from this LCU.
 */
static void init_lcu_t(const encoder_state_t * const state, const int x, const int y, lcu_t *lcu, const yuv_t *hor_buf, const yuv_t *ver_buf)
{
  const videoframe_t * const frame = state->tile->frame;

  FILL(*lcu, 0);
  
  lcu->rec.chroma_format = state->encoder_control->chroma_format;
  lcu->ref.chroma_format = state->encoder_control->chroma_format;

  // Copy reference cu_info structs from neighbouring LCUs.

  // Copy top CU row.
  if (y > 0) {
    for (int i = 0; i < LCU_WIDTH; i += SCU_WIDTH) {
      const cu_info_t *from_cu = uvg_cu_array_at_const(frame->cu_array, x + i, y - 1);
      cu_info_t *to_cu = LCU_GET_CU_AT_PX(lcu, i, -1);
      memcpy(to_cu, from_cu, sizeof(*to_cu));
    }
  }
  // Copy left CU column.
  if (x > 0) {
    for (int i = 0; i < LCU_WIDTH; i += SCU_WIDTH) {
      const cu_info_t *from_cu = uvg_cu_array_at_const(frame->cu_array, x - 1, y + i);
      cu_info_t *to_cu = LCU_GET_CU_AT_PX(lcu, -1, i);
      memcpy(to_cu, from_cu, sizeof(*to_cu));
    }
  }
  // Copy top-left CU.
  if (x > 0 && y > 0) {
    const cu_info_t *from_cu = uvg_cu_array_at_const(frame->cu_array, x - 1, y - 1);
    cu_info_t *to_cu = LCU_GET_CU_AT_PX(lcu, -1, -1);
    memcpy(to_cu, from_cu, sizeof(*to_cu));
  }

  // Copy top-right CU, available only without WPP
  if (y > 0 && x + LCU_WIDTH < frame->width && !state->encoder_control->cfg.wpp) {
    const cu_info_t *from_cu = uvg_cu_array_at_const(frame->cu_array, x + LCU_WIDTH, y - 1);
    cu_info_t *to_cu = LCU_GET_TOP_RIGHT_CU(lcu);
    memcpy(to_cu, from_cu, sizeof(*to_cu));
  }

  // Copy reference pixels.
  {
    const int pic_width = frame->width;
    // Copy top reference pixels.
    if (y > 0) {
      // hor_buf is of size pic_width so there might not be LCU_REF_PX_WIDTH
      // number of allocated pixels left.
      int x_max = MIN(LCU_REF_PX_WIDTH, pic_width - x);
      int x_min_in_lcu = (x>0) ? 0 : 1;
      int luma_offset = OFFSET_HOR_BUF(x, y, frame, x_min_in_lcu - 1);
      int chroma_offset = OFFSET_HOR_BUF_C(x, y, frame, x_min_in_lcu - 1);
      int luma_bytes = (x_max + (1 - x_min_in_lcu))*sizeof(uvg_pixel);
      int chroma_bytes = (x_max / 2 + (1 - x_min_in_lcu))*sizeof(uvg_pixel);

      memcpy(&lcu->top_ref.y[x_min_in_lcu], &hor_buf->y[luma_offset], luma_bytes);

      if (state->encoder_control->chroma_format != UVG_CSP_400) {
        memcpy(&lcu->top_ref.u[x_min_in_lcu], &hor_buf->u[chroma_offset], chroma_bytes);
        memcpy(&lcu->top_ref.v[x_min_in_lcu], &hor_buf->v[chroma_offset], chroma_bytes);
      }
    }
    // Copy left reference pixels.
    if (x > 0) {
      int y_min_in_lcu = (y>0) ? 0 : 1;
      int luma_offset = OFFSET_VER_BUF(x, y, frame, y_min_in_lcu - 1);
      int chroma_offset = OFFSET_VER_BUF_C(x, y, frame, y_min_in_lcu - 1);
      int luma_bytes = (LCU_WIDTH + (1 - y_min_in_lcu)) * sizeof(uvg_pixel);
      int chroma_bytes = (LCU_WIDTH / 2 + (1 - y_min_in_lcu)) * sizeof(uvg_pixel);

      memcpy(&lcu->left_ref.y[y_min_in_lcu], &ver_buf->y[luma_offset], luma_bytes);

      if (state->encoder_control->chroma_format != UVG_CSP_400) {
        memcpy(&lcu->left_ref.u[y_min_in_lcu], &ver_buf->u[chroma_offset], chroma_bytes);
        memcpy(&lcu->left_ref.v[y_min_in_lcu], &ver_buf->v[chroma_offset], chroma_bytes);
      }
    }
  }

  // Copy LCU pixels.
  {
    const videoframe_t * const frame = state->tile->frame;
    int x_max = MIN(x + LCU_WIDTH, frame->width) - x;
    int y_max = MIN(y + LCU_WIDTH, frame->height) - y;

    int x_c = x / 2;
    int y_c = y / 2;
    int x_max_c = x_max / 2;
    int y_max_c = y_max / 2;

    uvg_pixel* source = NULL;
    if (state->tile->frame->lmcs_aps->m_sliceReshapeInfo.sliceReshaperEnableFlag) {
      source = frame->source_lmcs->y;
    } else {
      source = frame->source->y;
    }

    // Use LMCS pixels for luma if they are available, otherwise source_lmcs is mapped to normal source
    uvg_pixels_blit(&source[x + y * frame->source->stride], lcu->ref.y,
                        x_max, y_max, frame->source->stride, LCU_WIDTH);
    if (state->encoder_control->chroma_format != UVG_CSP_400) {
      uvg_pixels_blit(&frame->source->u[x_c + y_c * frame->source->stride / 2], lcu->ref.u,
                      x_max_c, y_max_c, frame->source->stride / 2, LCU_WIDTH / 2);
      uvg_pixels_blit(&frame->source->v[x_c + y_c * frame->source->stride / 2], lcu->ref.v,
                      x_max_c, y_max_c, frame->source->stride / 2, LCU_WIDTH / 2);
    }
  }
}


/**
 * Copy CU and pixel data to it's place in picture datastructure.
 */
static void copy_lcu_to_cu_data(const encoder_state_t * const state, int x_px, int y_px, const lcu_t *lcu)
{
  // Copy non-reference CUs to picture.
  uvg_cu_array_copy_from_lcu(state->tile->frame->cu_array, x_px, y_px, lcu);

  // Copy pixels to picture.
  {
    videoframe_t * const pic = state->tile->frame;
    const int pic_width = pic->width;
    const int x_max = MIN(x_px + LCU_WIDTH, pic_width) - x_px;
    const int y_max = MIN(y_px + LCU_WIDTH, pic->height) - y_px;

    uvg_pixels_blit(lcu->rec.y, &pic->rec->y[x_px + y_px * pic->rec->stride],
                        x_max, y_max, LCU_WIDTH, pic->rec->stride);

    if (state->tile->frame->lmcs_aps->m_sliceReshapeInfo.sliceReshaperEnableFlag) {
      uvg_pixels_blit(lcu->rec.y, &pic->rec_lmcs->y[x_px + y_px * pic->rec->stride],
        x_max, y_max, LCU_WIDTH, pic->rec->stride);
    }

    if (state->encoder_control->chroma_format != UVG_CSP_400) {
      uvg_pixels_blit(lcu->rec.u, &pic->rec->u[(x_px / 2) + (y_px / 2) * (pic->rec->stride / 2)],
                      x_max / 2, y_max / 2, LCU_WIDTH / 2, pic->rec->stride / 2);
      uvg_pixels_blit(lcu->rec.v, &pic->rec->v[(x_px / 2) + (y_px / 2) * (pic->rec->stride / 2)],
                      x_max / 2, y_max / 2, LCU_WIDTH / 2, pic->rec->stride / 2);
    }
  }
}


/**
 * Search LCU for modes.
 * - Best mode gets copied to current picture.
 */
void uvg_search_lcu(encoder_state_t * const state, const int x, const int y, const yuv_t * const hor_buf, const yuv_t * const ver_buf, lcu_coeff_t *coeff)
{
  assert(x % LCU_WIDTH == 0);
  assert(y % LCU_WIDTH == 0);

  // Initialize the same starting state to every depth. The search process
  // will use these as temporary storage for predictions before making
  // a decision on which to use, and they get updated during the search
  // process.
  lcu_t work_tree[MAX_PU_DEPTH + 1];
  init_lcu_t(state, x, y, &work_tree[0], hor_buf, ver_buf);
  for (int depth = 1; depth <= MAX_PU_DEPTH; ++depth) {
    work_tree[depth] = work_tree[0];
  }

  // If the ML depth prediction is enabled, 
  // generate the depth prediction interval 
  // for the current lcu
  constraint_t* constr = state->constraint;
  if (constr->ml_intra_depth_ctu) {
    uvg_lcu_luma_depth_pred(constr->ml_intra_depth_ctu, work_tree[0].ref.y, state->qp);
  }

  // Start search from depth 0.
  double cost = search_cu(state, x, y, 0, work_tree);

  // Save squared cost for rate control.
  if(state->encoder_control->cfg.rc_algorithm == UVG_LAMBDA) {
    uvg_get_lcu_stats(state, x / LCU_WIDTH, y / LCU_WIDTH)->weight = cost * cost;
  }

  // The best decisions through out the LCU got propagated back to depth 0,
  // so copy those back to the frame.
  copy_lcu_to_cu_data(state, x, y, &work_tree[0]);

  // Copy coeffs to encoder state.
  copy_coeffs(work_tree[0].coeff.y, coeff->y, LCU_WIDTH);
  copy_coeffs(work_tree[0].coeff.u, coeff->u, LCU_WIDTH_C);
  copy_coeffs(work_tree[0].coeff.v, coeff->v, LCU_WIDTH_C);
  if (state->encoder_control->cfg.jccr) {
    copy_coeffs(work_tree[0].coeff.joint_uv, coeff->joint_uv, LCU_WIDTH_C);
  }
}
