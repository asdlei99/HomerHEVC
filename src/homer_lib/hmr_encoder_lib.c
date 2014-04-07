/*****************************************************************************
 * hmr_encoder_lib.c : homerHEVC encoding library
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
#include <stdio.h>
#include	<malloc.h>
#include	<memory.h>
#include	<math.h>
#include	<limits.h>

#include	"hmr_os_primitives.h"
#include 	"homer_hevc_enc_api.h"
#include 	"hmr_common.h"
//#include	"hmr_tables.h"
#include	"hmr_profiler.h"
#include	"hmr_sse42_functions.h"


static const ushort ang_table[] = {0,    2,    5,   9,  13,  17,  21,  26,  32}; 
static const ushort inv_ang_table[] = {0, 4096, 1638, 910, 630, 482, 390, 315, 256}; // (256 * 32) / Angle

#ifdef COMPUTE_METRICS
profiler_t frame_metrics = PROFILER_INIT("frame_metrics");
#endif

#ifdef _TIME_PROFILING_
profiler_t intra_luma = PROFILER_INIT("intra_luma");
profiler_t intra_chroma = PROFILER_INIT("intra_chroma");
profiler_t intra = PROFILER_INIT("intra");
profiler_t cabac = PROFILER_INIT("cabac");
//profiling intra_luma
profiler_t intra_luma_bucle1 = PROFILER_INIT("intra_luma.bucle1");
profiler_t intra_luma_bucle1_sad = PROFILER_INIT("intra_luma.bucle1.sad");
profiler_t intra_luma_bucle2 = PROFILER_INIT("intra_luma.bucle2");
profiler_t intra_luma_bucle3 = PROFILER_INIT("intra_luma.bucle3");
profiler_t intra_luma_generate_prediction = PROFILER_INIT("intra_luma.generate_prediction");
profiler_t intra_luma_predict = PROFILER_INIT("intra_luma_predict");
profiler_t intra_luma_tr = PROFILER_INIT("intra_luma.tr");
profiler_t intra_luma_q = PROFILER_INIT("intra_luma.q");
profiler_t intra_luma_iq = PROFILER_INIT("intra_luma.iq");
profiler_t intra_luma_itr = PROFILER_INIT("intra_luma.itr");
profiler_t intra_luma_recon_ssd = PROFILER_INIT("intra_luma.recon+ssd");
#endif




static int num_scaling_list[NUM_SCALING_MODES]={6,6,6,2};

void *HOMER_enc_init()
{
	int i, size, size_index, list_index, qp;
	hvenc_t* phvenc = (hvenc_t*)calloc(1,sizeof(hvenc_t));
	unsigned short* aux_ptr;
	//int max_width = 1920, max_height = 1080;

	phvenc->num_encoded_frames = 0;
	phvenc->ctu_group_size = MAX_MB_GROUP_SIZE;
	phvenc->blocks_per_macroblock = 6;//420
	phvenc->ctu_width[0] = phvenc->ctu_height[0] = 64;
	phvenc->ctu_width[1] = phvenc->ctu_width[2] = 32;
	phvenc->ctu_height[1] = phvenc->ctu_height[2] = 32;
	phvenc->bit_depth = 8;

	//---------------------------------- general tables ------------------------------------------------
	//for partition order inside a ctu
	phvenc->abs2raster_table = (unsigned short*)calloc (phvenc->ctu_width[0] * phvenc->ctu_height[0], sizeof(unsigned short));//number of elements in CU
	phvenc->raster2abs_table = (unsigned short*)calloc (phvenc->ctu_width[0] * phvenc->ctu_height[0], sizeof(unsigned short));//number of elements in CU
	aux_ptr = phvenc->abs2raster_table;
	create_abs2raster_tables(&phvenc->abs2raster_table, 5, 1, 0);
	phvenc->abs2raster_table = aux_ptr;
	create_raster2abs_tables( phvenc->abs2raster_table, phvenc->raster2abs_table, phvenc->ctu_width[0], phvenc->ctu_height[0], 5);


	size=2;
	for ( i=0; i<MAX_CU_DEPTHS; i++ ) //scan block size (2x2, ....., 128x128)
	{
		phvenc->scan_pyramid[0][i] = (uint*) aligned_alloc (size*size, sizeof(uint));
		phvenc->scan_pyramid[1][i] = (uint*) aligned_alloc (size*size, sizeof(uint));
		phvenc->scan_pyramid[2][i] = (uint*) aligned_alloc (size*size, sizeof(uint));
		phvenc->scan_pyramid[3][i] = (uint*) aligned_alloc (size*size, sizeof(uint));
		init_scan_pyramid( phvenc, phvenc->scan_pyramid[0][i], phvenc->scan_pyramid[1][i], phvenc->scan_pyramid[2][i], phvenc->scan_pyramid[3][i], size, size, i);

		size <<= 1;
	}  

	size=4;
	for ( size_index=0; size_index<NUM_SCALING_MODES; size_index++ )//size_index (4x4,8x8,16x16,32x32)
	{
		int ratio = size/min(NUM_MAX_MATRIX_SIZE,size);
		for ( list_index=0; list_index<num_scaling_list[size_index]; list_index++ )//list_index
		{
			short *quant_def_table = get_default_qtable(size_index, list_index);
			for ( qp=0; qp<NUM_SCALING_REM_LISTS; qp++ )//qp
			{
				phvenc->quant_pyramid[size_index][list_index][qp] = (int*) aligned_alloc (size*size, sizeof(uint));
				phvenc->dequant_pyramid[size_index][list_index][qp] = (int*) aligned_alloc (size*size, sizeof(uint));
				phvenc->scaling_error_pyramid[size_index][list_index][qp] = (double*) aligned_alloc (size*size, sizeof(double));
				init_quant_pyramids( phvenc, phvenc->quant_pyramid[size_index][list_index][qp], phvenc->dequant_pyramid[size_index][list_index][qp], phvenc->scaling_error_pyramid[size_index][list_index][qp],
									quant_def_table, size, size, ratio, min(NUM_MAX_MATRIX_SIZE, size), QUANT_DEFAULT_DC, size_index+2, qp);

//				init_flat_quant_pyramids( phvenc, phvenc->quant_pyramid[size_index][list_index][qp], phvenc->dequant_pyramid[size_index][list_index][qp], phvenc->scaling_error_pyramid[size_index][list_index][qp], size*size, size_index+2, qp);
			}  
		}
		size <<= 1;
	}
	
	//angular intra table
	phvenc->ang_table = (ushort*)calloc (9, sizeof(ushort));//number of elements in CU
	phvenc->inv_ang_table = (ushort*)calloc (9, sizeof(ushort));//number of elements in CU

	memcpy(phvenc->ang_table, ang_table, sizeof(ang_table));
	memcpy(phvenc->inv_ang_table, inv_ang_table, sizeof(inv_ang_table));

	sync_cont_init(&phvenc->input_hmr_container);
	cont_init(&phvenc->output_hmr_container);
	cont_init(&phvenc->cont_empty_reference_wnds);

#ifdef WRITE_REF_FRAMES
	FILE *refs_file = fopen("refs.yuv","wb");
	for(i=0;i<NUM_REFF_WNDS;i++)
	{
		phvenc->ref_wnds[i].out_file = refs_file;
	}
#endif

	phvenc->ctu_info_t = (ctu_info_t*)calloc (MAX_NUM_CTUs, sizeof(ctu_info_t));

	for(i=0;i<MAX_NUM_CTUs;i++)
	{
		//------- ctu encoding info -------
		//cbf
		phvenc->ctu_info_t[i].cbf[Y_COMP] = (byte*)calloc (NUM_PICT_COMPONENTS*MAX_NUM_PARTITIONS, sizeof(byte));
		phvenc->ctu_info_t[i].cbf[CHR_COMP] = phvenc->ctu_info_t[i].cbf[Y_COMP]+MAX_NUM_PARTITIONS;
		phvenc->ctu_info_t[i].cbf[V_COMP] = phvenc->ctu_info_t[i].cbf[U_COMP]+MAX_NUM_PARTITIONS;
		//intra mode
		phvenc->ctu_info_t[i].intra_mode[Y_COMP] = (byte*)calloc (2*MAX_NUM_PARTITIONS, sizeof(byte));
		phvenc->ctu_info_t[i].intra_mode[CHR_COMP] = phvenc->ctu_info_t[i].intra_mode[Y_COMP]+MAX_NUM_PARTITIONS;
//		phvenc->ctu_info_t[i].intra_mode[V_COMP] = phvenc->ctu_info_t[i].intra_mode[U_COMP]+MAX_NUM_PARTITIONS;
		//tr_idx, pred_depth, part_size_type, pred_mode
		phvenc->ctu_info_t[i].tr_idx = (byte*)calloc (MAX_NUM_PARTITIONS, sizeof(byte));
		phvenc->ctu_info_t[i].pred_depth = (byte*)calloc (MAX_NUM_PARTITIONS, sizeof(byte));
		phvenc->ctu_info_t[i].part_size_type = (byte*)calloc (MAX_NUM_PARTITIONS, sizeof(byte));
//		phvenc->ctu_info_t[i].pred_mode = (byte*)calloc (MAX_NUM_PARTITIONS, sizeof(byte));
	}

	phvenc->wfpp_enable = 0;
	phvenc->num_sub_streams = 0;
	phvenc->wfpp_num_threads = 0;

	//------------------------processing element------------------------------------
	//bitstreams
	hmr_bitstream_alloc(&phvenc->slice_bs, 0x8000000);
	hmr_bitstream_alloc(&phvenc->vps_nalu.bs, 256);
	hmr_bitstream_alloc(&phvenc->sps_nalu.bs, 256);
	hmr_bitstream_alloc(&phvenc->pps_nalu.bs, 256);
	for(i=0;i<NUM_OUTPUT_NALUS;i++)
		hmr_bitstream_alloc(&phvenc->slice_nalu_list[i].bs, 0x8000000);

	return phvenc;
}


void HOMER_enc_close(void* h)
{
	hvenc_t* phvenc = (hvenc_t*)h;
	int i;
	int ithreads;
	int size_index;
	if(phvenc->run==1)
	{
		phvenc->run = 0;

		if(phvenc->encoder_thread!=NULL)
			JOINT_THREAD(phvenc->encoder_thread);		
	}

	for(ithreads=0;ithreads<phvenc->wfpp_num_threads;ithreads++)//hasta ahora solo hemos alojado 1
	{
		henc_thread_t* henc_th = phvenc->thread[ithreads];
		if(henc_th==NULL)
			break;

		SEM_DESTROY(henc_th->synchro_signal);
		free(henc_th->partition_info);				

		//----------------------------current thread processing buffers deallocation	-----
		wnd_delete(&henc_th->curr_mbs_wnd);
		
		//alloc processing windows and buffers
		aligned_free(henc_th->adi_pred_buff);
		aligned_free(henc_th->adi_filtered_pred_buff);
		aligned_free(henc_th->top_pred_buff);
		aligned_free(henc_th->left_pred_buff);
		aligned_free(henc_th->bottom_pred_buff);
		aligned_free(henc_th->right_pred_buff);

		wnd_delete(&henc_th->prediction_wnd);
		wnd_delete(&henc_th->residual_wnd);

		for(i=0;i<NUM_QUANT_WNDS;i++)
			wnd_delete(&henc_th->transform_quant_wnd[i]);
		wnd_delete(&henc_th->itransform_iquant_wnd);

		for(i=0;i<NUM_DECODED_WNDS;i++)
			wnd_delete(&henc_th->decoded_mbs_wnd[i]);

		aligned_free(henc_th->pred_aux_buff);
		aligned_free(henc_th->aux_buff);
		aligned_free(henc_th->cabac_aux_buff);

		//alloc buffers to gather and consolidate information
		for(i=0;i<NUM_CBF_BUFFS;i++)
		{
			aligned_free(henc_th->cbf_buffs[Y_COMP][i]);
			aligned_free(henc_th->cbf_buffs[U_COMP][i]);
			aligned_free(henc_th->cbf_buffs[V_COMP][i]);
			aligned_free(henc_th->intra_mode_buffs[Y_COMP][i]);
			aligned_free(henc_th->intra_mode_buffs[U_COMP][i]);
			aligned_free(henc_th->intra_mode_buffs[V_COMP][i]);
			aligned_free(henc_th->tr_idx_buffs[i]);
		}

		aligned_free(henc_th->cbf_buffs_chroma[U_COMP]);
		aligned_free(henc_th->cbf_buffs_chroma[V_COMP]);

		free(henc_th->ctu_rd->part_size_type);
//		free(henc_th->ctu_rd->pred_mode);
		free(henc_th->ctu_rd);

		free(henc_th);
	}

	//-------------------------------------------------------------------------------------------------------
	
	for(i=0;i<NUM_INPUT_FRAMES;i++)
	{
		wnd_delete(&phvenc->input_frames[i].img);
	}


	for(i=0;i<NUM_REFF_WNDS;i++)
	{
		wnd_delete(&phvenc->ref_wnds[i]);
	}

	//delete previous streams
	for(i=0;i<phvenc->num_sub_streams;i++)
		hmr_bitstream_free(&phvenc->aux_bs[i]);

	free(phvenc->aux_bs);
	free(phvenc->sub_streams_entry_point_list);

	//delete enviroments
	for(i=0;i<phvenc->num_ee;i++)
	{
		free(phvenc->ee_list[i]->e_ctx);
		free(phvenc->ee_list[i]->contexts);
		free(phvenc->ee_list[i]->b_ctx);
		phvenc->ee_list[i]->type = EE_INVALID;
	}
	free(phvenc->ee_list);

	//delete previous rd counters
	for(i=0;i<phvenc->num_ec;i++)
	{
		free(phvenc->ec_list[i].e_ctx);phvenc->ec_list[i].e_ctx=NULL;
		free(phvenc->ec_list[i].contexts);phvenc->ec_list[i].contexts=NULL;
		free(phvenc->ec_list[i].b_ctx);phvenc->ec_list[i].b_ctx=NULL;
		phvenc->ec_list[i].type = EE_INVALID;
	}
	free(phvenc->ec_list);

	//---------------------------------------------------------del init -----------------------------------
	free(phvenc->abs2raster_table);
	free(phvenc->raster2abs_table);

	for ( i=0; i<MAX_CU_DEPTHS; i++ ) //scan block size (2x2, ....., 128x128)
	{
		aligned_free(phvenc->scan_pyramid[0][i]);
		aligned_free(phvenc->scan_pyramid[1][i]);
		aligned_free(phvenc->scan_pyramid[2][i]);
		aligned_free(phvenc->scan_pyramid[3][i]);
	}

	for ( size_index=0; size_index<NUM_SCALING_MODES; size_index++ )//size_index (4x4,8x8,16x16,32x32)
	{
		int list_index;
		for ( list_index=0; list_index<num_scaling_list[size_index]; list_index++ )//list_index
		{
			int qp;
			for ( qp=0; qp<NUM_SCALING_REM_LISTS; qp++ )//qp
			{
				aligned_free(phvenc->quant_pyramid[size_index][list_index][qp]);
				aligned_free(phvenc->dequant_pyramid[size_index][list_index][qp]);
				aligned_free(phvenc->scaling_error_pyramid[size_index][list_index][qp]);
			}  
		}
	}
	
	//angular intra table
	free(phvenc->ang_table);
	free(phvenc->inv_ang_table);


	for(i=0;i<phvenc->pict_total_cu;i++)
	{
		free(phvenc->ctu_info_t[i].cbf[Y_COMP]);
		//intra mode
		free(phvenc->ctu_info_t[i].intra_mode[Y_COMP]);
		//tr_idx, pred_depth, part_size_type, pred_mode
		free(phvenc->ctu_info_t[i].tr_idx);
		free(phvenc->ctu_info_t[i].pred_depth);
		free(phvenc->ctu_info_t[i].part_size_type);
//		free(phvenc->ctu_info_t[i].pred_mode);
	}

	free(phvenc->ctu_info_t);

	hmr_bitstream_free(&phvenc->slice_bs);
	hmr_bitstream_free(&phvenc->vps_nalu.bs);
	hmr_bitstream_free(&phvenc->sps_nalu.bs);
	hmr_bitstream_free(&phvenc->pps_nalu.bs);

	for(i=0;i<NUM_OUTPUT_NALUS;i++)
		hmr_bitstream_free(&phvenc->slice_nalu_list[i].bs);

	free(phvenc);
}


void put_frame_to_encode(hvenc_t *ed, unsigned char *picture_t[])
{
	video_frame_t	*p;
	sync_cont_get_empty(ed->input_hmr_container, (void**)&p);

	memcpy(WND_DATA_PTR(byte*, p->img, Y_COMP), picture_t[0], ed->pict_width[0]*ed->pict_height[0]);
	memcpy(WND_DATA_PTR(byte*, p->img, U_COMP), picture_t[1], ed->pict_width[1]*ed->pict_height[1]);
	memcpy(WND_DATA_PTR(byte*, p->img, V_COMP), picture_t[2], ed->pict_width[2]*ed->pict_height[2]);
	sync_cont_put_filled(ed->input_hmr_container, p);
}


void get_frame_to_encode(hvenc_t *ed, video_frame_t **picture_t)
{
	sync_cont_get_filled(ed->input_hmr_container, (void**)picture_t);
}

void put_avaliable_frame(hvenc_t *ed, video_frame_t *picture_t)
{
	sync_cont_put_empty(ed->input_hmr_container, picture_t);
}


static const int pad_unit_x[]={1,2,2,1};
static int pad_unit_y[]={1,2,1,1};

int HOMER_enc_control(void *h, int cmd, void *in)
{
    hvenc_t*  phvenc = (hvenc_t*)h;
	int err=0;
	int i, aux;
	unsigned short* aux_ptr;
	int cpu_info[4];
	int ithreads;

	switch (cmd)
	{
		case HENC_SETCFG :
		{
			HVENC_Cfg *cfg = (HVENC_Cfg *)in;
			int prev_num_sub_streams, prev_num_ee, prev_num_ec;
			unsigned int min_cu_size = phvenc->min_cu_size, min_cu_size_mask;

			if(phvenc->run==1)
			{
				phvenc->run = 0;

				if(phvenc->encoder_thread!=NULL)
					JOINT_THREAD(phvenc->encoder_thread);
			}

			phvenc->performance_mode = clip(cfg->performance_mode,0,2);
			phvenc->rd_mode = clip(cfg->rd_mode,0,2);
			phvenc->ctu_width[0] = phvenc->ctu_height[0] = cfg->cu_size;
			phvenc->ctu_width[1] = phvenc->ctu_width[2] = cfg->cu_size>>1;
			phvenc->ctu_height[1] = phvenc->ctu_height[2] = cfg->cu_size>>1;
			phvenc->pict_qp = cfg->qp;

			phvenc->max_cu_size = cfg->cu_size;//MAX_CU_SIZE;
			phvenc->max_cu_size_shift = 0;//MAX_CU_SIZE_SHIFT;
			while(phvenc->max_cu_size>(1<<phvenc->max_cu_size_shift))phvenc->max_cu_size_shift++;

			phvenc->max_pred_partition_depth = (cfg->max_pred_partition_depth>(phvenc->max_cu_size_shift-MIN_TU_SIZE_SHIFT))?(phvenc->max_cu_size_shift-MIN_TU_SIZE_SHIFT):cfg->max_pred_partition_depth;
			//depth of TU tree 
			phvenc->max_intra_tr_depth = (cfg->max_intra_tr_depth>(phvenc->max_cu_size_shift-MIN_TU_SIZE_SHIFT+1))?(phvenc->max_cu_size_shift-MIN_TU_SIZE_SHIFT+1):cfg->max_intra_tr_depth; 
			phvenc->max_inter_tr_depth = (cfg->max_inter_tr_depth>(phvenc->max_cu_size_shift-MIN_TU_SIZE_SHIFT+1))?(phvenc->max_cu_size_shift-MIN_TU_SIZE_SHIFT+1):cfg->max_inter_tr_depth; 
			phvenc->max_cu_size_shift_chroma = phvenc->max_cu_size_shift-1;
			phvenc->min_tu_size_shift = MIN_TU_SIZE_SHIFT;
			phvenc->max_tu_size_shift = phvenc->max_cu_size_shift<MAX_TU_SIZE_SHIFT?phvenc->max_cu_size_shift:MAX_TU_SIZE_SHIFT;//

			//--------------------------------these are default values for coding cu and tu sizes and herarchy depth-----------------
			phvenc->mincu_mintr_shift_diff = (phvenc->max_cu_size_shift-phvenc->max_pred_partition_depth) - phvenc->min_tu_size_shift;
			phvenc->max_cu_depth = phvenc->max_pred_partition_depth+phvenc->mincu_mintr_shift_diff;
			phvenc->mincu_mintr_shift_diff++;

			phvenc->min_cu_size = phvenc->max_cu_size  >> ( phvenc->max_cu_depth-phvenc->mincu_mintr_shift_diff);
			aux = phvenc->min_cu_size;
			phvenc->min_cu_size_shift = 0;
			while (aux>1)
			{
				aux>>=1;
				phvenc->min_cu_size_shift++;
			}

			phvenc->num_partitions_in_cu_shift = 4;//each partition is a 4x4 square
			phvenc->num_partitions_in_cu = ((phvenc->max_cu_size*phvenc->max_cu_size)>>phvenc->num_partitions_in_cu_shift);

			aux_ptr = phvenc->abs2raster_table;
			create_abs2raster_tables(&phvenc->abs2raster_table, phvenc->max_cu_size_shift-1, 1, 0);
			phvenc->abs2raster_table = aux_ptr;
			create_raster2abs_tables( phvenc->abs2raster_table, phvenc->raster2abs_table, phvenc->ctu_width[0], phvenc->ctu_height[0], phvenc->max_cu_size_shift-1);

			phvenc->profile = cfg->profile;
			phvenc->gop_size = cfg->N;
			phvenc->num_b = (cfg->M-1)>0?(cfg->M-1):0;
			//conformance wnd
			min_cu_size = phvenc->min_cu_size;
			min_cu_size_mask = phvenc->min_cu_size-1;
			phvenc->pad_left = 0;
			if ((cfg->width & min_cu_size_mask) != 0)
				 phvenc->pad_right = (min_cu_size - (cfg->width & min_cu_size_mask));
			else
				phvenc->pad_left = phvenc->pad_right = 0;

			phvenc->pad_top = 0;
			if ((cfg->height & min_cu_size_mask) != 0)
				phvenc->pad_bottom = (min_cu_size - (cfg->height & min_cu_size_mask));
			else
				phvenc->pad_bottom = 0;

			phvenc->conformance_mode = 1;//(0: no conformance, 1:automatic padding
			phvenc->pict_width[0] = cfg->width+phvenc->pad_left + phvenc->pad_right;
			phvenc->pict_height[0] = cfg->height+phvenc->pad_top + phvenc->pad_bottom;
			phvenc->pict_width[1] = phvenc->pict_width[2] = phvenc->pict_width[0]>>1;
			phvenc->pict_height[1] = phvenc->pict_height[2] = phvenc->pict_height[0]>>1;

			sync_cont_reset(phvenc->input_hmr_container);
			for(i=0;i<NUM_INPUT_FRAMES;i++)
			{
				wnd_realloc(&phvenc->input_frames[i].img, phvenc->pict_width[0], phvenc->pict_height[0], 0, 0, sizeof(byte));
				sync_cont_put_empty(phvenc->input_hmr_container, &phvenc->input_frames[i]);
			}

			cont_reset(phvenc->output_hmr_container);

			cont_reset(phvenc->cont_empty_reference_wnds);
			for(i=0;i<NUM_REFF_WNDS;i++)
			{
				wnd_realloc(&phvenc->ref_wnds[i], phvenc->pict_width[0], phvenc->pict_height[0], 0, 0, sizeof(byte));
				cont_put(phvenc->cont_empty_reference_wnds, &phvenc->ref_wnds[i]);
			}

			//------------------------processing elements------------------------------------
			phvenc->wfpp_enable = cfg->wfpp_enable;
			prev_num_sub_streams = phvenc->num_sub_streams;

			//bitstreams (one per ctu line y wfpp)
			phvenc->num_sub_streams = (phvenc->wfpp_enable==0)?1:(phvenc->pict_height[0] + phvenc->ctu_height[0]-1)/phvenc->ctu_height[0];
			if(prev_num_sub_streams!=phvenc->num_sub_streams)
			{

				//delete previous streams
				for(i=0;i<prev_num_sub_streams;i++)
					hmr_bitstream_free(&phvenc->aux_bs[i]);

				if(phvenc->aux_bs!=NULL)
					free(phvenc->aux_bs);
				if(phvenc->sub_streams_entry_point_list)
					free(phvenc->sub_streams_entry_point_list);

				phvenc->sub_streams_entry_point_list = (uint*)calloc (phvenc->num_sub_streams, sizeof(uint));
				//create new streams
				phvenc->aux_bs = (bitstream_t	*)calloc (phvenc->num_sub_streams, sizeof(bitstream_t));
				for(i=0;i<phvenc->num_sub_streams;i++)
					hmr_bitstream_alloc(&phvenc->aux_bs[i], 0x8000000/phvenc->num_sub_streams);
			}

			//encoding enviroments and rd enviroments (one per thread if wfpp)
			phvenc->wfpp_num_threads = (phvenc->wfpp_enable)?((cfg->wfpp_num_threads<=phvenc->num_sub_streams)?cfg->wfpp_num_threads:phvenc->num_sub_streams):1;
			prev_num_ee = phvenc->num_ee;
			prev_num_ec = phvenc->num_ec;
			phvenc->num_ee = (phvenc->wfpp_enable)?2*phvenc->wfpp_num_threads:1;
			phvenc->num_ec = (phvenc->wfpp_enable)?phvenc->wfpp_num_threads:1;

			if(prev_num_ee != phvenc->num_ee)
			{
				//delete previous enviroments
				for(i=0;i<prev_num_ee;i++)
				{
					free(phvenc->ee_list[i]->e_ctx);phvenc->ee_list[i]->e_ctx=NULL;
					free(phvenc->ee_list[i]->contexts);phvenc->ee_list[i]->contexts=NULL;
					free(phvenc->ee_list[i]->b_ctx);phvenc->ee_list[i]->b_ctx=NULL;
					phvenc->ee_list[i]->type = EE_INVALID;
				}
				if(phvenc->ee_list!=NULL)
					free(phvenc->ee_list);

				//create new enviroments
				phvenc->ee_list = (enc_env_t **)calloc (phvenc->num_ee, sizeof(enc_env_t*));
				for(i=0;i<phvenc->num_ee;i++)
				{
					phvenc->ee_list[i] = (enc_env_t *)calloc (1, sizeof(enc_env_t));
					phvenc->ee_list[i]->e_ctx = (entropy_model_t*)calloc(1, sizeof(entropy_model_t)); 	
					phvenc->ee_list[i]->contexts = (context_model_t*)calloc(NUM_CTXs, sizeof(context_model_t));
					phvenc->ee_list[i]->b_ctx = (binary_model_t*)calloc(1, sizeof(binary_model_t));//calloc(NUM_CTXs, sizeof(binary_model_t));
					phvenc->ee_list[i]->type = EE_ENCODER;
					ee_init_contexts(phvenc->ee_list[i]);
					bm_map_funcs(phvenc->ee_list[i]);
				}
			}

			if(prev_num_ec != phvenc->num_ec)
			{
				//delete previous rd counters
				for(i=0;i<prev_num_ec;i++)
				{
					free(phvenc->ec_list[i].e_ctx);phvenc->ec_list[i].e_ctx=NULL;
					free(phvenc->ec_list[i].contexts);phvenc->ec_list[i].contexts=NULL;
					free(phvenc->ec_list[i].b_ctx);phvenc->ec_list[i].b_ctx=NULL;
					phvenc->ec_list[i].type = EE_INVALID;
				}
				if(phvenc->ec_list!=NULL)
					free(phvenc->ec_list);

				//create new rd counters
				phvenc->ec_list = (enc_env_t *)calloc (phvenc->num_ec, sizeof(enc_env_t));

				for(i=0;i<phvenc->num_ec;i++)
				{
					phvenc->ec_list[i].e_ctx = (entropy_model_t*)calloc(1, sizeof(entropy_model_t)); 	
					phvenc->ec_list[i].contexts = (context_model_t*)calloc(NUM_CTXs, sizeof(context_model_t));
					phvenc->ec_list[i].b_ctx = (binary_model_t*)calloc(1, sizeof(binary_model_t));//calloc(NUM_CTXs, sizeof(binary_model_t));
					phvenc->ec_list[i].type = EE_COUNTER;
					ee_init_contexts(&phvenc->ec_list[i]);
					bm_map_funcs(&phvenc->ec_list[i]);

				}
			}

			phvenc->frame_rate = cfg->frame_rate;
			phvenc->pic_interlaced = 0;
			phvenc->mb_interlaced = 0;

			phvenc->bit_depth = 8;

			phvenc->pict_width_in_cu = (phvenc->pict_width[0]>>phvenc->max_cu_size_shift) + ((phvenc->pict_width[0]%phvenc->max_cu_size)!=0);
			phvenc->pict_height_in_cu = (phvenc->pict_height[0]>>phvenc->max_cu_size_shift) + ((phvenc->pict_height[0]%phvenc->max_cu_size)!=0);

			phvenc->pict_total_cu = phvenc->pict_width_in_cu*phvenc->pict_height_in_cu;

			if(phvenc->ctu_info_t!=NULL)
				free(phvenc->ctu_info_t);

			phvenc->ctu_info_t = (ctu_info_t*)calloc (phvenc->pict_total_cu, sizeof(ctu_info_t));

			for(i=0;i<phvenc->pict_total_cu;i++)
			{
				//------- ctu encoding info -------
				//cbf
				phvenc->ctu_info_t[i].cbf[Y_COMP] = (byte*)calloc (NUM_PICT_COMPONENTS*MAX_NUM_PARTITIONS, sizeof(byte));
				phvenc->ctu_info_t[i].cbf[CHR_COMP] = phvenc->ctu_info_t[i].cbf[Y_COMP]+MAX_NUM_PARTITIONS;
				phvenc->ctu_info_t[i].cbf[V_COMP] = phvenc->ctu_info_t[i].cbf[U_COMP]+MAX_NUM_PARTITIONS;
				//intra mode
				phvenc->ctu_info_t[i].intra_mode[Y_COMP] = (byte*)calloc (2*MAX_NUM_PARTITIONS, sizeof(byte));
				phvenc->ctu_info_t[i].intra_mode[CHR_COMP] = phvenc->ctu_info_t[i].intra_mode[Y_COMP]+MAX_NUM_PARTITIONS;
		//		phvenc->ctu_info_t[i].intra_mode[V_COMP] = phvenc->ctu_info_t[i].intra_mode[U_COMP]+MAX_NUM_PARTITIONS;
				//tr_idx, pred_depth, part_size_type, pred_mode
				phvenc->ctu_info_t[i].tr_idx = (byte*)calloc (MAX_NUM_PARTITIONS, sizeof(byte));
				phvenc->ctu_info_t[i].pred_depth = (byte*)calloc (MAX_NUM_PARTITIONS, sizeof(byte));
				phvenc->ctu_info_t[i].part_size_type = (byte*)calloc (MAX_NUM_PARTITIONS, sizeof(byte));
		//		phvenc->ctu_info_t[i].pred_mode = (byte*)calloc (MAX_NUM_PARTITIONS, sizeof(byte));
			}


			phvenc->max_sublayers = 1;//TLayers en HM
			phvenc->max_layers = 1;

#if defined _MSC_VER	//Microsoft code
			__cpuid(cpu_info, 1);
#else		//gcc
			{
				int a,b,c,d;
				__asm__ __volatile__ ("cpuid":	"=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (1));
				cpu_info[0] = a;
				cpu_info[1] = b;
				cpu_info[2] = c;
				cpu_info[3] = d;
			}
#endif
#ifndef COMPUTE_SSE_FUNCS
			cpu_info[2] = 0;
#endif
			if(cpu_info[2] & 0x100000)
			{
				printf("tenemos SSE42!!");

				phvenc->funcs.sad = sse_aligned_sad;
				phvenc->funcs.ssd = sse_aligned_ssd;
				phvenc->funcs.modified_variance = sse_modified_variance;
				phvenc->funcs.predict = sse_aligned_predict;
				phvenc->funcs.reconst = sse_aligned_reconst;
				phvenc->funcs.create_intra_planar_prediction = sse_create_intra_planar_prediction;
				phvenc->funcs.create_intra_angular_prediction = sse_create_intra_angular_prediction;

				phvenc->funcs.quant = sse_aligned_quant;
				phvenc->funcs.inv_quant = sse_aligned_inv_quant;

				phvenc->funcs.transform = sse_transform;
				phvenc->funcs.itransform = sse_itransform;
			}
			else
			{
				phvenc->funcs.sad = sad;
				phvenc->funcs.ssd = ssd;
				phvenc->funcs.modified_variance = modified_variance;
				phvenc->funcs.predict = predict;
				phvenc->funcs.reconst = reconst;


				phvenc->funcs.create_intra_planar_prediction = create_intra_planar_prediction;
				phvenc->funcs.create_intra_angular_prediction = create_intra_angular_prediction;

				phvenc->funcs.quant = quant;
				phvenc->funcs.inv_quant = iquant;

				phvenc->funcs.transform = transform;
				phvenc->funcs.itransform = itransform;
			}


			for(ithreads=0;ithreads<phvenc->wfpp_num_threads;ithreads++)//hasta ahora solo hemos alojado 1
			{
				int depth_aux;
				henc_thread_t* henc_th = (henc_thread_t*)calloc(1, sizeof(henc_thread_t));
//				henc_th = &phvenc->_thread;
				phvenc->thread[ithreads] = henc_th;
				henc_th->ed = phvenc;
				henc_th->index = ithreads;
				henc_th->funcs = &phvenc->funcs;
				henc_th->wfpp_enable = phvenc->wfpp_enable;
				henc_th->wfpp_num_threads = phvenc->wfpp_num_threads;

				SEM_INIT(henc_th->synchro_sem, 0,1000);
				SEM_COPY(henc_th->synchro_signal, henc_th->synchro_sem);
				
				henc_th->vps = &phvenc->vps;
				henc_th->sps = &phvenc->sps;
				henc_th->pps = &phvenc->pps;

				memcpy(henc_th->pict_width, phvenc->pict_width, sizeof(henc_th->pict_width));
				memcpy(henc_th->pict_height, phvenc->pict_height, sizeof(henc_th->pict_height));
				henc_th->pict_width_in_cu = phvenc->pict_width_in_cu; 
				henc_th->pict_height_in_cu = phvenc->pict_height_in_cu;			
				henc_th->pict_total_cu = phvenc->pict_total_cu;

				memcpy(henc_th->ctu_width, phvenc->ctu_width, sizeof(henc_th->ctu_width));
				memcpy(henc_th->ctu_height, phvenc->ctu_height, sizeof(henc_th->ctu_height));
				henc_th->ctu_group_size = phvenc->ctu_group_size;

				henc_th->max_cu_size = phvenc->max_cu_size;
				henc_th->max_cu_size_shift = phvenc->max_cu_size_shift;//log2 del tama�o del CU maximo
				henc_th->max_cu_size_shift_chroma = phvenc->max_cu_size_shift_chroma;//log2 del tama�o del CU maximo
				henc_th->max_intra_tr_depth = phvenc->max_intra_tr_depth;
				henc_th->max_inter_tr_depth = phvenc->max_inter_tr_depth;
				henc_th->max_pred_partition_depth = phvenc->max_pred_partition_depth;//max depth for prediction

				henc_th->num_partitions_in_cu = phvenc->num_partitions_in_cu;
				henc_th->num_partitions_in_cu_shift = phvenc->num_partitions_in_cu_shift;
				henc_th->mincu_mintr_shift_diff = phvenc->mincu_mintr_shift_diff;
				henc_th->max_cu_depth = phvenc->max_cu_depth;
				henc_th->min_cu_size = phvenc->min_cu_size;
				henc_th->min_cu_size_shift = phvenc->min_cu_size_shift;
				henc_th->min_tu_size_shift = phvenc->min_tu_size_shift;
				henc_th->max_tu_size_shift = phvenc->max_tu_size_shift;

				henc_th->profile = phvenc->profile;
				henc_th->bit_depth = phvenc->bit_depth;
				henc_th->performance_mode = phvenc->performance_mode;
				henc_th->rd_mode = phvenc->rd_mode;

				henc_th->partition_depth_start = phvenc->partition_depth_start;
				aux = 1;//el CTU
				for(depth_aux=1;depth_aux<MAX_PARTITION_DEPTH;depth_aux++)//5 = max partition depth 
				{
					aux += (4<<(2*(depth_aux-1)));
				}

				henc_th->partition_info = (cu_partition_info_t*)calloc (aux, sizeof(cu_partition_info_t));
				//init partition list info
				init_partition_info(henc_th, henc_th->partition_info);

				//----------------------------current thread processing buffers allocation	-----
				//alloc processing windows and buffers
				henc_th->adi_size = 2*henc_th->ctu_height[0]+2*henc_th->ctu_width[0]+1;//vecinos de la columna izq y fila superior + la esquina
				henc_th->adi_pred_buff = (short*)aligned_alloc (henc_th->adi_size, sizeof(short));
				henc_th->adi_filtered_pred_buff = (short*)aligned_alloc (henc_th->adi_size, sizeof(short));
				henc_th->top_pred_buff = (short*)aligned_alloc (henc_th->adi_size, sizeof(short));
				henc_th->left_pred_buff = (short*)aligned_alloc (henc_th->adi_size, sizeof(short));
				henc_th->bottom_pred_buff = (short*)aligned_alloc (henc_th->adi_size, sizeof(short));
				henc_th->right_pred_buff = (short*)aligned_alloc (henc_th->adi_size, sizeof(short));


				wnd_alloc(&henc_th->curr_mbs_wnd, henc_th->ctu_group_size*(henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(byte));

				henc_th->pred_aux_buff_size = MAX_CU_SIZE*MAX_CU_SIZE;//tama�o del buffer auxiliar
				henc_th->pred_aux_buff = (short*) aligned_alloc (henc_th->pred_aux_buff_size, sizeof(short));

				wnd_alloc(&henc_th->prediction_wnd, henc_th->ctu_group_size*(henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(byte));
				wnd_alloc(&henc_th->residual_wnd, henc_th->ctu_group_size*(henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(ushort));

				henc_th->aux_buff = (short*) aligned_alloc (MAX_CU_SIZE*MAX_CU_SIZE, sizeof(int));

				for(i=0;i<NUM_QUANT_WNDS;i++)
					wnd_alloc(&henc_th->transform_quant_wnd[i], henc_th->ctu_group_size*(henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(ushort));		

				wnd_alloc(&henc_th->itransform_iquant_wnd, henc_th->ctu_group_size*(henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(ushort));

				for(i=0;i<NUM_DECODED_WNDS;i++)
					wnd_alloc(&henc_th->decoded_mbs_wnd[i], (henc_th->ctu_group_size+1)*(henc_th->ctu_width[0]), henc_th->ctu_height[0]*2, 1, 1, sizeof(byte));

				henc_th->cabac_aux_buff_size = MAX_CU_SIZE*MAX_CU_SIZE;//MAX_TU_SIZE_SHIFT*MAX_TU_SIZE_SHIFT;//tama�o del buffer auxiliar
				henc_th->cabac_aux_buff = (unsigned char*) aligned_alloc (henc_th->cabac_aux_buff_size, sizeof(unsigned char));

				//alloc buffers to gather and consolidate information
				for(i=0;i<NUM_CBF_BUFFS;i++)
				{
					henc_th->cbf_buffs[Y_COMP][i] = (byte*) aligned_alloc (256, sizeof(byte));
					henc_th->cbf_buffs[U_COMP][i] = (byte*) aligned_alloc (256, sizeof(byte));
					henc_th->cbf_buffs[V_COMP][i] = (byte*) aligned_alloc (256, sizeof(byte));
					henc_th->intra_mode_buffs[Y_COMP][i] = (byte*) aligned_alloc (256, sizeof(byte));
					henc_th->intra_mode_buffs[U_COMP][i] = (byte*) aligned_alloc (256, sizeof(byte));
					henc_th->intra_mode_buffs[V_COMP][i] = (byte*) aligned_alloc (256, sizeof(byte));
					henc_th->tr_idx_buffs[i] = (byte*) aligned_alloc (256, sizeof(byte));
				}

				henc_th->cbf_buffs_chroma[U_COMP] = (byte*) aligned_alloc (256, sizeof(byte));
				henc_th->cbf_buffs_chroma[V_COMP] = (byte*) aligned_alloc (256, sizeof(byte));

				henc_th->ctu_rd = (ctu_info_t*)calloc (1, sizeof(ctu_info_t));
				henc_th->ctu_rd->part_size_type = (byte*)calloc (MAX_NUM_PARTITIONS, sizeof(byte));
//				henc_th->ctu_rd->pred_mode = (byte*)calloc (MAX_NUM_PARTITIONS, sizeof(byte));

				henc_th->ee = phvenc->ee_list[2*henc_th->index];
				henc_th->ec = &phvenc->ec_list[henc_th->index];

			}

			//exterchange wait and signal semaphores between sucessive threads
			if(phvenc->wfpp_num_threads==1)
			{
				SEM_COPY(phvenc->thread[0]->synchro_wait, phvenc->thread[0]->synchro_sem);
			}
			else
			{
				for(i=0;i<phvenc->wfpp_num_threads;i++)
				{
					SEM_COPY(phvenc->thread[i]->synchro_wait, phvenc->thread[(i+phvenc->wfpp_num_threads-1)%phvenc->wfpp_num_threads]->synchro_sem);
				}			
			}

			phvenc->ptl.generalPTL.profileIdc = phvenc->profile;

			phvenc->ptl.generalPTL.profileCompatibilityFlag[phvenc->profile] = 1;
			if (phvenc->profile == PROFILE_MAIN)// A PROFILE_MAIN10 can decode PROFILE_MAIN
				phvenc->ptl.generalPTL.profileCompatibilityFlag[PROFILE_MAIN10] = 1;

			if (phvenc->profile == PROFILE_MAIN10 && phvenc->bit_depth == 8)// PROFILE_MAIN10 with 8 bits = PROFILE_MAIN
				phvenc->ptl.generalPTL.profileCompatibilityFlag[PROFILE_MAIN] = 1;

			//profiles and levels
			memset(phvenc->ptl.subLayerProfilePresentFlag, 0, sizeof(phvenc->ptl.subLayerProfilePresentFlag));
			memset(phvenc->ptl.subLayerLevelPresentFlag,   0, sizeof(phvenc->ptl.subLayerLevelPresentFlag  ));

			//reference picture_t lists
			phvenc->num_ref_lists =2;
			phvenc->num_refs_idx_active_list[0] = 4;
			phvenc->num_refs_idx_active_list[1] = 4;

			phvenc->num_short_term_ref_pic_sets = phvenc->num_ref_lists;
			memset(phvenc->ref_pic_set_list,   0, sizeof(phvenc->ref_pic_set_list));
			phvenc->ref_pic_set_index = 0;

			//----------------- start vps ------------------
			phvenc->vps.video_parameter_set_id = 0;
			phvenc->vps.temporal_id_nesting_flag = (phvenc->max_sublayers == 1);
			phvenc->vps.ptl = &phvenc->ptl;

			phvenc->vps.sub_layer_ordering_info_present_flag = 1;
			for(i = 0; i < MAX_TLAYER; i++)
			{
				phvenc->vps.max_num_reorder_pics[i] = 0;
				phvenc->vps.max_dec_pic_buffering[i] = 0;
				phvenc->vps.max_latency_increase[i] = 0;
			}
	
			phvenc->vps.timing_info_present_flag = 0;

			//----------------- end vps ------------------

			//----------------- start sps ------------------
			phvenc->sps.video_parameter_set_id = 0;
			
			phvenc->sps.ptl = &phvenc->ptl;

			phvenc->sps.seq_parameter_set_id = 0;
			phvenc->sps.chroma_format_idc = CHROMA420;

			phvenc->sps.pic_width_in_luma_samples = phvenc->pict_width[0];
			phvenc->sps.pic_height_in_luma_samples = phvenc->pict_height[0];
			phvenc->sps.conformance_window_flag = phvenc->conformance_mode!=0;
			phvenc->sps.conf_win_left_offset = phvenc->pad_left/pad_unit_x[phvenc->sps.chroma_format_idc];
			phvenc->sps.conf_win_right_offset = phvenc->pad_right/pad_unit_x[phvenc->sps.chroma_format_idc];
			phvenc->sps.conf_win_top_offset = phvenc->pad_top/pad_unit_y[phvenc->sps.chroma_format_idc];
			phvenc->sps.conf_win_bottom_offset = phvenc->pad_bottom/pad_unit_y[phvenc->sps.chroma_format_idc];
			phvenc->sps.bit_depth_luma_minus8 = phvenc->sps.bit_depth_chroma_minus8 = phvenc->bit_depth-8;
			phvenc->sps.pcm_enabled_flag = 0;
			phvenc->sps.log2_max_pic_order_cnt_lsb_minus4 = 4; //bits for poc=8
			for(i = 0; i < MAX_TLAYER; i++)
			{
				phvenc->sps.max_num_reorder_pics[i] = 0;
				phvenc->sps.max_dec_pic_buffering[i] = 0;
				phvenc->sps.max_latency_increase[i] = 0;
			}

			phvenc->sps.restricted_ref_pic_lists_flag = 1;
			phvenc->sps.lists_modification_present_flag = 0;
			phvenc->sps.log2_min_coding_block_size_minus3 = phvenc->min_cu_size_shift-3;
			phvenc->sps.log2_diff_max_min_coding_block_size = phvenc->max_cu_depth-phvenc->mincu_mintr_shift_diff;//phvenc->num_partitions_in_cu_shift;//-phvenc->min_cu_size_shift;
//			phvenc->sps.log2_diff_max_min_coding_block_size = phvenc->max_cu_size_shift-phvenc->min_cu_size_shift;
			phvenc->sps.log2_min_transform_block_size_minus2 = phvenc->min_tu_size_shift-2;
			phvenc->sps.log2_diff_max_min_transform_block_size = phvenc->max_tu_size_shift - phvenc->min_tu_size_shift;
			phvenc->sps.max_transform_hierarchy_depth_inter = phvenc->max_inter_tr_depth-1;
			phvenc->sps.max_transform_hierarchy_depth_intra = phvenc->max_intra_tr_depth-1;
			phvenc->sps.scaling_list_enabled_flag = 1;
			phvenc->sps.scaling_list_data_present_flag = 0;

			phvenc->sps.amp_enabled_flag = 1;
			phvenc->sps.sample_adaptive_offset_enabled_flag = 0;//cfg->UseSAO;
			phvenc->sps.temporal_id_nesting_flag = (phvenc->max_sublayers == 1);
			phvenc->num_long_term_ref_pic_sets = 0;
			phvenc->sps.temporal_mvp_enable_flag = 1;
			phvenc->sps.strong_intra_smooth_enabled_flag = 1;
			phvenc->sps.vui_parameters_present_flag = 0;
			//----------------- end sps ------------------
			
			//----------------- start pps ------------------
			phvenc->pps.pic_parameter_set_id = 0;
			phvenc->pps.seq_parameter_set_id = phvenc->sps.seq_parameter_set_id;
			phvenc->pps.dependent_slice_enabled_flag = 0;
			phvenc->pps.output_flag_present_flag = 0;
			phvenc->pps.num_extra_slice_header_bits = 0;
			phvenc->pps.sign_data_hiding_flag = cfg->sign_hiding;
			phvenc->pps.cabac_init_present_flag = 1;

			phvenc->pps.pic_init_qp_minus26 = 0;

			phvenc->pps.constrained_intra_pred_flag = 0;
			phvenc->pps.transform_skip_enabled_flag = 0;
			phvenc->pps.cu_qp_delta_enabled_flag = 0;

			phvenc->pps.pic_cb_qp_offset = 0;
			phvenc->pps.pic_cr_qp_offset = 0;

			phvenc->pps.pic_slice_level_chroma_qp_offsets_present_flag = 0;
			phvenc->pps.weighted_pred_flag = 0;
			phvenc->pps.weighted_bipred_flag = 0;
			phvenc->pps.output_flag_present_flag = 0;
			phvenc->pps.transquant_bypass_enable_flag = 0;
			phvenc->pps.tiles_enabled_flag = 0;	
			phvenc->pps.entropy_coded_sync_enabled_flag = phvenc->wfpp_enable;

/*			if(phvenc->pps.tiles_enabled_flag)
				//....................		
*/
			phvenc->pps.loop_filter_across_slices_enabled_flag = 1;
			phvenc->pps.deblocking_filter_control_present_flag = 0;
/*			if(phvenc->pps.deblocking_filter_control_present_flag)
				//.......................
*/			phvenc->pps.pps_scaling_list_data_present_flag = 0;
//			if(phvenc->pps.pps_scaling_list_data_present_flag)
				//.......................
			phvenc->pps.lists_modification_present_flag = 0;
			phvenc->pps.log2_parallel_merge_level_minus2 = 0;
			phvenc->pps.num_extra_slice_header_bits = 0;
			phvenc->pps.slice_header_extension_present_flag = 0;
			//----------------- end pps ------------------

			//----------------- slice ---------------------
			

			//---------------- end slice -------------------
			phvenc->run = 1;
			CREATE_THREAD(phvenc->encoder_thread, encoder_thread, phvenc);
		}	
		break;

	}   

	if(err)     
    	return (FALSE);
	else
		return (TRUE);
}

int get_nal_unit_type(hvenc_t* ed, slice_t *curr_slice, int curr_poc)
{
	if (curr_poc == 0)
	{
		return NALU_CODED_SLICE_IDR_W_RADL;
	}

	return NALU_CODED_SLICE_TRAIL_R;
}



void init_slice(hvenc_t* ed, picture_t *currpict, slice_t *currslice)
{
	currslice->qp =  ed->pict_qp;
	currslice->poc = ed->last_poc;
	currslice->sps = &ed->sps;
	currslice->pps = &ed->pps;
	currslice->slice_index = 0;
	currslice->curr_cu_address = currslice->first_cu_address = 0;
	currslice->last_cu_address = ed->pict_total_cu*ed->num_partitions_in_cu;

	currslice->num_ref_idx[REF_PIC_LIST_0] = ed->num_refs_idx_active_list[REF_PIC_LIST_0];
	currslice->num_ref_idx[REF_PIC_LIST_1] = ed->num_refs_idx_active_list[REF_PIC_LIST_1];
	currslice->slice_temporal_mvp_enable_flag = ed->sps.temporal_mvp_enable_flag;
	currslice->disable_deblocking_filter_flag = 0;//enabled
	currslice->slice_loop_filter_across_slices_enabled_flag = 1;//disabled

	if((currslice->poc%ed->gop_size)==0)
	{
		ed->pict_type = I_SLICE;
		currslice->slice_type = I_SLICE;
		currslice->slice_temporal_layer_non_reference_flag = 1;
		currslice->is_dependent_slice = 0;
		currslice->nalu_type = get_nal_unit_type(ed, currslice, currslice->poc);//NALU_CODED_SLICE_IDR;
		currslice->sublayer = 0;
		currslice->depth = 0;
	}
	if(ed->last_poc == 0)//first image?
	{
//		sublayer_non_reference = 0;		
	}

	switch(currslice->slice_type)
	{
		case I_SLICE:
		{
			currslice->referenced = 1;
		}
		break;
	}

}



void CuGetNeighbors(henc_thread_t* et, ctu_info_t* ctu)
{
	if(ctu->x[Y_COMP]==0)
		ctu->ctu_left = NULL;
	else
	{
		ctu->ctu_left = &et->ed->ctu_info_t[ctu->ctu_number-1];
	}
	ctu->ctu_left_bottom = NULL;//en raster order este no existe. En wavefront si

	if(ctu->y[Y_COMP]==0)
	{
		ctu->ctu_top = NULL;	
		ctu->ctu_top_right = NULL;
	}
	else
	{
		ctu->ctu_top = &et->ed->ctu_info_t[ctu->ctu_number-et->pict_width_in_cu];	

		if(et->cu_current_y==0 || ((et->cu_current_x % et->pict_width_in_cu) == (et->pict_width_in_cu-1)))
			ctu->ctu_top_right = NULL;
		else
		{
			ctu->ctu_top_right = &et->ed->ctu_info_t[ctu->ctu_number-et->pict_width_in_cu+1];
		}
	}
}



int HOMER_enc_write_annex_b_output(nalu_t *nalu_out[], unsigned int num_nalus, encoder_in_out_t *vout)
{
	int nalu_idx, bytes_written=0;
	byte code[4] = {0x0,0x0,0x0,0x1};

	for (nalu_idx=0;nalu_idx<num_nalus;nalu_idx++)
	{
		nalu_t *nalu = nalu_out[nalu_idx];
		uint size = 0;

		if (nalu_idx==0 || nalu->nal_unit_type == NALU_TYPE_SPS || nalu->nal_unit_type == NALU_TYPE_PPS)
		{
			memcpy(&vout->stream.streams[0][bytes_written], code, 4);
			bytes_written += 4;
			size += 4;
		}
		else
		{
			memcpy(&vout->stream.streams[0][bytes_written], code+1, 3);
			bytes_written += 3;
			size += 3;
		}

		//
		memcpy(&vout->stream.streams[0][bytes_written], nalu->bs.bitstream, nalu->bs.streambytecnt);
		bytes_written += nalu->bs.streambytecnt;
		size += nalu->bs.streambytecnt;

	}
	vout->stream.data_size[0] = bytes_written;
	return bytes_written;
}

void copy_ctu(ctu_info_t* src_ctu, ctu_info_t* dst_ctu)
{
	byte *part_size_type = dst_ctu->part_size_type;
	memcpy(dst_ctu, src_ctu, sizeof(ctu_info_t));
	dst_ctu->part_size_type = part_size_type;
}


THREAD_RETURN intra_encode_thread(void *h)
{
	henc_thread_t* et = (henc_thread_t*)h;
	int gcnt=0;
	picture_t *currpict = &et->ed->current_pict;
	slice_t *currslice = &currpict->slice;
	ctu_info_t* ctu;

	//printf("		+intra_encode_thread %d\r\n", et->index);

	et->cu_current = 0;
	et->cu_current_x = 0;
	et->cu_current_y = et->index;

	ctu = &et->ed->ctu_info_t[et->cu_current_y*et->pict_width_in_cu];


	if(et->index==0)
	{
		et->ec = &et->ed->ec_list[0];
		et->ee = et->ed->ee_list[0];
		et->ee->bs = &et->ed->aux_bs[0];
		hmr_bitstream_init(et->ee->bs);

		//resetEntropy
		ee_start_entropy_model(et->ee, currslice->slice_type, currslice->qp);//Init CABAC contexts
		//cabac - reset binary encoder and entropy
		et->ee->ee_start(et->ee->b_ctx);
		et->ee->ee_reset_bits(et->ee->b_ctx);//ee_reset(&ed->ee);
	}

#define  GRAIN					1
#define  GRAIN_MASK				(GRAIN-1)

	while(et->cu_current < et->pict_total_cu)//all ctus loop
	{
		et->cu_current = et->pict_width_in_cu*(et->cu_current_y)+et->cu_current_x;
		et->cu_next = et->cu_current+min(1,et->pict_width_in_cu-et->cu_current_x);

		if(et->cu_current_y > 0 && ((et->cu_current_x & GRAIN_MASK) == 0))
		{
			SEM_WAIT(et->synchro_wait);
		}

		if(et->cu_current_x==0 && et->wfpp_enable)
		{
			if(et->cu_current_y > 0)
			{
				ptrswap(enc_env_t*, et->ed->ee_list[(2*et->index)], et->ed->ee_list[(2*et->index+et->ed->num_ee-1)%et->ed->num_ee]);//get inherited enviroment, leave current as avaliable for the previous thread of the list
			}

			et->ec = &et->ed->ec_list[et->index];
			et->ee = et->ed->ee_list[(2*et->index)];
			et->ee->bs = &et->ed->aux_bs[et->cu_current_y];
			hmr_bitstream_init(et->ee->bs);
			et->ee->ee_start(et->ee->b_ctx);
			et->ee->ee_reset_bits(et->ee->b_ctx);//ee_reset(&ed->ee);
		}


//		for(et->cu_current;et->cu_current<et->cu_next;et->cu_current++)
		{
			//init ctu
			et->curr_ctu_group_info = &et->ed->ctu_info_t[et->cu_current];
			ctu = &et->curr_ctu_group_info[0];
			ctu->ctu_number = et->cu_current;
			ctu->x[Y_COMP] = et->cu_current_x*et->ctu_width[Y_COMP];
			ctu->y[Y_COMP] = et->cu_current_y*et->ctu_height[Y_COMP];
			ctu->x[U_COMP] = ctu->x[V_COMP] = et->cu_current_x*et->ctu_width[U_COMP];
			ctu->y[U_COMP] = ctu->y[V_COMP] = et->cu_current_y*et->ctu_height[U_COMP];
			ctu->size = et->max_cu_size;
			ctu->num_part_in_ctu = et->num_partitions_in_cu;
			ctu->partition_list = &et->partition_info[0];

			CuGetNeighbors(et, ctu);//raster order
			
			//Prepare Memory
			mem_transfer_move_curr_ctu_group(et, et->cu_current_x, et->cu_current_y);	//move MBs from image to currMbWnd
			mem_transfer_intra_refs(et, ctu);//copy left and top info for intra prediction

			copy_ctu(ctu, et->ctu_rd);

			PROFILER_RESET(intra)
			//make ctu intra prediction
			motion_intra(et, ctu, gcnt);
			PROFILER_ACCUMULATE(intra)
			mem_transfer_decoded_blocks(et, ctu);

			if(et->cu_current_x>GRAIN && et->cu_current_y+1 != et->pict_height_in_cu)
			{
				SEM_POST(et->synchro_signal);
			}
			//cabac - encode ctu
			PROFILER_RESET(cabac)
			ctu->coeff_wnd = &et->transform_quant_wnd[0];
			ee_encode_ctu(et, et->ee, currslice, ctu, gcnt);
			PROFILER_ACCUMULATE(cabac)
			et->cu_current_x++;
		}

		if(et->cu_current_x==2 && et->cu_current_y+1 != et->pict_height_in_cu)
		{
			if(et->wfpp_enable)
				ee_copy_entropy_model(et->ee, et->ed->ee_list[(2*et->index+1)%et->ed->num_ee]);
			SEM_POST(et->synchro_signal);
		}

		//notify sinchronization
		if(et->cu_current_x==et->pict_width_in_cu && et->cu_current_y+1 != et->pict_height_in_cu)
		{
			SEM_POST(et->synchro_signal);
		}
		if(et->cu_current_x==et->pict_width_in_cu)
		{
			if(et->wfpp_enable)
				ee_end_slice(et->ee, currslice, ctu);
			et->cu_current_y+=et->wfpp_num_threads;
			et->cu_current_x=0;
		}

		et->cu_current = et->pict_width_in_cu*(et->cu_current_y)+et->cu_current_x;
	}
	
	if(!et->wfpp_enable)
		ee_end_slice(et->ee, currslice, ctu);

	return 0;
}



int HOMER_enc_encode(void* handle, unsigned char *picture_t[])
{
	hvenc_t* ed = (hvenc_t*)handle;
	put_frame_to_encode(ed, picture_t);

	return 0;
}

int HOMER_enc_get_coded_frame(void* handle, nalu_t *nalu_out[], unsigned int *nalu_list_size)
{
	hvenc_t* ed = (hvenc_t*)handle;
	*nalu_list_size = 0;

	if(is_data_avaliable(ed->output_hmr_container))
	{
		nalu_set_t* ouput_nalus;
		cont_get(ed->output_hmr_container, (void**)&ouput_nalus);
		memcpy(nalu_out, ouput_nalus->nalu_list, ouput_nalus->num_nalus*sizeof(ouput_nalus->nalu_list[0]));
		*nalu_list_size = ouput_nalus->num_nalus;
	}

	return 0;
}


#define SYNC_THREAD_CONTEXT(ed, et)									\
		et->rd = ed->rd;

//encodes a frame
THREAD_RETURN encoder_thread(void *h)
{
	hvenc_t* ed = (hvenc_t*)h;
	picture_t *currpict = &ed->current_pict;
	slice_t *currslice = &currpict->slice;
	int n, i;

	while(ed->run)
	{
		nalu_set_t* ouput_nalus = &ed->output_nalus[ed->num_encoded_frames & NUM_OUTPUT_NALUS_MASK];
		int		output_nalu_cnt = 0;
		int		nalu_list_size = NALU_SET_SIZE;
		nalu_t	**output_nalu_list = ouput_nalus->nalu_list;
	

		PROFILER_START(intra)
		PROFILER_START(cabac)
		PROFILER_START(intra_luma)
		PROFILER_START(intra_chroma)
		PROFILER_START(intra_luma_bucle1);//("intra_luma.bucle1");
		PROFILER_START(intra_luma_bucle1_sad)//("intra_luma.bucle1.sad");
		PROFILER_START(intra_luma_bucle2)//("intra_luma.bucle2");
		PROFILER_START(intra_luma_bucle3)//("intra_luma.bucle3");
		PROFILER_START(intra_luma_generate_prediction)//("intra_luma.generate_prediction");
		PROFILER_START(intra_luma_predict)//("intra_luma_predict");
		PROFILER_START(intra_luma_tr)//("intra_luma.tr+q+iq+itr");
		PROFILER_START(intra_luma_q)//("intra_luma.tr+q+iq+itr");
		PROFILER_START(intra_luma_iq)//("intra_luma.tr+q+iq+itr");
		PROFILER_START(intra_luma_itr)//("intra_luma.tr+q+iq+itr");
		PROFILER_START(intra_luma_recon_ssd)//("intra_luma.recon+ssd");

		memset(output_nalu_list, 0, (nalu_list_size)*sizeof(output_nalu_list[0]));

		ed->slice_nalu = &ed->slice_nalu_list[ed->num_encoded_frames & NUM_OUTPUT_NALUS_MASK];
		hmr_bitstream_init(&ed->slice_nalu->bs);

		//get next image
		get_frame_to_encode(ed, &ed->current_pict.img2encode);//get next image to encode and init type

#ifdef COMPUTE_METRICS
		if(ed->num_encoded_frames == 0)
		{
			static char psnr_string[256];
			int str_length;
			ed->accumulated_psnr[0] = ed->accumulated_psnr[1] = ed->accumulated_psnr[2] = 0;
/*			ed->f_psnr = metricsfile;//fopen("homer_metrics.txt","a");
			if(ed->f_psnr)
			{
				str_length = sprintf(psnr_string, "\r\nhomerPSNRY	homerPSNRU	homerPSNRV	homerAPSNRY	homerAPSNRU	homerAPSNRV	homerTOTALBITS	homerAVGBITS	homerTIME(ms)\r\n");
				fwrite(psnr_string, 1, str_length, ed->f_psnr);
				fflush(ed->f_psnr);
			}
*/		}
		profiler_start(&frame_metrics);
#endif

		init_slice(ed, &ed->current_pict, &currpict->slice);
		init_rd(ed);

		//get free img for decodede blocks
		cont_get(ed->cont_empty_reference_wnds,(void**)&ed->curr_ref_wnd);
	
		if(currslice->poc==0)//if(ed->pict_type == I_SLICE)
		{
			hmr_bitstream_init(&ed->vps_nalu.bs);
			hmr_bitstream_init(&ed->sps_nalu.bs);
			hmr_bitstream_init(&ed->pps_nalu.bs);

			ed->vps_nalu.nal_unit_type = NALU_TYPE_VPS;
			ed->vps_nalu.temporal_id = ed->vps_nalu.rsvd_zero_bits = 0;
			output_nalu_list[output_nalu_cnt++] = &ed->vps_nalu;
			hmr_put_vps_header(ed);//vps header

			ed->sps_nalu.nal_unit_type = NALU_TYPE_SPS;
			ed->sps_nalu.temporal_id = ed->sps_nalu.rsvd_zero_bits = 0;
			output_nalu_list[output_nalu_cnt++] = &ed->sps_nalu;
			hmr_put_seq_header(ed);//seq header

			ed->pps_nalu.nal_unit_type = NALU_TYPE_PPS;
			ed->pps_nalu.temporal_id = ed->pps_nalu.rsvd_zero_bits = 0;
			output_nalu_list[output_nalu_cnt++] = &ed->pps_nalu;
			hmr_put_pic_header(ed);//pic header
		}

		//init Entropy Coder Context Models

		ed->last_poc++;
		ed->num_pictures++;

		for(n = 0; n<ed->wfpp_num_threads;n++)
		{
			SYNC_THREAD_CONTEXT(ed, ed->thread[n]);
			//reset remafore
			SEM_RESET(ed->thread[n]->synchro_wait)
		}

		CREATE_THREADS(ed->hthreads, intra_encode_thread, ed->thread, ed->wfpp_num_threads)

		JOINT_THREADS(ed->hthreads, ed->wfpp_num_threads)	

		cont_put(ed->cont_empty_reference_wnds,ed->curr_ref_wnd);

		//slice header
		ed->slice_nalu->nal_unit_type = currslice->nalu_type;
		ed->slice_nalu->temporal_id = ed->slice_nalu->rsvd_zero_bits = 0;
		output_nalu_list[output_nalu_cnt++] = ed->slice_nalu;
		hmr_put_slice_header(ed, currslice);//slice header
		if(ed->wfpp_enable)
			hmr_slice_header_code_wfpp_entry_points(ed);
		hmr_bitstream_rbsp_trailing_bits(&ed->slice_bs);

		for(i=0;i<ed->num_sub_streams;i++)
		{
			memcpy(&ed->slice_bs.bitstream[ed->slice_bs.streambytecnt], ed->aux_bs[i].bitstream, ed->aux_bs[i].streambytecnt);
			ed->slice_bs.streambytecnt += ed->aux_bs[i].streambytecnt;
		}

		//escribimos la nalu
		hmr_bitstream_put_nal_unit_header(&ed->slice_nalu->bs, currslice->nalu_type, 0, 0);
		hmr_bitstream_nalu_ebsp(&ed->slice_bs,&ed->slice_nalu->bs);

		PROFILER_PRINT(intra)
		PROFILER_PRINT(cabac)
		PROFILER_PRINT(intra_luma)
		PROFILER_PRINT(intra_chroma)
		PROFILER_PRINT(intra_luma_bucle1);//("intra_luma.bucle1");
		PROFILER_PRINT(intra_luma_bucle1_sad)//("intra_luma.bucle1.sad");
		PROFILER_PRINT(intra_luma_bucle2)//("intra_luma.bucle2");
		PROFILER_PRINT(intra_luma_bucle3)//("intra_luma.bucle3");
		PROFILER_PRINT(intra_luma_generate_prediction)//("intra_luma.generate_prediction");
		PROFILER_PRINT(intra_luma_predict)//("intra_luma_predict");
		PROFILER_PRINT(intra_luma_tr)//("intra_luma.tr+q+iq+itr");
		PROFILER_PRINT(intra_luma_q)//("intra_luma.tr+q+iq+itr");
		PROFILER_PRINT(intra_luma_iq)//("intra_luma.tr+q+iq+itr");
		PROFILER_PRINT(intra_luma_itr)//("intra_luma.tr+q+iq+itr");
		PROFILER_PRINT(intra_luma_recon_ssd)//("intra_luma.recon+ssd");

#ifdef WRITE_REF_FRAMES
		wnd_write2file(ed->curr_ref_wnd);
#endif
		
		ed->num_encoded_frames++;
#ifdef COMPUTE_METRICS
		{
			char psnr_string[256];
			int str_length;

			profiler_accumulate(&frame_metrics);

			homer_psnr(&ed->current_pict, ed->curr_ref_wnd, ed->pict_width, ed->pict_height, ed->current_psnr); 
			ed->accumulated_psnr[0] += ed->current_psnr[Y_COMP];
			ed->accumulated_psnr[1] += ed->current_psnr[U_COMP];
			ed->accumulated_psnr[2] += ed->current_psnr[V_COMP];
			printf("\r\nPSNRY: %.2f, PSNRU: %.2f,PSNRV: %.2f", ed->current_psnr[Y_COMP], ed->current_psnr[U_COMP], ed->current_psnr[V_COMP]);
			printf("- Average PSNRY: %.2f, PSNRU: %.2f,PSNRV: %.2f\r\n", ed->accumulated_psnr[Y_COMP]/ed->num_encoded_frames, ed->accumulated_psnr[U_COMP]/ed->num_encoded_frames, ed->accumulated_psnr[V_COMP]/ed->num_encoded_frames);
			fflush(stdout);
/*			if(ed->f_psnr)
			{
				int bytes = 0;
				static int totalbytes = 0;
				for(i=0;i<output_nalu_cnt;i++)
					bytes += ouput_nalus->nalu_list[i]->bs.streambytecnt;
				totalbytes+=bytes;

				str_length = sprintf(psnr_string, "\r\n%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%d\t%d\t%.3f", ed->current_psnr[Y_COMP], ed->current_psnr[U_COMP], ed->current_psnr[V_COMP],
																							ed->accumulated_psnr[Y_COMP]/ed->num_encoded_frames, ed->accumulated_psnr[U_COMP]/ed->num_encoded_frames, ed->accumulated_psnr[V_COMP]/ed->num_encoded_frames,
																							bytes*8, totalbytes*8/ed->num_encoded_frames, profiler_get_result(&frame_metrics));


				fwrite(psnr_string, 1, str_length, ed->f_psnr);
				fflush(ed->f_psnr);
			}
*/
		}
#endif
		put_avaliable_frame(ed, ed->current_pict.img2encode);

		ouput_nalus->num_nalus = output_nalu_cnt;
		cont_put(ed->output_hmr_container, ouput_nalus);
	}

	return 0;
}
