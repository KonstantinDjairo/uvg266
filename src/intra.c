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

#include "intra.h"

#include <stdlib.h>

#include "image.h"
#include "kvz_math.h"
#include "strategies/strategies-intra.h"
#include "tables.h"
#include "transform.h"
#include "videoframe.h"

// Tables for looking up the number of intra reference pixels based on
// prediction units coordinate within an LCU.
// generated by "tools/generate_ref_pixel_tables.py".
static const uint8_t num_ref_pixels_top[16][16] = {
  { 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64 },
  {  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4 },
  { 16, 12,  8,  4, 16, 12,  8,  4, 16, 12,  8,  4, 16, 12,  8,  4 },
  {  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4 },
  { 32, 28, 24, 20, 16, 12,  8,  4, 32, 28, 24, 20, 16, 12,  8,  4 },
  {  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4 },
  { 16, 12,  8,  4, 16, 12,  8,  4, 16, 12,  8,  4, 16, 12,  8,  4 },
  {  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4 },
  { 64, 60, 56, 52, 48, 44, 40, 36, 32, 28, 24, 20, 16, 12,  8,  4 },
  {  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4 },
  { 16, 12,  8,  4, 16, 12,  8,  4, 16, 12,  8,  4, 16, 12,  8,  4 },
  {  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4 },
  { 32, 28, 24, 20, 16, 12,  8,  4, 32, 28, 24, 20, 16, 12,  8,  4 },
  {  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4 },
  { 16, 12,  8,  4, 16, 12,  8,  4, 16, 12,  8,  4, 16, 12,  8,  4 },
  {  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4 }
};
static const uint8_t num_ref_pixels_left[16][16] = {
  { 64,  4,  8,  4, 16,  4,  8,  4, 32,  4,  8,  4, 16,  4,  8,  4 },
  { 60,  4,  4,  4, 12,  4,  4,  4, 28,  4,  4,  4, 12,  4,  4,  4 },
  { 56,  4,  8,  4,  8,  4,  8,  4, 24,  4,  8,  4,  8,  4,  8,  4 },
  { 52,  4,  4,  4,  4,  4,  4,  4, 20,  4,  4,  4,  4,  4,  4,  4 },
  { 48,  4,  8,  4, 16,  4,  8,  4, 16,  4,  8,  4, 16,  4,  8,  4 },
  { 44,  4,  4,  4, 12,  4,  4,  4, 12,  4,  4,  4, 12,  4,  4,  4 },
  { 40,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4 },
  { 36,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4 },
  { 32,  4,  8,  4, 16,  4,  8,  4, 32,  4,  8,  4, 16,  4,  8,  4 },
  { 28,  4,  4,  4, 12,  4,  4,  4, 28,  4,  4,  4, 12,  4,  4,  4 },
  { 24,  4,  8,  4,  8,  4,  8,  4, 24,  4,  8,  4,  8,  4,  8,  4 },
  { 20,  4,  4,  4,  4,  4,  4,  4, 20,  4,  4,  4,  4,  4,  4,  4 },
  { 16,  4,  8,  4, 16,  4,  8,  4, 16,  4,  8,  4, 16,  4,  8,  4 },
  { 12,  4,  4,  4, 12,  4,  4,  4, 12,  4,  4,  4, 12,  4,  4,  4 },
  { 8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4,  8,  4 },
  { 4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4 }
};

int8_t kvz_intra_get_dir_luma_predictor(
  const uint32_t x,
  const uint32_t y,
  int8_t *preds,
  const cu_info_t *const cur_pu,
  const cu_info_t *const left_pu,
  const cu_info_t *const above_pu)
{
  enum {
    PLANAR_IDX = 0,
    DC_IDX = 1,
    HOR_IDX = 18,
    VER_IDX = 50,
  };

  int8_t number_of_candidates = 0;

  // The default mode if block is not coded yet is INTRA_PLANAR.
  int8_t left_intra_dir  = 0;
  if (left_pu && left_pu->type == CU_INTRA) {
    left_intra_dir = left_pu->intra.mode;
  }

  int8_t above_intra_dir = 0;
  if (above_pu && above_pu->type == CU_INTRA && y % LCU_WIDTH != 0) {
    above_intra_dir = above_pu->intra.mode;
  }

  const int offset = 61;
  const int mod = 64;

  preds[0] = PLANAR_IDX;
  preds[1] = DC_IDX;
  preds[2] = VER_IDX;
  preds[3] = HOR_IDX;
  preds[4] = VER_IDX - 4;
  preds[5] = VER_IDX + 4;

  // If the predictions are the same, add new predictions
  if (left_intra_dir == above_intra_dir) {
    number_of_candidates = 1;
    if (left_intra_dir > DC_IDX) { // angular modes
      preds[0] = PLANAR_IDX;
      preds[1] = left_intra_dir;
      preds[2] = ((left_intra_dir + offset) % mod) + 2;
      preds[3] = ((left_intra_dir - 1) % mod) + 2;
      preds[4] = ((left_intra_dir + offset - 1) % mod) + 2;
      preds[5] = (left_intra_dir % mod) + 2;
    }
  } else { // If we have two distinct predictions
    number_of_candidates = 2;
    uint8_t max_cand_mode_idx = preds[0] > preds[1] ? 0 : 1;
    
    if (left_intra_dir > DC_IDX && above_intra_dir > DC_IDX) {
      preds[0] = PLANAR_IDX;
      preds[1] = left_intra_dir;
      preds[2] = above_intra_dir;
      max_cand_mode_idx = preds[1] > preds[2] ? 1 : 2;
      uint8_t min_cand_mode_idx = preds[1] > preds[2] ? 2 : 1;

      if (preds[max_cand_mode_idx] - preds[min_cand_mode_idx] == 1) {
        preds[3] = ((preds[min_cand_mode_idx] + offset) % mod) + 2;
        preds[4] = ((preds[max_cand_mode_idx] - 1) % mod) + 2;
        preds[5] = ((preds[min_cand_mode_idx] + offset - 1) % mod) + 2;
      } else  if (preds[max_cand_mode_idx] - preds[min_cand_mode_idx] >= 62) {
        preds[3] = ((preds[min_cand_mode_idx] - 1) % mod) + 2; 
        preds[4] = ((preds[max_cand_mode_idx] + offset) % mod) + 2;
        preds[5] = (preds[min_cand_mode_idx] % mod) + 2;
      } else  if (preds[max_cand_mode_idx] - preds[min_cand_mode_idx] == 2) {
        preds[3] = ((preds[min_cand_mode_idx] - 1) % mod) + 2;
        preds[4] = ((preds[min_cand_mode_idx] + offset) % mod) + 2;
        preds[5] = ((preds[max_cand_mode_idx] - 1) % mod) + 2;
      } else {
        preds[3] = ((preds[min_cand_mode_idx] + offset) % mod) + 2;
        preds[4] = ((preds[min_cand_mode_idx] - 1) % mod) + 2;
        preds[5] = ((preds[max_cand_mode_idx] + offset) % mod) + 2;
      }
    } else if(left_intra_dir + above_intra_dir >= 2){  // Add DC mode if it's not present, otherwise VER_IDX.
      preds[0] = PLANAR_IDX;
      preds[1] = (left_intra_dir < above_intra_dir) ? above_intra_dir : left_intra_dir;
      
      max_cand_mode_idx = 1;

      preds[2] = ((preds[max_cand_mode_idx] + offset) % mod) + 2;
      preds[3] = ((preds[max_cand_mode_idx] - 1) % mod) + 2;
      preds[4] = ((preds[max_cand_mode_idx] +offset - 1) % mod) + 2;
      preds[5] = ( preds[max_cand_mode_idx] % mod) + 2;
    }
  }

  return number_of_candidates;
}

static void intra_filter_reference(
  int_fast8_t log2_width,
  kvz_intra_references *refs)
{
  if (refs->filtered_initialized) {
    return;
  } else {
    refs->filtered_initialized = true;
  }

  const int_fast8_t ref_width = 2 * (1 << log2_width) + 1;
  kvz_intra_ref *ref = &refs->ref;
  kvz_intra_ref *filtered_ref = &refs->filtered_ref;

  // Starting point at top left for both iterations
  filtered_ref->left[0] = (ref->left[1] + 2 * ref->left[0] + ref->top[1] + 2) >> 2;
  filtered_ref->top[0] = filtered_ref->left[0];

  // TODO: use block height here instead of ref_width
  // Top to bottom
  for (int_fast8_t y = 1; y < ref_width - 1; ++y) {
    kvz_pixel *p = &ref->left[y];
    filtered_ref->left[y] = (p[-1] + 2 * p[0] + p[1] + 2) >> 2;
  }
  // Bottom left (not filtered) 
  filtered_ref->left[ref_width - 1] = ref->left[ref_width - 1];

  // Left to right
  for (int_fast8_t x = 1; x < ref_width - 1; ++x) {
    kvz_pixel *p = &ref->top[x];
    filtered_ref->top[x] = (p[-1] + 2 * p[0] + p[1] + 2) >> 2;
  }
  // Top right (not filtered)
  filtered_ref->top[ref_width - 1] = ref->top[ref_width - 1];
}


/**
* \brief Generage planar prediction.
* \param log2_width    Log2 of width, range 2..5.
* \param in_ref_above  Pointer to -1 index of above reference, length=width*2+1.
* \param in_ref_left   Pointer to -1 index of left reference, length=width*2+1.
* \param dst           Buffer of size width*width.
*/
static void intra_pred_dc(
  const int_fast8_t log2_width,
  const kvz_pixel *const ref_top,
  const kvz_pixel *const ref_left,
  kvz_pixel *const out_block)
{
  int_fast8_t width = 1 << log2_width;

  int_fast16_t sum = 0;
  for (int_fast8_t i = 0; i < width; ++i) {
    sum += ref_top[i + 1];
    sum += ref_left[i + 1];
  }
  
  // JVET_K0122
  // TODO: take non-square blocks into account
  const int denom     = width << 1;
  const int divShift  = kvz_math_floor_log2(denom);
  const int divOffset = denom >> 1;
  
  const kvz_pixel dc_val = (sum + divOffset) >> divShift;
  //const kvz_pixel dc_val = (sum + width) >> (log2_width + 1);
  const int_fast16_t block_size = 1 << (log2_width * 2);

  for (int_fast16_t i = 0; i < block_size; ++i) {
    out_block[i] = dc_val;
  }
}


enum lm_mode
{
  LM_CHROMA_IDX = 81,
  LM_CHROMA_L_IDX = 82,
  LM_CHROMA_T_IDX = 83,
};


static void get_cclm_parameters(
  encoder_state_t const* const state,
  int8_t width, int8_t height, int8_t mode,
  int x0, int y0, int avai_above_right_units, int avai_left_below_units,
  kvz_intra_ref* luma_src, kvz_intra_references*chroma_ref,
  int16_t *a, int16_t*b, int16_t*shift) {

  const int base_unit_size = 1 << (6 - PU_DEPTH_INTRA_MAX);

  // TODO: take into account YUV422
  const int unit_w = base_unit_size >> 1;
  const int unit_h = base_unit_size >> 1;

  const int c_height = height;
  const int c_width = width;
  height *= 2;
  width *= 2;

  const int tu_width_in_units = c_width / unit_w;
  const int tu_height_in_units = c_height / unit_h;


  //int top_template_samp_num = width; // for MDLM, the template sample number is 2W or 2H;
  //int left_template_samp_num = height;

  // These are used for calculating some stuff for non-square CUs
  //int total_above_units = (top_template_samp_num + (unit_w - 1)) / unit_w;
  //int total_left_units = (left_template_samp_num + (unit_h - 1)) / unit_h;
  //int total_units = total_left_units + total_above_units + 1;
  //int above_right_units = total_above_units - tu_width_in_units;
  //int left_below_units = total_left_units - tu_height_in_units;
  //int avai_above_right_units = 0;  // TODO these are non zero only with non-square CUs
  //int avai_left_below_units = 0;
  int avai_above_units = CLIP(0, tu_height_in_units, y0/base_unit_size);
  int avai_left_units = CLIP(0, tu_width_in_units, x0 / base_unit_size);

  bool above_available = avai_above_units != 0;
  bool left_available = avai_left_units != 0;
    
  char internal_bit_depth = state->encoder_control->bitdepth;

  int min_luma[2] = { MAX_INT, 0 };
  int max_luma[2] = { -MAX_INT, 0 };
  
  kvz_pixel* src;
  int actualTopTemplateSampNum = 0;
  int actualLeftTemplateSampNum = 0;
  if (mode == LM_CHROMA_T_IDX)
  {
    left_available = 0;
    avai_above_right_units = avai_above_right_units > (c_height / unit_w) ? c_height / unit_w : avai_above_right_units;
    actualTopTemplateSampNum = unit_w * (avai_above_units + avai_above_right_units);
  }
  else if (mode == LM_CHROMA_L_IDX)
  {
    above_available = 0;
    avai_left_below_units = avai_left_below_units > (c_width / unit_h) ? c_width / unit_h : avai_left_below_units;
    actualLeftTemplateSampNum = unit_h * (avai_left_units + avai_left_below_units);
  }
  else if (mode == LM_CHROMA_IDX)
  {
    actualTopTemplateSampNum = c_width;
    actualLeftTemplateSampNum = c_height;
  }
  int startPos[2]; //0:Above, 1: Left
  int pickStep[2];

  int aboveIs4 = left_available ? 0 : 1;
  int leftIs4 = above_available ? 0 : 1;

  startPos[0] = actualTopTemplateSampNum >> (2 + aboveIs4);
  pickStep[0] = MAX(1, actualTopTemplateSampNum >> (1 + aboveIs4));

  startPos[1] = actualLeftTemplateSampNum >> (2 + leftIs4);
  pickStep[1] = MAX(1, actualLeftTemplateSampNum >> (1 + leftIs4));

  kvz_pixel selectLumaPix[4] = { 0, 0, 0, 0 };
  kvz_pixel selectChromaPix[4] = { 0, 0, 0, 0 };

  int cntT, cntL;
  cntT = cntL = 0;
  int cnt = 0;
  if (above_available)
  {
    cntT = MIN(actualTopTemplateSampNum, (1 + aboveIs4) << 1);
    src = luma_src->top;
    const kvz_pixel* cur = chroma_ref->ref.top + 1;
    for (int pos = startPos[0]; cnt < cntT; pos += pickStep[0], cnt++)
    {
      selectLumaPix[cnt] = src[pos];
      selectChromaPix[cnt] = cur[pos];
    }
  }

  if (left_available)
  {
    cntL = MIN(actualLeftTemplateSampNum, (1 + leftIs4) << 1);
    src = luma_src->left;
    const kvz_pixel* cur = chroma_ref->ref.left + 1;
    for (int pos = startPos[1], cnt = 0; cnt < cntL; pos += pickStep[1], cnt++)
    {
      selectLumaPix[cnt + cntT] = src[pos];
      selectChromaPix[cnt + cntT] = cur[pos];
    }
  }
  cnt = cntL + cntT;

  if (cnt == 2)
  {
    selectLumaPix[3] = selectLumaPix[0]; selectChromaPix[3] = selectChromaPix[0];
    selectLumaPix[2] = selectLumaPix[1]; selectChromaPix[2] = selectChromaPix[1];
    selectLumaPix[0] = selectLumaPix[1]; selectChromaPix[0] = selectChromaPix[1];
    selectLumaPix[1] = selectLumaPix[3]; selectChromaPix[1] = selectChromaPix[3];
  }

  int minGrpIdx[2] = { 0, 2 };
  int maxGrpIdx[2] = { 1, 3 };
  int* tmpMinGrp = minGrpIdx;
  int* tmpMaxGrp = maxGrpIdx;
  if (selectLumaPix[tmpMinGrp[0]] > selectLumaPix[tmpMinGrp[1]])
  {
    SWAP(tmpMinGrp[0], tmpMinGrp[1], int);
  }
  if (selectLumaPix[tmpMaxGrp[0]] > selectLumaPix[tmpMaxGrp[1]])
  {
    SWAP(tmpMaxGrp[0], tmpMaxGrp[1], int);
  }
  if (selectLumaPix[tmpMinGrp[0]] > selectLumaPix[tmpMaxGrp[1]])
  {
    SWAP(tmpMinGrp, tmpMaxGrp, int*);
  }
  if (selectLumaPix[tmpMinGrp[1]] > selectLumaPix[tmpMaxGrp[0]])
  {
    SWAP(tmpMinGrp[1], tmpMaxGrp[0], int);
  }

  min_luma[0] = (selectLumaPix[tmpMinGrp[0]] + selectLumaPix[tmpMinGrp[1]] + 1) >> 1;
  min_luma[1] = (selectChromaPix[tmpMinGrp[0]] + selectChromaPix[tmpMinGrp[1]] + 1) >> 1;
  max_luma[0] = (selectLumaPix[tmpMaxGrp[0]] + selectLumaPix[tmpMaxGrp[1]] + 1) >> 1;
  max_luma[1] = (selectChromaPix[tmpMaxGrp[0]] + selectChromaPix[tmpMaxGrp[1]] + 1) >> 1;

  if (left_available || above_available)
  {
    int diff = max_luma[0] - min_luma[0];
    if (diff > 0)
    {
      int diffC = max_luma[1] - min_luma[1];
      int x = kvz_math_floor_log2(diff);
      static const uint8_t DivSigTable[1 << 4] = {
        // 4bit significands - 8 ( MSB is omitted )
        0,  7,  6,  5,  5,  4,  4,  3,  3,  2,  2,  1,  1,  1,  1,  0
      };
      int normDiff = (diff << 4 >> x) & 15;
      int v = DivSigTable[normDiff] | 8;
      x += normDiff != 0;

      int y = diffC ? kvz_math_floor_log2(abs(diffC)) + 1 : 0;
      int add = 1 << y >> 1;
      *a = (diffC * v + add) >> y;
      *shift = 3 + x - y;
      if (*shift < 1)
      {
        *shift = 1;
        *a = ((*a == 0) ? 0 : (*a < 0) ? -15 : 15);   // a=Sign(a)*15
      }
      *b = min_luma[1] - ((*a * min_luma[0]) >> *shift);
    }
    else
    {
      *a = 0;
      *b = min_luma[1];
      *shift = 0;
    }
  }
  else
  {
    *a = 0;

    *b = 1 << (internal_bit_depth - 1);

    *shift = 0;
  }
}

static void linear_transform_cclm(cclm_parameters_t* cclm_params, kvz_pixel * src, kvz_pixel * dst, int stride, int height) {
  int scale = cclm_params->a;
  int shift = cclm_params->shift;
  int offset = cclm_params->b;
  for (int y = 0; y < height; ++y) {
    for (int x=0; x < stride; ++x) {
      int val = src[x + y * stride] * scale;
      val >>= shift;
      val += offset;
      val = CLIP_TO_PIXEL(val);
      dst[x + y * stride] = val;
    }
  }
}


void kvz_predict_cclm(
  encoder_state_t const* const state,
  const color_t color,
  const int8_t width,
  const int8_t height,
  const int16_t x0,
  const int16_t y0,
  const int16_t stride,
  const int8_t mode,
  lcu_t* const lcu,
  kvz_intra_references* chroma_ref,
  kvz_pixel* dst,
  cclm_parameters_t* cclm_params
)
{
  assert(mode == LM_CHROMA_IDX || mode == LM_CHROMA_L_IDX || mode == LM_CHROMA_T_IDX);
  assert(state->encoder_control->cfg.cclm);

  
  kvz_intra_ref sampled_luma_ref;
  kvz_pixel sampled_luma[LCU_CHROMA_SIZE];

  int x_scu = SUB_SCU(x0);
  int y_scu = SUB_SCU(y0);

  int available_above_right = 0;
  int available_left_below = 0;


  kvz_pixel *y_rec = lcu->rec.y + x_scu + y_scu * LCU_WIDTH;

  // Essentially what this does is that it uses 6-tap filtering to downsample
  // the luma intra references down to match the resolution of the chroma channel.
  // The luma reference is only needed when we are not on the edge of the picture.
  // Because the reference pixels that are needed on the edge of the ctu this code
  // is kinda messy but what can you do

  if (y0) {
    for (; available_above_right < width / 2; available_above_right++) {
      int x_extension = x_scu + width * 2 + 4 * available_above_right;
      cu_info_t* pu = LCU_GET_CU_AT_PX(lcu, x_extension, y_scu - 4);
      if (x_extension >= LCU_WIDTH || pu->type == CU_NOTSET) break;
    }
    if(y_scu == 0) {
      if(!state->encoder_control->cfg.wpp) available_above_right = MIN(width / 2, (state->tile->frame->width - x0 - width * 2) / 4);
      memcpy(sampled_luma_ref.top, &state->tile->frame->cclm_luma_rec_top_line[x0 / 2 + (y0 / 64 - 1) * (stride / 2)], sizeof(kvz_pixel) * (width + available_above_right * 2));
    }
    else {
      for (int x = 0; x < width * (available_above_right ? 4 : 2); x += 2) {
        bool left_padding = x0 || x;
        int s = 4;
        s += y_scu ? y_rec[x - LCU_WIDTH * 2] * 2            : state->tile->frame->rec->y[x0 + x + (y0 - 2) * stride] * 2;
        s += y_scu ? y_rec[x - LCU_WIDTH * 2 + 1]            : state->tile->frame->rec->y[x0 + x + 1 + (y0 - 2) * stride];
        s += y_scu && !(x0 && !x && !x_scu) ? y_rec[x - LCU_WIDTH * 2 - left_padding] : state->tile->frame->rec->y[x0 + x - left_padding + (y0 - 2) * stride];
        s += y_scu ? y_rec[x - LCU_WIDTH] * 2                : state->tile->frame->rec->y[x0 + x + (y0 - 1) * stride] * 2;
        s += y_scu ? y_rec[x - LCU_WIDTH + 1]                : state->tile->frame->rec->y[x0 + x + 1 + (y0 - 1) * stride];
        s += y_scu && !(x0 && !x && !x_scu) ? y_rec[x - LCU_WIDTH - left_padding]     : state->tile->frame->rec->y[x0 + x - left_padding + (y0 - 1) * stride];
        sampled_luma_ref.top[x / 2] = s >> 3;
      }
    }
  }

  if(x0) {
    for (; available_left_below < height / 2; available_left_below++) {
      int y_extension = y_scu + height * 2 + 4 * available_left_below;
      cu_info_t* pu = LCU_GET_CU_AT_PX(lcu, x_scu - 4, y_extension);
      if (y_extension >= LCU_WIDTH || pu->type == CU_NOTSET) break;
      if(x_scu == 32 && y_scu == 0 && pu->depth == 0) break;
    }
    for(int i = 0; i < height + available_left_below * 2; i++) {
      sampled_luma_ref.left[i] = state->tile->frame->cclm_luma_rec[(y0/2 + i) * (stride/2) + x0 / 2 - 1];
    }    
  }

  kvz_pixels_blit(&state->tile->frame->cclm_luma_rec[x0 / 2 + (y0 * stride) / 4], sampled_luma, width, height, stride / 2, width);

  int16_t a, b, shift;
  get_cclm_parameters(state, width, height, mode,x0, y0, available_above_right, available_left_below, &sampled_luma_ref, chroma_ref, &a, &b, &shift);
  cclm_params->shift = shift;
  cclm_params->a = a;
  cclm_params->b = b;

  if(dst)
    linear_transform_cclm(cclm_params, sampled_luma, dst, width, height);
}

void kvz_intra_predict(
  encoder_state_t *const state,
  kvz_intra_references *refs,
  int_fast8_t log2_width,
  int_fast8_t mode,
  color_t color,
  kvz_pixel *dst,
  bool filter_boundary)
{
  const int_fast8_t width = 1 << log2_width;
  const kvz_config *cfg = &state->encoder_control->cfg;

  const kvz_intra_ref *used_ref = &refs->ref;
  if (cfg->intra_smoothing_disabled || color != COLOR_Y || mode == 1 || width == 4) {
    // For chroma, DC and 4x4 blocks, always use unfiltered reference.
  } else if (mode == 0) {
    // Otherwise, use filtered for planar.
    if (width * width > 32) {
      used_ref = &refs->filtered_ref;
    }
  } else {
    // Angular modes use smoothed reference pixels, unless the mode is close
    // to being either vertical or horizontal.
    static const int kvz_intra_hor_ver_dist_thres[8] = {24, 24, 24, 14, 2, 0, 0, 0 };
    int filter_threshold = kvz_intra_hor_ver_dist_thres[(log2_width + log2_width) >> 1];
    int dist_from_vert_or_hor = MIN(abs(mode - 50), abs(mode - 18));
    if (dist_from_vert_or_hor > filter_threshold) {

      static const int16_t modedisp2sampledisp[32] = { 0,    1,    2,    3,    4,    6,     8,   10,   12,   14,   16,   18,   20,   23,   26,   29,   32,   35,   39,  45,  51,  57,  64,  73,  86, 102, 128, 171, 256, 341, 512, 1024 };
      const int_fast8_t mode_disp = (mode >= 34) ? mode - 50 : 18 - mode;
      const int_fast8_t sample_disp = (mode_disp < 0 ? -1 : 1) * modedisp2sampledisp[abs(mode_disp)];
      if ((abs(sample_disp) & 0x1F) == 0) {
        used_ref = &refs->filtered_ref;
      }
    }
  }

  if (used_ref == &refs->filtered_ref && !refs->filtered_initialized) {
    intra_filter_reference(log2_width, refs);
  }

  if (mode == 0) {
    kvz_intra_pred_planar(log2_width, used_ref->top, used_ref->left, dst);
  } else if (mode == 1) {
    intra_pred_dc(log2_width, used_ref->top, used_ref->left, dst);
  } else {
    kvz_angular_pred(log2_width, mode, color, used_ref->top, used_ref->left, dst);
  }

  // pdpc
  // bool pdpcCondition = (mode == 0 || mode == 1 || mode == 18 || mode == 50);
  bool pdpcCondition = (mode == 0 || mode == 1); // Planar and DC
  if (pdpcCondition)
  {
    kvz_pdpc_planar_dc(mode, width, log2_width, used_ref, dst);
  }
}


void kvz_intra_build_reference_any(
  const int_fast8_t log2_width,
  const color_t color,
  const vector2d_t *const luma_px,
  const vector2d_t *const pic_px,
  const lcu_t *const lcu,
  kvz_intra_references *const refs)
{
  assert(log2_width >= 2 && log2_width <= 5);

  refs->filtered_initialized = false;
  kvz_pixel *out_left_ref = &refs->ref.left[0];
  kvz_pixel *out_top_ref = &refs->ref.top[0];

  const kvz_pixel dc_val = 1 << (KVZ_BIT_DEPTH - 1); //TODO: add used bitdepth as a variable
  const int is_chroma = color != COLOR_Y ? 1 : 0;
  const int_fast8_t width = 1 << log2_width;

  // Convert luma coordinates to chroma coordinates for chroma.
  const vector2d_t lcu_px = {
    luma_px->x % LCU_WIDTH,
    luma_px->y % LCU_WIDTH
  };
  const vector2d_t px = {
    lcu_px.x >> is_chroma,
    lcu_px.y >> is_chroma,
  };

  // Init pointers to LCUs reconstruction buffers, such that index 0 refers to block coordinate 0.
  const kvz_pixel *left_ref = !color ? &lcu->left_ref.y[1] : (color == 1) ? &lcu->left_ref.u[1] : &lcu->left_ref.v[1];
  const kvz_pixel *top_ref = !color ? &lcu->top_ref.y[1] : (color == 1) ? &lcu->top_ref.u[1] : &lcu->top_ref.v[1];
  const kvz_pixel *rec_ref = !color ? lcu->rec.y : (color == 1) ? lcu->rec.u : lcu->rec.v;

  // Init top borders pointer to point to the correct place in the correct reference array.
  const kvz_pixel *top_border;
  if (px.y) {
    top_border = &rec_ref[px.x + (px.y - 1) * (LCU_WIDTH >> is_chroma)];
  } else {
    top_border = &top_ref[px.x];
  }

  // Init left borders pointer to point to the correct place in the correct reference array.
  const kvz_pixel *left_border;
  int left_stride; // Distance between reference samples.
  if (px.x) {
    left_border = &rec_ref[px.x - 1 + px.y * (LCU_WIDTH >> is_chroma)];
    left_stride = LCU_WIDTH >> is_chroma;
  } else {
    left_border = &left_ref[px.y];
    left_stride = 1;
  }

  // Generate left reference.
  if (luma_px->x > 0) {
    // Get the number of reference pixels based on the PU coordinate within the LCU.
    int px_available_left = num_ref_pixels_left[lcu_px.y / 4][lcu_px.x / 4] >> is_chroma;

    // Limit the number of available pixels based on block size and dimensions
    // of the picture.
    px_available_left = MIN(px_available_left, width * 2);
    px_available_left = MIN(px_available_left, (pic_px->y - luma_px->y) >> is_chroma);

    // Copy pixels from coded CUs.
    for (int i = 0; i < px_available_left; ++i) {
      out_left_ref[i + 1] = left_border[i * left_stride];
    }
    // Extend the last pixel for the rest of the reference values.
    kvz_pixel nearest_pixel = out_left_ref[px_available_left];
    for (int i = px_available_left; i < width * 2; ++i) {
      out_left_ref[i + 1] = nearest_pixel;
    }
  } else {
    // If we are on the left edge, extend the first pixel of the top row.
    kvz_pixel nearest_pixel = luma_px->y > 0 ? top_border[0] : dc_val;
    for (int i = 0; i < width * 2; i++) {
      out_left_ref[i + 1] = nearest_pixel;
    }
  }

  // Generate top-left reference.
  if (luma_px->x > 0 && luma_px->y > 0) {
    // If the block is at an LCU border, the top-left must be copied from
    // the border that points to the LCUs 1D reference buffer.
    if (px.x == 0) {
      out_left_ref[0] = left_border[-1 * left_stride];
      out_top_ref[0] = left_border[-1 * left_stride];
    } else {
      out_left_ref[0] = top_border[-1];
      out_top_ref[0] = top_border[-1];
    }
  } else {
    // Copy reference clockwise.
    out_left_ref[0] = out_left_ref[1];
    out_top_ref[0] = out_left_ref[1];
  }

  // Generate top reference.
  if (luma_px->y > 0) {
    // Get the number of reference pixels based on the PU coordinate within the LCU.
    int px_available_top = num_ref_pixels_top[lcu_px.y / 4][lcu_px.x / 4] >> is_chroma;

    // Limit the number of available pixels based on block size and dimensions
    // of the picture.
    px_available_top = MIN(px_available_top, width * 2);
    px_available_top = MIN(px_available_top, (pic_px->x - luma_px->x) >> is_chroma);

    // Copy all the pixels we can.
    for (int i = 0; i < px_available_top; ++i) {
      out_top_ref[i + 1] = top_border[i];
    }
    // Extend the last pixel for the rest of the reference values.
    kvz_pixel nearest_pixel = top_border[px_available_top - 1];
    for (int i = px_available_top; i < width * 2; ++i) {
      out_top_ref[i + 1] = nearest_pixel;
    }
  } else {
    // Extend nearest pixel.
    kvz_pixel nearest_pixel = luma_px->x > 0 ? left_border[0] : dc_val;
    for (int i = 0; i < width * 2; i++) {
      out_top_ref[i + 1] = nearest_pixel;
    }
  }
}

void kvz_intra_build_reference_inner(
  const int_fast8_t log2_width,
  const color_t color,
  const vector2d_t *const luma_px,
  const vector2d_t *const pic_px,
  const lcu_t *const lcu,
  kvz_intra_references *const refs,
  bool entropy_sync)
{
  assert(log2_width >= 2 && log2_width <= 5);

  refs->filtered_initialized = false;
  kvz_pixel * __restrict out_left_ref = &refs->ref.left[0];
  kvz_pixel * __restrict out_top_ref = &refs->ref.top[0];

  const int is_chroma = color != COLOR_Y ? 1 : 0;
  const int_fast8_t width = 1 << log2_width;

  // Convert luma coordinates to chroma coordinates for chroma.
  const vector2d_t lcu_px = {
    luma_px->x % LCU_WIDTH,
    luma_px->y % LCU_WIDTH
  };
  const vector2d_t px = {
    lcu_px.x >> is_chroma,
    lcu_px.y >> is_chroma,
  };

  // Init pointers to LCUs reconstruction buffers, such that index 0 refers to block coordinate 0.
  const kvz_pixel * __restrict left_ref = !color ? &lcu->left_ref.y[1] : (color == 1) ? &lcu->left_ref.u[1] : &lcu->left_ref.v[1];
  const kvz_pixel * __restrict top_ref = !color ? &lcu->top_ref.y[1] : (color == 1) ? &lcu->top_ref.u[1] : &lcu->top_ref.v[1];
  const kvz_pixel * __restrict rec_ref = !color ? lcu->rec.y : (color == 1) ? lcu->rec.u : lcu->rec.v;

  // Init top borders pointer to point to the correct place in the correct reference array.
  const kvz_pixel * __restrict top_border;
  if (px.y) {
    top_border = &rec_ref[px.x + (px.y - 1) * (LCU_WIDTH >> is_chroma)];
  } else {
    top_border = &top_ref[px.x];

  }

  // Init left borders pointer to point to the correct place in the correct reference array.
  const kvz_pixel * __restrict left_border;
  int left_stride; // Distance between reference samples.

  // Generate top-left reference.
  // If the block is at an LCU border, the top-left must be copied from
  // the border that points to the LCUs 1D reference buffer.
  if (px.x) {
    left_border = &rec_ref[px.x - 1 + px.y * (LCU_WIDTH >> is_chroma)];
    left_stride = LCU_WIDTH >> is_chroma;
    out_left_ref[0] = top_border[-1];
    out_top_ref[0] = top_border[-1];
  } else {
    left_border = &left_ref[px.y];
    left_stride = 1;
    out_left_ref[0] = left_border[-1 * left_stride];
    out_top_ref[0] = left_border[-1 * left_stride];
  }

  // Generate left reference.

  // Get the number of reference pixels based on the PU coordinate within the LCU.
  int px_available_left = num_ref_pixels_left[lcu_px.y / 4][lcu_px.x / 4] >> is_chroma;

  // Limit the number of available pixels based on block size and dimensions
  // of the picture.
  px_available_left = MIN(px_available_left, width * 2);
  px_available_left = MIN(px_available_left, (pic_px->y - luma_px->y) >> is_chroma);

  // Copy pixels from coded CUs.
  int i = 0;
  do {
    out_left_ref[i + 1] = left_border[(i + 0) * left_stride];
    out_left_ref[i + 2] = left_border[(i + 1) * left_stride];
    out_left_ref[i + 3] = left_border[(i + 2) * left_stride];
    out_left_ref[i + 4] = left_border[(i + 3) * left_stride];
    i += 4;
  } while (i < px_available_left);

  // Extend the last pixel for the rest of the reference values.
  kvz_pixel nearest_pixel = out_left_ref[i];
  for (; i < width * 2; i += 4) {
    out_left_ref[i + 1] = nearest_pixel;
    out_left_ref[i + 2] = nearest_pixel;
    out_left_ref[i + 3] = nearest_pixel;
    out_left_ref[i + 4] = nearest_pixel;
  }

  // Generate top reference.

  // Get the number of reference pixels based on the PU coordinate within the LCU.
  int px_available_top = num_ref_pixels_top[lcu_px.y / 4][lcu_px.x / 4] >> is_chroma;

  // Limit the number of available pixels based on block size and dimensions
  // of the picture.
  px_available_top = MIN(px_available_top, width * 2);
  px_available_top = MIN(px_available_top, (pic_px->x - luma_px->x) >> is_chroma);

  if (entropy_sync && px.y == 0) px_available_top = MIN(px_available_top, ((LCU_WIDTH >> is_chroma) - px.x) -1);

  // Copy all the pixels we can.
  i = 0;
  do {
    memcpy(out_top_ref + i + 1, top_border + i, 4 * sizeof(kvz_pixel));
    i += 4;
  } while (i < px_available_top);

  // Extend the last pixel for the rest of the reference values.
  nearest_pixel = out_top_ref[i];
  for (; i < width * 2; i += 4) {
    out_top_ref[i + 1] = nearest_pixel;
    out_top_ref[i + 2] = nearest_pixel;
    out_top_ref[i + 3] = nearest_pixel;
    out_top_ref[i + 4] = nearest_pixel;
  }
}

void kvz_intra_build_reference(
  const int_fast8_t log2_width,
  const color_t color,
  const vector2d_t *const luma_px,
  const vector2d_t *const pic_px,
  const lcu_t *const lcu,
  kvz_intra_references *const refs,
  bool entropy_sync)
{
  // Much logic can be discarded if not on the edge
  if (luma_px->x > 0 && luma_px->y > 0) {
    kvz_intra_build_reference_inner(log2_width, color, luma_px, pic_px, lcu, refs, entropy_sync);
  } else {
    kvz_intra_build_reference_any(log2_width, color, luma_px, pic_px, lcu, refs);
  }
}

static void intra_recon_tb_leaf(
  encoder_state_t *const state,
  int x,
  int y,
  int depth,
  int8_t intra_mode,
  cclm_parameters_t *cclm_params,
  lcu_t *lcu,
  color_t color)
{
  const kvz_config *cfg = &state->encoder_control->cfg;
  const int shift = color == COLOR_Y ? 0 : 1;

  int log2width = LOG2_LCU_WIDTH - depth;
  if (color != COLOR_Y && depth < MAX_PU_DEPTH) {
    // Chroma width is half of luma width, when not at maximum depth.
    log2width -= 1;
  }
  const int width = 1 << log2width;
  const int lcu_width = LCU_WIDTH >> shift;

  const vector2d_t luma_px = { x, y };
  const vector2d_t pic_px = {
    state->tile->frame->width,
    state->tile->frame->height,
  };
  int x_scu = SUB_SCU(x);
  int y_scu = SUB_SCU(y);
  const vector2d_t lcu_px = {x_scu >> shift, y_scu >> shift };

  kvz_intra_references refs;
  kvz_intra_build_reference(log2width, color, &luma_px, &pic_px, lcu, &refs, cfg->wpp);

  kvz_pixel pred[32 * 32];
  int stride = state->tile->frame->source->stride;
  const bool filter_boundary = color == COLOR_Y && !(cfg->lossless && cfg->implicit_rdpcm);
  if(intra_mode < 68) {
    kvz_intra_predict(state, &refs, log2width, intra_mode, color, pred, filter_boundary);
  } else {
    kvz_pixels_blit(&state->tile->frame->cclm_luma_rec[x / 2 + (y * stride) / 4], pred, width, width, stride / 2, width);
    if(cclm_params == NULL) {
      cclm_parameters_t temp_params;
      kvz_predict_cclm(
        state, color, width, width, x, y, stride, intra_mode, lcu, &refs, pred, &temp_params);
    }
    else {
      linear_transform_cclm(&cclm_params[color == COLOR_U ? 0 : 1], pred, pred, width, width);
    }
  }

  const int index = lcu_px.x + lcu_px.y * lcu_width;
  kvz_pixel *block = NULL;
  kvz_pixel *block2 = NULL;
  switch (color) {
    case COLOR_Y:
      block = &lcu->rec.y[index];
      break;
    case COLOR_U:
      block = &lcu->rec.u[index];
      block2 = &lcu->rec.joint_u[index];
      break;
    case COLOR_V:
      block = &lcu->rec.v[index];
      block2 = &lcu->rec.joint_v[index];
      break;
    default: break;
  }

  kvz_pixels_blit(pred, block , width, width, width, lcu_width);
  if(color != COLOR_Y && cfg->jccr) {
    kvz_pixels_blit(pred, block2, width, width, width, lcu_width);
  }
}

/**
 * \brief Reconstruct an intra CU
 *
 * \param state         encoder state
 * \param x             x-coordinate of the CU in luma pixels
 * \param y             y-coordinate of the CU in luma pixels
 * \param depth         depth in the CU tree
 * \param mode_luma     intra mode for luma, or -1 to skip luma recon
 * \param mode_chroma   intra mode for chroma, or -1 to skip chroma recon
 * \param cur_cu        pointer to the CU, or NULL to fetch CU from LCU
 * \param cclm_params   pointer for the cclm_parameters, can be NULL if the mode is not cclm mode
 * \param lcu           containing LCU
 */
void kvz_intra_recon_cu(
  encoder_state_t *const state,
  int x,
  int y,
  int depth,
  int8_t mode_luma,
  int8_t mode_chroma,
  cu_info_t *cur_cu,
  cclm_parameters_t *cclm_params,
  lcu_t *lcu)
{
  const vector2d_t lcu_px = { SUB_SCU(x), SUB_SCU(y) };
  const int8_t width = LCU_WIDTH >> depth;
  if (cur_cu == NULL) {
    cur_cu = LCU_GET_CU_AT_PX(lcu, lcu_px.x, lcu_px.y);
  }

  // Reset CBFs because CBFs might have been set
  // for depth earlier
  if (mode_luma >= 0) {
    cbf_clear(&cur_cu->cbf, depth, COLOR_Y);
  }
  if (mode_chroma >= 0) {
    cbf_clear(&cur_cu->cbf, depth, COLOR_U);
    cbf_clear(&cur_cu->cbf, depth, COLOR_V);
  }

  if (depth == 0 || cur_cu->tr_depth > depth) {

    const int offset = width / 2;
    const int32_t x2 = x + offset;
    const int32_t y2 = y + offset;

    kvz_intra_recon_cu(state, x,  y,  depth + 1, mode_luma, mode_chroma, NULL, NULL, lcu);
    kvz_intra_recon_cu(state, x2, y,  depth + 1, mode_luma, mode_chroma, NULL, NULL, lcu);
    kvz_intra_recon_cu(state, x,  y2, depth + 1, mode_luma, mode_chroma, NULL, NULL, lcu);
    kvz_intra_recon_cu(state, x2, y2, depth + 1, mode_luma, mode_chroma, NULL, NULL, lcu);

    // Propagate coded block flags from child CUs to parent CU.
    uint16_t child_cbfs[3] = {
      LCU_GET_CU_AT_PX(lcu, lcu_px.x + offset, lcu_px.y         )->cbf,
      LCU_GET_CU_AT_PX(lcu, lcu_px.x,          lcu_px.y + offset)->cbf,
      LCU_GET_CU_AT_PX(lcu, lcu_px.x + offset, lcu_px.y + offset)->cbf,
    };

    if (mode_luma != -1 && depth <= MAX_DEPTH) {
      cbf_set_conditionally(&cur_cu->cbf, child_cbfs, depth, COLOR_Y);
    }
    if (mode_chroma != -1 && depth <= MAX_DEPTH) {
      cbf_set_conditionally(&cur_cu->cbf, child_cbfs, depth, COLOR_U);
      cbf_set_conditionally(&cur_cu->cbf, child_cbfs, depth, COLOR_V);
    }
  } else {
    const bool has_luma = mode_luma != -1;
    const bool has_chroma = mode_chroma != -1 &&  (x % 8 == 0 && y % 8 == 0);
    // Process a leaf TU.
    if (has_luma) {
      intra_recon_tb_leaf(state, x, y, depth, mode_luma, cclm_params, lcu, COLOR_Y);
    }
    if (has_chroma) {
      intra_recon_tb_leaf(state, x, y, depth, mode_chroma, cclm_params, lcu, COLOR_U);
      intra_recon_tb_leaf(state, x, y, depth, mode_chroma, cclm_params, lcu, COLOR_V);
    }

    kvz_quantize_lcu_residual(state, has_luma, has_chroma, x, y, depth, cur_cu, lcu, false);
  }
}
