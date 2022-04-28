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

#include "strategies/strategies-ipol.h"

#include "strategies/avx2/ipol-avx2.h"
#include "strategies/generic/ipol-generic.h"
#include "strategyselector.h"


// Define function pointers.
ipol_blocks_func * uvg_filter_hpel_blocks_hor_ver_luma;
ipol_blocks_func * uvg_filter_hpel_blocks_diag_luma;
ipol_blocks_func * uvg_filter_qpel_blocks_hor_ver_luma;
ipol_blocks_func * uvg_filter_qpel_blocks_diag_luma;
epol_func *uvg_get_extended_block;
uvg_sample_quarterpel_luma_func * uvg_sample_quarterpel_luma;
uvg_sample_octpel_chroma_func * uvg_sample_octpel_chroma;
uvg_sample_quarterpel_luma_hi_func * uvg_sample_quarterpel_luma_hi;
uvg_sample_octpel_chroma_hi_func * uvg_sample_octpel_chroma_hi;


int uvg_strategy_register_ipol(void* opaque, uint8_t bitdepth) {
  bool success = true;

  success &= uvg_strategy_register_ipol_generic(opaque, bitdepth);

  if (uvg_g_hardware_flags.intel_flags.avx2) {
    success &= uvg_strategy_register_ipol_avx2(opaque, bitdepth);
  }
  return success;
}
