/*****************************************************************************
* hmr_sao.c : homerHEVC encoding library
/*****************************************************************************
* Copyright (C) 2014-2015 homerHEVC project
*
* Juan Casal <jcasal@homerhevc.com>
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
*****************************************************************************/
/*
* some of the work below is derived from HM HEVC reference code where 
* the following license applies
*****************************************************************************
* The copyright in this software is being made available under the BSD
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





uint g_saoMaxOffsetQVal[NUM_PICT_COMPONENTS];
uint m_offsetStepLog2[NUM_PICT_COMPONENTS];


int skiped_lines_r[NUM_PICT_COMPONENTS] = {5,3,3};
int skiped_lines_b[NUM_PICT_COMPONENTS] = {4,2,2};


void sao_init(int bit_depth)
{
	int component;
	for(component =0; component < NUM_PICT_COMPONENTS; component++)
	{
		m_offsetStepLog2  [component] = max(bit_depth - MAX_SAO_TRUNCATED_BITDEPTH, 0);
		g_saoMaxOffsetQVal[component] = (1<<(min(bit_depth,MAX_SAO_TRUNCATED_BITDEPTH)-5))-1; //Table 9-32, inclusive
	}
}


void sao_get_ctu_stats(henc_thread_t *wpp_thread, slice_t *currslice, ctu_info_t* ctu, sao_stat_data_t stats[][NUM_SAO_NEW_TYPES])	
{
	int component;
	int l_available, r_available, t_available, b_available, tl_available, bl_available, tr_available, br_available;
	int height_luma = (ctu->y[Y_COMP] + ctu->size > wpp_thread->pict_height[Y_COMP])?(wpp_thread->pict_height[Y_COMP]-ctu->y[Y_COMP]):ctu->size;
	int width_luma = (ctu->x[Y_COMP] + ctu->size > wpp_thread->pict_width[Y_COMP])?(wpp_thread->pict_width[Y_COMP]-ctu->x[Y_COMP]):ctu->size;
	int calculate_preblock_stats = wpp_thread->enc_engine->calculate_preblock_stats;

	l_available = (ctu->x[Y_COMP]> 0);
	t_available = (ctu->y[Y_COMP]> 0);
	r_available = ((ctu->x[Y_COMP] + ctu->size) < wpp_thread->pict_width[Y_COMP]);
	b_available = ((ctu->y[Y_COMP] + ctu->size) < wpp_thread->pict_height[Y_COMP]);
	tl_available = t_available & l_available;
	tr_available = t_available & r_available;
	bl_available = b_available & l_available;
	br_available = b_available & r_available;

	for(component=Y_COMP; component < NUM_PICT_COMPONENTS; component++)
	{
		int skip_lines_r = skiped_lines_r[component];
		int skip_lines_b = skiped_lines_b[component];
		int decoded_buff_stride = WND_STRIDE_2D(wpp_thread->enc_engine->curr_reference_frame->img, component);
		int16_t *decoded_buff  = WND_POSITION_2D(int16_t *, wpp_thread->enc_engine->curr_reference_frame->img, component, ctu->x[component], ctu->y[component], 0, wpp_thread->ctu_width);
		int orig_buff_stride = WND_STRIDE_2D(wpp_thread->enc_engine->current_pict.img2encode->img, component);
		int16_t *orig_buff = WND_POSITION_2D(int16_t *, wpp_thread->enc_engine->current_pict.img2encode->img, component, ctu->x[component], ctu->y[component], 0, wpp_thread->ctu_width);
		int type_idx;
		int chroma_shift = (component==Y_COMP)?0:1;
		int height = height_luma>>chroma_shift;
		int width = width_luma>>chroma_shift;

		//getBlkStats
		for(type_idx=0; type_idx< NUM_SAO_NEW_TYPES; type_idx++)
		{
			sao_stat_data_t *curr_stat_data = &stats[component][type_idx];
			int64_t *diff = curr_stat_data->diff;
			int64_t *count = curr_stat_data->count;
			int x,y, start_x, start_y, end_x, end_y, edge_type, first_line_start_x, first_line_end_x;
			int16_t sign_left, sign_right, sign_down;
			int16_t *src_line = decoded_buff;
			int src_stride = decoded_buff_stride;
			int16_t *org_line = orig_buff;
			int org_stride = orig_buff_stride;

			switch(type_idx)
			{
			case SAO_TYPE_EO_0:
				{
					diff +=2;
					count+=2;
					end_y   = (b_available) ? (height - skip_lines_b) : height;
					start_x = (!calculate_preblock_stats) ? (l_available ? 0 : 1): (r_available ? (width - skip_lines_r) : (width - 1));
					end_x   = (!calculate_preblock_stats) ? (r_available ? (width - skip_lines_r) : (width - 1)): (r_available ? width : (width - 1));

					for (y=0; y<end_y; y++)
					{
						int aux = src_line[start_x] - src_line[start_x-1];
						sign_left = aux==0?0:(SIGN(aux));//(Char)m_sign[srcLine[startX] - srcLine[startX-1]];
						for (x=start_x; x<end_x; x++)
						{
							int aux = src_line[x] - src_line[x+1];
							sign_right =  aux==0?0:(SIGN(aux));//(Char)m_sign[srcLine[x] - srcLine[x+1]]; 
							edge_type  =  sign_right + sign_left;
							sign_left  = -sign_right;

							diff [edge_type] += (org_line[x] - src_line[x]);
							count[edge_type] ++;
						}
						src_line  += src_stride;
						org_line  += org_stride;
					}				
				}
				break;
			case SAO_TYPE_EO_90:
				{
					int16_t *signUpLine = wpp_thread->sao_sign_line_buff1;//m_signLineBuf1;
					int16_t* srcLineAbove, *srcLineBelow;
					diff +=2;
					count+=2;
					
					start_x = (!calculate_preblock_stats)?0:(r_available ? (width - skip_lines_r) : width);
					start_y = t_available ? 0 : 1;
					end_x   = (!calculate_preblock_stats) ? (r_available ? (width - skip_lines_r) : width): width;
					end_y   = b_available ? (height - skip_lines_b) : (height - 1);
					if (!t_available)
					{
						src_line  += src_stride;
						org_line  += org_stride;
					}

					srcLineAbove = src_line - src_stride;
					for (x=start_x; x<end_x; x++) 
					{
						int aux = src_line[x] - srcLineAbove[x];
						signUpLine[x] = aux==0?0:(SIGN(aux));//(Char)m_sign[srcLine[x] - srcLineAbove[x]];
					}

					for (y=start_y; y<end_y; y++)
					{
						srcLineBelow = src_line + src_stride;

						for (x=start_x; x<end_x; x++)
						{
							int aux = src_line[x] - srcLineBelow[x];
							sign_down  = aux==0?0:(SIGN(aux));//(Char)m_sign[srcLine[x] - srcLineBelow[x]]; 
							edge_type  = sign_down + signUpLine[x];
							signUpLine[x]= -sign_down;

							diff [edge_type] += (org_line[x] - src_line[x]);
							count[edge_type] ++;
						}
						src_line += src_stride;
						org_line += org_stride;
					}

				}
				break;
			case SAO_TYPE_EO_135:
				{
					int16_t *signUpLine, *signDownLine, *signTmpLine;
					int16_t* srcLineBelow, *srcLineAbove;
					diff +=2;
					count+=2;
					
					signUpLine = wpp_thread->sao_sign_line_buff1;//m_signLineBuf1;
					signDownLine = wpp_thread->sao_sign_line_buff2;//m_signLineBuf2;

					start_x = (!calculate_preblock_stats) ? (l_available ? 0 : 1) : (r_available ? (width - skip_lines_r) : (width - 1));

					end_x = (!calculate_preblock_stats) ? (r_available ? (width - skip_lines_r): (width - 1)) : (r_available ? width : (width - 1));
					end_y = b_available ? (height - skip_lines_b) : (height - 1);

					//prepare 2nd line's upper sign
					srcLineBelow = src_line + src_stride;
					for (x=start_x; x<end_x+1; x++)
					{
						int aux = srcLineBelow[x] - src_line[x-1];
						signUpLine[x] = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLineBelow[x] - srcLine[x-1]];
					}

					//1st line
					srcLineAbove = src_line - src_stride;
					first_line_start_x = (!calculate_preblock_stats) ? (tl_available ? 0 : 1) : start_x;
					first_line_end_x   = (!calculate_preblock_stats) ? (t_available ? end_x : 1) : end_x;
					for(x=first_line_start_x; x<first_line_end_x; x++)
					{
						int aux = src_line[x] - srcLineAbove[x-1];
						edge_type = ((aux==0)?0:(SIGN(aux))) - signUpLine[x+1];//m_sign[srcLine[x] - srcLineAbove[x-1]] - signUpLine[x+1];

						diff [edge_type] += (org_line[x] - src_line[x]);
						count[edge_type] ++;
					}
					src_line  += src_stride;
					org_line  += org_stride;

					//middle lines
					for (y=1; y<end_y; y++)
					{
						int aux;
						srcLineBelow = src_line + src_stride;

						for (x=start_x; x<end_x; x++)
						{
							int aux0 = src_line[x] - srcLineBelow[x+1];
							sign_down = (aux0==0)?0:(SIGN(aux0));//(Char)m_sign[srcLine[x] - srcLineBelow[x+1]] ;
							
							edge_type = sign_down + signUpLine[x];
							diff [edge_type] += (org_line[x] - src_line[x]);
							count[edge_type] ++;

							signDownLine[x+1] = -sign_down; 
						}
						aux = srcLineBelow[start_x] - src_line[start_x-1];
						signDownLine[start_x] = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLineBelow[startX] - srcLine[startX-1]];

						signTmpLine  = signUpLine;
						signUpLine   = signDownLine;
						signDownLine = signTmpLine;

						src_line += src_stride;
						org_line += org_stride;
					}

				}
				break;
			case SAO_TYPE_EO_45:
				{
					int16_t *signUpLine = wpp_thread->sao_sign_line_buff1+1;//m_signLineBuf1+1;
					int16_t *srcLineBelow, *srcLineAbove;
					diff +=2;
					count+=2;

					start_x = (!calculate_preblock_stats) ? (l_available ? 0 : 1) : (r_available ? (width - skip_lines_r) : (width - 1));
					end_x = (!calculate_preblock_stats) ? (r_available ? (width - skip_lines_r) : (width - 1)) : (r_available ? width : (width - 1));
					end_y = b_available ? (height - skip_lines_b) : (height - 1);

					//prepare 2nd line upper sign
					srcLineBelow = src_line + src_stride;
					for (x=start_x-1; x<end_x; x++)
					{
						int aux = srcLineBelow[x] - src_line[x+1];
						signUpLine[x] = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLineBelow[x] - srcLine[x+1]];
					}

					//first line
					srcLineAbove = src_line - src_stride;
					first_line_start_x = (!calculate_preblock_stats) ? (t_available ? start_x : end_x) : start_x;
					first_line_end_x   = (!calculate_preblock_stats) ? ((!r_available && tr_available) ? width : end_x) : end_x;
					for(x=first_line_start_x; x<first_line_end_x; x++)
					{
						int aux = src_line[x] - srcLineAbove[x+1];
						edge_type = ((aux==0)?0:(SIGN(aux))) - signUpLine[x-1];//m_sign[srcLine[x] - srcLineAbove[x+1]] - signUpLine[x-1];
						diff [edge_type] += (org_line[x] - src_line[x]);
						count[edge_type] ++;
					}

					src_line += src_stride;
					org_line += org_stride;

					//middle lines
					for (y=1; y<end_y; y++)
					{
						int aux;
						srcLineBelow = src_line + src_stride;

						for(x=start_x; x<end_x; x++)
						{
							int aux0 = src_line[x] - srcLineBelow[x-1];
							sign_down = (aux0==0)?0:(SIGN(aux0));//(Char)m_sign[srcLine[x] - srcLineBelow[x-1]] ;
							edge_type = sign_down + signUpLine[x];

							diff [edge_type] += (org_line[x] - src_line[x]);
							count[edge_type]++;

							signUpLine[x-1] = -sign_down; 
						}
						aux = srcLineBelow[end_x-1] - src_line[end_x];
						signUpLine[end_x-1] = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLineBelow[endX-1] - srcLine[endX]];
						src_line  += src_stride;
						org_line  += org_stride;
					}
				}
				break;
			case SAO_TYPE_BO:
				{
					int shiftBits = wpp_thread->bit_depth - NUM_SAO_BO_CLASSES_LOG2;
					start_x = (!calculate_preblock_stats) ? 0:( r_available?(width- skip_lines_r):width);
					end_x = (!calculate_preblock_stats) ? (r_available ? (width - skip_lines_r) : (width)) : width;
					end_y = b_available ? (height - skip_lines_b) : height;
					
					for (y=0; y< end_y; y++)
					{
						for (x=start_x; x< end_x; x++)
						{
							int bandIdx= src_line[x] >> shiftBits; 
							diff [bandIdx] += (org_line[x] - src_line[x]);
							count[bandIdx] ++;
						}
						src_line += src_stride;
						org_line += org_stride;
					}
				}
				break;
			default:
				{
					printf("Not a supported SAO types\n");
//					assert(0);
					exit(-1);
				}
			}

		}

	}
}

void sao_decide_pic_params(int *slice_enable, int sao_enable_luma, int sao_enable_chroma)	
{
	int component;
	for(component=Y_COMP; component < NUM_PICT_COMPONENTS; component++)
	{		// reset flags & counters
//		slice_enable[component] = TRUE;
	}
	slice_enable[Y_COMP] = sao_enable_luma;
	slice_enable[U_COMP] = sao_enable_chroma;
	slice_enable[V_COMP] = sao_enable_chroma;
}


int sao_get_merge_list(henc_thread_t *wpp_thread, ctu_info_t *ctu, sao_blk_param_t* merge_list[], int *merge_list_size)
{
	int ctu_idx = ctu->ctu_number;
	int ctuX = ctu_idx % wpp_thread->pict_width_in_ctu;
	int ctuY = ctu_idx / wpp_thread->pict_width_in_ctu;
	int mergedCTUPos;
	int numValidMergeCandidates = 0;
	int merge_type;
	int merge_idx = 0;


	for(merge_type=0; merge_type< NUM_SAO_MERGE_TYPES; merge_type++)
	{
		sao_blk_param_t *mergeCandidate = NULL;

		switch(merge_type)
		{
		case SAO_MERGE_ABOVE:
			{
				if(ctuY > 0)
				{
					ctu_info_t *ctu_aux = ctu - wpp_thread->pict_width_in_ctu;
					//mergedCTUPos = ctu_idx - enc_engine->pict_width_in_ctu;
//					if( pic->getSAOMergeAvailability(ctu_idx, mergedCTUPos) )//availability depends on slice or tile coherence
					{
						mergeCandidate = &(ctu_aux->recon_params);
					}
				}
			}
			break;
		case SAO_MERGE_LEFT:
			{
				if(ctuX > 0)
				{
					ctu_info_t *ctu_aux = ctu - 1;
//					mergedCTUPos = ctu_idx - 1;
//					if( pic->getSAOMergeAvailability(ctu_idx, mergedCTUPos) )//availability depends on slice or tile coherence
					{
						mergeCandidate = &(ctu_aux->recon_params);
					}
				}
			}
			break;
		default:
			{
				printf("not a supported merge type");
//				//assert(0);
				exit(-1);
			}
		}

//		mergeList.push_back(mergeCandidate);
		merge_list[merge_idx++] = mergeCandidate;
		if (mergeCandidate != NULL)
		{
			numValidMergeCandidates++;
		}
	}

	*merge_list_size = merge_idx;

	return numValidMergeCandidates;
}

double xRoundIbdi2(int bit_depth, double x)
{
	return ((x)>0) ? (int)(((int)(x)+(1<<(bit_depth-8-1)))/(1<<(bit_depth-8))) : ((int)(((int)(x)-(1<<(bit_depth-8-1)))/(1<<(bit_depth-8))));
}

double x_round_ibdi(int bit_depth, double x)
{
	return (bit_depth > 8 ? xRoundIbdi2(bit_depth, (x)) : ((x)>=0 ? ((int)((x)+0.5)) : ((int)((x)-0.5)))) ;
}


//inline Int64 TEncSampleAdaptiveOffset::estSaoDist(Int64 count, Int64 offset, Int64 diffSum, Int shift)
int64_t est_sao_dist(int64_t count, int64_t offset, int64_t diffSum, int shift)
{
  return (( count*offset*offset-diffSum*offset*2 ) >> shift);
}

//inline int TEncSampleAdaptiveOffset::estIterOffset(int type_idx, int class_idx, Double lambda, int offset_input, Int64 count, Int64 diff_sum, int shift, int bit_increase, Int64& best_dist, Double& bestCost, int offset_thrshl )
int est_iter_offset(int type_idx, int class_idx, double lambda, int offset_input, int64_t count, int64_t diff_sum, int shift, int bit_increase, int64_t *best_dist, double* best_cost, int offset_thrshl)
{
	int iterOffset, tempOffset;
	int64_t tempDist, tempRate;
	double tempCost, tempMinCost;
	int offsetOutput = 0;
	iterOffset = offset_input;
	// Assuming sending quantized value 0 results in zero offset and sending the value zero needs 1 bit. entropy coder can be used to measure the exact rate here. 
	tempMinCost = lambda; 
	while (iterOffset != 0)
	{
		// Calculate the bits required for signaling the offset
		tempRate = (type_idx == SAO_TYPE_BO) ? (abs((int)iterOffset)+2) : (abs((int)iterOffset)+1); 
		if (abs((int)iterOffset)==offset_thrshl) //inclusive 
		{  
			tempRate --;
		}
		// Do the dequantization before distortion calculation
		tempOffset  = iterOffset << bit_increase;
		tempDist    = est_sao_dist( count, tempOffset, diff_sum, shift);
		tempCost    = ((double)tempDist + lambda * (double) tempRate);
		if(tempCost < tempMinCost)
		{
			tempMinCost = tempCost;
			offsetOutput = iterOffset;
			*best_dist = tempDist;
			*best_cost = tempCost;
		}
		iterOffset = (iterOffset > 0) ? (iterOffset-1):(iterOffset+1);
	}
	return offsetOutput;
}


//Void TEncSampleAdaptiveOffset::deriveOffsets(int ctu, int compIdx, int typeIdc, SAOStatData& statData, int* quant_offsets, int& typeAuxInfo)
void sao_derive_offsets(henc_thread_t *wpp_thread, int component, int type_idc, sao_stat_data_t *stats, int* quant_offsets, int* type_aux_info)
{
	int bit_depth = wpp_thread->bit_depth;//(component== Y_COMP) ? g_bitDepthY : g_bitDepthC;
	int shift = 2 * DISTORTION_PRECISION_ADJUSTMENT(bit_depth-8);
	int offset_thrshld = g_saoMaxOffsetQVal[component];  //inclusive
	int num_classes, class_idx;
	double *lambdas = wpp_thread->enc_engine->sao_lambdas;

	memset(quant_offsets, 0, sizeof(int)*MAX_NUM_SAO_CLASSES);

	//derive initial offsets 
	num_classes = (type_idc == SAO_TYPE_BO)?((int)NUM_SAO_BO_CLASSES):((int)NUM_SAO_EO_CLASSES);
	for(class_idx=0; class_idx< num_classes; class_idx++)
	{
		if( (type_idc != SAO_TYPE_BO) && (class_idx==SAO_CLASS_EO_PLAIN)  ) 
		{
			continue; //offset will be zero
		}

		if(stats->count[class_idx] == 0)
		{
			continue; //offset will be zero
		}

		quant_offsets[class_idx] = (int) x_round_ibdi(bit_depth, (double)( stats->diff[class_idx]<<(bit_depth-8))/(double)(stats->count[class_idx]<< m_offsetStepLog2[component]));

		quant_offsets[class_idx] = clip(quant_offsets[class_idx], -offset_thrshld, offset_thrshld);
	}

	// adjust offsets
	switch(type_idc)
	{
	case SAO_TYPE_EO_0:
	case SAO_TYPE_EO_90:
	case SAO_TYPE_EO_135:
	case SAO_TYPE_EO_45:
		{
			int64_t class_dist;
			double class_cost;
			int class_idx;
			for(class_idx=0; class_idx<NUM_SAO_EO_CLASSES; class_idx++)  
			{         
				if(class_idx==SAO_CLASS_EO_FULL_VALLEY && quant_offsets[class_idx] < 0) quant_offsets[class_idx] =0;
				if(class_idx==SAO_CLASS_EO_HALF_VALLEY && quant_offsets[class_idx] < 0) quant_offsets[class_idx] =0;
				if(class_idx==SAO_CLASS_EO_HALF_PEAK   && quant_offsets[class_idx] > 0) quant_offsets[class_idx] =0;
				if(class_idx==SAO_CLASS_EO_FULL_PEAK   && quant_offsets[class_idx] > 0) quant_offsets[class_idx] =0;

				if( quant_offsets[class_idx] != 0 ) //iterative adjustment only when derived offset is not zero
				{
					quant_offsets[class_idx] = est_iter_offset( type_idc, class_idx, lambdas[component], quant_offsets[class_idx], stats->count[class_idx], stats->diff[class_idx], shift, m_offsetStepLog2[component], &class_dist , &class_cost , offset_thrshld);
				}
			}

			*type_aux_info = 0;
		}
		break;
	case SAO_TYPE_BO:
		{
			int64_t dist_bo_classes[NUM_SAO_BO_CLASSES];
			double cost_bo_classes[NUM_SAO_BO_CLASSES];
			int clear_quant_offset[NUM_SAO_BO_CLASSES];
			double min_cost, cost;
			int band, i;
			int class_idx;
			memset(dist_bo_classes, 0, sizeof(dist_bo_classes));
			for(class_idx=0; class_idx< NUM_SAO_BO_CLASSES; class_idx++)
			{         
				cost_bo_classes[class_idx]= lambdas[component];
				if( quant_offsets[class_idx] != 0 ) //iterative adjustment only when derived offset is not zero
				{
					quant_offsets[class_idx] = est_iter_offset( type_idc, class_idx, lambdas[component], quant_offsets[class_idx], stats->count[class_idx], stats->diff[class_idx], shift, m_offsetStepLog2[component], &dist_bo_classes[class_idx], &cost_bo_classes[class_idx], offset_thrshld);
				}
			}

			//decide the starting band index
			min_cost = MAX_COST;
			for(band=0; band< NUM_SAO_BO_CLASSES- 4+ 1; band++) 
			{
				cost  = cost_bo_classes[band  ];
				cost += cost_bo_classes[band+1];
				cost += cost_bo_classes[band+2];
				cost += cost_bo_classes[band+3];

				if(cost < min_cost)
				{
					min_cost = cost;
					*type_aux_info = band;
				}
			}
			//clear those unused classes

			memset(clear_quant_offset, 0, sizeof(int)*NUM_SAO_BO_CLASSES);
			for(i=0; i< 4; i++) 
			{
				int band = (*type_aux_info+i)%NUM_SAO_BO_CLASSES;
				clear_quant_offset[band] = quant_offsets[band];
			}
			memcpy(quant_offsets, clear_quant_offset, sizeof(clear_quant_offset));        
		}
		break;
	default:
		{
			printf("Not a supported type");
			//assert(0);
			exit(-1);
		}

	}
}


//Void TComSampleAdaptiveOffset::invertQuantOffsets(Int compIdx, Int typeIdc, Int typeAuxInfo, Int* dstOffsets, Int* srcOffsets)
void sao_invert_quant_offsets(int component, int type_idc, int typeAuxInfo, int* dstOffsets, int* srcOffsets)
{
  int codedOffset[MAX_NUM_SAO_CLASSES];

  memcpy(codedOffset, srcOffsets, sizeof(int)*MAX_NUM_SAO_CLASSES);
  memset(dstOffsets, 0, sizeof(int)*MAX_NUM_SAO_CLASSES);

  if(type_idc == SAO_TYPE_START_BO)
  {
	int i;
    for(i=0; i< 4; i++)
    {
      dstOffsets[(typeAuxInfo+ i)%NUM_SAO_BO_CLASSES] = codedOffset[(typeAuxInfo+ i)%NUM_SAO_BO_CLASSES]*(1<<m_offsetStepLog2[component]);
    }
  }
  else //EO
  {
	int i;
    for(i=0; i< NUM_SAO_EO_CLASSES; i++)
    {
      dstOffsets[i] = codedOffset[i] *(1<<m_offsetStepLog2[component]);
    }
//    assert(dstOffsets[SAO_CLASS_EO_PLAIN] == 0); //keep EO plain offset as zero
  }

}

//Int64 TEncSampleAdaptiveOffset::getDistortion(Int ctu, Int compIdx, Int typeIdc, Int typeAuxInfo, Int* invQuantOffset, SAOStatData& statData)
int64_t sao_get_distortion(int typeIdc, int typeAuxInfo, int* invQuantOffset, sao_stat_data_t *stats, int bit_depth)
{
  int64_t dist=0;
  int inputBitDepth = bit_depth;//(component == Y_COMP) ? g_bitDepthY : g_bitDepthC ;
  int shift = 2 * DISTORTION_PRECISION_ADJUSTMENT(inputBitDepth-8);

  switch(typeIdc)
  {
    case SAO_TYPE_EO_0:
    case SAO_TYPE_EO_90:
    case SAO_TYPE_EO_135:
    case SAO_TYPE_EO_45:
      {
		int offsetIdx;
        for (offsetIdx=0; offsetIdx<NUM_SAO_EO_CLASSES; offsetIdx++)
        {
          dist += est_sao_dist( stats->count[offsetIdx], invQuantOffset[offsetIdx], stats->diff[offsetIdx], shift);
        }        
      }
      break;
    case SAO_TYPE_BO:
      {
		  int offsetIdx;
        for (offsetIdx=typeAuxInfo; offsetIdx<typeAuxInfo+4; offsetIdx++)
        {
          int bandIdx = offsetIdx % NUM_SAO_BO_CLASSES ; 
          dist += est_sao_dist( stats->count[bandIdx], invQuantOffset[bandIdx], stats->diff[bandIdx], shift);
        }
      }
      break;
    default:
      {
        printf("Not a supported type");
//        assert(0);
        exit(-1);
      }
  }

  return dist;
}


//Void TEncSampleAdaptiveOffset::deriveModeNewRDO(int ctu, std::vector<SAOBlkParam*>& mergeList, Bool* sliceEnabled, SAOStatData*** blkStats, SAOBlkParam& mode_param, Double& modeNormCost, TEncSbac** cabacCoderRDO, int inCabacLabel)
void sao_derive_mode_new_rdo(henc_thread_t *wpp_thread, sao_blk_param_t** merge_list, int merge_list_size, sao_stat_data_t stats[][NUM_SAO_NEW_TYPES], sao_blk_param_t *mode_param, double *mode_cost, int slice_enabled[] )
{
	double minCost, cost;
	int rate;
	uint previousWrittenBits;
	int64_t dist[NUM_PICT_COMPONENTS], modeDist[NUM_PICT_COMPONENTS];
	sao_offset_t testOffset[NUM_PICT_COMPONENTS];
	int component;
	int invQuantOffset[MAX_NUM_SAO_CLASSES];
	int type_idc;
	double *lambdas = wpp_thread->enc_engine->sao_lambdas;

	modeDist[Y_COMP]= modeDist[U_COMP] = modeDist[V_COMP] = 0;

	//pre-encode merge flags
//	mode_param->offsetParam[Y_COMP].modeIdc = SAO_MODE_OFF;
//	m_pcRDGoOnSbacCoder->load(cabacCoderRDO[inCabacLabel]);
//	m_pcRDGoOnSbacCoder->codeSAOBlkParam(mode_param, sliceEnabled, (mergeList[SAO_MERGE_LEFT]!= NULL), (mergeList[SAO_MERGE_ABOVE]!= NULL), true);
//	m_pcRDGoOnSbacCoder->store(cabacCoderRDO[SAO_CABACSTATE_BLK_MID]);

	//------ luma --------//
	component = Y_COMP;
	//"off" case as initial cost
	mode_param->offsetParam[component].modeIdc = SAO_MODE_OFF;
//	m_pcRDGoOnSbacCoder->resetBits();
//	m_pcRDGoOnSbacCoder->codeSAOOffsetParam(component, mode_param[component], sliceEnabled[component]);
	modeDist[component] = 0;
#ifdef COMPUTE_AS_HM
//	minCost = MAX_COST;
	minCost = 2.5*lambdas[component];//MAX_COST;
#else
	rate = rd_code_sao_offset_param(wpp_thread, component, &mode_param->offsetParam[component], slice_enabled[component], wpp_thread->ee->contexts, wpp_thread->ee->b_ctx);
	minCost = lambdas[component]*rate; 
	bm_copy_binary_model(wpp_thread->ec->b_ctx, &wpp_thread->aux_bm);
	ee_copy_entropy_model(wpp_thread->ec->contexts, wpp_thread->aux_contexts);
#endif
//	minCost= m_lambda[component]*((Double)m_pcRDGoOnSbacCoder->getNumberOfWrittenBits());
//	m_pcRDGoOnSbacCoder->store(cabacCoderRDO[SAO_CABACSTATE_BLK_TEMP]);
	if(slice_enabled[component])
	{
//		int type_idc;
		for(type_idc=0; type_idc< NUM_SAO_NEW_TYPES; type_idc++)
		{
			testOffset[component].modeIdc = SAO_MODE_NEW;
			testOffset[component].typeIdc = type_idc;

			//derive coded offset
//			deriveOffsets(ctu, component, type_idc, blkStats[ctu][component][type_idc], testOffset[component].offset, testOffset[component].typeAuxInfo);
			
			sao_derive_offsets(wpp_thread, component, type_idc, &stats[component][type_idc], testOffset[component].offset, &testOffset[component].typeAuxInfo);

			//inversed quantized offsets
			//invertQuantOffsets(component, type_idc, testOffset[component].typeAuxInfo, invQuantOffset, testOffset[component].offset);
			sao_invert_quant_offsets(component, type_idc, testOffset[component].typeAuxInfo, invQuantOffset, testOffset[component].offset);

			//get distortion
			//dist[component] = getDistortion(ctu, component, testOffset[component].type_idc, testOffset[component].typeAuxInfo, invQuantOffset, blkStats[ctu][component][type_idc]);
			dist[component] = sao_get_distortion(testOffset[component].typeIdc, testOffset[component].typeAuxInfo, invQuantOffset, &stats[component][type_idc], wpp_thread->enc_engine->bit_depth);
//				int64_t get_distortion(int typeIdc, int typeAuxInfo, int* invQuantOffset, sao_stat_data_t *stats, int bit_depth)
			//get rate
//			m_pcRDGoOnSbacCoder->load(cabacCoderRDO[SAO_CABACSTATE_BLK_MID]);
//			m_pcRDGoOnSbacCoder->resetBits();
//			m_pcRDGoOnSbacCoder->codeSAOOffsetParam(component, testOffset[component], sliceEnabled[component]);
//			rate = m_pcRDGoOnSbacCoder->getNumberOfWrittenBits();

			cost = (double)dist[component];// + m_lambda[component]*((Double)rate);
#ifdef COMPUTE_AS_HM
			if(type_idc==SAO_TYPE_BO)
				cost += lambdas[component]*11;
			else
				cost += lambdas[component]*8;
#else
//			cost += lambdas[component]*rd_code_sao_offset_param(wpp_thread, component, &testOffset[component], slice_enabled[component]);
			rate = rd_code_sao_offset_param(wpp_thread, component, &testOffset[component], slice_enabled[component], wpp_thread->ee->contexts, wpp_thread->ee->b_ctx);
			cost += lambdas[component]*rate; 
#endif

			if(cost < minCost)
			{
				minCost = cost;
				modeDist[component] = dist[component];
				mode_param->offsetParam[component]= testOffset[component];
				bm_copy_binary_model(wpp_thread->ec->b_ctx, &wpp_thread->aux_bm);
				ee_copy_entropy_model(wpp_thread->ec->contexts, wpp_thread->aux_contexts);
//				m_pcRDGoOnSbacCoder->store(cabacCoderRDO[SAO_CABACSTATE_BLK_TEMP]);
			}
		}
	}
//	m_pcRDGoOnSbacCoder->load(cabacCoderRDO[SAO_CABACSTATE_BLK_TEMP]);
//	m_pcRDGoOnSbacCoder->store(cabacCoderRDO[SAO_CABACSTATE_BLK_MID]);

	//------ chroma --------//
	//"off" case as initial cost
	cost = 0;
	previousWrittenBits = 0;
//	m_pcRDGoOnSbacCoder->resetBits();
	for (component = U_COMP; component < NUM_PICT_COMPONENTS; component++)
	{
		mode_param->offsetParam[component].modeIdc = SAO_MODE_OFF; 
		modeDist [component] = 0;
		if(component == U_COMP)
			rate = rd_code_sao_offset_param(wpp_thread, component, &mode_param->offsetParam[component], slice_enabled[component], wpp_thread->aux_contexts, &wpp_thread->aux_bm);
		else
			rate = rd_code_sao_offset_param(wpp_thread, component, &mode_param->offsetParam[component], slice_enabled[component], wpp_thread->ec->contexts, wpp_thread->ec->b_ctx);
		cost += lambdas[component]*rate; 
//		m_pcRDGoOnSbacCoder->codeSAOOffsetParam(component, mode_param[component], sliceEnabled[component]);

//		const UInt currentWrittenBits = m_pcRDGoOnSbacCoder->getNumberOfWrittenBits();
//		cost += m_lambda[component] * (currentWrittenBits - previousWrittenBits);
//		previousWrittenBits = currentWrittenBits;
	}

	minCost = cost;
//	minCost = MAX_COST;
#ifdef COMPUTE_AS_HM
	minCost = 2.5*lambdas[U_COMP];//MAX_COST;
#endif
	//doesn't need to store cabac status here since the whole CTU parameters will be re-encoded at the end of this function

	for(type_idc=0; type_idc< NUM_SAO_NEW_TYPES; type_idc++)
	{
//		m_pcRDGoOnSbacCoder->load(cabacCoderRDO[SAO_CABACSTATE_BLK_MID]);
//		m_pcRDGoOnSbacCoder->resetBits();
		previousWrittenBits = 0;
		cost = 0;

		for(component= U_COMP; component< NUM_PICT_COMPONENTS; component++)
		{
			if(!slice_enabled[component])
			{
				testOffset[component].modeIdc = SAO_MODE_OFF;
				dist[component]= 0;
				continue;
			}
			testOffset[component].modeIdc = SAO_MODE_NEW;
			testOffset[component].typeIdc = type_idc;

			sao_derive_offsets(wpp_thread, component, type_idc, &stats[component][type_idc], testOffset[component].offset, &testOffset[component].typeAuxInfo);

			sao_invert_quant_offsets(component, type_idc, testOffset[component].typeAuxInfo, invQuantOffset, testOffset[component].offset);

			dist[component] = sao_get_distortion(testOffset[component].typeIdc, testOffset[component].typeAuxInfo, invQuantOffset, &stats[component][type_idc], wpp_thread->bit_depth);

//			m_pcRDGoOnSbacCoder->codeSAOOffsetParam(component, testOffset[component], sliceEnabled[component]);
			
//			const UInt currentWrittenBits = m_pcRDGoOnSbacCoder->getNumberOfWrittenBits();
			cost += dist[component];// + (m_lambda[component] * (currentWrittenBits - previousWrittenBits));
			if(component == U_COMP)
				rate = rd_code_sao_offset_param(wpp_thread, component, &testOffset[component], slice_enabled[component], wpp_thread->aux_contexts, &wpp_thread->aux_bm);
			else
				rate = rd_code_sao_offset_param(wpp_thread, component, &testOffset[component], slice_enabled[component], wpp_thread->ec->contexts, wpp_thread->ec->b_ctx);
			
#ifdef COMPUTE_AS_HM
			if(type_idc==SAO_TYPE_BO)
				cost += lambdas[component]*11;
			else
				cost += lambdas[component]*8;
#else
			cost += lambdas[component]*rate; 
#endif
//			previousWrittenBits = currentWrittenBits;
		}

		if(cost < minCost)
		{
			minCost = cost;
			for(component= U_COMP; component< NUM_PICT_COMPONENTS; component++)
			{
				modeDist [component] = dist      [component];
				mode_param->offsetParam[component] = testOffset[component];
			}
		}
	}

	//----- re-gen rate & normalized cost----//
	*mode_cost = (double)modeDist[Y_COMP]/lambdas[Y_COMP] + (double)modeDist[U_COMP]/lambdas[U_COMP] + (double)modeDist[V_COMP]/lambdas[V_COMP];
//	for(component = Y_COMP; component < NUM_PICT_COMPONENTS; component++)
//	{
//		mode_cost += (double)modeDist[component];// / m_lambda[component];
//	}
//	m_pcRDGoOnSbacCoder->load(cabacCoderRDO[inCabacLabel]);
//	m_pcRDGoOnSbacCoder->resetBits();
//	m_pcRDGoOnSbacCoder->codeSAOBlkParam(mode_param, sliceEnabled, (mergeList[SAO_MERGE_LEFT]!= NULL), (mergeList[SAO_MERGE_ABOVE]!= NULL), false);
//	modeNormCost += (Double)m_pcRDGoOnSbacCoder->getNumberOfWrittenBits();
#ifndef COMPUTE_AS_HM
//	if(mode_param->offsetParam[Y_COMP].modeIdc != SAO_MODE_OFF || mode_param->offsetParam[U_COMP].modeIdc != SAO_MODE_OFF || mode_param->offsetParam[V_COMP].modeIdc != SAO_MODE_OFF)
		*mode_cost += rd_code_sao_blk_param(wpp_thread, mode_param, slice_enabled, (merge_list[SAO_MERGE_LEFT]!= NULL), (merge_list[SAO_MERGE_ABOVE]!= NULL), FALSE, wpp_thread->ee->contexts, wpp_thread->ee->b_ctx);
#endif
}

//Void TEncSampleAdaptiveOffset::deriveModeMergeRDO(Int ctu, std::vector<SAOBlkParam*>& mergeList, Bool* sliceEnabled, SAOStatData*** blkStats, SAOBlkParam& modeParam, Double& modeNormCost, TEncSbac** cabacCoderRDO, Int inCabacLabel)
void sao_derive_mode_merge_rdo(henc_thread_t *wpp_thread, sao_blk_param_t** merge_list, int merge_list_size, int* slice_enable, sao_stat_data_t stats[][NUM_SAO_NEW_TYPES], sao_blk_param_t* mode_param, double *mode_cost)//, TEncSbac** cabacCoderRDO, Int inCabacLabel)
{
	double cost;
	sao_blk_param_t test_blk_param;
	int merge_type;
	int component;
	uint rate;
	double *lambdas = wpp_thread->enc_engine->sao_lambdas;
	//  int mergeListSize = (Int)mergeList.size();
	*mode_cost = MAX_COST;

	for(merge_type=0; merge_type< merge_list_size; merge_type++)
	{
		double norm_dist=0;
		if(merge_list[merge_type] == NULL)
		{
			continue;
		}

		test_blk_param = *(merge_list[merge_type]);
		//normalized distortion

		for(component=0; component< NUM_PICT_COMPONENTS; component++)
		{
			sao_offset_t *merged_offsetparam;
			test_blk_param.offsetParam[component].modeIdc = SAO_MODE_MERGE;
			test_blk_param.offsetParam[component].typeIdc = merge_type;

			merged_offsetparam = &(*(merge_list[merge_type])).offsetParam[component];

			if( merged_offsetparam->modeIdc != SAO_MODE_OFF)
			{
				//offsets have been reconstructed. Don't call inversed quantization function.
				//get_distortion(testOffset[component].typeIdc, testOffset[component].typeAuxInfo, invQuantOffset, &stats[component][type_idc], enc_engine->bit_depth);        
				norm_dist += (((double)sao_get_distortion(merged_offsetparam->typeIdc, merged_offsetparam->typeAuxInfo, merged_offsetparam->offset, &stats[component][merged_offsetparam->typeIdc], wpp_thread->bit_depth)))/wpp_thread->enc_engine->sao_lambdas[component];///m_lambda[component]);
			}
		}

		//rate
		//    m_pcRDGoOnSbacCoder->load(cabacCoderRDO[inCabacLabel]);
		//    m_pcRDGoOnSbacCoder->resetBits();
		//    m_pcRDGoOnSbacCoder->codeSAOBlkParam(testBlkParam, sliceEnabled, (mergeList[SAO_MERGE_LEFT]!= NULL), (mergeList[SAO_MERGE_ABOVE]!= NULL), false);
		//    Int rate = m_pcRDGoOnSbacCoder->getNumberOfWrittenBits();
		rate = rd_code_sao_blk_param(wpp_thread, &test_blk_param, slice_enable, (merge_list[SAO_MERGE_LEFT]!= NULL), (merge_list[SAO_MERGE_ABOVE]!= NULL), FALSE, wpp_thread->ee->contexts, wpp_thread->ee->b_ctx);

		//    cost = norm_dist+lambdas[Y_COMP]*(double)rate;
		cost = norm_dist;
#ifndef COMPUTE_AS_HM
		cost += (double)rate;
#endif

		if(cost < *mode_cost)
		{
			*mode_cost = cost;
			*mode_param    = test_blk_param;
			//      m_pcRDGoOnSbacCoder->store(cabacCoderRDO[SAO_CABACSTATE_BLK_TEMP]);
		}
	}

	//  m_pcRDGoOnSbacCoder->load(cabacCoderRDO[SAO_CABACSTATE_BLK_TEMP]);
}



//Void TComSampleAdaptiveOffset::reconstructBlkSAOParam(SAOBlkParam& recParam, std::vector<SAOBlkParam*>& mergeList)
void reconstruct_blk_sao_param(sao_blk_param_t *rec_param, sao_blk_param_t* merge_list[], int merge_list_size)
{
	int component;
  for(component=0; component< NUM_PICT_COMPONENTS; component++)
  {
    sao_offset_t *offset_param = &rec_param->offsetParam[component];

    if(offset_param->modeIdc == SAO_MODE_OFF)
    {
      continue;
    }

    switch(offset_param->modeIdc)
    {
    case SAO_MODE_NEW:
      {
//        invertQuantOffsets(component, offsetParam.typeIdc, offsetParam.typeAuxInfo, offsetParam.offset, offsetParam.offset);
		  sao_invert_quant_offsets(component, offset_param->typeIdc, offset_param->typeAuxInfo, offset_param->offset, offset_param->offset);
      }
      break;
    case SAO_MODE_MERGE:
      {
        sao_blk_param_t* merge_target = merge_list[offset_param->typeIdc];
//        assert(mergeTarget != NULL);

        *offset_param = (*merge_target).offsetParam[component];
      }
      break;
    default:
      {
        printf("Not a supported mode");
//        assert(0);
        exit(-1);
      }
    }
  }
}

//Void TComSampleAdaptiveOffset::offsetBlock(Int component, Int typeIdx, Int* offset  
//										   , Pel* srcBlk, Pel* resBlk, Int srcStride, Int resStride,  Int width, Int height
//										   , Bool isLeftAvail,  Bool isRightAvail, Bool isAboveAvail, Bool isBelowAvail, Bool isAboveLeftAvail, Bool isAboveRightAvail, Bool isBelowLeftAvail, Bool isBelowRightAvail)
void offset_block(henc_thread_t *wpp_thread, int compIdx, int typeIdx, int* offset, int16_t* srcBlk, int16_t* resBlk, int srcStride, int resStride,  int width, int height,
				int isLeftAvail,  int isRightAvail, int isAboveAvail, int isBelowAvail, int isAboveLeftAvail, int isAboveRightAvail, int isBelowLeftAvail, int isBelowRightAvail, int bit_depth)
{

/*	if(m_lineBufWidth != m_maxCUWidth)
	{
		m_lineBufWidth = m_maxCUWidth;

		if (m_signLineBuf1) delete[] m_signLineBuf1; m_signLineBuf1 = NULL;
		m_signLineBuf1 = new Char[m_lineBufWidth+1];

		if (m_signLineBuf2) delete[] m_signLineBuf2; m_signLineBuf2 = NULL;
		m_signLineBuf2 = new Char[m_lineBufWidth+1];
	}
*/
	//int* offsetClip = m_offsetClip[compIdx];

	int x,y, startX, startY, endX, endY, edgeType;
	int firstLineStartX, firstLineEndX, lastLineStartX, lastLineEndX;
	char signLeft, signRight, signDown;

	int16_t* srcLine = srcBlk;
	int16_t* resLine = resBlk;

	switch(typeIdx)
	{
	case SAO_TYPE_EO_0:
		{
			offset += 2;
			startX = isLeftAvail ? 0 : 1;
			endX   = isRightAvail ? width : (width -1);
			for (y=0; y< height; y++)
			{
				int aux = srcLine[startX] - srcLine[startX-1];
				signLeft = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLine[startX] - srcLine[startX-1]];
				for (x=startX; x< endX; x++)
				{
					aux = srcLine[x] - srcLine[x+1];
					signRight = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLine[x] - srcLine[x+1]]; 
					edgeType =  signRight + signLeft;
					signLeft  = -signRight;

					resLine[x] = clip(srcLine[x] + offset[edgeType], 0, 255);//offsetClip[srcLine[x] + offset[edgeType]];
				}
				srcLine  += srcStride;
				resLine += resStride;
			}

		}
		break;
	case SAO_TYPE_EO_90:
		{
			int16_t* signUpLine = wpp_thread->sao_sign_line_buff1;//m_signLineBuf1;
			int16_t* srcLineAbove, *srcLineBelow;
			offset += 2;

			startY = isAboveAvail ? 0 : 1;
			endY   = isBelowAvail ? height : height-1;
			if (!isAboveAvail)
			{
				srcLine += srcStride;
				resLine += resStride;
			}

			srcLineAbove = srcLine- srcStride;
			for (x=0; x< width; x++)
			{
				int aux = srcLine[x] - srcLineAbove[x];
				signUpLine[x] = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLine[x] - srcLineAbove[x]];
			}

			
			for (y=startY; y<endY; y++)
			{
				srcLineBelow = srcLine+ srcStride;

				for (x=0; x< width; x++)
				{
					int aux = srcLine[x] - srcLineBelow[x];
					signDown  = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLine[x] - srcLineBelow[x]]; 
					edgeType = signDown + signUpLine[x];
					signUpLine[x]= -signDown;

					resLine[x] = clip(srcLine[x] + offset[edgeType], 0, 255);//offsetClip[srcLine[x] + offset[edgeType]];
				}
				srcLine += srcStride;
				resLine += resStride;
			}

		}
		break;
	case SAO_TYPE_EO_135:
		{
			int16_t *signUpLine, *signDownLine, *signTmpLine;
			int16_t *srcLineBelow, *srcLineAbove;

			offset += 2;
			signUpLine  = wpp_thread->sao_sign_line_buff1;//m_signLineBuf1;
			signDownLine= wpp_thread->sao_sign_line_buff2;//m_signLineBuf2;

			startX = isLeftAvail ? 0 : 1 ;
			endX   = isRightAvail ? width : (width-1);

			//prepare 2nd line's upper sign
			srcLineBelow= srcLine+ srcStride;
			for (x=startX; x< endX+1; x++)
			{
				int aux = srcLineBelow[x] - srcLine[x- 1];
				signUpLine[x] = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLineBelow[x] - srcLine[x- 1]];
			}

			//1st line
			srcLineAbove= srcLine- srcStride;
			firstLineStartX = isAboveLeftAvail ? 0 : 1;
			firstLineEndX   = isAboveAvail? endX: 1;
			for(x= firstLineStartX; x< firstLineEndX; x++)
			{
				int aux = srcLine[x] - srcLineAbove[x- 1];
				edgeType  =  ((aux==0)?0:(SIGN(aux))) - signUpLine[x+1];//m_sign[srcLine[x] - srcLineAbove[x- 1]] - signUpLine[x+1];
				resLine[x] = clip(srcLine[x] + offset[edgeType], 0, 255);//offsetClip[srcLine[x] + offset[edgeType]];
			}
			srcLine  += srcStride;
			resLine  += resStride;


			//middle lines
			for (y= 1; y< height-1; y++)
			{
				int aux;
				srcLineBelow= srcLine+ srcStride;

				for (x=startX; x<endX; x++)
				{
					aux = srcLine[x] - srcLineBelow[x+ 1];
					signDown = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLine[x] - srcLineBelow[x+ 1]] ;
					edgeType =  signDown + signUpLine[x];
					resLine[x] = clip(srcLine[x] + offset[edgeType], 0, 255);//offsetClip[srcLine[x] + offset[edgeType]];

					signDownLine[x+1] = -signDown; 
				}
				aux = srcLineBelow[startX] - srcLine[startX-1];
				signDownLine[startX] = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLineBelow[startX] - srcLine[startX-1]];

				signTmpLine  = signUpLine;
				signUpLine   = signDownLine;
				signDownLine = signTmpLine;

				srcLine += srcStride;
				resLine += resStride;
			}

			//last line
			srcLineBelow= srcLine+ srcStride;
			lastLineStartX = isBelowAvail ? startX : (width -1);
			lastLineEndX   = isBelowRightAvail ? width : (width -1);
			for(x= lastLineStartX; x< lastLineEndX; x++)
			{
				int aux = srcLine[x] - srcLineBelow[x+ 1];
				edgeType =  ((aux==0)?0:(SIGN(aux))) + signUpLine[x];//m_sign[srcLine[x] - srcLineBelow[x+ 1]] + signUpLine[x];
				resLine[x] = clip(srcLine[x] + offset[edgeType], 0, 255);//offsetClip[srcLine[x] + offset[edgeType]];
			}
		}
		break;
	case SAO_TYPE_EO_45:
		{
			int16_t *signUpLine = wpp_thread->sao_sign_line_buff1+1;//m_signLineBuf1+1;
			int16_t *srcLineBelow, *srcLineAbove;

			offset += 2;
			startX = isLeftAvail ? 0 : 1;
			endX   = isRightAvail ? width : (width -1);

			//prepare 2nd line upper sign
			srcLineBelow= srcLine+ srcStride;
			for (x=startX-1; x< endX; x++)
			{
				int aux = srcLineBelow[x] - srcLine[x+1]; 
				signUpLine[x] = ((aux==0)?0:(SIGN(aux)));//(Char)m_sign[srcLineBelow[x] - srcLine[x+1]];
			}


			//first line
			srcLineAbove= srcLine- srcStride;
			firstLineStartX = isAboveAvail ? startX : (width -1 );
			firstLineEndX   = isAboveRightAvail ? width : (width-1);
			for(x= firstLineStartX; x< firstLineEndX; x++)
			{
				int aux = srcLine[x] - srcLineAbove[x+1];
				edgeType = ((aux==0)?0:(SIGN(aux))) - signUpLine[x-1];//m_sign[srcLine[x] - srcLineAbove[x+1]] -signUpLine[x-1];
				resLine[x] = clip(srcLine[x] + offset[edgeType], 0, 255);//offsetClip[srcLine[x] + offset[edgeType]];
			}
			srcLine += srcStride;
			resLine += resStride;

			//middle lines
			for (y= 1; y< height-1; y++)
			{
				int aux;
				srcLineBelow= srcLine+ srcStride;

				for(x= startX; x< endX; x++)
				{
					aux = srcLine[x] - srcLineBelow[x-1];
					signDown =  (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLine[x] - srcLineBelow[x-1]] ;
					edgeType =  signDown + signUpLine[x];
					resLine[x] = clip(srcLine[x] + offset[edgeType], 0, 255);//offsetClip[srcLine[x] + offset[edgeType]];
					signUpLine[x-1] = -signDown; 
				}
				aux = srcLineBelow[endX-1] - srcLine[endX];
				signUpLine[endX-1] = (aux==0)?0:(SIGN(aux));//(Char)m_sign[srcLineBelow[endX-1] - srcLine[endX]];
				srcLine  += srcStride;
				resLine += resStride;
			}

			//last line
			srcLineBelow= srcLine+ srcStride;
			lastLineStartX = isBelowLeftAvail ? 0 : 1;
			lastLineEndX   = isBelowAvail ? endX : 1;
			for(x= lastLineStartX; x< lastLineEndX; x++)
			{
				int aux = srcLine[x] - srcLineBelow[x-1];
				edgeType = ((aux==0)?0:(SIGN(aux))) + signUpLine[x]; //m_sign[srcLine[x] - srcLineBelow[x-1]] + signUpLine[x];
				resLine[x] = clip(srcLine[x] + offset[edgeType], 0, 255);//offsetClip[srcLine[x] + offset[edgeType]];
			}
		}
		break;
	case SAO_TYPE_BO:
		{
			int shiftBits = bit_depth - NUM_SAO_BO_CLASSES_LOG2;//((compIdx == Y_COMP)?g_bitDepthY:g_bitDepthC)- NUM_SAO_BO_CLASSES_LOG2;
			for (y=0; y< height; y++)
			{
				for (x=0; x< width; x++)
				{
					resLine[x] = clip(srcLine[x] + offset[srcLine[x] >> shiftBits], 0, 255);//offsetClip[ srcLine[x] + offset[srcLine[x] >> shiftBits] ];
				}
				srcLine += srcStride;
				resLine += resStride;
			}
		}
		break;
	default:
		{
			printf("Not a supported SAO types\n");
//			assert(0);
			exit(-1);
		}
	}
}

//Void TComSampleAdaptiveOffset::offsetCTU(Int ctu, TComPicYuv* srcYuv, TComPicYuv* resYuv, SAOBlkParam& saoblkParam, TComPic* pPic)
void sao_offset_ctu(henc_thread_t *wpp_thread, ctu_info_t *ctu, sao_blk_param_t* sao_blk_param)
{
	int y_pos, x_pos, height, width;
	int pic_height = wpp_thread->pict_height[Y_COMP];
	int pic_width = wpp_thread->pict_width[Y_COMP];
	int max_cu_size = wpp_thread->max_cu_size;
	//int isLeftAvail,isRightAvail,isAboveAvail,isBelowAvail,isAboveLeftAvail,isAboveRightAvail,isBelowLeftAvail,isBelowRightAvail;
	int l_available, r_available, t_available, b_available, tl_available, bl_available, tr_available, br_available;
	int component;


	l_available = (ctu->x[Y_COMP]> 0);
	t_available = (ctu->y[Y_COMP]> 0);
	r_available = ((ctu->x[Y_COMP] + ctu->size) < wpp_thread->pict_width[Y_COMP]);
	b_available = ((ctu->y[Y_COMP] + ctu->size) < wpp_thread->pict_height[Y_COMP]);
	tl_available = t_available & l_available;
	tr_available = t_available & r_available;
	bl_available = b_available & l_available;
	br_available = b_available & r_available;

//	wpp_thread->enc_engine->sao_debug_mode[sao_blk_param->offsetParam[Y_COMP].modeIdc]++; 
//	wpp_thread->enc_engine->sao_debug_mode[sao_blk_param->offsetParam[U_COMP].modeIdc]++; 
//	wpp_thread->enc_engine->sao_debug_mode[sao_blk_param->offsetParam[V_COMP].modeIdc]++; 

	if((sao_blk_param->offsetParam[Y_COMP].modeIdc == SAO_MODE_OFF) && (sao_blk_param->offsetParam[U_COMP].modeIdc == SAO_MODE_OFF) && (sao_blk_param->offsetParam[V_COMP].modeIdc == SAO_MODE_OFF))
	{
		return;
	}

	//block boundary availability
//	pPic->getPicSym()->deriveLoopFilterBoundaryAvailibility(ctu, isLeftAvail,isRightAvail,isAboveAvail,isBelowAvail,isAboveLeftAvail,isAboveRightAvail,isBelowLeftAvail,isBelowRightAvail);

//	y_pos   = (ctu->ctu_number / m_numCTUInWidth)*m_maxCUHeight;
//	x_pos   = (ctu->ctu_number % m_numCTUInWidth)*m_maxCUWidth;
	y_pos   = ctu->y[Y_COMP];//(ctu->ctu_number / enc_engine->pict_width_in_ctu)*enc_engine->max_cu_size;
	x_pos   = ctu->x[Y_COMP];//(ctu->ctu_number % enc_engine->pict_width_in_ctu)*enc_engine->max_cu_size;

	height = (y_pos + max_cu_size > pic_height)?(pic_height- y_pos):max_cu_size;
	width  = (x_pos + max_cu_size > pic_width)?(pic_width - x_pos):max_cu_size;

	for(component= Y_COMP; component < NUM_PICT_COMPONENTS; component++)
	{
		sao_offset_t* ctb_offset = &sao_blk_param->offsetParam[component];

		if(ctb_offset->modeIdc != SAO_MODE_OFF)
		{
			int isLuma     = (component == Y_COMP);
			int formatShift= isLuma?0:1;

			int  blkWidth   = (width  >> formatShift);
			int  blkHeight  = (height >> formatShift);
			//int  blkYPos    = (y_pos   >> formatShift);
			//int  blkXPos    = (x_pos   >> formatShift);

			int decoded_buff_stride = WND_STRIDE_2D(wpp_thread->enc_engine->curr_reference_frame->img, component);
			int16_t *decoded_buff  = WND_POSITION_2D(int16_t *, wpp_thread->enc_engine->curr_reference_frame->img, component, ctu->x[component], ctu->y[component], 0, wpp_thread->ctu_width);
			int src_buff_stride = WND_STRIDE_2D(wpp_thread->enc_engine->sao_aux_wnd, component);
			int16_t *src_buff = WND_POSITION_2D(int16_t *, wpp_thread->enc_engine->sao_aux_wnd, component, ctu->x[component], ctu->y[component], 0, wpp_thread->ctu_width);

			wpp_thread->enc_engine->sao_debug_type[ctb_offset->typeIdc]++; 
//			int  srcStride = isLuma?srcYuv->getStride():srcYuv->getCStride();
//			uint8_t* srcBlk    = getPicBuf(srcYuv, compIdx)+ (yPos >> formatShift)*srcStride+ (xPos >> formatShift);

//			Int  resStride  = isLuma?resYuv->getStride():resYuv->getCStride();
//			Pel* resBlk     = getPicBuf(resYuv, compIdx)+ blkYPos*resStride+ blkXPos;



/*			offsetBlock( compIdx, ctbOffset.typeIdc, ctbOffset.offset
				, srcBlk, resBlk, srcStride, resStride, blkWidth, blkHeight
				, isLeftAvail, isRightAvail
				, isAboveAvail, isBelowAvail
				, isAboveLeftAvail, isAboveRightAvail
				, isBelowLeftAvail, isBelowRightAvail
				);
*/

			offset_block(wpp_thread, component, ctb_offset->typeIdc, ctb_offset->offset, src_buff, decoded_buff, src_buff_stride,  decoded_buff_stride, blkWidth, blkHeight,
				l_available,  r_available, t_available, b_available, tl_available, tr_available, bl_available, br_available, wpp_thread->bit_depth);
		}
	} //component

}


void sao_decide_blk_params(henc_thread_t *wpp_thread, slice_t *currslice, ctu_info_t *ctu, sao_stat_data_t stats[][NUM_SAO_NEW_TYPES], int *slice_enable)
{
	sao_blk_param_t* merge_list[12]; 
	int is_all_blks_disabled = FALSE;
	sao_blk_param_t mode_param;
	double minCost, modeCost;
	int ctu_idx = ctu->ctu_number;
	int component;
	int num_lcus_for_sao_off[NUM_PICT_COMPONENTS];
	sao_blk_param_t *coded_params = &ctu->coded_params;
	sao_blk_param_t *recon_params = &ctu->recon_params;

	if(!slice_enable[Y_COMP] && !slice_enable[U_COMP] && !slice_enable[V_COMP])
	{
		is_all_blks_disabled = TRUE;
	}

	//  m_pcRDGoOnSbacCoder->load(m_pppcRDSbacCoder[ SAO_CABACSTATE_PIC_INIT ]);

//	for(ctu_idx=0; ctu_idx< enc_engine->pict_total_ctu; ctu_idx++)
	{
		int mode;
		int merge_list_size;
		if(is_all_blks_disabled)
		{
			sao_blk_param_t *cp = coded_params;
			for(component=0; component< 3; component++)
			{
				cp->offsetParam[component].modeIdc = SAO_MODE_OFF;
				cp->offsetParam[component].typeIdc = -1;
				cp->offsetParam[component].typeAuxInfo = -1;
				memset(cp->offsetParam[component].offset, 0, sizeof(cp->offsetParam[component].offset));
			}
			return;
		}

//		get_merge_list(enc_engine, ctu_idx, recon_params, merge_list, &merge_list_size);
		sao_get_merge_list(wpp_thread, ctu, merge_list, &merge_list_size);

		minCost = MAX_COST;
		for(mode=0; mode < NUM_SAO_MODES; mode++)
		{
			switch(mode)
			{
			case SAO_MODE_OFF:
				{
					continue; //not necessary, since all-off case will be tested in SAO_MODE_NEW case.
				}
				break;
			case SAO_MODE_NEW:
				{
					//deriveModeNewRDO(ctu, mergeList, sliceEnabled, blkStats, mode_param, modeCost, m_pppcRDSbacCoder, SAO_CABACSTATE_BLK_CUR);
//					derive_mode_new_rdo(hvenc_engine_t* enc_engine, sao_stat_data_t stats[][NUM_PICT_COMPONENTS][NUM_SAO_NEW_TYPES], sao_blk_param_t *mode_param, int slice_enabled[] )
					sao_derive_mode_new_rdo(wpp_thread, merge_list, merge_list_size, stats, &mode_param, &modeCost, slice_enable);
				}
				break;
			case SAO_MODE_MERGE:
				{
//					deriveModeMergeRDO(ctu, mergeList, sliceEnabled, blkStats , mode_param, modeCost, m_pppcRDSbacCoder, SAO_CABACSTATE_BLK_CUR);
					sao_derive_mode_merge_rdo(wpp_thread, merge_list, merge_list_size, slice_enable, stats, &mode_param, &modeCost);//, TEncSbac** cabacCoderRDO, Int inCabacLabel)
				}
				break;
			default:
				{
					printf("Not a supported SAO mode\n");
					//assert(0);
					exit(-1);
				}
			}

			if(modeCost < minCost)
			{
				minCost = modeCost;
				*coded_params = mode_param;
//				m_pcRDGoOnSbacCoder->store(m_pppcRDSbacCoder[ SAO_CABACSTATE_BLK_NEXT ]);

			}
		} //mode
		wpp_thread->enc_engine->sao_debug_mode[coded_params->offsetParam[Y_COMP].modeIdc]++; 
		wpp_thread->enc_engine->sao_debug_mode[coded_params->offsetParam[U_COMP].modeIdc]++; 
		wpp_thread->enc_engine->sao_debug_mode[coded_params->offsetParam[V_COMP].modeIdc]++; 
//		m_pcRDGoOnSbacCoder->load(m_pppcRDSbacCoder[ SAO_CABACSTATE_BLK_NEXT ]);

		//apply reconstructed offsets
		*recon_params = *coded_params;

		//reconstructBlkSAOParam(reconParams[ctu], mergeList);
		reconstruct_blk_sao_param(recon_params, merge_list, merge_list_size);

//		offsetCTU(ctu, srcYuv, resYuv, reconParams[ctu], pic);
//#ifdef COMPUTE_AS_HM
//		offset_ctu(wpp_thread, ctu, recon_params);
//#endif
	} //ctu

//#if SAO_ENCODING_CHOICE 
/*	num_lcus_for_sao_off[Y_COMP ] = num_lcus_for_sao_off[U_COMP]= num_lcus_for_sao_off[V_COMP]= 0;

	for (component=Y_COMP; component<NUM_PICT_COMPONENTS; component++)
	{
		for(ctu_idx=0; ctu_idx< enc_engine->pict_total_ctu; ctu_idx++)
		{
			if( recon_params->offsetParam[component].modeIdc == SAO_MODE_OFF)
			{
				num_lcus_for_sao_off[component]++;
			}
		}
	}
//#if SAO_ENCODING_CHOICE_CHROMA
	for (component=Y_COMP; component<NUM_PICT_COMPONENTS; component++)
	{
		sao_disabled_rate[component] = (double)num_lcus_for_sao_off[component]/(double)enc_engine->pict_total_ctu;
	}
*/
//#endif

}

extern const uint8_t chroma_scale_conversion_table[];

void hmr_wpp_sao_ctu(henc_thread_t *wpp_thread, slice_t *currslice, ctu_info_t* ctu)
{
#ifndef COMPUTE_AS_HM
#define SHIFT_QP	12
	int		bitdepth_luma_qp_scale = 0;
	double	qp_temp = (double) ctu->qp[0] + bitdepth_luma_qp_scale - SHIFT_QP;//
	double	qp_factor = 0.4624;//this comes from the cfg file of HM
	double	lambda_scale = 1.0 - clip(0.05*(double)(/*enc_engine->mb_interlaced*/0 ? (wpp_thread->enc_engine->gop_size-1)/2 : (wpp_thread->enc_engine->gop_size-1)), 0.0, 0.5);

	if(currslice->slice_type == I_SLICE)
	{
		qp_factor=0.57*lambda_scale;
	}
	wpp_thread->enc_engine->sao_lambdas[0] = qp_factor*pow( 1.4, qp_temp/(1.4));	
	wpp_thread->enc_engine->sao_lambdas[1] =  wpp_thread->enc_engine->sao_lambdas[2] = qp_factor*pow( 1.4, (qp_temp+wpp_thread->enc_engine->chroma_qp_offset)/(1.4));
#endif

	memset(&ctu->recon_params, 0, sizeof(ctu->recon_params));
	memset(&ctu->stat_data[0][0], 0, sizeof(ctu->stat_data));

	wnd_copy_ctu(wpp_thread->funcs->sse_copy_16_16, &wpp_thread->enc_engine->curr_reference_frame->img, &wpp_thread->enc_engine->sao_aux_wnd, ctu);
	reference_picture_border_padding_ctu(&wpp_thread->enc_engine->sao_aux_wnd, ctu);

	wpp_thread->funcs->get_sao_stats(wpp_thread, currslice, ctu, ctu->stat_data);
	sao_decide_blk_params(wpp_thread, currslice, ctu, ctu->stat_data, wpp_thread->enc_engine->slice_enabled);
}

