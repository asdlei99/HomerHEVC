/*****************************************************************************
 * hmr_arithmetic_encoding.c : homerHEVC encoding library
/*****************************************************************************
 * Copyright (C) 2014 homerHEVC project
 *
 * Juan Casal <jcasal.homer@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *****************************************************************************/
/*
 * some of the work below is derived from HM HEVC reference code where 
 * the following license applies
/****************************************************************************
/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.  
 *
 * Copyright (c) 2010-2014, ITU/ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ITU/ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *****************************************************************************/

#include <math.h>
#include <memory.h>
#include "hmr_common.h"
#include "hmr_private.h"
#include "hmr_ctx_tables.h"
#include "hmr_cabac_tables.h"


const uint g_uiMinInGroup[ 10 ] = {0,1,2,3,4,6,8,12,16,24};
const uint g_uiGroupIdx[ 32 ]   = {0,1,2,3,4,4,5,5,6,6,6,6,7,7,7,7,8,8,8,8,8,8,8,8,9,9,9,9,9,9,9,9};

const uint g_sigLastScan8x8[ 4 ][ 4 ] =
{
  {0, 1, 2, 3},
  {0, 1, 2, 3},
  {0, 2, 1, 3},
  {0, 2, 1, 3}
};
uint g_sigLastScanCG32x32[ 64 ];

// Rice parameters for absolute transform levels
const uint g_auiGoRiceRange[5] =
{
  7, 14, 26, 46, 78
};

const uint g_auiGoRicePrefixLen[5] =
{
  8, 7, 6, 5, 4
};



#define CBF(ctu, abs_index, comp, tr_depth) ((ctu->cbf[comp][abs_index]>>(tr_depth))&1)

int init_context(context_model_buff_t *cm, context_model_t *ctx, int size_y, int size_x, const byte *ref_ctx_model)
{
	cm->ctx = ctx;
	cm->size_x = size_x;
	cm->size_y = size_y;
	cm->size_xy = size_x*size_y;
	cm->ref_ctx_model = ref_ctx_model;
	return cm->size_xy;
}

//void ee_init_contexts(context_model_t *curr_ctx, entropy_model *entropy_models)
void ee_init_contexts(enc_env_t *ee)
{
	context_model_t		*curr_ctx		= ee->contexts;
	entropy_model_t		*entropy_models = ee->e_ctx;
//	int total_ctx = NUM_CTXs;
	curr_ctx+=init_context(&entropy_models->cu_split_flag_model, curr_ctx, 1, NUM_SPLIT_FLAG_CTX, &INIT_SPLIT_FLAG[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_skip_flag_model, curr_ctx, 1, NUM_SKIP_FLAG_CTX, &INIT_SKIP_FLAG[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_merge_flag_model, curr_ctx, 1, NUM_MERGE_FLAG_EXT_CTX, &INIT_MERGE_FLAG_EXT[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_merge_idx_model, curr_ctx, 1, NUM_MERGE_IDX_EXT_CTX, &INIT_MERGE_IDX_EXT[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_part_size_model, curr_ctx, 1, NUM_PART_SIZE_CTX, &INIT_PART_SIZE[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_amp_model, curr_ctx, 1, NUM_CU_AMP_CTX, &INIT_CU_AMP_POS[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_pred_mode_flag_model, curr_ctx, 1, NUM_PRED_MODE_CTX, &INIT_PRED_MODE[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_intra_pred_model, curr_ctx, 1, NUM_ADI_CTX, &INIT_INTRA_PRED_MODE[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_chroma_pred_model, curr_ctx, 1, NUM_CHROMA_PRED_CTX, &INIT_CHROMA_PRED_MODE[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_inter_dir_model, curr_ctx, 1, NUM_INTER_DIR_CTX, &INIT_INTER_DIR[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_mvd_model, curr_ctx, 1, NUM_MV_RES_CTX, &INIT_MVD[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_ref_pic_model, curr_ctx, 1, NUM_REF_NO_CTX, &INIT_REF_PIC[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_delta_qp_model, curr_ctx, 1, NUM_DELTA_QP_CTX, &INIT_DQP[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_qt_cbf_model, curr_ctx, 2, NUM_QT_CBF_CTX, &INIT_QT_CBF[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_qt_root_cbf_model, curr_ctx, 1, NUM_QT_ROOT_CBF_CTX, &INIT_QT_ROOT_CBF[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_sig_coeff_group_model, curr_ctx, 2, NUM_SIG_CG_FLAG_CTX, &INIT_SIG_CG_FLAG[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_sig_model, curr_ctx, 1, NUM_SIG_FLAG_CTX, &INIT_SIG_FLAG[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_ctx_last_x_model, curr_ctx, 2, NUM_CTX_LAST_FLAG_XY, &INIT_LAST[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_ctx_last_y_model, curr_ctx, 2, NUM_CTX_LAST_FLAG_XY, &INIT_LAST[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_one_model, curr_ctx, 1, NUM_ONE_FLAG_CTX, &INIT_ONE_FLAG[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_abs_model, curr_ctx, 1, NUM_ABS_FLAG_CTX, &INIT_ABS_FLAG[0][0]);
	curr_ctx+=init_context(&entropy_models->mvp_idx_model, curr_ctx, 1, NUM_MVP_IDX_CTX, &INIT_MVP_IDX[0][0]);
	curr_ctx+=init_context(&entropy_models->cu_trans_subdiv_flag_model, curr_ctx, 1, NUM_TRANS_SUBDIV_FLAG_CTX, &INIT_TRANS_SUBDIV_FLAG[0][0]);
	curr_ctx+=init_context(&entropy_models->sao_merge_model, curr_ctx, 1, NUM_SAO_MERGE_FLAG_CTX, &INIT_SAO_MERGE_FLAG[0][0]);
	curr_ctx+=init_context(&entropy_models->sao_type_model, curr_ctx, 1, NUM_SAO_TYPE_IDX_CTX, &INIT_SAO_TYPE_IDX[0][0]);
	curr_ctx+=init_context(&entropy_models->transform_skip_model, curr_ctx, 2, NUM_TRANSFORMSKIP_FLAG_CTX, &INIT_TRANSFORMSKIP_FLAG[0][0]);
	curr_ctx+=init_context(&entropy_models->transquant_bypass_flag_model, curr_ctx, 1, NUM_CU_TRANSQUANT_BYPASS_FLAG_CTX, &INIT_CU_TRANSQUANT_BYPASS_FLAG[0][0]);	
}



int calc_ctx_state(int qp, int init_value)
{
	int  slope      = (init_value>>4)*5 - 45;
	int  offset     = ((init_value&15)<<3)-16;
	int  initState  =  min( max( 1, ( ( ( slope * qp ) >> 4 ) + offset ) ), 126 );
	uint mpState    = (initState >= 64 );
	return ((mpState? (initState - 64):(63 - initState)) <<1) + mpState;	
}


void start_context(context_model_buff_t *cm, int init_type, int qp)
{
	int i;
	const byte *ref_ctx_model = cm->ref_ctx_model + init_type*cm->size_xy;
	context_model_t *ctx = cm->ctx;
		
	for(i=0;i<cm->size_xy;i++)
	{
		ctx[i].state = calc_ctx_state(qp, ref_ctx_model[i]);
		ctx[i].num_bins_coded = 0;
	}
}



void ee_start_entropy_model(enc_env_t *ee, int slice_type, int qp)
{
	entropy_model_t		*entropy_models = ee->e_ctx;
	int init_type;

	if(slice_type == I_SLICE)//this is done as in HM where data is reorganized to match slice type definitions, not as in draft 
		init_type = I_SLICE;//init_type = 0			

	start_context(&entropy_models->cu_split_flag_model, init_type, qp);
	start_context(&entropy_models->cu_skip_flag_model, init_type, qp);
	start_context(&entropy_models->cu_merge_flag_model, init_type, qp);
	start_context(&entropy_models->cu_merge_idx_model, init_type, qp);
	start_context(&entropy_models->cu_part_size_model, init_type, qp);
	start_context(&entropy_models->cu_amp_model, init_type, qp);
	start_context(&entropy_models->cu_pred_mode_flag_model, init_type, qp);
	start_context(&entropy_models->cu_intra_pred_model, init_type, qp);
	start_context(&entropy_models->cu_chroma_pred_model, init_type, qp);
	start_context(&entropy_models->cu_inter_dir_model, init_type, qp);
	start_context(&entropy_models->cu_mvd_model, init_type, qp);
	start_context(&entropy_models->cu_ref_pic_model, init_type, qp);
	start_context(&entropy_models->cu_delta_qp_model, init_type, qp);
	start_context(&entropy_models->cu_qt_cbf_model, init_type, qp);
	start_context(&entropy_models->cu_qt_root_cbf_model, init_type, qp);
	start_context(&entropy_models->cu_sig_coeff_group_model, init_type, qp);
	start_context(&entropy_models->cu_sig_model, init_type, qp);
	start_context(&entropy_models->cu_ctx_last_x_model, init_type, qp);
	start_context(&entropy_models->cu_ctx_last_y_model, init_type, qp);
	start_context(&entropy_models->cu_one_model, init_type, qp);
	start_context(&entropy_models->cu_abs_model, init_type, qp);
	start_context(&entropy_models->mvp_idx_model, init_type, qp);
	start_context(&entropy_models->cu_trans_subdiv_flag_model, init_type, qp);
	start_context(&entropy_models->sao_merge_model, init_type, qp);
	start_context(&entropy_models->sao_type_model, init_type, qp);
	start_context(&entropy_models->transform_skip_model, init_type, qp);
	start_context(&entropy_models->transquant_bypass_flag_model, init_type, qp);
}

void ee_copy_context(context_model_buff_t *cm_src, context_model_buff_t *cm_dst)
{
	int i;
	context_model_t *ctx_src = cm_src->ctx, *ctx_dst = cm_dst->ctx;
		
	for(i=0;i<cm_src->size_xy;i++)
	{
		ctx_dst[i].state = ctx_src[i].state;
		ctx_dst[i].num_bins_coded = ctx_src[i].num_bins_coded;
	}
}

void ee_copy_entropy_model(enc_env_t *ee_src, enc_env_t *ee_dst)
{
	memcpy(ee_dst->contexts, ee_src->contexts, NUM_CTXs*sizeof(context_model_t));
}

#define GET_CONTEXT_XYZ(cm, z, y, x) (&(cm.ctx[(z)*(cm.size_xy)+(y)*(cm.size_x)+(x)]))
#define GET_CONTEXT_YZ(cm, z, y) (&cm.ctx[(z)*cm.size_xy+(y)*cm.size_x])
#define GET_CONTEXT_Z(cm, z) (&(cm)->ctx[(z)*(cm)->size_xy])


ctu_info_t *get_pu_left(ctu_info_t* ctu, cu_partition_info_t* curr_partition_info, uint *aux_part_idx)
{
	int	ctu_line_size_in_partitions_mask = (ctu->size>>2)-1;

	if((curr_partition_info->raster_index & ctu_line_size_in_partitions_mask) == 0)//columna izq del ctu
	{		
		*aux_part_idx = curr_partition_info->abs_index_left_partition;
		return ctu->ctu_left;
	}
	else
	{
		*aux_part_idx = curr_partition_info->abs_index_left_partition;
		return ctu;
	}
}

ctu_info_t *get_pu_top(ctu_info_t* ctu, cu_partition_info_t* curr_partition_info, uint *aux_part_idx, int planarAtLCUBoundary)
{
	int	ctu_line_size_in_partitions = (ctu->size>>2);

	if(curr_partition_info->raster_index < ctu_line_size_in_partitions)
	{
		if(planarAtLCUBoundary)
			return NULL;
		*aux_part_idx = curr_partition_info->abs_index_top_partition;

		return ctu->ctu_top;
	}
	else
	{
		*aux_part_idx = curr_partition_info->abs_index_top_partition;
		return ctu;
	}

	return NULL;
}



void encode_split_flag(enc_env_t* ee, ctu_info_t* ctu, cu_partition_info_t* curr_partition_info)
{
	ctu_info_t	*ctu_left, *ctu_top;
	uint		aux_part_idx = 0;

	int ctx = 0;
	int split_flag = ctu->pred_depth[curr_partition_info->abs_index]>curr_partition_info->depth;
	context_model_t *cm;

	ctu_left = get_pu_left(ctu, curr_partition_info, &aux_part_idx);//ctu->ctu_left;
	ctx = ctu_left ? ((ctu_left->pred_depth[aux_part_idx ] > curr_partition_info->depth) ? 1 : 0 ) : 0;

	ctu_top = get_pu_top(ctu, curr_partition_info, &aux_part_idx, FALSE);//ctu->ctu_top;//getPUAbove( aux_part_idx, m_uiAbsIdxInLCU + abs_index, TRUE, TRUE );
	ctx += ctu_top ? ((ctu_top->pred_depth[aux_part_idx ] > curr_partition_info->depth) ? 1 : 0 ) : 0;

	cm = GET_CONTEXT_XYZ(ee->e_ctx->cu_split_flag_model,0, 0, ctx); 
	ee->ee_encode_bin(ee, cm, split_flag);
}

void encode_part_size(henc_thread_t* et, enc_env_t* ee, cu_partition_info_t* curr_partition_info, int part_size_type, int is_intra)
{

	context_model_t *cm = GET_CONTEXT_XYZ(ee->e_ctx->cu_part_size_model,0, 0, 0); 

	if (is_intra)
	{
		if( curr_partition_info->depth == (et->max_cu_depth - et->mincu_mintr_shift_diff))
		{
			ee->ee_encode_bin( ee, cm, (part_size_type==SIZE_2Nx2N)? 1 : 0);
		}
		return;
	}
}

int get_intra_dir_luma_predictor(ctu_info_t* ctu, cu_partition_info_t* curr_partition_info, int* arr_intra_dir, int* piMode  )
{
	ctu_info_t	*ctu_left, *ctu_top;
	uint		aux_part_idx = 0;
	int         left_intra_dir, top_intra_dir;
	int         pred_num = 0;

	// Get intra direction of left PU
	ctu_left = get_pu_left(ctu, curr_partition_info, &aux_part_idx);//ctu->ctu_left;

	left_intra_dir  = ctu_left ? ((ctu_left->pred_mode == INTRA_MODE) ? ctu_left->intra_mode[Y_COMP][aux_part_idx] : DC_IDX ) : DC_IDX;

	ctu_top = get_pu_top(ctu, curr_partition_info, &aux_part_idx, TRUE);//ctu->ctu_top;//getPUAbove( aux_part_idx, m_uiAbsIdxInLCU + abs_index, TRUE, TRUE );

	top_intra_dir = ctu_top ? ((ctu_top->pred_mode == INTRA_MODE) ? ctu_top->intra_mode[Y_COMP][aux_part_idx] : DC_IDX ) : DC_IDX;

	pred_num = 3;
	if(left_intra_dir == top_intra_dir)
	{
		if( piMode )
		{
			*piMode = 1;
		}

		if (left_intra_dir > 1) // angular modes
		{
			arr_intra_dir[0] = left_intra_dir;
			arr_intra_dir[1] = ((left_intra_dir + 29) % 32) + 2;
			arr_intra_dir[2] = ((left_intra_dir - 1 ) % 32) + 2;
		}
		else //non-angular
		{
			arr_intra_dir[0] = PLANAR_IDX;
			arr_intra_dir[1] = DC_IDX;
			arr_intra_dir[2] = VER_IDX; 
		}
	}
	else
	{
		if( piMode )
		{
			*piMode = 2;
		}
		arr_intra_dir[0] = left_intra_dir;
		arr_intra_dir[1] = top_intra_dir;

		if (left_intra_dir && top_intra_dir ) //both modes are non-planar
		{
			arr_intra_dir[2] = PLANAR_IDX;
		}
		else
		{
			arr_intra_dir[2] =  (left_intra_dir+top_intra_dir)<2? VER_IDX : DC_IDX;
		}
	}

	return pred_num;
}

void encode_intra_dir_luma_ang(enc_env_t* ee, ctu_info_t* ctu, cu_partition_info_t* curr_partition_info, int is_multiple)
{
	int dir[4],j;
	int preds[4][3] = {{-1, -1, -1},{-1, -1, -1},{-1, -1, -1},{-1, -1, -1}};
	int predNum[4], predIdx[4] ={ -1,-1,-1,-1};
	context_model_t *cm = GET_CONTEXT_XYZ(ee->e_ctx->cu_intra_pred_model,0, 0, 0); 
	int abs_index =  curr_partition_info->abs_index;
	PartSize part_size_type = (PartSize)ctu->part_size_type[abs_index];
	int partNum = is_multiple?(part_size_type==SIZE_NxN?4:1):1;

	for (j=0;j<partNum;j++)
	{
		uint i;
		if(is_multiple)
		{
			dir[j] = ctu->intra_mode[Y_COMP][curr_partition_info->children[j]->abs_index];
			predNum[j] = get_intra_dir_luma_predictor(ctu, curr_partition_info->children[j], preds[j], NULL);  
		}
		else
		{
			dir[j] = ctu->intra_mode[Y_COMP][curr_partition_info->abs_index];
			predNum[j] = get_intra_dir_luma_predictor(ctu, curr_partition_info, preds[j], NULL);  
		}
		
		for(i = 0; i < predNum[j]; i++)
		{
			if(dir[j] == preds[j][i])
			{
				predIdx[j] = i;
			}
		}
		ee->ee_encode_bin( ee, cm, ((predIdx[j] != -1)? 1 : 0));
	}  
	for (j=0;j<partNum;j++)
	{
		if(predIdx[j] != -1)
		{
			ee->ee_encode_bin_EP(ee,  predIdx[j] ? 1 : 0 );
			if (predIdx[j])
			{
				ee->ee_encode_bin_EP(ee,  predIdx[j]-1 );
			}
		}
		else
		{//ordenamos de menor a mayor
			int i;
			if (preds[j][0] > preds[j][1])
			{ 
				iswap(preds[j][0], preds[j][1]); 
			}
			if (preds[j][0] > preds[j][2])
			{
				iswap(preds[j][0], preds[j][2]);
			}
			if (preds[j][1] > preds[j][2])
			{
				iswap(preds[j][1], preds[j][2]);
			}
			for(i = (predNum[j] - 1); i >= 0; i--)
			{
				dir[j] = dir[j] > preds[j][i] ? dir[j] - 1 : dir[j];
			}
			ee->ee_encode_bins_EP(ee,dir[j], 5 );
		}
	}
	return;

}

void encode_intra_dir_chroma(enc_env_t* ee, ctu_info_t* ctu, cu_partition_info_t* curr_partition_info)
{
	int i;
	int abs_index =  curr_partition_info->abs_index;
	uint intra_dir_chroma = ctu->intra_mode[CHR_COMP][abs_index];
	context_model_t *cm = GET_CONTEXT_XYZ(ee->e_ctx->cu_chroma_pred_model,0, 0, 0); 

	if( intra_dir_chroma == DM_CHROMA_IDX ) 
	{
		ee->ee_encode_bin( ee, cm, 0);
	}
	else
	{ 
		int  mode_list[ NUM_CHROMA_MODE ];
		int luma_mode = ctu->intra_mode[Y_COMP][abs_index];
		create_chroma_dir_list(mode_list, luma_mode);

		for(i=0; i<NUM_CHROMA_MODE-1; i++)
		{
			if( intra_dir_chroma == mode_list[i] )
			{
				intra_dir_chroma = i;
				break;
			}
		}
		ee->ee_encode_bin( ee, cm, 1);
		ee->ee_encode_bins_EP(ee, intra_dir_chroma, 2 );
	}
}

void encode_qt_cbf(enc_env_t* ee, cu_partition_info_t *curr_partition_info, int component, int tr_depth, int cbf)
{
	uint ctx;
	context_model_t *cm;

	if(component)
	{
		ctx = tr_depth;
	}
	else
	{
		ctx = ( tr_depth == 0 ? 1 : 0 );
	}
	cm = GET_CONTEXT_XYZ(ee->e_ctx->cu_qt_cbf_model, 0, component!=Y_COMP, ctx); 
	ee->ee_encode_bin(ee, cm, cbf);
}

void encode_last_significant_XY(enc_env_t* ee, int x, int y, int width, int height, int width_shift, int height_shift, int component, int curr_scan_mode)
{
	int i;
	context_model_t *cm_x = GET_CONTEXT_YZ(ee->e_ctx->cu_ctx_last_x_model, 0, component!=Y_COMP);
	context_model_t *cm_y = GET_CONTEXT_YZ(ee->e_ctx->cu_ctx_last_y_model, 0, component!=Y_COMP);
	int group_index_x, group_index_y;
	int blk_size_offset_x, blk_size_offset_y, shift_x, shift_y;
	int ctx_last;

	// swap
	if(curr_scan_mode == VER_SCAN)
	{
		iswap( x, y);
	}

	group_index_x = g_uiGroupIdx[x];
	group_index_y = g_uiGroupIdx[y];

	blk_size_offset_x = component ? 0: ((width_shift-2) *3 + (((width_shift-2) +1)>>2));
	blk_size_offset_y = component ? 0: ((height_shift-2)*3 + (((height_shift-2)+1)>>2));
	shift_x= component ? (width_shift-2) :(((width_shift-2)+3)>>2);
	shift_y= component ? (height_shift-2) :(((height_shift-2)+3)>>2);

	// last_sig_coeff_x_prefix
	for( ctx_last = 0; ctx_last < group_index_x; ctx_last++ )
	{
		ee->ee_encode_bin(ee, (cm_x+blk_size_offset_x+(ctx_last>>shift_x)), 1);
	}
	if( group_index_x < g_uiGroupIdx[width-1])
	{
		ee->ee_encode_bin(ee, (cm_x+blk_size_offset_x+(ctx_last>>shift_x)), 0);
	}

	// last_sig_coeff_y_prefix
	for( ctx_last = 0; ctx_last < group_index_y; ctx_last++ )
	{
		ee->ee_encode_bin(ee, (cm_y+blk_size_offset_y+(ctx_last>>shift_y)), 1);
	}
	if( group_index_y < g_uiGroupIdx[height-1])
	{
		ee->ee_encode_bin(ee, (cm_y+blk_size_offset_y+(ctx_last>>shift_y)), 0);
	}
	
	//last_sig_coeff_x_suffix
	if (group_index_x > 3)
	{      
		uint count = (group_index_x - 2) >> 1;
		x       = x - g_uiMinInGroup[group_index_x];
		for (i = count - 1 ; i >= 0; i--)
		{
			ee->ee_encode_bin_EP( ee, ( x >> i ) & 1 );
		}
	}
	if (group_index_y > 3)
	{      
		uint count = (group_index_y - 2) >> 1;
		x       = x - g_uiMinInGroup[group_index_y];
		for (i = count - 1 ; i >= 0; i--)
		{
			ee->ee_encode_bin_EP( ee, ( y >> i ) & 1 );
		}
	}
}


int get_sig_ctx_inc(int pattern_sig_ctx, int scan_mode, int pos_x, int pos_y, int part_size_shift, int component)
{
	int offset;
	int posXinSubset, posYinSubset;
	int cnt = 0;
  const int ctxIndMap[16] =
  {
    0, 1, 4, 5,
    2, 3, 4, 5,
    6, 6, 8, 8,
    7, 7, 8, 8
  };

  if( pos_x + pos_y == 0 )
  {
    return 0;
  }

  if ( part_size_shift == 2 )
  {
    return ctxIndMap[ 4 * pos_y + pos_x ];
  }

  offset = part_size_shift == 3 ? (scan_mode==DIAG_SCAN ? 9 : 15) : (component == Y_COMP ? 21 : 12);

  posXinSubset = pos_x-((pos_x>>2)<<2);
  posYinSubset = pos_y-((pos_y>>2)<<2);
  cnt = 0;
  if(pattern_sig_ctx==0)
  {
    cnt = posXinSubset+posYinSubset<=2 ? (posXinSubset+posYinSubset==0 ? 2 : 1) : 0;
  }
  else if(pattern_sig_ctx==1)
  {
    cnt = posYinSubset<=1 ? (posYinSubset==0 ? 2 : 1) : 0;
  }
  else if(pattern_sig_ctx==2)
  {
    cnt = posXinSubset<=1 ? (posXinSubset==0 ? 2 : 1) : 0;
  }
  else
  {
    cnt = 2;
  }

  return (( component == Y_COMP && ((pos_x>>2) + (pos_y>>2)) > 0 ) ? 3 : 0) + offset + cnt;
}


#define LOG2_SCAN_SET_SIZE					4
#define SCAN_SET_SIZE						16

#define SBH_THRESHOLD						4  

#define C1FLAG_NUMBER						8 //maximum number of largerThan1 flag coded in one chunk :  16 in HM5
#define C2FLAG_NUMBER						1 //maximum number of largerThan2 flag coded in one chunk:  16 in HM5 

#define COEF_REMAIN_BIN_REDUCTION			3 ///indicates the level at which the VLC 

//residual_coding		//codeCoeffNxN
void encode_residual(henc_thread_t* et, enc_env_t *ee, ctu_info_t *ctu, cu_partition_info_t *partition_info, int component, int gcnt)
{
	int i, last_x, last_y;
	int is_luma = (component==Y_COMP);
	int abs_index = partition_info->abs_index;
	cu_partition_info_t* curr_partition_info = is_luma?partition_info:(partition_info->size_chroma>2)?partition_info:partition_info->parent;//4 2x2 partitions are coded as one 4x4 partition
	int curr_part_size = is_luma?curr_partition_info->size:curr_partition_info->size_chroma;
	int width = curr_part_size;
	int height = curr_part_size;
	int blk_width = width >> 2, blk_height = height >> 2;
	int num_nonzero_coeffs = 0;
	int curr_part_size_shift = is_luma?et->max_cu_size_shift-curr_partition_info->depth:et->max_cu_size_shift_chroma-curr_partition_info->depth;
	short*__restrict coeff_buff = WND_POSITION_1D(short  *__restrict, *ctu->coeff_wnd, component, gcnt, et->ctu_width, (curr_partition_info->abs_index<<(et->num_partitions_in_cu_shift-(is_luma?0:2))));
	int num_part_in_pred_cu = ctu->num_part_in_ctu>>(ctu->pred_depth[abs_index]*2);
	int scan_mode = find_scan_mode(ctu->pred_mode==INTRA_MODE, is_luma, curr_part_size, ctu->intra_mode[(component==Y_COMP)?Y_COMP:CHR_COMP][curr_partition_info->abs_index], ctu->intra_mode[Y_COMP][(abs_index/num_part_in_pred_cu)*num_part_in_pred_cu]);
	uint *scan = et->ed->scan_pyramid[scan_mode][curr_part_size_shift-1], *scan_cg;
	byte *coeff_flag_buff = (byte *)et->cabac_aux_buff;
	int scan_position, scan_position_last, last_scan_set, raster_pos_last = 0;
	int coeff;
	int blk_index;
	uint c1 = 1;
	uint go_rice_param;
	int  scan_position_sig;
	int sub_pos;
	short *abs_coeff = et->aux_buff;
	uint coeff_signs = 0;
	int last_nz_pos_in_cg, first_nz_pos_in_cg;
	context_model_t *base_coeff_group_ctx = GET_CONTEXT_YZ(ee->e_ctx->cu_sig_coeff_group_model, 0, !is_luma);
	context_model_t *base_ctx = GET_CONTEXT_YZ(ee->e_ctx->cu_sig_model, 0, 0) + ((is_luma)?0:NUM_SIG_FLAG_CTX_LUMA);
	int valid;
	int subset;

	if( curr_part_size > (1<<et->max_tu_size_shift) )
	{
		height = width  = (1<<et->max_tu_size_shift);
	}

	scan_cg = et->ed->scan_pyramid[scan_mode][(curr_part_size_shift>3)?curr_part_size_shift-3:0];
    if( curr_part_size_shift == 3 )
    {
      scan_cg = (uint*)g_sigLastScan8x8[ scan_mode ];
    }
    else if( curr_part_size_shift == 5 )
    {
      scan_cg = g_sigLastScanCG32x32;
    }

	memset(coeff_flag_buff,0,curr_part_size*curr_part_size*sizeof(coeff_flag_buff[0]));
	for ( i = 0; i < curr_part_size*curr_part_size; i++ )
	{
		scan_position = scan[i];
		coeff = coeff_buff[scan_position];
		if(coeff != 0)
		{
			raster_pos_last = i;
			scan_position_last = scan_position;
			num_nonzero_coeffs++;
			last_y = scan_position>>curr_part_size_shift;
			last_x = scan_position-(last_y<<curr_part_size_shift);
			blk_index = (curr_part_size >> 2) * (last_y >> 2) + (last_x >> 2);//2 is the size of a 4x4 block
			coeff_flag_buff[blk_index] = 1;
		}
	}

	if ( num_nonzero_coeffs == 0 )
		return;


	valid = et->pps->sign_data_hiding_flag;

	encode_last_significant_XY(ee, last_x, last_y, curr_part_size, curr_part_size, curr_part_size_shift, curr_part_size_shift, component, scan_mode);

	//encode significance flag
	last_scan_set = raster_pos_last >> LOG2_SCAN_SET_SIZE;
	c1 = 1;
	go_rice_param = 0;
	scan_position_sig = raster_pos_last;
	for(subset = last_scan_set; subset >= 0; subset--)
	{
		int cg_block_pos;
		int cg_pos_y;
		int cg_pos_x;
		int num_non_zero = 0;
		sub_pos = subset << LOG2_SCAN_SET_SIZE;
		go_rice_param    = 0;
		coeff_signs = 0;

		last_nz_pos_in_cg = -1, first_nz_pos_in_cg = SCAN_SET_SIZE;

		if(scan_position_sig == raster_pos_last)
		{
			abs_coeff[ 0 ] = abs( coeff_buff[scan_position_last] );
			coeff_signs    = ( coeff_buff[scan_position_last] < 0 );
			num_non_zero    = 1;
			last_nz_pos_in_cg  = scan_position_sig;
			first_nz_pos_in_cg = scan_position_sig;
			scan_position_sig--;
		}

		//encode significant_coeffgroup_flag
		cg_block_pos = scan_cg[ subset ];
		cg_pos_y   = cg_block_pos / (curr_part_size >> 2);
		cg_pos_x   = cg_block_pos - (cg_pos_y * (curr_part_size >> 2));
		if( subset == last_scan_set || subset == 0)
		{
			coeff_flag_buff[ cg_block_pos ] = 1;
		}
		else
		{
			uint sig_coeff_group   = (coeff_flag_buff[ cg_block_pos ] != 0);
			int ctx_sig;//  
			int i_right = 0, i_lower = 0;

			if( cg_pos_x < blk_width - 1 )
			{
				i_right = (coeff_flag_buff[ cg_pos_y * blk_width + cg_pos_x + 1 ] != 0);
			}
			if (cg_pos_y < blk_height - 1 )
			{
				i_lower = (coeff_flag_buff[ (cg_pos_y  + 1 ) * blk_width + cg_pos_x ] != 0);
			}
			ctx_sig = (i_right||i_lower);
			ee->ee_encode_bin(ee, &base_coeff_group_ctx[ctx_sig], sig_coeff_group);
		}

		// encode significant_coeff_flag
		if( coeff_flag_buff[ cg_block_pos ] )
		{
			int pattern_sig_ctx;
			uint uiBlkPos, pos_y, pos_x, uiSig, uiCtxSig;
			uint sigRight = 0;
			uint sigLower = 0;

			if( width == 4 && height == 4 )
				pattern_sig_ctx = -1;

			if( cg_pos_x < blk_width - 1 )
			{
				sigRight = (coeff_flag_buff[ cg_pos_y * blk_width + cg_pos_x + 1 ] != 0);
			}
			if (cg_pos_y < blk_height - 1 )
			{
				sigLower = (coeff_flag_buff[ (cg_pos_y + 1 ) * blk_width + cg_pos_x] != 0);
			}
			pattern_sig_ctx = sigRight + (sigLower<<1);
			//----------------------------------------------------------------------------------------------------------------------------


			for( ; scan_position_sig >= sub_pos; scan_position_sig-- )
			{
				uiBlkPos  = scan[ scan_position_sig ]; 
				pos_y    = uiBlkPos >> curr_part_size_shift;
				pos_x    = uiBlkPos - (pos_y << curr_part_size_shift);
				uiSig     = (coeff_buff[uiBlkPos] != 0);
				if(scan_position_sig > sub_pos || subset == 0 || num_non_zero)
				{
					uiCtxSig  = get_sig_ctx_inc( pattern_sig_ctx, scan_mode, pos_x, pos_y, curr_part_size_shift, component);
					ee->ee_encode_bin(ee, &base_ctx[uiCtxSig], uiSig);
				}
				if( uiSig )
				{
					abs_coeff[ num_non_zero ] = abs( coeff_buff[ uiBlkPos ] );
					coeff_signs = 2 * coeff_signs + ( coeff_buff[ uiBlkPos ] < 0 );
					num_non_zero++;
					if( last_nz_pos_in_cg == -1 )
					{
						last_nz_pos_in_cg = scan_position_sig;
					}
					first_nz_pos_in_cg = scan_position_sig;
				}
			}
		}
		else
		{
			scan_position_sig = sub_pos - 1;
		}

		if( num_non_zero > 0 )
		{
			int idx;
			int sign_hidden = ( last_nz_pos_in_cg - first_nz_pos_in_cg >= SBH_THRESHOLD );
			uint uiCtxSet = (subset > 0 && is_luma) ? 2 : 0;
			context_model_t *base_ctx_mod;
			int numC1Flag;
			int firstC2FlagIdx;

			if( c1 == 0 )
			{
				uiCtxSet++;
			}
			c1 = 1;
			base_ctx_mod = GET_CONTEXT_YZ(ee->e_ctx->cu_one_model, 0, 0) + 4 * uiCtxSet + ((is_luma)?0:NUM_ONE_FLAG_CTX_LUMA);

			numC1Flag = min(num_non_zero, C1FLAG_NUMBER);
			firstC2FlagIdx = -1;
			for(idx = 0; idx < numC1Flag; idx++)
			{
				uint uiSymbol = abs_coeff[ idx ] > 1;
				ee->ee_encode_bin(ee, &base_ctx_mod[c1], uiSymbol);
				if( uiSymbol )
				{
					c1 = 0;

					if (firstC2FlagIdx == -1)
					{
						firstC2FlagIdx = idx;
					}
				}
				else if( (c1 < 3) && (c1 > 0) )
				{
					c1++;
				}
			}

			if (c1 == 0)
			{
				base_ctx_mod = GET_CONTEXT_YZ(ee->e_ctx->cu_abs_model, 0, 0) + uiCtxSet + ((is_luma)?0:NUM_ABS_FLAG_CTX_LUMA);
				if ( firstC2FlagIdx != -1)
				{

					uint symbol = abs_coeff[ firstC2FlagIdx ] > 2;
					ee->ee_encode_bin(ee, &base_ctx_mod[0], symbol);
				}
			}

			if( valid && sign_hidden )
			{
				ee->ee_encode_bins_EP( ee, (coeff_signs >> 1), num_non_zero-1);
			}
			else
			{
				ee->ee_encode_bins_EP( ee, (coeff_signs), num_non_zero);
			}

			if (c1 == 0 || num_non_zero > C1FLAG_NUMBER)
			{
				int iFirstCoeff2 = 1;    
				int idx;
				for (idx = 0; idx < num_non_zero; idx++ )
				{
					uint baseLevel  = (idx < C1FLAG_NUMBER)? (2 + iFirstCoeff2 ) : 1;

					if( abs_coeff[ idx ] >= baseLevel)
					{
						{
							int codeNumber  = (int)(abs_coeff[ idx ] - baseLevel);
							uint length;
							uint rParam = go_rice_param;
							if (codeNumber < (COEF_REMAIN_BIN_REDUCTION << rParam))
							{
								length = codeNumber>>rParam;
								ee->ee_encode_bins_EP( ee, (1<<(length+1))-2, length+1);
								ee->ee_encode_bins_EP( ee, codeNumber%(1<<rParam), rParam);
							}
							else
							{
								length = rParam;
								codeNumber  = codeNumber - ( COEF_REMAIN_BIN_REDUCTION << rParam);
								while (codeNumber >= (1<<length))
								{
									codeNumber -=  (1<<(length++));    
								}
								ee->ee_encode_bins_EP( ee, (1<<(COEF_REMAIN_BIN_REDUCTION+length+1-rParam))-2,COEF_REMAIN_BIN_REDUCTION+length+1-rParam);
								ee->ee_encode_bins_EP( ee, codeNumber,length);
							}
						}	
						
						if(abs_coeff[idx] > 3*(1<<go_rice_param))
						{
							go_rice_param = min((go_rice_param+ 1), 4);
						}
					}
					if(abs_coeff[ idx ] >= 2)  
					{
						iFirstCoeff2 = 0;
					}
				}        
			}
		}
	}
}

void transform_tree(henc_thread_t* et, enc_env_t* ee, ctu_info_t* ctu, cu_partition_info_t* partition_list, int gcnt)
{
	cu_partition_info_t* parent_part_info = partition_list;
	cu_partition_info_t* curr_partition_info = parent_part_info;
	int depth = curr_partition_info->depth;
	int curr_depth = curr_partition_info->depth;
	int curr_part_size_shift;
	int depth_state[MAX_PARTITION_DEPTH] = {0,0,0,0,0};
	int tr_depth, tr_idx;
	int is_first_part_of_cu;
	int abs_index;
	int log2_tr_size, log2_cu_size;
	int tu_log_min_size = et->min_tu_size_shift;
	int tu_log_max_size = et->max_tu_size_shift;
	int tu_log_min_size_in_cu;
	int split_flag, is_intra, part_size_type, pred_depth;

	while(curr_depth!=depth || depth_state[curr_depth]!=1)//tenemos que iterar un cu de la profundidad inicial
	{
		curr_depth = curr_partition_info->depth;
		abs_index = curr_partition_info->abs_index;
		curr_part_size_shift = et->max_cu_size_shift-curr_depth;
		pred_depth = ctu->pred_depth[abs_index];
		tr_depth = curr_depth-pred_depth;//trafoDepth
		is_first_part_of_cu = (tr_depth==0);
		tr_idx = ctu->tr_idx[abs_index];
		is_intra = ctu->pred_mode == INTRA_MODE;
		part_size_type = ctu->part_size_type[abs_index];
		split_flag = ((tr_idx + pred_depth) > curr_depth );
		log2_tr_size = et->max_cu_size_shift-(curr_depth);
		log2_cu_size = et->max_cu_size_shift-pred_depth;

		if(log2_cu_size < tu_log_min_size+(is_intra?et->max_intra_tr_depth:et->max_inter_tr_depth)-1+(is_intra && part_size_type==SIZE_NxN))//falta el flag de inter
			tu_log_min_size_in_cu = et->min_tu_size_shift;
		else
		{
			tu_log_min_size_in_cu = log2_cu_size - ((is_intra?et->max_intra_tr_depth:et->max_inter_tr_depth)-1+(is_intra && part_size_type==SIZE_NxN));
			if (tu_log_min_size_in_cu > tu_log_max_size)
			  tu_log_min_size_in_cu = tu_log_max_size;
		}

		if(!(is_intra && part_size_type == SIZE_NxN && curr_depth == pred_depth) && 
			!(!is_intra && part_size_type != SIZE_2Nx2N && curr_depth == pred_depth) && 
			!(log2_tr_size>tu_log_max_size) && !(log2_tr_size==tu_log_min_size) && !(log2_tr_size==tu_log_min_size_in_cu))
		{
			ee->ee_encode_bin(ee, GET_CONTEXT_XYZ(ee->e_ctx->cu_trans_subdiv_flag_model ,0, 0, 5 - curr_part_size_shift), split_flag);
		}

		if(is_first_part_of_cu || curr_part_size_shift > 2)
		{
			if(is_first_part_of_cu || CBF(ctu, abs_index, U_COMP, tr_depth-1))
			{
				encode_qt_cbf(ee, curr_partition_info, U_COMP, tr_depth, CBF(ctu, abs_index, U_COMP, tr_depth));
			}

			if(is_first_part_of_cu || CBF(ctu, abs_index, V_COMP, tr_depth-1))
			{
				encode_qt_cbf(ee, curr_partition_info, V_COMP, tr_depth, CBF(ctu, abs_index, V_COMP, tr_depth));
			}
		}

		depth_state[curr_depth]++;
		if(split_flag)//prolog
		{
			parent_part_info = curr_partition_info;
			curr_depth++;
		}
		else 
		{
			if( ctu->pred_mode==INTRA_MODE || tr_depth != 0 || CBF(ctu, abs_index, U_COMP, tr_depth) || 
				CBF(ctu, abs_index, V_COMP, tr_depth) ) 
			{
				encode_qt_cbf(ee, curr_partition_info, Y_COMP, tr_depth, CBF(ctu, abs_index, Y_COMP, tr_depth));
			}

			if(CBF(ctu, abs_index, Y_COMP, tr_depth))
			{
				encode_residual(et, ee, ctu, curr_partition_info, Y_COMP, gcnt);
			}

			if(curr_part_size_shift > 2)
			{
				if(CBF(ctu, abs_index, U_COMP, tr_depth))
				{
					encode_residual(et, ee, ctu, curr_partition_info, U_COMP, gcnt);
				}
				if(CBF(ctu, abs_index, V_COMP, tr_depth))
				{
					encode_residual(et, ee, ctu, curr_partition_info, V_COMP, gcnt);
				}
			}
			else //chroma of 4x4 partitions is coded in 4x4 instead of 2x2
			{
				if(curr_partition_info->list_index == curr_partition_info->parent->children[0]->list_index+3)//last of the four 2x2 partitions
				{
					if(CBF(ctu, abs_index, U_COMP, tr_depth))
					{
						encode_residual(et, ee, ctu, curr_partition_info, U_COMP, gcnt);
					}
					if(CBF(ctu, abs_index, V_COMP, tr_depth))
					{
						encode_residual(et, ee, ctu, curr_partition_info, V_COMP, gcnt);
					}
				}
			}			

			while(depth_state[curr_depth]==4)
			{
				depth_state[curr_depth] = 0;
				parent_part_info = parent_part_info->parent;
				curr_depth--;
			}
			if(curr_depth==0 && depth_state[curr_depth]==1)
				break;
		}
		curr_partition_info = parent_part_info->children[depth_state[curr_depth]];
	}
}

void ee_encode_coding_unit(henc_thread_t* et, enc_env_t* ee, ctu_info_t* ctu, cu_partition_info_t* curr_partition_info, int gcnt)
{
	slice_t *currslice = &et->ed->current_pict.slice;
	int abs_index = curr_partition_info->abs_index;
	int is_intra = ctu->pred_mode==INTRA_MODE;
	int part_size_type =	ctu->part_size_type[abs_index];

	encode_part_size(et, ee, curr_partition_info, part_size_type, is_intra);

	if(is_intra)
	{
		encode_intra_dir_luma_ang(ee, ctu, curr_partition_info, TRUE);
		encode_intra_dir_chroma(ee, ctu, curr_partition_info);
	}

	transform_tree(et, ee, ctu, curr_partition_info, gcnt);
}

void encode_end_of_cu(henc_thread_t* et, enc_env_t* ee, slice_t *currslice, ctu_info_t* ctu, cu_partition_info_t* curr_partition_info)
{
	uint cu_addr = ctu->ctu_number*et->num_partitions_in_cu + curr_partition_info->abs_index;
	uint pos_x, pos_y;
	uint width = et->sps->pic_width_in_luma_samples;
	uint height = et->sps->pic_height_in_luma_samples;
	uint uiGranularityWidth;
	int bTerminateSlice = FALSE;
	int granularityBoundary;
	uint uiRealEndAddress;

	uiRealEndAddress = et->pict_total_cu*et->num_partitions_in_cu;

	bTerminateSlice = FALSE;

	if (cu_addr+curr_partition_info->num_part_in_cu == uiRealEndAddress)
	{
		bTerminateSlice = TRUE;
	}
	uiGranularityWidth = et->max_cu_size;//ctu_width[0];
	pos_x = ctu->x[Y_COMP]+curr_partition_info->x_position;
	pos_y = ctu->y[Y_COMP]+curr_partition_info->y_position;
	granularityBoundary=((pos_x+curr_partition_info->size)%uiGranularityWidth==0 || (pos_x+curr_partition_info->size)==width) && ((pos_y+curr_partition_info->size)%uiGranularityWidth==0 || (pos_y+curr_partition_info->size)==height);

	if(granularityBoundary)
	{		
		if (!bTerminateSlice)
			ee->ee_encode_bin_TRM( ee, bTerminateSlice);
	}
}

void ee_encode_ctu(henc_thread_t* et, enc_env_t* ee, slice_t *currslice, ctu_info_t* ctu, int gcnt)
{
	int curr_depth = 0;
	cu_partition_info_t*	curr_partition_info;
	int depth_state[MAX_PARTITION_DEPTH] = {0,0,0,0,0};
	int intra_pred_depth;
	curr_partition_info = ctu->partition_list;

	//encode parent
	curr_depth = curr_partition_info->depth;
	memset(depth_state, 0, sizeof(depth_state));

	//coding_quadtree
	while(curr_depth!=0|| depth_state[curr_depth]!=1)
	{
		if(curr_partition_info->is_r_inside_frame && curr_partition_info->is_b_inside_frame)
		{
			if(curr_partition_info->depth != (et->max_cu_depth - et->mincu_mintr_shift_diff))
				encode_split_flag(ee, ctu, curr_partition_info);
		}

		intra_pred_depth = ctu->pred_depth[curr_partition_info->abs_index];

		depth_state[curr_depth]++;

		if(curr_depth < intra_pred_depth)//go down one level
		{
			curr_depth++;
			curr_partition_info = curr_partition_info->children[depth_state[curr_depth]];
		}
		else //go up 1 level 
		{	
			if(curr_partition_info->is_r_inside_frame && curr_partition_info->is_b_inside_frame)
			{
				ee_encode_coding_unit(et, ee, ctu, curr_partition_info, gcnt);

				encode_end_of_cu(et, ee, currslice, ctu, curr_partition_info);
			}
			while(depth_state[curr_depth]==4)
			{
				depth_state[curr_depth] = 0;
				curr_depth--;
				curr_partition_info = curr_partition_info->parent;
			}

			if(curr_partition_info->parent != NULL)
				curr_partition_info = curr_partition_info->parent->children[depth_state[curr_depth]];
		}
	}
}


void ee_end_slice(enc_env_t* ee, slice_t *currslice, ctu_info_t* ctu)
{
	ee->ee_encode_bin_TRM( ee, 1);
	ee->ee_finish(ee);
	hmr_bitstream_rbsp_trailing_bits(ee->bs);
}


//--------------------------------- rd estimation ------------------------------------------------

void rd_encode_intra_dir_luma_ang(enc_env_t* ee, cu_partition_info_t* curr_partition_info, int dir, int* preds, int num_preds)
{
	int predIdx = -1;//{ -1,-1,-1,-1};
	context_model_t *cm = GET_CONTEXT_XYZ(ee->e_ctx->cu_intra_pred_model,0, 0, 0); 
	int i;

	for(i = 0; i < num_preds; i++)
	{
		if(dir == preds[i])
		{
			predIdx = i;
			break;
		}
	}
	ee->ee_encode_bin( ee, cm, ((predIdx != -1)? 1 : 0));

	if(predIdx != -1)
	{
		ee->ee_encode_bin_EP(ee,  predIdx ? 1 : 0 );
		if (predIdx)
		{
			ee->ee_encode_bin_EP(ee,  predIdx-1 );
		}
	}
	else
	{//ordenamos de menor a mayor
		int i;
		if (preds[0] > preds[1])
		{ 
			iswap(preds[0], preds[1]); 
		}
		if (preds[0] > preds[2])
		{
			iswap(preds[0], preds[2]);
		}
		if (preds[1] > preds[2])
		{
			iswap(preds[1], preds[2]);
		}
		for(i = (num_preds - 1); i >= 0; i--)
		{
			dir = dir > preds[i] ? dir - 1 : dir;
		}
		ee->ee_encode_bins_EP(ee,dir, 5 );
	}
}

uint fast_rd_estimate_bits_intra_luma_mode( henc_thread_t* et, cu_partition_info_t* partition_info, uint pred_depth, int dir, int *preds, int num_preds)
{
	bm_copy_binary_model(et->ee->b_ctx, et->ec->b_ctx);
	ee_copy_context(&et->ee->e_ctx->cu_intra_pred_model, &et->ec->e_ctx->cu_intra_pred_model);
	et->ec->ee_reset_bits(et->ec->b_ctx);

	rd_encode_intra_dir_luma_ang(et->ec, partition_info, dir, preds, num_preds);

	return et->ec->ee_bitcnt(et->ec->bs, et->ec->b_ctx);
}


uint rd_estimate_bits_intra_mode( henc_thread_t* et, ctu_info_t* ctu, cu_partition_info_t* partition_info, uint pred_depth, int is_luma)
{
	bm_copy_binary_model(et->ee->b_ctx, et->ec->b_ctx);
	ee_copy_context(&et->ee->e_ctx->cu_intra_pred_model, &et->ec->e_ctx->cu_intra_pred_model);

	et->ec->ee_reset_bits(et->ec->b_ctx);
	if(is_luma)
		encode_intra_dir_luma_ang(et->ec, ctu, partition_info, FALSE);
	else
		encode_intra_dir_chroma(et->ec, ctu, partition_info);

	return et->ec->ee_bitcnt(et->ec->bs, et->ec->b_ctx);
}



void rd_est_intra_header( henc_thread_t* et, enc_env_t* ec, ctu_info_t* ctu, cu_partition_info_t* partition_info, uint pred_depth, int luma)
{
	uint abs_index = partition_info->abs_index;

	slice_t *currslice = &et->ed->current_pict.slice;
	int is_intra = ctu->pred_mode == INTRA_MODE;//ctu->intra_mode[abs_index];
	PartSize part_size_type = (PartSize)ctu->part_size_type[abs_index];

	if( luma )
	{
		// CU header
		if( abs_index == 0 )
		{
			encode_part_size(et, ec, partition_info, part_size_type, is_intra);
		}
		encode_intra_dir_luma_ang(ec, ctu, partition_info, FALSE);
	}
	else
	{
		encode_intra_dir_chroma(ec, ctu, partition_info);
	}
}



void rd_transform_tree(henc_thread_t* et, enc_env_t* ec, ctu_info_t* ctu, cu_partition_info_t* partition_list, int is_luma, int gcnt)
{
	cu_partition_info_t* parent_part_info = partition_list;
	cu_partition_info_t* curr_partition_info = parent_part_info;
	int depth = curr_partition_info->depth;
	int curr_depth = curr_partition_info->depth;
	int curr_part_size_shift;
	int depth_state[MAX_PARTITION_DEPTH] = {0,0,0,0,0};
	int tr_depth, tr_idx;
	int is_first_part_of_cu;
	int abs_index;
	int log2_tr_size, log2_cu_size;
	int tu_log_min_size = et->min_tu_size_shift;
	int tu_log_max_size = et->max_tu_size_shift;
	int tu_log_min_size_in_cu;
	int is_intra_ctu = (ctu->pred_mode==INTRA_MODE);
	int split_flag, is_intra, part_size_type, pred_depth;
	
	while(curr_depth!=depth || depth_state[curr_depth]!=1)
	{
		curr_depth = curr_partition_info->depth;
		abs_index = curr_partition_info->abs_index;
		curr_part_size_shift = et->max_cu_size_shift-curr_depth;
		pred_depth = ctu->pred_depth[abs_index];
		tr_depth = curr_depth-pred_depth;
		is_first_part_of_cu = (tr_depth==0);
		tr_idx = ctu->tr_idx[abs_index];
		is_intra = ctu->pred_mode == INTRA_MODE;
		part_size_type = ctu->part_size_type[abs_index];
		split_flag = ((tr_idx + pred_depth) > curr_depth );
		log2_tr_size = et->max_cu_size_shift-(curr_depth);
		log2_cu_size = et->max_cu_size_shift-pred_depth;

		if(log2_cu_size < tu_log_min_size+(is_intra?et->max_intra_tr_depth:et->max_inter_tr_depth)-1+(is_intra && part_size_type==SIZE_NxN))//falta el flag de inter
			tu_log_min_size_in_cu = et->min_tu_size_shift;
		else
		{
			tu_log_min_size_in_cu = log2_cu_size -((is_intra?et->max_intra_tr_depth:et->max_inter_tr_depth)-1+(is_intra && part_size_type==SIZE_NxN));
			if (tu_log_min_size_in_cu > tu_log_max_size)
			  tu_log_min_size_in_cu = tu_log_max_size;
		}

		if(is_luma && !(is_intra && part_size_type == SIZE_NxN && curr_depth == pred_depth) && 
			!(!is_intra && part_size_type != SIZE_2Nx2N && curr_depth == pred_depth) && 
			!(log2_tr_size>tu_log_max_size) && !(log2_tr_size==tu_log_min_size) && !(log2_tr_size==tu_log_min_size_in_cu))
		{
			ec->ee_encode_bin(ec, GET_CONTEXT_XYZ(ec->e_ctx->cu_trans_subdiv_flag_model ,0, 0, 5 - curr_part_size_shift), split_flag);
		}


		if(!is_luma && (is_first_part_of_cu || curr_part_size_shift > 2))
		{
			if(is_first_part_of_cu || CBF(ctu, abs_index, U_COMP, tr_depth-1))//  ( uiAbsPartIdx, TEXT_CHROMA_U, uiTrDepthCurr - 1 ) )
			{
				encode_qt_cbf(ec, curr_partition_info, U_COMP, tr_depth, CBF(ctu, abs_index, U_COMP, tr_depth));
			}

			if(is_first_part_of_cu || CBF(ctu, abs_index, V_COMP, tr_depth-1))
			{
				encode_qt_cbf(ec, curr_partition_info, V_COMP, tr_depth, CBF(ctu, abs_index, V_COMP, tr_depth));
			}
		}

		depth_state[curr_depth]++;
		if(split_flag)//prolog
		{
			parent_part_info = curr_partition_info;
			curr_depth++;
		}
		else 
		{			
			if(is_luma)
			{
				encode_qt_cbf(ec, curr_partition_info, Y_COMP, tr_depth, CBF(ctu, abs_index, Y_COMP, tr_depth));
			}

			//transform_unit

			if(is_luma && CBF(ctu, abs_index, Y_COMP, tr_depth))
			{
				encode_residual(et, ec, ctu, curr_partition_info, Y_COMP, gcnt);
			}
			else if(!is_luma)
			{
				if(curr_part_size_shift > 2)
				{
					if(CBF(ctu, abs_index, U_COMP, tr_depth))
					{
						encode_residual(et, ec, ctu, curr_partition_info, U_COMP, gcnt);
					}
					if(CBF(ctu, abs_index, V_COMP, tr_depth))
					{
						encode_residual(et, ec, ctu, curr_partition_info, V_COMP, gcnt);
					}
				}
				else //chroma of 4x4 partitions is coded in 4x4 instead of 2x2
				{
					if(curr_partition_info->list_index == curr_partition_info->parent->children[0]->list_index+3)//last of the four 2x2 partitions
					{
						if(CBF(ctu, abs_index, U_COMP, tr_depth))
						{
							encode_residual(et, ec, ctu, curr_partition_info, U_COMP, gcnt);
						}
						if(CBF(ctu, abs_index, V_COMP, tr_depth))
						{
							encode_residual(et, ec, ctu, curr_partition_info, V_COMP, gcnt);
						}
					}
				}			
			}
			while(depth_state[curr_depth]==4)
			{
				depth_state[curr_depth] = 0;
				parent_part_info = parent_part_info->parent;
				curr_depth--;
			}
			if(curr_depth==0 && depth_state[curr_depth]==1)
				break;
		}
		curr_partition_info = parent_part_info->children[depth_state[curr_depth]];
	}
}


uint rd_get_intra_bits_qt( henc_thread_t* et, ctu_info_t* ctu, cu_partition_info_t* partition_info, uint pred_depth, int is_luma, int gcnt)//( TComDataCU*  pcCU, UInt uiTrDepth, UInt uiAbsPartIdx, Bool bLuma, Bool bChroma, Bool bRealCoeff /* just for test */ )
{
	bm_copy_binary_model(et->ee->b_ctx, et->ec->b_ctx);
	ee_copy_entropy_model(et->ee, et->ec);

	et->ec->ee_reset_bits(et->ec->b_ctx);
	  
	rd_est_intra_header( et, et->ec, ctu, partition_info, pred_depth, is_luma);
	rd_transform_tree(et, et->ec, ctu, partition_info, is_luma, gcnt);

	return et->ec->ee_bitcnt(et->ec->bs, et->ec->b_ctx);

}