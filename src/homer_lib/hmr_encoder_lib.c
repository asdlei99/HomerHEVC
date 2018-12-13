/*****************************************************************************
 * hmr_encoder_lib.c : homerHEVC encoding library
/*****************************************************************************
 * Copyright (C) 2014 homerHEVC project
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
#include <stdio.h>
#include	<memory.h>
#include	<math.h>
#include	<limits.h>

#include	"hmr_os_primitives.h"
#include 	"homer_hevc_enc_api.h"
#include 	"hmr_common.h"
//#include	"hmr_tables.h"
#include	"hmr_profiler.h"
#include	"hmr_sse42_functions.h"


static const uint16_t ang_table[] = {0,    2,    5,   9,  13,  17,  21,  26,  32}; 
static const uint16_t inv_ang_table[] = {0, 4096, 1638, 910, 630, 482, 390, 315, 256}; // (256 * 32) / Angle

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
//	hvenc_engine_t* phvenc = (hvenc_engine_t*)calloc(1,sizeof(hvenc_engine_t));
	hvenc_enc_t* hvenc = (hvenc_enc_t*)calloc(1,sizeof(hvenc_enc_t));
	unsigned short* aux_ptr;
	int cpu_info[4];

	//int max_width = 1920, max_height = 1080;

	printf("\r\n\r\n---------------------------------------------------------------------------------------------\r\n");	
	printf("---------------- HomerHEVC - The Open-Source H265-HEVC encoder under LGPL license -----------\r\n");
	printf("---------------------- see www.homerhevc.com for extended information------------------------\r\n");	
	printf("------------------------- Copyright (C) 2014-2015 homerHEVC project -------------------------\r\n");
	printf("---------------------------- by Juan Casal: jcasal@homerhevc.com ----------------------------\r\n");
	printf("---------------------------------------------------------------------------------------------\r\n\r\n");	


	hvenc->num_encoded_frames = 0;
	hvenc->num_encoder_engines = 0;

//	hvenc->ctu_group_size = MAX_MB_GROUP_SIZE;
	hvenc->ctu_width[0] = hvenc->ctu_height[0] = 64;
	hvenc->ctu_width[1] = hvenc->ctu_width[2] = 32;
	hvenc->ctu_height[1] = hvenc->ctu_height[2] = 32;
	hvenc->bit_depth = 8;

	//---------------------------------- general tables ------------------------------------------------
	//for partition order inside a ctu
	hvenc->abs2raster_table = (unsigned short*)calloc (hvenc->ctu_width[0] * hvenc->ctu_height[0], sizeof(unsigned short));//number of elements in CU
	hvenc->raster2abs_table = (unsigned short*)calloc (hvenc->ctu_width[0] * hvenc->ctu_height[0], sizeof(unsigned short));//number of elements in CU
	aux_ptr = hvenc->abs2raster_table;
	create_abs2raster_tables(&hvenc->abs2raster_table, 5, 1, 0);
	hvenc->abs2raster_table = aux_ptr;
	create_raster2abs_tables( hvenc->abs2raster_table, hvenc->raster2abs_table, hvenc->ctu_width[0], hvenc->ctu_height[0], 5);

	size=2;
	for ( i=0; i<MAX_CU_DEPTHS; i++ ) //scan block size (2x2, ....., 128x128)
	{
		hvenc->scan_pyramid[0][i] = (uint*) hmr_aligned_alloc (size*size, sizeof(uint32_t));
		hvenc->scan_pyramid[1][i] = (uint*) hmr_aligned_alloc (size*size, sizeof(uint32_t));
		hvenc->scan_pyramid[2][i] = (uint*) hmr_aligned_alloc (size*size, sizeof(uint32_t));
		hvenc->scan_pyramid[3][i] = (uint*) hmr_aligned_alloc (size*size, sizeof(uint32_t));
		init_scan_pyramid( hvenc, hvenc->scan_pyramid[0][i], hvenc->scan_pyramid[1][i], hvenc->scan_pyramid[2][i], hvenc->scan_pyramid[3][i], size, size, i);

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
				hvenc->quant_pyramid[size_index][list_index][qp] = (int*) hmr_aligned_alloc (size*size, sizeof(uint32_t));
				hvenc->dequant_pyramid[size_index][list_index][qp] = (int*) hmr_aligned_alloc (size*size, sizeof(uint32_t));
				hvenc->scaling_error_pyramid[size_index][list_index][qp] = (double*) hmr_aligned_alloc (size*size, sizeof(double));
				init_quant_pyramids( hvenc, hvenc->quant_pyramid[size_index][list_index][qp], hvenc->dequant_pyramid[size_index][list_index][qp], hvenc->scaling_error_pyramid[size_index][list_index][qp],
									quant_def_table, size, size, ratio, min(NUM_MAX_MATRIX_SIZE, size), QUANT_DEFAULT_DC, size_index+2, qp);

//				init_flat_quant_pyramids( hvenc, hvenc->quant_pyramid[size_index][list_index][qp], hvenc->dequant_pyramid[size_index][list_index][qp], hvenc->scaling_error_pyramid[size_index][list_index][qp], size*size, size_index+2, qp);
			}  
		}
		size <<= 1;
	}

	for ( qp=0; qp<NUM_SCALING_REM_LISTS; qp++ )//qp
	{
		hvenc->quant_pyramid[SCALING_MODE_32x32][3][qp] = hvenc->quant_pyramid[SCALING_MODE_32x32][1][qp];
		hvenc->dequant_pyramid[SCALING_MODE_32x32][3][qp] = hvenc->dequant_pyramid[SCALING_MODE_32x32][1][qp];
		hvenc->scaling_error_pyramid[SCALING_MODE_32x32][3][qp] = hvenc->scaling_error_pyramid[SCALING_MODE_32x32][1][qp];
	}  

	//angular intra table
	hvenc->ang_table = ang_table;//(ushort*)calloc (9, sizeof(ushort));//number of elements in CU
	hvenc->inv_ang_table = inv_ang_table;//(ushort*)calloc (9, sizeof(ushort));//number of elements in CU

//	memcpy(hvenc->ang_table, ang_table, sizeof(ang_table));
//	memcpy(hvenc->inv_ang_table, inv_ang_table, sizeof(inv_ang_table));

	sync_cont_init(&hvenc->gop_container);
	sync_cont_init(&hvenc->input_hmr_container);
//	cont_init(&hvenc->input_hmr_container);
	cont_init(&hvenc->output_hmr_container);
	cont_init(&hvenc->cont_empty_reference_wnds);

	GET_CPU_ID(cpu_info)
#ifndef COMPUTE_SSE_FUNCS
	cpu_info[2] = 0;
#endif
	if(cpu_info[2] & 0x100000)////
	{
		printf("SSE42 avaliable!!\r\n");

		hvenc->funcs.sse_copy_16_16 = sse_copy_16_16;
		hvenc->funcs.sse_copy_8_16 = sse_copy_8_16;
		hvenc->funcs.sse_copy_16_8 = sse_copy_16_8;
		hvenc->funcs.sad = sse_aligned_sad;
//		hvenc->funcs.ssd = sse_aligned_ssd;
		hvenc->funcs.ssd16b = sse_aligned_ssd16b;
		hvenc->funcs.modified_variance = sse_modified_variance;
		hvenc->funcs.predict = sse_aligned_predict;
		hvenc->funcs.reconst = sse_aligned_reconst;
		hvenc->funcs.create_intra_planar_prediction = sse_create_intra_planar_prediction;
		hvenc->funcs.create_intra_angular_prediction = sse_create_intra_angular_prediction;
				
		hvenc->funcs.interpolate_luma_m_compensation = sse_interpolate_luma;
		hvenc->funcs.interpolate_chroma_m_compensation = sse_interpolate_chroma;
		hvenc->funcs.interpolate_luma_m_estimation = sse_interpolate_luma;//hmr_fake_interpolate_luma;//
		hvenc->funcs.weighted_average_motion = sse_weighted_average_motion;//weighted_average_motion;//

		hvenc->funcs.quant = sse_aligned_quant;
		hvenc->funcs.inv_quant = sse_aligned_inv_quant;

		hvenc->funcs.transform = sse_transform;
		hvenc->funcs.itransform = sse_itransform;
		
		hvenc->funcs.get_sao_stats = sse_sao_get_ctu_stats;		
	}
	else
	{
		hvenc->funcs.sse_copy_16_16 = copy_16_16;
		hvenc->funcs.sse_copy_8_16 = copy_8_16;
		hvenc->funcs.sse_copy_16_8 = copy_16_8;
		hvenc->funcs.sad = sad;
//		hvenc->funcs.ssd = ssd;
		hvenc->funcs.ssd16b = ssd16b;
		hvenc->funcs.modified_variance = modified_variance;
		hvenc->funcs.predict = predict;
		hvenc->funcs.reconst = reconst;
		hvenc->funcs.create_intra_planar_prediction = create_intra_planar_prediction;
		hvenc->funcs.create_intra_angular_prediction = create_intra_angular_prediction;

		hvenc->funcs.interpolate_luma_m_compensation = hmr_interpolate_luma;
		hvenc->funcs.interpolate_chroma_m_compensation = hmr_interpolate_chroma;
		hvenc->funcs.interpolate_luma_m_estimation = hmr_interpolate_luma;
		hvenc->funcs.weighted_average_motion = weighted_average_motion;

		hvenc->funcs.quant = quant;
		hvenc->funcs.inv_quant = iquant;

		hvenc->funcs.transform = transform;
		hvenc->funcs.itransform = itransform;

		hvenc->funcs.get_sao_stats = sao_get_ctu_stats;		
	}

	return hvenc;
}


void gop_reinit(gop_info_t *gop)
{
	gop->gop_size = 0;
	gop->gop_num_b = 0;
	gop->gop_first_poc = 0;
	memset(gop->gop_decode_order_poc, 0, sizeof(gop->gop_decode_order_poc));
	memset(gop->gop_decode_order_poc_inv, 0, sizeof(gop->gop_decode_order_poc_inv));
	memset(gop->gop_decode_order_type, 0, sizeof(gop->gop_decode_order_type));
	memset(gop->gop_decode_order_layer, 0, sizeof(gop->gop_decode_order_layer));
	memset(gop->gop_decode_order_is_reference, 0, sizeof(gop->gop_decode_order_is_reference));

	if(gop->gop_frame_container==NULL)
		sync_cont_init(&gop->gop_frame_container);
	else
		sync_cont_reset(gop->gop_frame_container);
}

void gop_copy_restart(gop_info_t *src_gop, gop_info_t *dst_gop)
{
	dst_gop->gop_size = src_gop->gop_size;
	dst_gop->gop_num_b = src_gop->gop_num_b;
	dst_gop->gop_first_poc = src_gop->gop_first_poc;
	memcpy(dst_gop->gop_decode_order_poc, src_gop->gop_decode_order_poc, sizeof(dst_gop->gop_decode_order_poc));
	memcpy(dst_gop->gop_decode_order_poc_inv, src_gop->gop_decode_order_poc_inv, sizeof(dst_gop->gop_decode_order_poc_inv));
	memcpy(dst_gop->gop_decode_order_type, src_gop->gop_decode_order_type, sizeof(dst_gop->gop_decode_order_type));
	memcpy(dst_gop->gop_decode_order_layer, src_gop->gop_decode_order_layer, sizeof(dst_gop->gop_decode_order_layer));
	memcpy(dst_gop->gop_decode_order_is_reference, src_gop->gop_decode_order_is_reference, sizeof(dst_gop->gop_decode_order_is_reference));

	if(dst_gop->gop_frame_container==NULL)
		sync_cont_init(&dst_gop->gop_frame_container);
	else
		sync_cont_reset(dst_gop->gop_frame_container);
}


void gop_delete(gop_info_t *gop)
{
	sync_cont_delete(gop->gop_frame_container);	
}



void put_frame_to_encode(hvenc_enc_t* enc, encoder_in_out_t* input_frame)
{
	video_frame_t *pic;
	gop_info_t *gop;
	int comp, j;
	wnd_t wnd_src_aux;
	wnd_t *wnd_dst;
	uint32_t current_poc = enc->poc;
	uint32_t img_type;// = input_frame->image_type;

	if(input_frame==NULL)//we are closing
	{
		sync_cont_put_filled(enc->gop_container, NULL);
		return;
	}

	img_type = input_frame->image_type;

	//--insert frame into the input frame list--
	sync_cont_get_empty(enc->input_hmr_container, (void**)&pic);


	pic->temp_info.pts = input_frame->pts;
	pic->img_type = input_frame->image_type;
	pic->temp_info.poc = enc->poc++;

	if((pic->temp_info.poc+1)%9==0)
	{
		int iiiii=0;
	}

	wnd_dst = &pic->img;
	wnd_src_aux.data_width[0] = wnd_dst->data_width[0];
	wnd_src_aux.pix_size = 1;//wnd_dst->pix_size;
	wnd_src_aux.pwnd[Y_COMP] = input_frame->stream.streams[Y_COMP];
	wnd_src_aux.pwnd[U_COMP] = input_frame->stream.streams[U_COMP];
	wnd_src_aux.pwnd[V_COMP] = input_frame->stream.streams[V_COMP];
	wnd_src_aux.data_width[Y_COMP] = wnd_src_aux.window_size_x[Y_COMP] = enc->pict_width[Y_COMP];
	wnd_src_aux.data_width[U_COMP] = wnd_src_aux.window_size_x[U_COMP] = enc->pict_width[U_COMP];
	wnd_src_aux.data_width[V_COMP] = wnd_src_aux.window_size_x[V_COMP] = enc->pict_width[V_COMP];
	wnd_src_aux.data_height[Y_COMP] = wnd_src_aux.window_size_y[Y_COMP] = enc->pict_height[Y_COMP];
	wnd_src_aux.data_height[U_COMP] = wnd_src_aux.window_size_y[U_COMP] = enc->pict_height[U_COMP];
	wnd_src_aux.data_height[V_COMP] = wnd_src_aux.window_size_y[V_COMP] = enc->pict_height[V_COMP];
	wnd_copy(enc->funcs.sse_copy_8_16, &wnd_src_aux, wnd_dst);

	sync_cont_put_filled(enc->input_hmr_container, pic);
	//-----

	//check if there are enough available frames to create a gop to encode
	if(current_poc==0 || 
		(enc->intra_period!=0 && current_poc==(enc->last_intra + enc->intra_period) && img_type == IMAGE_AUTO) || 
		(enc->intra_period==0 && current_poc==0) || img_type == IMAGE_I)
	{
		//this is an I image - create intra gop 
		sync_cont_get_empty(enc->gop_container, (void**)&gop);
		gop_reinit(gop);
		gop->gop_size = 1;//just the I image
		gop->gop_first_poc = pic->temp_info.poc;

		sync_cont_get_filled(enc->input_hmr_container, (void**)&pic);
		pic->img_type = IMAGE_I;
		pic->temp_info.gop_decode_order_idx = 0;
		pic->sublayer = 0;
		pic->is_reference = TRUE;
		enc->last_intra = enc->last_intra = pic->temp_info.poc;
		enc->last_gop_reinit = pic->temp_info.poc;
		sync_cont_put_filled(gop->gop_frame_container, pic);

		sync_cont_put_filled(enc->gop_container, gop);
	}
	else if((current_poc-enc->last_intra)%enc->gop_size == 0)
	{
/*		if((enc->num_b==0 && img_type == IMAGE_AUTO) || img_type == IMAGE_P || (enc->intra_period==0 && enc->num_b==0))//IPPPPPP..
		{
			int n;
			sync_cont_get_empty(enc->gop_container, (void**)&gop);
			gop_reinit(gop);

			gop->gop_size = enc->gop_size;
			gop->gop_num_b = enc->num_b;
			for(n=0;n<gop->gop_size;n++)
			{
				sync_cont_get_filled(enc->input_hmr_container, (void**)&pic);
				pic->img_type = IMAGE_P;
				pic->temp_info.gop_decode_order_idx = n;
				pic->sublayer = 0;

				if(n == 0)
					gop->gop_first_poc = pic->temp_info.poc;
				
				sync_cont_put_filled(gop->gop_frame_container, pic);
			}
			
			sync_cont_put_filled(enc->gop_container, gop);
		}
		else if((enc->num_b!=0 && img_type == IMAGE_AUTO) || img_type == IMAGE_B || enc->intra_period==0)
*/		{
			int n;
			video_frame_t *picts[MAX_GOP_SIZE];
			sync_cont_get_empty(enc->gop_container, (void**)&gop);
			gop_copy_restart(&enc->default_gop, gop);

			gop->gop_size = enc->gop_size;
			gop->gop_num_b = enc->num_b;

			//change frame order from poc order to decode order (2 loops)
			for(n=0;n<gop->gop_size;n++)
			{
//				if(n==gop->gop_size-1)
//					gop->gop_decode_order_poc[n] = 0;
//				else
//					gop->gop_decode_order_poc[n] = n+1;

				sync_cont_get_filled(enc->input_hmr_container, (void**)&picts[gop->gop_decode_order_poc_inv[n]]);

//				if(gop->gop_decode_order_poc[n]==0 && gop->gop_size>gop->gop_num_b)
//					picts[gop->gop_decode_order_poc[n]]->img_type = IMAGE_P;
//				else
//					picts[gop->gop_decode_order_poc[n]]->img_type = IMAGE_B;

				picts[gop->gop_decode_order_poc_inv[n]]->img_type = gop->gop_decode_order_type[gop->gop_decode_order_poc_inv[n]];
				picts[gop->gop_decode_order_poc_inv[n]]->sublayer = gop->gop_decode_order_layer[gop->gop_decode_order_poc_inv[n]];
				picts[gop->gop_decode_order_poc_inv[n]]->is_reference = gop->gop_decode_order_is_reference[gop->gop_decode_order_poc_inv[n]];
				picts[gop->gop_decode_order_poc_inv[n]]->temp_info.gop_decode_order_idx = gop->gop_decode_order_poc_inv[n];
			}
			
			gop->gop_first_poc = current_poc;			
			for(n=0;n<gop->gop_size;n++)
			{
				if(gop->gop_first_poc > picts[n]->temp_info.poc)
					gop->gop_first_poc = picts[n]->temp_info.poc;
				
				sync_cont_put_filled(gop->gop_frame_container, picts[n]);
			}
			
			sync_cont_put_filled(enc->gop_container, gop);
		}
	}
}

int get_frame_to_encode(hvenc_enc_t* enc, video_frame_t **picture)//, int poc)
{
	//get a new gop to encode if needed
	if(enc->input_gop == NULL)
	{
		sync_cont_get_filled(enc->gop_container, (void**)&enc->input_gop);//get new gop to process
	}
	if(enc->input_gop == NULL)// we are closing the encoder
		return FALSE;
	

	//get next frame to encode
	sync_cont_get_filled(enc->input_gop->gop_frame_container, (void**)picture);

	//if gop is finished, make it available
	if(sync_cont_is_empty(enc->input_gop->gop_frame_container))
	{
		sync_cont_put_empty(enc->gop_container, (void*)enc->input_gop);//leave gop available as it is already finished
		enc->input_gop = NULL;
	}

	return (*picture)!=NULL;
}

void put_available_frame(hvenc_enc_t * enc, video_frame_t *picture)
{
	//put frame as avaliable
	sync_cont_put_empty(enc->input_hmr_container, picture);
}


#define HMR_FREE(a) if(a!=NULL)free(a);(a)=NULL;
#define HMR_ALIGNED_FREE(a) if(a!=NULL)hmr_aligned_free(a);(a)=NULL;

void HOMER_enc_close(void* h)
{
	hvenc_enc_t* hvenc = (hvenc_enc_t*)h;
	int i,j;
	int ithreads;
	int size_index;
	int engine;
	if(hvenc->run==TRUE)
	{
		hvenc->run = FALSE;

		if(hvenc->encoder_mod_thread[0]!=NULL)
		{
			int n_mods;
			for(n_mods = 0;n_mods<hvenc->num_encoder_engines;n_mods++)
				sync_cont_put_filled(hvenc->gop_container, NULL);//wake encoder_engine_thread if it is waiting
			JOIN_THREADS(hvenc->encoder_mod_thread, hvenc->num_encoder_engines);
		}
	}

	HMR_FREE(hvenc->ref_pic_set_list)

	//for all encoding_modules
	for(engine = 0;engine<hvenc->num_encoder_engines;engine++)
	{
		hvenc_engine_t* phvenc_engine = hvenc->encoder_engines[engine];
		for(ithreads=0;ithreads<phvenc_engine->wfpp_num_threads;ithreads++)
		{
			henc_thread_t* henc_th = phvenc_engine->thread[ithreads];
			if(henc_th==NULL)
				break;

			HMR_FREE(henc_th->aux_contexts)

			HMR_FREE(henc_th->ctu_rd->merge_idx)
			HMR_FREE(henc_th->ctu_rd->merge)
			HMR_FREE(henc_th->ctu_rd->skipped)
			HMR_FREE(henc_th->ctu_rd->pred_mode)
			HMR_FREE(henc_th->ctu_rd->part_size_type)
			HMR_FREE(henc_th->ctu_rd)
	
			HMR_ALIGNED_FREE(henc_th->cbf_buffs_chroma[V_COMP])
			HMR_ALIGNED_FREE(henc_th->cbf_buffs_chroma[U_COMP])

			for(i=0;i<NUM_CBF_BUFFS;i++)
			{
				HMR_ALIGNED_FREE(henc_th->tr_idx_buffs[i])
				HMR_ALIGNED_FREE(henc_th->intra_mode_buffs[V_COMP][i])
				HMR_ALIGNED_FREE(henc_th->intra_mode_buffs[U_COMP][i])
				HMR_ALIGNED_FREE(henc_th->intra_mode_buffs[Y_COMP][i])
				HMR_ALIGNED_FREE(henc_th->cbf_buffs[V_COMP][i])
				HMR_ALIGNED_FREE(henc_th->cbf_buffs[U_COMP][i])
				HMR_ALIGNED_FREE(henc_th->cbf_buffs[Y_COMP][i])			
			}

			HMR_ALIGNED_FREE(henc_th->cabac_aux_buff)

			for(j=0;j<4;j++)
			{
				wnd_delete(&henc_th->filtered_block_temp_wnd[j]);
				for(i=0;i<4;i++)
				{
					wnd_delete(&henc_th->filtered_block_wnd[j][i]);
				}
			}		

			HMR_ALIGNED_FREE(henc_th->sao_sign_line_buff1);
			HMR_ALIGNED_FREE(henc_th->sao_sign_line_buff2);

			HMR_ALIGNED_FREE(henc_th->deblock_edge_filter[EDGE_HOR])
			HMR_ALIGNED_FREE(henc_th->deblock_edge_filter[EDGE_VER])
			HMR_ALIGNED_FREE(henc_th->deblock_filter_strength_bs[EDGE_HOR])
			HMR_ALIGNED_FREE(henc_th->deblock_filter_strength_bs[EDGE_VER])
//			HMR_FREE(henc_th->deblock_partition_info)

			for(i=0;i<NUM_QUANT_WNDS;i++)
				wnd_delete(&henc_th->transform_quant_wnd_[i]);

			for(i=0;i<NUM_DECODED_WNDS;i++)
				wnd_delete(&henc_th->decoded_mbs_wnd_[i]);

			wnd_delete(&henc_th->itransform_iquant_wnd);

			HMR_ALIGNED_FREE(henc_th->aux_buff)

			wnd_delete(&henc_th->residual_dec_wnd);
			wnd_delete(&henc_th->residual_wnd);
			wnd_delete(&henc_th->prediction_wnd[0]);
			wnd_delete(&henc_th->prediction_wnd[1]);
			wnd_delete(&henc_th->prediction_wnd[2]);

			HMR_ALIGNED_FREE(henc_th->pred_aux_buff)

			wnd_delete(&henc_th->curr_mbs_wnd);

			HMR_ALIGNED_FREE(henc_th->adi_pred_buff)
			HMR_ALIGNED_FREE(henc_th->adi_filtered_pred_buff)
			HMR_ALIGNED_FREE(henc_th->top_pred_buff)
			HMR_ALIGNED_FREE(henc_th->left_pred_buff)
			HMR_ALIGNED_FREE(henc_th->bottom_pred_buff)
			HMR_ALIGNED_FREE(henc_th->right_pred_buff)

//			HMR_FREE(henc_th->partition_info)

			SEM_DESTROY(henc_th->synchro_signal[1]);
			SEM_DESTROY(henc_th->synchro_signal[0]);

			HMR_FREE(henc_th);
		}

		for(i=0;i<phvenc_engine->pict_total_ctu;i++)
		{
			wnd_delete(phvenc_engine->ctu_info[i].coeff_wnd);
			HMR_FREE(phvenc_engine->ctu_info[i].coeff_wnd)
			HMR_FREE(phvenc_engine->ctu_info[i].mv_diff_ref_idx[REF_PIC_LIST_0])
			HMR_FREE(phvenc_engine->ctu_info[i].mv_diff[REF_PIC_LIST_0])
			HMR_FREE(phvenc_engine->ctu_info[i].mv_ref_idx[REF_PIC_LIST_0])
			HMR_FREE(phvenc_engine->ctu_info[i].mv_ref[REF_PIC_LIST_0])
			HMR_FREE(phvenc_engine->ctu_info[i].qp)
			HMR_FREE(phvenc_engine->ctu_info[i].merge_idx)
			HMR_FREE(phvenc_engine->ctu_info[i].merge)
			HMR_FREE(phvenc_engine->ctu_info[i].skipped)
			HMR_FREE(phvenc_engine->ctu_info[i].pred_mode)
			HMR_FREE(phvenc_engine->ctu_info[i].part_size_type)
			HMR_FREE(phvenc_engine->ctu_info[i].pred_depth)
			HMR_FREE(phvenc_engine->ctu_info[i].tr_idx)
			HMR_FREE(phvenc_engine->ctu_info[i].inter_mode)
			HMR_FREE(phvenc_engine->ctu_info[i].intra_mode[Y_COMP])
			HMR_FREE(phvenc_engine->ctu_info[i].cbf[Y_COMP])
			HMR_FREE(phvenc_engine->ctu_info[i].partition_list)
		}

		HMR_FREE(phvenc_engine->ctu_info)

		for(i=0;i<phvenc_engine->num_ec;i++)
		{
			HMR_FREE(phvenc_engine->ec_list[i].b_ctx)
			HMR_FREE(phvenc_engine->ec_list[i].contexts)
			HMR_FREE(phvenc_engine->ec_list[i].e_ctx)
		}
		HMR_FREE(phvenc_engine->ec_list)

		for(i=0;i<phvenc_engine->num_ee;i++)
		{
			HMR_FREE(phvenc_engine->ee_list[i]->b_ctx)
			HMR_FREE(phvenc_engine->ee_list[i]->contexts)
			HMR_FREE(phvenc_engine->ee_list[i]->e_ctx)
			HMR_FREE(phvenc_engine->ee_list[i]);
		}
		HMR_FREE(phvenc_engine->ee_list)

		for(i=0;i<phvenc_engine->num_sub_streams;i++)
			hmr_bitstream_free(&phvenc_engine->aux_bs[i]);
		HMR_FREE(phvenc_engine->aux_bs)

		HMR_FREE(phvenc_engine->sub_streams_entry_point_list)

		for(i=0;i<STREAMS_PER_ENGINE;i++)
			hmr_bitstream_free(&phvenc_engine->slice_nalu_list[i].bs);

		hmr_bitstream_free(&phvenc_engine->slice_bs);

		if(phvenc_engine->output_signal)
			SEM_DESTROY(phvenc_engine->output_signal);

		if(phvenc_engine->input_signal)
			SEM_DESTROY(phvenc_engine->input_signal);


		wnd_delete(&phvenc_engine->sao_aux_wnd);

	//	HMR_FREE(phvenc_engine->ang_table)
	//	HMR_FREE(phvenc_engine->inv_ang_table)

		HMR_FREE(phvenc_engine)
	}
	// end for all modules

	hmr_bitstream_free(&hvenc->aux_bs);
	hmr_bitstream_free(&hvenc->pps_nalu.bs);
	hmr_bitstream_free(&hvenc->sps_nalu.bs);
	hmr_bitstream_free(&hvenc->vps_nalu.bs);

	sync_cont_delete(hvenc->gop_container);
	sync_cont_delete(hvenc->input_hmr_container);
	cont_delete(hvenc->output_hmr_container);
	cont_delete(hvenc->cont_empty_reference_wnds);

	for(i=0;i<2*MAX_NUM_REF;i++)
	{
		wnd_delete(&hvenc->ref_wnds[i].img);
	}

	for(i=0;i<MAX_NUM_INPUT_GOPS;i++)
	{
		gop_delete(&hvenc->input_gops[i]);
	}

	for(i=0;i<NUM_INPUT_FRAMES(hvenc->num_encoder_engines);i++)
	{
		wnd_delete(&hvenc->input_frames[i].img);
	}

	//--------------------------------------------------------HOMER_enc_init-----------------------------------------------------------------------------

	for ( size_index=0; size_index<NUM_SCALING_MODES; size_index++ )
	{
		int list_index;
		for ( list_index=0; list_index<num_scaling_list[size_index]; list_index++ )//list_index
		{
			int qp;
			short *quant_def_table = get_default_qtable(size_index, list_index);
			for ( qp=0; qp<NUM_SCALING_REM_LISTS; qp++ )//qp
			{
				HMR_ALIGNED_FREE(hvenc->scaling_error_pyramid[size_index][list_index][qp])
				HMR_ALIGNED_FREE(hvenc->dequant_pyramid[size_index][list_index][qp])
				HMR_ALIGNED_FREE(hvenc->quant_pyramid[size_index][list_index][qp])
			}  
		}
	}

	for ( i=0; i<MAX_CU_DEPTHS; i++ ) 
	{
		HMR_ALIGNED_FREE(hvenc->scan_pyramid[3][i])
		HMR_ALIGNED_FREE(hvenc->scan_pyramid[2][i])
		HMR_ALIGNED_FREE(hvenc->scan_pyramid[1][i])
		HMR_ALIGNED_FREE(hvenc->scan_pyramid[0][i])
	}  

	HMR_FREE(hvenc->raster2abs_table)
	HMR_FREE(hvenc->abs2raster_table)

//	MUTEX_CLOSE(hvenc->mutex_start_frame);

	HMR_FREE(hvenc)
}



static const int pad_unit_x[]={1,2,2,1};
static int pad_unit_y[]={1,2,1,1};

int HOMER_enc_control(void *h, int cmd, void *in)
{
	hvenc_enc_t* hvenc = (hvenc_enc_t*)h;
	int err=0;
	int i, aux;
	unsigned short* aux_ptr;
	int calculate_preblock_stats = FALSE;//TRUE;
//	int cpu_info[4];

	switch (cmd)
	{
		case HOMER_END:
		{
			int engines;
			if(hvenc->run == TRUE && hvenc->stop == FALSE)
			{
				hvenc->stop = TRUE;
				for(engines=0;engines<hvenc->num_encoder_engines;engines++)
				{
					//we close sending null frames, that will be passed all over the encoder chain
					HOMER_enc_encode(hvenc, NULL);//, nalu_out, &num_nalus);
				}
				JOIN_THREADS(hvenc->encoder_mod_thread, hvenc->num_encoder_engines);			
			}
		}
		break;
		case HOMER_SETCFG :
		{
			HVENC_Cfg *cfg = (HVENC_Cfg *)in;
			int n_enc_engines;
			hvenc_engine_t*  phvenc_engine;
			int num_merge_candidates = 2;
			int bitstream_size = 0x2000000;

#ifdef COMPUTE_AS_HM
				cfg->rd_mode = RD_DIST_ONLY;    //0 only distortion 
				cfg->bitrate_mode = BR_FIXED_QP;//0=fixed qp, 1=cbr (constant bit rate)
				cfg->qp = 32;
				cfg->performance_mode = PERF_FULL_COMPUTATION;//0 full computation(HM)
				cfg->chroma_qp_offset = 0;
				cfg->num_enc_engines = 1;
				cfg->wfpp_num_threads = 1;
				cfg->intra_period = 20;
				num_merge_candidates = MERGE_MVP_MAX_NUM_CANDS;
				cfg->num_enc_engines = 1;
				calculate_preblock_stats = FALSE;
				cfg->reinit_gop_on_scene_change = 0;
#endif

			if(hvenc->run==TRUE)
			{
				hvenc->run = FALSE;

				if(hvenc->encoder_mod_thread[0]!=NULL)//synchronized delete of encoder_engine threads 
				{
					int n_mods;
					for(n_mods = 0;n_mods<hvenc->num_encoder_engines;n_mods++)
						sync_cont_put_filled(hvenc->gop_container, NULL);//wake encoder_engine_thread if it is waiting
					JOIN_THREADS(hvenc->encoder_mod_thread, hvenc->num_encoder_engines);
				}
			}

			//---------------- config restrictions ------------------------------
			cfg->num_b = clip(cfg->num_b,0,1);//now we admit up to 1 b frame
			cfg->gop_size = clip(cfg->gop_size, 1, cfg->num_b+1);
			cfg->intra_period = clip(cfg->intra_period, cfg->gop_size+1, ((cfg->intra_period-1)/cfg->gop_size)*cfg->gop_size + 1);
			cfg->num_ref_frames = (cfg->gop_size==cfg->num_b)?1:clip(cfg->num_ref_frames,0,16);
			cfg->num_enc_engines = min(cfg->num_enc_engines,MAX_NUM_ENCODER_ENGINES);
			//---------------- config restrictions ------------------------------


			hvenc->profile = cfg->profile;

			//---------- to be changed for gop type and reference definition --------------
			hvenc->intra_period = cfg->intra_period;
			hvenc->gop_size = hvenc->intra_period==1?1:min(cfg->gop_size,hvenc->intra_period);
			hvenc->num_b = hvenc->intra_period==1?0:min(cfg->num_b,hvenc->gop_size);
			hvenc->num_ref_frames = hvenc->intra_period==1?0:cfg->num_ref_frames;	
			hvenc->ctu_height[0] = hvenc->ctu_width[0] = cfg->cu_size;
			hvenc->ctu_height[1] = hvenc->ctu_width[1] = hvenc->ctu_height[2] = hvenc->ctu_width[2] = cfg->cu_size>>1;
			hvenc->num_encoder_engines = min(cfg->num_enc_engines,MAX_NUM_ENCODER_ENGINES);

			hvenc->max_sublayers = (hvenc->num_b==0 || hvenc->num_b==hvenc->gop_size)?1:2;//TLayers en HM

			//sao
			sao_init(hvenc->bit_depth);

			//bitstreams
			hmr_bitstream_free(&hvenc->aux_bs);
			hmr_bitstream_alloc(&hvenc->aux_bs, 256);
			hmr_bitstream_free(&hvenc->vps_nalu.bs);
			hmr_bitstream_alloc(&hvenc->vps_nalu.bs, 256);
			hmr_bitstream_free(&hvenc->sps_nalu.bs);
			hmr_bitstream_alloc(&hvenc->sps_nalu.bs, 256);
			hmr_bitstream_free(&hvenc->pps_nalu.bs);
			hmr_bitstream_alloc(&hvenc->pps_nalu.bs, 256);

			//hvenc->num_short_term_ref_pic_sets = hvenc->gop_size+(hvenc->intra_period==1?1:(hvenc->num_b!=0 && hvenc->gop_size>hvenc->num_b)?hvenc->num_ref_frames-1:hvenc->num_ref_frames);//gop size does not mean the same as HM. Here it means numB+1
			hvenc->num_short_term_ref_pic_sets = hvenc->gop_size+(hvenc->intra_period==1?1:hvenc->num_ref_frames);//gop size does not mean the same as HM. Here it means numB+1
			if(hvenc->ref_pic_set_list)
				memset(hvenc->ref_pic_set_list, 0, sizeof(ref_pic_set_t));
			else
				hvenc->ref_pic_set_list = (ref_pic_set_t*)calloc (hvenc->num_short_term_ref_pic_sets, sizeof(ref_pic_set_t));

			//Create default gop struct for frame type and frame poc
			memset(&hvenc->default_gop, 0, sizeof(hvenc->default_gop));
			hvenc->default_gop.gop_size = hvenc->gop_size;
			hvenc->default_gop.gop_num_b = hvenc->num_b;

			//create gop format
			for(i=0;i<hvenc->default_gop.gop_size;i++)
			{
				if(hvenc->default_gop.gop_num_b==0 || hvenc->default_gop.gop_size==hvenc->default_gop.gop_num_b)
				{
					hvenc->default_gop.gop_decode_order_poc[i] = i;
					hvenc->default_gop.gop_decode_order_poc_inv[i] = i;
					if(hvenc->default_gop.gop_num_b==0)
						hvenc->default_gop.gop_decode_order_type[i] = IMAGE_P;
					else
						hvenc->default_gop.gop_decode_order_type[i] = IMAGE_B;
					hvenc->default_gop.gop_decode_order_layer[i] = 0;//P
					hvenc->default_gop.gop_decode_order_layer[i] = 0;//P
					hvenc->default_gop.gop_decode_order_is_reference[i] = TRUE;
				}
				else //if(hvenc->default_gop.gop_num_b>0 && hvenc->default_gop.gop_size>hvenc->default_gop.gop_num_b)
				{
					if(i==hvenc->default_gop.gop_size-1)
					{
						hvenc->default_gop.gop_decode_order_poc[0] = i;//P
						hvenc->default_gop.gop_decode_order_poc_inv[i] = 0;//P
						hvenc->default_gop.gop_decode_order_type[hvenc->default_gop.gop_decode_order_poc_inv[i]] = IMAGE_P;
						hvenc->default_gop.gop_decode_order_layer[hvenc->default_gop.gop_decode_order_poc_inv[i]] = 0;//P
						hvenc->default_gop.gop_decode_order_is_reference[hvenc->default_gop.gop_decode_order_poc_inv[i]] = TRUE;
					}
					else
					{
						hvenc->default_gop.gop_decode_order_poc[i+1] = i;//B
						hvenc->default_gop.gop_decode_order_poc_inv[i] = i+1;//B
						hvenc->default_gop.gop_decode_order_type[hvenc->default_gop.gop_decode_order_poc_inv[i]] = IMAGE_B;
						hvenc->default_gop.gop_decode_order_layer[hvenc->default_gop.gop_decode_order_poc_inv[i]] = 1;//P
						hvenc->default_gop.gop_decode_order_is_reference[hvenc->default_gop.gop_decode_order_poc_inv[i]] = FALSE;
					}
				}
			}

			//TEncTop::xInitRPS
			for(i=0;i<hvenc->num_short_term_ref_pic_sets-1;i++)
			{
				if(hvenc->intra_period==1)
					hvenc->ref_pic_set_list[i].num_negative_pics = hvenc->ref_pic_set_list[i].num_positive_pics = hvenc->ref_pic_set_list[i].inter_ref_pic_set_prediction_flag = 0;
				else if(hvenc->num_b == 0 || hvenc->default_gop.gop_size==hvenc->default_gop.gop_num_b)
				{
					int j;
					if(i==0)
						hvenc->ref_pic_set_list[i].num_negative_pics = hvenc->num_ref_frames;//-(i);
					else
						hvenc->ref_pic_set_list[i].num_negative_pics = i;//hvenc->num_ref_frames-(hvenc->num_ref_frames-i-1);

					for(j=0;j<hvenc->ref_pic_set_list[i].num_negative_pics;j++)
					{
						hvenc->ref_pic_set_list[i].delta_poc_s0[j] = -(j+1);//use the last n pictures
						hvenc->ref_pic_set_list[i].used_by_curr_pic_S0_flag[j] = 1;
					}
					hvenc->ref_pic_set_list[i].num_positive_pics = 0;
					hvenc->ref_pic_set_list[i].inter_ref_pic_set_prediction_flag = 0;
					hvenc->ref_pic_set_list[i].num_pics = hvenc->ref_pic_set_list[i].num_negative_pics + hvenc->ref_pic_set_list[i].num_positive_pics;
				}
				else
				{
					int j;

					if(hvenc->default_gop.gop_decode_order_type[i] == IMAGE_P)
					{
						hvenc->ref_pic_set_list[i].num_negative_pics = hvenc->num_ref_frames;//-(i);
						for(j=0;j<hvenc->ref_pic_set_list[i].num_negative_pics;j++)
						{
							hvenc->ref_pic_set_list[i].delta_poc_s0[j] = -(j*hvenc->default_gop.gop_size + (hvenc->default_gop.gop_num_b+1));//use the last n pictures
							hvenc->ref_pic_set_list[i].used_by_curr_pic_S0_flag[j] = 1;

	//						if(i>1 && ((i)+hvenc->ref_pic_set_list[i].delta_poc_s0[j])<=0)//absPOC = curPOC+m_GOPList[curGOP].m_referencePics[i];//TAppEncCfg line 998 HM-13
	//							hvenc->ref_pic_set_list[i].inter_ref_pic_set_prediction_flag = 1;
						}
						hvenc->ref_pic_set_list[i].num_positive_pics = 0;//-(i);
					}
					else
					{
						hvenc->ref_pic_set_list[i].num_negative_pics = hvenc->num_ref_frames;//-(i);
						hvenc->ref_pic_set_list[i].num_positive_pics = hvenc->num_ref_frames;
							
						for(j=0;j<hvenc->ref_pic_set_list[i].num_negative_pics;j++)
						{
							hvenc->ref_pic_set_list[i].delta_poc_s0[j] = -(j+1);//use the last n pictures
							hvenc->ref_pic_set_list[i].used_by_curr_pic_S0_flag[j] = 1;
						}
						for(j=0;j<hvenc->ref_pic_set_list[i].num_positive_pics;j++)
						{
							hvenc->ref_pic_set_list[i].delta_poc_s0[hvenc->ref_pic_set_list[i].num_negative_pics+j] = (j+1);//use the last n pictures
							hvenc->ref_pic_set_list[i].used_by_curr_pic_S0_flag[hvenc->ref_pic_set_list[i].num_negative_pics+j] = 1;
						}
					}
				}
				hvenc->ref_pic_set_list[i].num_pics = hvenc->ref_pic_set_list[i].num_negative_pics + hvenc->ref_pic_set_list[i].num_positive_pics;
			}

			for(n_enc_engines=0;n_enc_engines<hvenc->num_encoder_engines;n_enc_engines++)
			{
				int ithreads;
				int depth_aux;
				int prev_num_sub_streams, prev_num_ee, prev_num_ec;
				unsigned int min_cu_size, min_cu_size_mask;

				phvenc_engine = (hvenc_engine_t*)calloc(1, sizeof(hvenc_engine_t));
				phvenc_engine->hvenc = hvenc;
				phvenc_engine->index = n_enc_engines;
				hvenc->encoder_engines[n_enc_engines] = phvenc_engine;

				phvenc_engine->frame_rate = cfg->frame_rate;

				phvenc_engine->wfpp_enable = 0;
				phvenc_engine->num_sub_streams = 0;
				phvenc_engine->wfpp_num_threads = 0;
				phvenc_engine->calculate_preblock_stats = calculate_preblock_stats;

				if(phvenc_engine->output_signal!=NULL)
					SEM_DESTROY(phvenc_engine->output_signal);

				if(phvenc_engine->input_signal!=NULL)
					SEM_DESTROY(phvenc_engine->input_signal);

				SEM_INIT(phvenc_engine->output_sem, 0,1000);
				SEM_COPY(phvenc_engine->output_sem, phvenc_engine->output_signal);

				SEM_INIT(phvenc_engine->input_sem, 0,1000);
				SEM_COPY(phvenc_engine->input_sem, phvenc_engine->input_signal);

				phvenc_engine->avg_dist = 1000;
				phvenc_engine->ctu_width[0] = hvenc->ctu_width[0];
				phvenc_engine->ctu_height[0] = hvenc->ctu_height[0];
				phvenc_engine->ctu_width[1] = phvenc_engine->ctu_width[2] = hvenc->ctu_width[1];
				phvenc_engine->ctu_height[1] = phvenc_engine->ctu_height[2] = hvenc->ctu_height[1];

				phvenc_engine->performance_mode = clip(cfg->performance_mode,0,NUM_PERF_MODES-1);

				switch(phvenc_engine->performance_mode)
				{ 
					case PERF_FULL_COMPUTATION:
					{
						phvenc_engine->performance_fast_skip_loop = FALSE;
						phvenc_engine->performance_min_depth = 0;
					}
					break;
					case PERF_FAST_COMPUTATION:
					{
						phvenc_engine->performance_fast_skip_loop = TRUE;
						phvenc_engine->performance_min_depth = 0;
					}
					break;
					case PERF_FASTER_COMPUTATION:
					{
						phvenc_engine->performance_fast_skip_loop = TRUE;
						phvenc_engine->performance_min_depth = 1;
					}
					break;
					case PERF_FASTEST_COMPUTATION:
					{
						phvenc_engine->performance_fast_skip_loop = TRUE;
						phvenc_engine->performance_min_depth = 2;
					}
					break;
				}

				phvenc_engine->rd_mode = clip(cfg->rd_mode,0,NUM_RD_MODES-1);
				phvenc_engine->bitrate_mode = clip(cfg->bitrate_mode,0,NUM_BR_MODES-1);
				phvenc_engine->bitrate = cfg->bitrate;
				if(phvenc_engine->bitrate_mode == BR_VBR)
				{
					phvenc_engine->vbv_size = cfg->vbv_size*20;//phvenc_engine->bitrate*20;//*40;
					phvenc_engine->vbv_init = ((double)cfg->vbv_init/(double)cfg->vbv_size)*phvenc_engine->vbv_size;
//					phvenc_engine->vbv_init = .35*phvenc_engine->vbv_size;
					phvenc_engine->qp_min = 15;
				}
				else
				{
					phvenc_engine->vbv_size = cfg->vbv_size;
					phvenc_engine->vbv_init = cfg->vbv_init;
				}
				if(phvenc_engine->bitrate_mode != BR_FIXED_QP)
					bitstream_size = max(hvenc->intra_period,50)*phvenc_engine->bitrate*1000/(8*phvenc_engine->frame_rate);
				else
					bitstream_size = 0x2000000;

#ifdef COMPUTE_AS_HM
				bitstream_size = 0x4000000;
#endif
				for(i=0;i<STREAMS_PER_ENGINE;i++)
					hmr_bitstream_free(&phvenc_engine->slice_nalu_list[i].bs);

				hmr_bitstream_free(&phvenc_engine->slice_bs);

				//bitstreams
				hmr_bitstream_alloc(&phvenc_engine->slice_bs, bitstream_size);
				for(i=0;i<STREAMS_PER_ENGINE;i++)
					hmr_bitstream_alloc(&phvenc_engine->slice_nalu_list[i].bs, bitstream_size);


				phvenc_engine->qp_depth = 0;//cfg->qp_depth;//if rc enabled qp_depth == 0

				phvenc_engine->pict_qp = cfg->qp;
				phvenc_engine->chroma_qp_offset = cfg->chroma_qp_offset;

				phvenc_engine->max_cu_size = cfg->cu_size;//MAX_CU_SIZE;
				phvenc_engine->max_cu_size_shift = 0;//MAX_CU_SIZE_SHIFT;
				while(phvenc_engine->max_cu_size>(1<<phvenc_engine->max_cu_size_shift))phvenc_engine->max_cu_size_shift++;

				phvenc_engine->max_pred_partition_depth = (cfg->max_pred_partition_depth>(phvenc_engine->max_cu_size_shift-MIN_TU_SIZE_SHIFT))?(phvenc_engine->max_cu_size_shift-MIN_TU_SIZE_SHIFT):cfg->max_pred_partition_depth;
				if(cfg->motion_estimation_precision==PEL)
					phvenc_engine->motion_estimation_precision=MOTION_PEL_MASK;
				else if(cfg->motion_estimation_precision==HALF_PEL)
					phvenc_engine->motion_estimation_precision=MOTION_HALF_PEL_MASK;
				else
					phvenc_engine->motion_estimation_precision = MOTION_QUARTER_PEL_MASK;

				phvenc_engine->num_merge_mvp_candidates = num_merge_candidates;//MERGE_MVP_MAX_NUM_CANDS;
				if(cfg->width%(phvenc_engine->max_cu_size>>(phvenc_engine->max_pred_partition_depth-1)) || cfg->height%(phvenc_engine->max_cu_size>>(phvenc_engine->max_pred_partition_depth-1)))
				{
					printf("HENC_SETCFG Error- size is not multiple of minimum cu size\r\n");
					goto config_error;
				}

				//depth of TU tree 
				phvenc_engine->max_intra_tr_depth = (cfg->max_intra_tr_depth>(phvenc_engine->max_cu_size_shift-MIN_TU_SIZE_SHIFT+1))?(phvenc_engine->max_cu_size_shift-MIN_TU_SIZE_SHIFT+1):cfg->max_intra_tr_depth; 
				phvenc_engine->max_inter_tr_depth = (cfg->max_inter_tr_depth>(phvenc_engine->max_cu_size_shift-MIN_TU_SIZE_SHIFT+1))?(phvenc_engine->max_cu_size_shift-MIN_TU_SIZE_SHIFT+1):cfg->max_inter_tr_depth; 
				phvenc_engine->max_cu_size_shift_chroma = phvenc_engine->max_cu_size_shift-1;
				phvenc_engine->min_tu_size_shift = MIN_TU_SIZE_SHIFT;
				phvenc_engine->max_tu_size_shift = phvenc_engine->max_cu_size_shift<MAX_TU_SIZE_SHIFT?phvenc_engine->max_cu_size_shift:MAX_TU_SIZE_SHIFT;//

				//--------------------------------these are default values for coding cu and tu sizes and herarchy depth-----------------
				phvenc_engine->mincu_mintr_shift_diff = (phvenc_engine->max_cu_size_shift-phvenc_engine->max_pred_partition_depth) - phvenc_engine->min_tu_size_shift;
				phvenc_engine->max_cu_depth = phvenc_engine->max_pred_partition_depth+phvenc_engine->mincu_mintr_shift_diff;
				phvenc_engine->mincu_mintr_shift_diff++;

				min_cu_size = phvenc_engine->max_cu_size  >> ( phvenc_engine->max_cu_depth-phvenc_engine->mincu_mintr_shift_diff);
				min_cu_size_mask = min_cu_size-1;

				phvenc_engine->min_cu_size = min_cu_size;
				aux = phvenc_engine->min_cu_size;
				phvenc_engine->min_cu_size_shift = 0;
				while (aux>1)
				{
					aux>>=1;
					phvenc_engine->min_cu_size_shift++;
				}

				phvenc_engine->num_partitions_in_cu_shift = 4;//each partition is a 4x4 square
				phvenc_engine->num_partitions_in_cu = ((phvenc_engine->max_cu_size*phvenc_engine->max_cu_size)>>phvenc_engine->num_partitions_in_cu_shift);

				phvenc_engine->abs2raster_table = hvenc->abs2raster_table;
				phvenc_engine->raster2abs_table  = hvenc->raster2abs_table;

				phvenc_engine->ang_table = hvenc->ang_table;
				phvenc_engine->inv_ang_table  = hvenc->inv_ang_table;

				phvenc_engine->intra_period = hvenc->intra_period;
				phvenc_engine->last_intra = -cfg->intra_period;
				phvenc_engine->gop_reinit_on_scene_change = cfg->reinit_gop_on_scene_change;
				phvenc_engine->gop_size = hvenc->gop_size;
				phvenc_engine->num_b = hvenc->num_b;
				phvenc_engine->num_ref_frames = hvenc->num_ref_frames;

				//conformance wnd
				phvenc_engine->pad_left = 0;
				if ((cfg->width & min_cu_size_mask) != 0)
					 phvenc_engine->pad_right = (min_cu_size - (cfg->width & min_cu_size_mask));
				else
					phvenc_engine->pad_left = phvenc_engine->pad_right = 0;

				phvenc_engine->pad_top = 0;
				if ((cfg->height & min_cu_size_mask) != 0)
					phvenc_engine->pad_bottom = (min_cu_size - (cfg->height & min_cu_size_mask));
				else
					phvenc_engine->pad_bottom = 0;

				phvenc_engine->conformance_mode = 1;//(0: no conformance, 1:automatic padding
				phvenc_engine->pict_width[0] = cfg->width+phvenc_engine->pad_left + phvenc_engine->pad_right;
				phvenc_engine->pict_height[0] = cfg->height+phvenc_engine->pad_top + phvenc_engine->pad_bottom;
				phvenc_engine->pict_width[1] = phvenc_engine->pict_width[2] = phvenc_engine->pict_width[0]>>1;
				phvenc_engine->pict_height[1] = phvenc_engine->pict_height[2] = phvenc_engine->pict_height[0]>>1;

				//------------------------processing elements------------------------------------
				phvenc_engine->wfpp_enable = cfg->wfpp_enable;
				prev_num_sub_streams = phvenc_engine->num_sub_streams;

				//bitstreams (one per ctu line y wfpp)
				phvenc_engine->num_sub_streams = (phvenc_engine->wfpp_enable==0)?1:(phvenc_engine->pict_height[0] + phvenc_engine->ctu_height[0]-1)/phvenc_engine->ctu_height[0];
				if(prev_num_sub_streams!=phvenc_engine->num_sub_streams)
				{

					//delete previous streams
					for(i=0;i<prev_num_sub_streams;i++)
						hmr_bitstream_free(&phvenc_engine->aux_bs[i]);

					if(phvenc_engine->aux_bs!=NULL)
						free(phvenc_engine->aux_bs);
					if(phvenc_engine->sub_streams_entry_point_list)
						free(phvenc_engine->sub_streams_entry_point_list);

					phvenc_engine->sub_streams_entry_point_list = (uint*)calloc (phvenc_engine->num_sub_streams, sizeof(uint32_t));
					//create new streams
//					phvenc_engine->bc_bs = (bitstream_t	*)calloc (phvenc_engine->num_sub_streams, sizeof(bitstream_t));
					phvenc_engine->aux_bs = (bitstream_t	*)calloc (phvenc_engine->num_sub_streams, sizeof(bitstream_t));
					for(i=0;i<phvenc_engine->num_sub_streams;i++)
						hmr_bitstream_alloc(&phvenc_engine->aux_bs[i], 4*bitstream_size/phvenc_engine->num_sub_streams);
				}

				//encoding enviroments and rd enviroments (one per thread if wfpp)
				phvenc_engine->wfpp_num_threads = (phvenc_engine->wfpp_enable)?((cfg->wfpp_num_threads<=phvenc_engine->num_sub_streams)?cfg->wfpp_num_threads:phvenc_engine->num_sub_streams):1;
				prev_num_ee = phvenc_engine->num_ee;
				prev_num_ec = phvenc_engine->num_ec;
				phvenc_engine->num_ee = (phvenc_engine->wfpp_enable)?2*phvenc_engine->wfpp_num_threads:1;
				phvenc_engine->num_ec = (phvenc_engine->wfpp_enable)?phvenc_engine->wfpp_num_threads:1;

				if(prev_num_ee != phvenc_engine->num_ee)
				{
					//delete previous enviroments
					for(i=0;i<prev_num_ee;i++)
					{
						free(phvenc_engine->ee_list[i]->e_ctx);phvenc_engine->ee_list[i]->e_ctx=NULL;
						free(phvenc_engine->ee_list[i]->contexts);phvenc_engine->ee_list[i]->contexts=NULL;
						free(phvenc_engine->ee_list[i]->b_ctx);phvenc_engine->ee_list[i]->b_ctx=NULL;
						phvenc_engine->ee_list[i]->type = EE_INVALID;
					}
					if(phvenc_engine->ee_list!=NULL)
						free(phvenc_engine->ee_list);

					//create new enviroments
					phvenc_engine->ee_list = (enc_env_t **)calloc (phvenc_engine->num_ee, sizeof(enc_env_t*));
					for(i=0;i<phvenc_engine->num_ee;i++)
					{
						phvenc_engine->ee_list[i] = (enc_env_t *)calloc (1, sizeof(enc_env_t));
						phvenc_engine->ee_list[i]->e_ctx = (entropy_model_t*)calloc(1, sizeof(entropy_model_t)); 	
						phvenc_engine->ee_list[i]->contexts = (context_model_t*)calloc(NUM_CTXs, sizeof(context_model_t));
						phvenc_engine->ee_list[i]->b_ctx = (binary_model_t*)calloc(1, sizeof(binary_model_t));//calloc(NUM_CTXs, sizeof(binary_model_t));
						phvenc_engine->ee_list[i]->type = EE_ENCODER;
						ee_init_contexts(phvenc_engine->ee_list[i]);
						bm_map_funcs(phvenc_engine->ee_list[i]);
					}
				}

				if(prev_num_ec != phvenc_engine->num_ec)
				{
					//delete previous rd counters
					for(i=0;i<prev_num_ec;i++)
					{
						free(phvenc_engine->ec_list[i].e_ctx);phvenc_engine->ec_list[i].e_ctx=NULL;
						free(phvenc_engine->ec_list[i].contexts);phvenc_engine->ec_list[i].contexts=NULL;
						free(phvenc_engine->ec_list[i].b_ctx);phvenc_engine->ec_list[i].b_ctx=NULL;
						phvenc_engine->ec_list[i].type = EE_INVALID;
					}
					if(phvenc_engine->ec_list!=NULL)
						free(phvenc_engine->ec_list);

					//create new rd counters
					phvenc_engine->ec_list = (enc_env_t *)calloc (phvenc_engine->num_ec, sizeof(enc_env_t));

					for(i=0;i<phvenc_engine->num_ec;i++)
					{
						phvenc_engine->ec_list[i].e_ctx = (entropy_model_t*)calloc(1, sizeof(entropy_model_t)); 	
						phvenc_engine->ec_list[i].contexts = (context_model_t*)calloc(NUM_CTXs, sizeof(context_model_t));
						phvenc_engine->ec_list[i].b_ctx = (binary_model_t*)calloc(1, sizeof(binary_model_t));//calloc(NUM_CTXs, sizeof(binary_model_t));
						phvenc_engine->ec_list[i].type = EE_COUNTER;
						ee_init_contexts(&phvenc_engine->ec_list[i]);
						bm_map_funcs(&phvenc_engine->ec_list[i]);
					}
				}
	//			phvenc_engine->pic_interlaced = 0;
	//			phvenc_engine->mb_interlaced = 0;

				phvenc_engine->bit_depth = hvenc->bit_depth;

				phvenc_engine->pict_width_in_ctu = (phvenc_engine->pict_width[Y_COMP]>>phvenc_engine->max_cu_size_shift) + ((phvenc_engine->pict_width[Y_COMP]%phvenc_engine->max_cu_size)!=0);
				phvenc_engine->pict_height_in_ctu = (phvenc_engine->pict_height[Y_COMP]>>phvenc_engine->max_cu_size_shift) + ((phvenc_engine->pict_height[Y_COMP]%phvenc_engine->max_cu_size)!=0);

				phvenc_engine->pict_total_ctu = phvenc_engine->pict_width_in_ctu*phvenc_engine->pict_height_in_ctu;

				if(phvenc_engine->ctu_info!=NULL)
				{
					for(i=0;i<phvenc_engine->pict_total_ctu;i++)
					{
						HMR_FREE(phvenc_engine->ctu_info[i].partition_list)

						HMR_FREE(phvenc_engine->ctu_info[i].cbf[Y_COMP]);
						//intra mode
						HMR_FREE(phvenc_engine->ctu_info[i].intra_mode[Y_COMP]);
						HMR_FREE(phvenc_engine->ctu_info[i].inter_mode);
						//tr_idx, pred_depth, part_size_type, pred_mode
						HMR_FREE(phvenc_engine->ctu_info[i].tr_idx);
						HMR_FREE(phvenc_engine->ctu_info[i].pred_depth);
						HMR_FREE(phvenc_engine->ctu_info[i].part_size_type);
						HMR_FREE(phvenc_engine->ctu_info[i].pred_mode);

						HMR_FREE(phvenc_engine->ctu_info[i].skipped);
						HMR_FREE(phvenc_engine->ctu_info[i].merge);
						HMR_FREE(phvenc_engine->ctu_info[i].merge_idx);
						//inter
						HMR_FREE(phvenc_engine->ctu_info[i].mv_ref[REF_PIC_LIST_0]);
						HMR_FREE(phvenc_engine->ctu_info[i].mv_ref_idx[REF_PIC_LIST_0]);
						HMR_FREE(phvenc_engine->ctu_info[i].mv_diff[REF_PIC_LIST_0]);
						HMR_FREE(phvenc_engine->ctu_info[i].mv_diff_ref_idx[REF_PIC_LIST_0]);
	//					free(phvenc_engine->ctu_info[i].mv_ref1);
					
	//					free(phvenc_engine->ctu_info[i].ref_idx1);
						free(phvenc_engine->ctu_info[i].qp);
					}
					free(phvenc_engine->ctu_info);
				}
				phvenc_engine->ctu_info = (ctu_info_t*)calloc (phvenc_engine->pict_total_ctu, sizeof(ctu_info_t));

				aux = 1;
				for(depth_aux=1;depth_aux<MAX_PARTITION_DEPTH;depth_aux++)//5 = max partition depth 
				{
					aux += (4<<(2*(depth_aux-1)));
				}

				for(i=0;i<phvenc_engine->pict_total_ctu;i++)
				{
					//------- ctu encoding info -------
					phvenc_engine->ctu_info[i].partition_list = (cu_partition_info_t*)calloc (aux, sizeof(cu_partition_info_t));
					//init partition list info
					init_partition_info(phvenc_engine, phvenc_engine->ctu_info[i].partition_list);

					//cbf
					phvenc_engine->ctu_info[i].cbf[Y_COMP] = (uint8_t*)calloc (NUM_PICT_COMPONENTS*MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].cbf[U_COMP] = phvenc_engine->ctu_info[i].cbf[Y_COMP]+MAX_NUM_PARTITIONS;
					phvenc_engine->ctu_info[i].cbf[V_COMP] = phvenc_engine->ctu_info[i].cbf[U_COMP]+MAX_NUM_PARTITIONS;
					//intra mode
					phvenc_engine->ctu_info[i].intra_mode[Y_COMP] = (uint8_t*)calloc (2*MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].intra_mode[CHR_COMP] = phvenc_engine->ctu_info[i].intra_mode[Y_COMP]+MAX_NUM_PARTITIONS;
					//inter mode
					phvenc_engine->ctu_info[i].inter_mode = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					//tr_idx, pred_depth, part_size_type, pred_mode, skipped
					phvenc_engine->ctu_info[i].tr_idx = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].pred_depth = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].part_size_type = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].pred_mode = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].skipped = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].merge = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].merge_idx = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].qp = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));

					//inter
					phvenc_engine->ctu_info[i].mv_ref[REF_PIC_LIST_0] = (motion_vector_t*)calloc (2*MAX_NUM_PARTITIONS, sizeof(motion_vector_t));
					phvenc_engine->ctu_info[i].mv_ref[REF_PIC_LIST_1] = phvenc_engine->ctu_info[i].mv_ref[REF_PIC_LIST_0]+MAX_NUM_PARTITIONS;
					phvenc_engine->ctu_info[i].mv_ref_idx[REF_PIC_LIST_0] = (int8_t*)calloc (2*MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].mv_ref_idx[REF_PIC_LIST_1] = phvenc_engine->ctu_info[i].mv_ref_idx[REF_PIC_LIST_0]+MAX_NUM_PARTITIONS;
					//init mv_ref_idx
					memset(phvenc_engine->ctu_info[i].mv_ref_idx[REF_PIC_LIST_0], -1, MAX_NUM_PARTITIONS*sizeof(phvenc_engine->ctu_info[i].mv_ref_idx[REF_PIC_LIST_0][0]));
					memset(phvenc_engine->ctu_info[i].mv_ref_idx[REF_PIC_LIST_1], -1, MAX_NUM_PARTITIONS*sizeof(phvenc_engine->ctu_info[i].mv_ref_idx[REF_PIC_LIST_1][0]));

					phvenc_engine->ctu_info[i].mv_diff[REF_PIC_LIST_0] = (motion_vector_t*)calloc (2*MAX_NUM_PARTITIONS, sizeof(motion_vector_t));
					phvenc_engine->ctu_info[i].mv_diff[REF_PIC_LIST_1] = phvenc_engine->ctu_info[i].mv_diff[REF_PIC_LIST_0]+MAX_NUM_PARTITIONS;
					phvenc_engine->ctu_info[i].mv_diff_ref_idx[REF_PIC_LIST_0] = (uint8_t*)calloc (2*MAX_NUM_PARTITIONS, sizeof(uint8_t));
					phvenc_engine->ctu_info[i].mv_diff_ref_idx[REF_PIC_LIST_1] = phvenc_engine->ctu_info[i].mv_diff_ref_idx[REF_PIC_LIST_0]+MAX_NUM_PARTITIONS;

//#ifdef COMPUTE_AS_HM
					phvenc_engine->ctu_info[i].coeff_wnd = (wnd_t*)calloc(1, sizeof(wnd_t));
					wnd_realloc(phvenc_engine->ctu_info[i].coeff_wnd, (phvenc_engine->ctu_width[0]), phvenc_engine->ctu_height[0], 0, 0, sizeof(int16_t));
//#endif
				}


				phvenc_engine->max_sublayers = hvenc->max_sublayers;//TLayers en HM
//				phvenc_engine->max_layers = hvenc->max_layers;

				for(ithreads=0;ithreads<phvenc_engine->wfpp_num_threads;ithreads++)
				{
//					int depth_aux;
					int j;
					henc_thread_t* henc_th = (henc_thread_t*)calloc(1, sizeof(henc_thread_t));
					int filter_buff_width, filter_buff_height;

	//				henc_th = &phvenc_engine->_thread;
					phvenc_engine->thread[ithreads] = henc_th;
					henc_th->enc_engine = phvenc_engine;
					henc_th->index = ithreads;
					henc_th->funcs = &hvenc->funcs;
					henc_th->wfpp_enable = phvenc_engine->wfpp_enable;
					henc_th->wfpp_num_threads = phvenc_engine->wfpp_num_threads;

					//alloc processing windows (processing buffers attached to windows will be allocated depending on the resolution)
					henc_th->wfpp_num_threads = phvenc_engine->wfpp_num_threads;

					HMR_FREE(henc_th->aux_contexts)
					henc_th->aux_contexts = (context_model_t*)calloc(NUM_CTXs, sizeof(context_model_t));

					if(henc_th->synchro_signal[0]!=NULL)
						SEM_DESTROY(henc_th->synchro_signal[0]);

					SEM_INIT(henc_th->synchro_sem[0], 0,1000);
					SEM_COPY(henc_th->synchro_sem[0], henc_th->synchro_signal[0]);
				
					if(henc_th->synchro_signal[1]!=NULL)
						SEM_DESTROY(henc_th->synchro_signal[1]);

					SEM_INIT(henc_th->synchro_sem[1], 0,1000);
					SEM_COPY(henc_th->synchro_sem[1], henc_th->synchro_signal[1]);

					henc_th->vps = &phvenc_engine->hvenc->vps;
					henc_th->sps = &phvenc_engine->hvenc->sps;
					henc_th->pps = &phvenc_engine->hvenc->pps;

					memcpy(henc_th->pict_width, phvenc_engine->pict_width, sizeof(henc_th->pict_width));
					memcpy(henc_th->pict_height, phvenc_engine->pict_height, sizeof(henc_th->pict_height));
					henc_th->pict_width_in_ctu = phvenc_engine->pict_width_in_ctu; 
					henc_th->pict_height_in_ctu = phvenc_engine->pict_height_in_ctu;			
					henc_th->pict_total_ctu = phvenc_engine->pict_total_ctu;

					memcpy(henc_th->ctu_width, phvenc_engine->ctu_width, sizeof(henc_th->ctu_width));
					memcpy(henc_th->ctu_height, phvenc_engine->ctu_height, sizeof(henc_th->ctu_height));
//					henc_th->ctu_group_size = phvenc_engine->ctu_group_size;

					henc_th->max_cu_size = phvenc_engine->max_cu_size;
					henc_th->max_cu_size_shift = phvenc_engine->max_cu_size_shift;//log2 of max CU size
					henc_th->max_cu_size_shift_chroma = phvenc_engine->max_cu_size_shift_chroma;//log2 of max CU size
					henc_th->max_intra_tr_depth = phvenc_engine->max_intra_tr_depth;
					henc_th->max_inter_tr_depth = phvenc_engine->max_inter_tr_depth;
					henc_th->max_pred_partition_depth = phvenc_engine->max_pred_partition_depth;//max depth for prediction
					henc_th->motion_estimation_precision = phvenc_engine->motion_estimation_precision;
	//				henc_th->max_inter_pred_depth = phvenc_engine->max_inter_pred_depth;//max depth for prediction

					henc_th->num_partitions_in_cu = phvenc_engine->num_partitions_in_cu;
					henc_th->num_partitions_in_cu_shift = phvenc_engine->num_partitions_in_cu_shift;
					henc_th->mincu_mintr_shift_diff = phvenc_engine->mincu_mintr_shift_diff;
					henc_th->max_cu_depth = phvenc_engine->max_cu_depth;
					henc_th->min_cu_size = phvenc_engine->min_cu_size;
					henc_th->min_cu_size_shift = phvenc_engine->min_cu_size_shift;
					henc_th->min_tu_size_shift = phvenc_engine->min_tu_size_shift;
					henc_th->max_tu_size_shift = phvenc_engine->max_tu_size_shift;

					henc_th->profile = hvenc->profile;
					henc_th->bit_depth = phvenc_engine->bit_depth;
					henc_th->performance_mode = phvenc_engine->performance_mode;
					henc_th->rd_mode = phvenc_engine->rd_mode;

					henc_th->partition_depth_start = phvenc_engine->partition_depth_start;
//					aux = 1;//el CTU

//					for(depth_aux=1;depth_aux<MAX_PARTITION_DEPTH;depth_aux++)//5 = max partition depth 
//					{
//						aux += (4<<(2*(depth_aux-1)));
//					}

//					henc_th->partition_info = (cu_partition_info_t*)calloc (aux, sizeof(cu_partition_info_t));
					//init partition list info
//					init_partition_info(henc_th, henc_th->partition_info);
					//----------------------------current thread processing buffers allocation	-----
					//alloc processing windows and buffers
					henc_th->adi_size = 2*henc_th->ctu_height[0]+2*henc_th->ctu_width[0]+1;//left, top and left-top corner neighbours
					henc_th->adi_pred_buff = (short*)hmr_aligned_alloc (henc_th->adi_size, sizeof(int16_t));
					henc_th->adi_filtered_pred_buff = (short*)hmr_aligned_alloc (henc_th->adi_size, sizeof(int16_t));
					henc_th->top_pred_buff = (short*)hmr_aligned_alloc (henc_th->adi_size, sizeof(int16_t));
					henc_th->left_pred_buff = (short*)hmr_aligned_alloc (henc_th->adi_size, sizeof(int16_t));
					henc_th->bottom_pred_buff = (short*)hmr_aligned_alloc (henc_th->adi_size, sizeof(int16_t));
					henc_th->right_pred_buff = (short*)hmr_aligned_alloc (henc_th->adi_size, sizeof(int16_t));


					wnd_realloc(&henc_th->curr_mbs_wnd, (henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(int16_t));

					henc_th->pred_aux_buff_size = MAX_CU_SIZE*MAX_CU_SIZE;//size of auxiliar buffer
					henc_th->pred_aux_buff = (short*) hmr_aligned_alloc (henc_th->pred_aux_buff_size, sizeof(short));

					wnd_realloc(&henc_th->curr_mbs_aux_wnd, (henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(int16_t));
					wnd_realloc(&henc_th->prediction_wnd[0], (henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(int16_t));
					wnd_realloc(&henc_th->prediction_wnd[1], (henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(int16_t));
					wnd_realloc(&henc_th->prediction_wnd[2], (henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(int16_t));


					wnd_realloc(&henc_th->residual_wnd, (henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(int16_t));
					wnd_realloc(&henc_th->residual_dec_wnd, (henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(int16_t));

					henc_th->aux_buff = (short*) hmr_aligned_alloc (MAX_CU_SIZE*MAX_CU_SIZE, sizeof(int));

					wnd_realloc(&henc_th->itransform_iquant_wnd, (henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(int16_t));

					for(i=0;i<NUM_DECODED_WNDS;i++)
					{
						wnd_realloc(&henc_th->decoded_mbs_wnd_[i], 2*(henc_th->ctu_width[0]), henc_th->ctu_height[0]*2, 1, 1, sizeof(int16_t));
						henc_th->decoded_mbs_wnd[i] = &henc_th->decoded_mbs_wnd_[i];//use pointers to exchange windows
					}

					for(i=0;i<NUM_QUANT_WNDS;i++)
					{
						wnd_realloc(&henc_th->transform_quant_wnd_[i], (henc_th->ctu_width[0]), henc_th->ctu_height[0], 0, 0, sizeof(int16_t));		
						henc_th->transform_quant_wnd[i] = &henc_th->transform_quant_wnd_[i];//use pointers to exchange windows
					}


					//deblocking filter
//					henc_th->deblock_partition_info = (cu_partition_info_t*)calloc (aux, sizeof(cu_partition_info_t));
//					init_partition_info(henc_th, henc_th->deblock_partition_info);
					henc_th->deblock_filter_strength_bs[EDGE_VER] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					henc_th->deblock_filter_strength_bs[EDGE_HOR] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					henc_th->deblock_edge_filter[EDGE_VER] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					henc_th->deblock_edge_filter[EDGE_HOR] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));

					henc_th->sao_sign_line_buff1 = (int16_t*) hmr_aligned_alloc (MAX_CU_SIZE+1, sizeof(uint16_t));
					henc_th->sao_sign_line_buff2 = (int16_t*) hmr_aligned_alloc (MAX_CU_SIZE+1, sizeof(uint16_t));

					//quarter pel interpolation
					filter_buff_width = MAX_CU_SIZE	+ 16;
					filter_buff_height = MAX_CU_SIZE + 2;//MAX_CU_SIZE + 1; - modified for the chroma
					for(j=0;j<4;j++)
					{
						wnd_realloc(&henc_th->filtered_block_temp_wnd[j], filter_buff_width, filter_buff_height+8, 0, 0, sizeof(int16_t));//filter_buff_height+7 - modified for chroma
						for(i=0;i<4;i++)
						{
							wnd_realloc(&henc_th->filtered_block_wnd[j][i], filter_buff_width, filter_buff_height, 0, 0, sizeof(int16_t));				
						}

					}

					henc_th->cabac_aux_buff_size = MAX_CU_SIZE*MAX_CU_SIZE;//MAX_TU_SIZE_SHIFT*MAX_TU_SIZE_SHIFT;//tama�o del buffer auxiliar
					henc_th->cabac_aux_buff = (unsigned char*) hmr_aligned_alloc (henc_th->cabac_aux_buff_size, sizeof(unsigned char));

					//alloc buffers to gather and consolidate information
					for(i=0;i<NUM_CBF_BUFFS;i++)
					{
						henc_th->cbf_buffs[Y_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
						henc_th->cbf_buffs[U_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
						henc_th->cbf_buffs[V_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
						henc_th->intra_mode_buffs[Y_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
						henc_th->intra_mode_buffs[U_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
						henc_th->intra_mode_buffs[V_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
						henc_th->tr_idx_buffs[i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));

						//inter
	/*					henc_th->mv_ref0[Y_COMP][i] = (motion_vector_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(motion_vector_t));
						henc_th->mv_ref0[U_COMP][i] = (motion_vector_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(motion_vector_t));
						henc_th->mv_ref0[V_COMP][i] = (motion_vector_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(motion_vector_t));

						henc_th->mv_ref1[Y_COMP][i] = (motion_vector_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(motion_vector_t));
						henc_th->mv_ref1[U_COMP][i] = (motion_vector_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(motion_vector_t));
						henc_th->mv_ref1[V_COMP][i] = (motion_vector_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(motion_vector_t));

						henc_th->ref_idx0[Y_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
						henc_th->ref_idx0[V_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
						henc_th->ref_idx0[U_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));

						henc_th->ref_idx1[Y_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
						henc_th->ref_idx1[V_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
						henc_th->ref_idx1[U_COMP][i] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
	*/				}

					henc_th->cbf_buffs_chroma[U_COMP] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					henc_th->cbf_buffs_chroma[V_COMP] = (uint8_t*) hmr_aligned_alloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));

					henc_th->ctu_rd = (ctu_info_t*)calloc (1, sizeof(ctu_info_t));
					henc_th->ctu_rd->part_size_type = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					henc_th->ctu_rd->pred_mode = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					henc_th->ctu_rd->skipped = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					henc_th->ctu_rd->merge = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					henc_th->ctu_rd->merge_idx = (uint8_t*)calloc (MAX_NUM_PARTITIONS, sizeof(uint8_t));
					henc_th->ee = phvenc_engine->ee_list[2*henc_th->index];
					henc_th->ec = &phvenc_engine->ec_list[henc_th->index];
				}

				//exterchange wait and signal semaphores between sucessive threads
				if(phvenc_engine->wfpp_num_threads==1)
				{
					SEM_COPY(phvenc_engine->thread[0]->synchro_sem[0], phvenc_engine->thread[0]->synchro_wait[0]);
				}
				else
				{
					for(i=0;i<phvenc_engine->wfpp_num_threads;i++)
					{
						SEM_COPY(phvenc_engine->thread[(i+phvenc_engine->wfpp_num_threads-1)%phvenc_engine->wfpp_num_threads]->synchro_sem[0], phvenc_engine->thread[i]->synchro_wait[0]);
					}
				}

				//reference picture lists
				phvenc_engine->num_ref_lists = 2;
				phvenc_engine->num_refs_idx_active_list[REF_PIC_LIST_0] = phvenc_engine->intra_period==1?4:phvenc_engine->num_ref_frames;//this will need to be a consistent decission taken depending on the configuration
				phvenc_engine->num_refs_idx_active_list[REF_PIC_LIST_1] = phvenc_engine->intra_period==1?4:phvenc_engine->num_ref_frames;

				hmr_rc_init(phvenc_engine);
			}// end for(n_enc_engines=0;n_enc_engines<hvenc->num_encoder_engines;n_enc_engines++)

			hvenc->pict_width[0] = phvenc_engine->pict_width[0];
			hvenc->pict_height[0] = phvenc_engine->pict_height[0];
			hvenc->pict_width[1] = hvenc->pict_width[2] = phvenc_engine->pict_width[1];
			hvenc->pict_height[1] = hvenc->pict_height[2] = phvenc_engine->pict_height[1];


			//copy sincronization between enc_mods
			for(n_enc_engines=0;n_enc_engines<hvenc->num_encoder_engines;n_enc_engines++)
			{
				hvenc_engine_t*  phvenc_signal_mod = hvenc->encoder_engines[(n_enc_engines+hvenc->num_encoder_engines-1)%hvenc->num_encoder_engines];
				hvenc_engine_t*  phvenc_wait_mod = hvenc->encoder_engines[n_enc_engines];

				SEM_COPY(phvenc_signal_mod->input_sem, phvenc_wait_mod->input_wait);
				SEM_COPY(phvenc_signal_mod->output_sem, phvenc_wait_mod->output_wait);
				for(i=0;i<phvenc_wait_mod->wfpp_num_threads;i++)
				{
					SEM_COPY(phvenc_signal_mod->thread[i]->synchro_sem[1], phvenc_wait_mod->thread[i]->synchro_wait[1]);
					if(hvenc->num_encoder_engines==1)
						phvenc_wait_mod->thread[i]->num_wait_sem = 1;
					else
						phvenc_wait_mod->thread[i]->num_wait_sem = 2;
				}
				wnd_realloc(&hvenc->encoder_engines[n_enc_engines]->sao_aux_wnd, hvenc->pict_width[0], hvenc->pict_height[0], hvenc->ctu_width[Y_COMP]+16, hvenc->ctu_height[Y_COMP]+16, sizeof(int16_t));
			}

						
			hvenc->pict_width[0] = phvenc_engine->pict_width[0];
			hvenc->pict_height[0] = phvenc_engine->pict_height[0];
			hvenc->pict_width[1] = hvenc->pict_width[2] = phvenc_engine->pict_width[1];
			hvenc->pict_height[1] = hvenc->pict_height[2] = phvenc_engine->pict_height[1];

			sync_cont_reset(hvenc->gop_container);
			for(i=0;i<MAX_NUM_INPUT_GOPS;i++)
			{
				gop_reinit(&hvenc->input_gops[i]);
				sync_cont_put_empty(hvenc->gop_container, &hvenc->input_gops[i]);
			}

			sync_cont_reset(hvenc->input_hmr_container);
			for(i=0;i<NUM_INPUT_FRAMES(hvenc->num_encoder_engines);i++)
			{
				wnd_realloc(&hvenc->input_frames[i].img, hvenc->pict_width[0], hvenc->pict_height[0], 0, 0, sizeof(int16_t));
				sync_cont_put_empty(hvenc->input_hmr_container, &hvenc->input_frames[i]);
			}

			cont_reset(hvenc->output_hmr_container);

			cont_reset(hvenc->cont_empty_reference_wnds);
			for(i=0;i<2*MAX_NUM_REF;i++)
			{
				wnd_realloc(&hvenc->ref_wnds[i].img, hvenc->pict_width[0], hvenc->pict_height[0], hvenc->ctu_width[Y_COMP]+16, hvenc->ctu_height[Y_COMP]+16, sizeof(int16_t));
				cont_put(hvenc->cont_empty_reference_wnds, &hvenc->ref_wnds[i]);
			}

			hvenc->ptl.generalPTL.profileIdc = hvenc->profile;

			hvenc->ptl.generalPTL.profileCompatibilityFlag[hvenc->profile] = 1;
			if (hvenc->profile == PROFILE_MAIN)// A PROFILE_MAIN10 can decode PROFILE_MAIN
				hvenc->ptl.generalPTL.profileCompatibilityFlag[PROFILE_MAIN10] = 1;

			if (hvenc->profile == PROFILE_MAIN10 && phvenc_engine->bit_depth == 8)// PROFILE_MAIN10 with 8 bits = PROFILE_MAIN
				hvenc->ptl.generalPTL.profileCompatibilityFlag[PROFILE_MAIN] = 1;

			//profiles and levels
			memset(hvenc->ptl.subLayerProfilePresentFlag, 0, sizeof(hvenc->ptl.subLayerProfilePresentFlag));
			memset(hvenc->ptl.subLayerLevelPresentFlag,   0, sizeof(hvenc->ptl.subLayerLevelPresentFlag  ));

			//----------------- start vps ------------------
			hvenc->vps.video_parameter_set_id = 0;
			hvenc->vps.temporal_id_nesting_flag = (hvenc->max_sublayers == 1);
			hvenc->vps.ptl = &hvenc->ptl;

			hvenc->vps.sub_layer_ordering_info_present_flag = 1;
			for(i = 0; i < MAX_TLAYER; i++)
			{
				int layer_incr = min(i,hvenc->max_sublayers-1);
				hvenc->vps.max_num_reorder_pics[i] = 0 + layer_incr;
				hvenc->vps.max_dec_pic_buffering[i] = (hvenc->intra_period==1)?1:hvenc->num_ref_frames+1+layer_incr;//m_maxDecPicBuffering[m_GOPList[i].m_temporalId] = m_GOPList[i].m_numRefPics + 1;
				hvenc->vps.max_latency_increase[i] = 0;
			}
	
			hvenc->vps.timing_info_present_flag = 0;

			//----------------- end vps ------------------

			//----------------- start sps ------------------
			hvenc->sps.video_parameter_set_id = 0;
			
			hvenc->sps.ptl = &hvenc->ptl;

			hvenc->sps.seq_parameter_set_id = 0;
			hvenc->sps.chroma_format_idc = CHROMA420;

			hvenc->sps.pic_width_in_luma_samples = hvenc->pict_width[0];
			hvenc->sps.pic_height_in_luma_samples = hvenc->pict_height[0];
			hvenc->sps.conformance_window_flag = phvenc_engine->conformance_mode!=0;
			hvenc->sps.conf_win_left_offset = phvenc_engine->pad_left/pad_unit_x[hvenc->sps.chroma_format_idc];
			hvenc->sps.conf_win_right_offset = phvenc_engine->pad_right/pad_unit_x[hvenc->sps.chroma_format_idc];
			hvenc->sps.conf_win_top_offset = phvenc_engine->pad_top/pad_unit_y[hvenc->sps.chroma_format_idc];
			hvenc->sps.conf_win_bottom_offset = phvenc_engine->pad_bottom/pad_unit_y[hvenc->sps.chroma_format_idc];
			hvenc->sps.bit_depth_luma_minus8 = hvenc->sps.bit_depth_chroma_minus8 = hvenc->bit_depth-8;
			hvenc->sps.pcm_enabled_flag = 0;
			hvenc->sps.log2_max_pic_order_cnt_lsb_minus4 = 0;//4; //bits for poc=8 - must be bigger than idr period
			for(i = 0; i < MAX_TLAYER; i++)
			{
				int layer_incr = min(i,hvenc->max_sublayers-1);
				hvenc->sps.max_num_reorder_pics[i] = 0+layer_incr;
				hvenc->sps.max_dec_pic_buffering[i] = (hvenc->intra_period==1)?1:hvenc->num_ref_frames+1+layer_incr;//m_maxDecPicBuffering[m_GOPList[i].m_temporalId] = m_GOPList[i].m_numRefPics + 1;
				hvenc->sps.max_latency_increase[i] = 0;
			}

			hvenc->sps.restricted_ref_pic_lists_flag = 1;
			hvenc->sps.lists_modification_present_flag = 0;
			hvenc->sps.log2_min_coding_block_size_minus3 = phvenc_engine->min_cu_size_shift-3;
			hvenc->sps.log2_diff_max_min_coding_block_size = phvenc_engine->max_cu_depth-phvenc_engine->mincu_mintr_shift_diff;//hvenc->num_partitions_in_cu_shift;//-hvenc->min_cu_size_shift;
//			hvenc->sps.log2_diff_max_min_coding_block_size = hvenc->max_cu_size_shift-hvenc->min_cu_size_shift;
			hvenc->sps.log2_min_transform_block_size_minus2 = phvenc_engine->min_tu_size_shift-2;
			hvenc->sps.log2_diff_max_min_transform_block_size = phvenc_engine->max_tu_size_shift - phvenc_engine->min_tu_size_shift;
			hvenc->sps.max_transform_hierarchy_depth_inter = phvenc_engine->max_inter_tr_depth-1;
			hvenc->sps.max_transform_hierarchy_depth_intra = phvenc_engine->max_intra_tr_depth-1;
			hvenc->sps.scaling_list_enabled_flag = 1;
			hvenc->sps.scaling_list_data_present_flag = 0;

			hvenc->sps.amp_enabled_flag = 0;
			hvenc->sps.sample_adaptive_offset_enabled_flag = cfg->sample_adaptive_offset;
			hvenc->sps.temporal_id_nesting_flag = (hvenc->max_sublayers == 1);
			hvenc->num_long_term_ref_pic_sets = 0;
			hvenc->sps.temporal_mvp_enable_flag = 0;
			hvenc->sps.strong_intra_smooth_enabled_flag = 1;
			hvenc->sps.vui_parameters_present_flag = 0;
			//----------------- end sps ------------------
			
			//----------------- start pps ------------------
			hvenc->pps.pic_parameter_set_id = 0;
			hvenc->pps.seq_parameter_set_id = hvenc->sps.seq_parameter_set_id;
			hvenc->pps.dependent_slice_enabled_flag = 0;
			hvenc->pps.output_flag_present_flag = 0;
			hvenc->pps.num_extra_slice_header_bits = 0;
			hvenc->pps.sign_data_hiding_flag = cfg->sign_hiding;
			hvenc->pps.cabac_init_present_flag = 0;//1

			hvenc->pps.num_ref_idx_l0_default_active_minus1 = hvenc->num_ref_frames-1;
			hvenc->pps.num_ref_idx_l1_default_active_minus1 = hvenc->num_ref_frames-1;
			
#ifdef COMPUTE_AS_HM
			hvenc->pps.pic_init_qp_minus26 = 0;
#else
			hvenc->pps.pic_init_qp_minus26 = phvenc_engine->pict_qp - 26;
#endif
			hvenc->pps.constrained_intra_pred_flag = 0;
			hvenc->pps.transform_skip_enabled_flag = 0;
			hvenc->pps.cu_qp_delta_enabled_flag = (phvenc_engine->bitrate_mode==BR_FIXED_QP)?0:1;
			hvenc->pps.diff_cu_qp_delta_depth = phvenc_engine->qp_depth;

			hvenc->pps.cb_qp_offset = phvenc_engine->chroma_qp_offset ;
			hvenc->pps.cr_qp_offset = phvenc_engine->chroma_qp_offset ;

			hvenc->pps.slice_chroma_qp_offsets_present_flag = 0;
			hvenc->pps.weighted_pred_flag = 0;
			hvenc->pps.weighted_bipred_flag = 0;
			hvenc->pps.output_flag_present_flag = 0;
			hvenc->pps.transquant_bypass_enable_flag = 0;
			hvenc->pps.tiles_enabled_flag = 0;	
			hvenc->pps.entropy_coded_sync_enabled_flag = phvenc_engine->wfpp_enable;

/*			if(hvenc->pps.tiles_enabled_flag)
				//....................		
*/
			hvenc->pps.loop_filter_across_slices_enabled_flag = 1;
			hvenc->pps.deblocking_filter_control_present_flag = 0;
			hvenc->pps.beta_offset_div2 = 0;
			hvenc->pps.tc_offset_div2 = 0;
/*			if(hvenc->pps.deblocking_filter_control_present_flag)
				//.......................
*/			hvenc->pps.pps_scaling_list_data_present_flag = 0;
//			if(hvenc->pps.pps_scaling_list_data_present_flag)
				//.......................
			hvenc->pps.lists_modification_present_flag = 0;
			hvenc->pps.log2_parallel_merge_level_minus2 = 0;
			hvenc->pps.num_extra_slice_header_bits = 0;
			hvenc->pps.slice_header_extension_present_flag = 0;
			//----------------- end pps ------------------
			hvenc->run = 1;
			for(i=0;i<hvenc->num_encoder_engines;i++)
				CREATE_THREAD(hvenc->encoder_mod_thread[i], encoder_engine_thread, hvenc->encoder_engines[i]);

		}	
		break;

	}   
	if(err)     
    	return (FALSE);
	else
		return (TRUE);
config_error:
	return (FALSE);
}

int get_nal_unit_type(hvenc_engine_t* enc_engine, slice_t *curr_slice, int curr_poc)
{
	if(curr_slice->slice_type == I_SLICE && (enc_engine->intra_period!=1 || (enc_engine->intra_period==1 && (curr_poc%(3*enc_engine->hvenc->num_encoder_engines+1)) == 0)))//if this condition is removed extended bitstream control is needed
	{
		return NALU_CODED_SLICE_IDR_W_RADL;
	}

	return NALU_CODED_SLICE_TRAIL_R;
}

void reference_picture_border_padding(wnd_t *wnd)
{
	int   component, j, i;

	for(component = Y_COMP; component <= V_COMP; component++)
	{
		int stride = WND_STRIDE_2D(*wnd, component);
		int padding_x = wnd->data_padding_x[component];
		int padding_y = wnd->data_padding_y[component];
		int data_width = wnd->data_width[component];
		int data_height = wnd->data_height[component];
		int16_t *ptr = WND_DATA_PTR(int16_t *, *wnd, component);
		int16_t *ptr_left = ptr-padding_x;
		int16_t *ptr_right = ptr+data_width;
		int16_t *ptr_top;// = ptr_left-stride;
		int16_t *ptr_bottom;// = ptr_left+(data_height)*stride;
		for(j=0;j<data_height;j++)
		{
			for(i=0;i<padding_x;i++)
			{
				ptr_left[i] = ptr[0];
				ptr_right[i]  = ptr[data_width-1];
			}
//			memset(ptr_left, ptr[0], padding_x);
//			memset(ptr_right, ptr[data_width-1], padding_x);
			ptr_left+=stride;
			ptr_right+=stride;
			ptr+=stride;
		}

		ptr = WND_DATA_PTR(int16_t *, *wnd, component);
		ptr += (data_height-1)*stride-padding_x;
		ptr_bottom = ptr+stride;
		
		for(j=0;j<padding_y;j++)
		{
			memcpy(ptr_bottom, ptr, stride*sizeof(ptr[0]));
			ptr_bottom+=stride;
		}

		ptr = WND_DATA_PTR(int16_t *, *wnd, component);
		ptr -= padding_x;
		ptr_top = ptr-padding_y*stride;
		for(j=0;j<padding_y;j++)
		{
			memcpy(ptr_top, ptr, stride*sizeof(ptr[0]));
			ptr_top+=stride;
		}
	}
}

void reference_picture_border_padding_ctu(wnd_t *wnd, ctu_info_t* ctu)
{
	int component, j, i;
	int pad_left = FALSE, pad_right = FALSE, pad_top = FALSE, pad_bottom = FALSE;
	int frame_width = WND_WIDTH_2D(*wnd, Y_COMP);
	int frame_height = WND_HEIGHT_2D(*wnd, Y_COMP);

	if(ctu->x[Y_COMP] == 0)
		pad_left = TRUE;

	if(ctu->y[Y_COMP] == 0)
		pad_top = TRUE;

	if(ctu->x[Y_COMP] + ctu->size >= frame_width)
		pad_right = TRUE;

	if(ctu->y[Y_COMP] + ctu->size >= frame_height)
		pad_bottom = TRUE;

	for(component = Y_COMP; component <= V_COMP; component++)
	{
		int stride = WND_STRIDE_2D(*wnd, component);
		int padding_x = wnd->data_padding_x[component];
		int padding_y = wnd->data_padding_y[component];
		int cu_width = (component==Y_COMP)?ctu->size:(ctu->size>>1);
		int cu_height = cu_width;
		int16_t *ptr = WND_DATA_PTR(int16_t *, *wnd, component) + ctu->y[component]*stride + ctu->x[component];

		frame_width = WND_WIDTH_2D(*wnd, component);
		frame_height = WND_HEIGHT_2D(*wnd, component);

		if(pad_left)
		{
			int16_t *ptr_left = ptr-padding_x;
			int16_t *ptr_orig = ptr;
			for(j=0;j<cu_height;j++)
			{
				for(i=0;i<padding_x;i++)
				{
					ptr_left[i] = ptr_orig[0];
				}
	//			memset(ptr_left, ptr[0], padding_x);
				ptr_left+=stride;
				ptr_orig+=stride;
			}
		}

		if(pad_right)
		{
			int padding_right_init = (frame_width-ctu->x[component])<cu_width?(frame_width-ctu->x[component]):cu_width;
			int16_t *ptr_right = ptr + padding_right_init;
			int16_t *ptr_orig = ptr + padding_right_init-1;
			int padding_size = cu_width;

			for(j=0;j<cu_height;j++)
			{
				for(i=0;i<padding_x;i++)
				{
					ptr_right[i] = ptr_orig[0];
				}
	//			memset(ptr_left, ptr_orig[0], padding_x);
				ptr_right+=stride;
				ptr_orig+=stride;
			}
		}

		if(pad_top)
		{
			int16_t *ptr_top = ptr - padding_y*stride;
			int16_t *ptr_orig = ptr;
			int padding_size = cu_width;
			if(pad_left)
			{
				ptr_top -= padding_x; 
				ptr_orig -= padding_x; 
				padding_size += padding_x;
			}
			if(pad_right)
			{
				int padding_right_init = (frame_width-ctu->x[component])<cu_width?(frame_width-ctu->x[component]):cu_width;
				padding_size = padding_right_init+padding_x;
			}
			for(j=0;j<padding_y;j++)
			{
				memcpy(ptr_top, ptr_orig, padding_size*sizeof(ptr_orig[0]));
				ptr_top+=stride;
			}
		}

		if(pad_bottom)
		{
			int padding_height_init = (frame_height-ctu->y[component])<cu_height?(frame_height-ctu->y[component]):cu_height;
			int16_t *ptr_bottom = ptr + (padding_height_init)*stride;
			int16_t *ptr_orig = ptr + (padding_height_init-1)*stride;
			int padding_size = cu_width;

			if(pad_left)
			{
				ptr_bottom -= padding_x; 
				ptr_orig -= padding_x; 
				padding_size += padding_x;// += cu_width;
			}
			if(pad_right)
			{
				int padding_right_init = (frame_width-ctu->x[component])<cu_width?(frame_width-ctu->x[component]):cu_width;
				padding_size = padding_right_init+padding_x;
			}
			for(j=0;j<padding_y;j++)
			{
				memcpy(ptr_bottom, ptr_orig, padding_size*sizeof(ptr_orig[0]));
				ptr_bottom += stride;
			}
		}
	}
}


//TEncTop::selectReferencePictureSet
void hmr_select_reference_picture_set(hvenc_enc_t* hvenc, slice_t *currslice)
{
	currslice->ref_pic_set_index = 0;

	if(currslice->nalu_type == NALU_CODED_SLICE_IDR_W_RADL)
	{
		currslice->ref_pic_set_index = max(0,hvenc->num_short_term_ref_pic_sets-1);
		currslice->ref_pic_set = &hvenc->ref_pic_set_list[currslice->ref_pic_set_index];
		currslice->ref_pic_set_index = 0;
	}
	else if(hvenc->intra_period>0)// && getDecodingRefreshType() > 0)
	{
		if(hvenc->num_b == 0 || hvenc->gop_size==hvenc->num_b)
		{
			if(currslice->poc>0)
			{
				int poc_idx = currslice->poc%hvenc->intra_period;
	//			if(currslice->gop_decode_order_idx < hvenc->num_ref_frames)
				if(poc_idx<hvenc->num_ref_frames)
				{
	//				if(hvenc->num_b==0)//this is for HM compatibility. Order difers from IPPPP, to IBBBBB
	//					currslice->ref_pic_set_index = hvenc->num_ref_frames-poc_idx;
	//				else
						currslice->ref_pic_set_index = poc_idx;//currslice->gop_decode_order_idx+1;
				}
			}
		}
		else//if(hvenc->default_gop.gop_num_b>0 && hvenc->default_gop.gop_size>hvenc->default_gop.gop_num_b)
		{
//			if(currslice->poc>0)
			{
//				if(currslice->gop_decode_order_idx < hvenc->num_ref_frames)
				{
					currslice->ref_pic_set_index = min(currslice->gop_decode_order_idx, hvenc->num_short_term_ref_pic_sets-2);
				}
			}
		}
		currslice->ref_pic_set = &hvenc->ref_pic_set_list[currslice->ref_pic_set_index];
		
	}
	currslice->ref_pic_set->num_pics = currslice->ref_pic_set->num_negative_pics+currslice->ref_pic_set->num_positive_pics;
}


//TComSlice::setRefPicList
void apply_reference_picture_set(hvenc_enc_t* hvenc, slice_t *currslice)
{
	int i, j;
	video_frame_t *refpic;

	currslice->ref_pic_list_cnt[REF_PIC_LIST_0] = currslice->ref_pic_list_cnt[REF_PIC_LIST_1] = 0;

	if(currslice->poc == 1)
	{
		int iiiii=0;
	}

	for(i=0;i<MAX_NUM_REF;i++)
	{
		refpic = hvenc->reference_picture_buffer[(hvenc->reference_list_index-1-i)&MAX_NUM_REF_MASK];
		if(refpic!=NULL)
		{
			refpic->is_reference = FALSE;
			for(j=0;j<currslice->ref_pic_set->num_pics;j++)
			{
				if(currslice->poc + currslice->ref_pic_set->delta_poc_s0[j] == refpic->temp_info.poc )
				{
					refpic->is_reference = currslice->ref_pic_set->used_by_curr_pic_S0_flag[j];

					if(refpic->is_reference)
					{
						if(currslice->ref_pic_set->delta_poc_s0[j] < 0)
						{
							currslice->ref_pic_list[REF_PIC_LIST_0][currslice->ref_pic_list_cnt[REF_PIC_LIST_0]] = refpic;
							currslice->ref_poc_list[REF_PIC_LIST_0][currslice->ref_pic_list_cnt[REF_PIC_LIST_0]] = refpic->temp_info.poc;
							currslice->ref_pic_list_cnt[REF_PIC_LIST_0]++;
						}
						else
						{
							currslice->ref_pic_list[REF_PIC_LIST_1][currslice->ref_pic_list_cnt[REF_PIC_LIST_1]] = refpic;
							currslice->ref_poc_list[REF_PIC_LIST_1][currslice->ref_pic_list_cnt[REF_PIC_LIST_1]] = refpic->temp_info.poc;
							currslice->ref_pic_list_cnt[REF_PIC_LIST_1]++;
						}
					}
				}
			}
		}
	}


	for(i=0;i<currslice->ref_pic_list_cnt[REF_PIC_LIST_0];i++)
	{
		currslice->ref_pic_list[REF_PIC_LIST_1][currslice->ref_pic_list_cnt[REF_PIC_LIST_1]+i] = currslice->ref_pic_list[REF_PIC_LIST_0][i];
		currslice->ref_poc_list[REF_PIC_LIST_1][currslice->ref_pic_list_cnt[REF_PIC_LIST_1]+i] = currslice->ref_poc_list[REF_PIC_LIST_0][i];
//		currslice->ref_pic_list_cnt[REF_PIC_LIST_1]++;
	}

	for(i=0;i<currslice->ref_pic_list_cnt[REF_PIC_LIST_1];i++)
	{
		currslice->ref_pic_list[REF_PIC_LIST_0][currslice->ref_pic_list_cnt[REF_PIC_LIST_0]+i] = currslice->ref_pic_list[REF_PIC_LIST_1][i];
		currslice->ref_poc_list[REF_PIC_LIST_0][currslice->ref_pic_list_cnt[REF_PIC_LIST_0]+i] = currslice->ref_poc_list[REF_PIC_LIST_1][i];
//		currslice->ref_pic_list_cnt[REF_PIC_LIST_0]++;
	}

}


int is_temporal_layer_switching_point(hvenc_enc_t* hvenc, uint32_t poc, int layer)
{
	int i, j;
	video_frame_t *iter_pic, *rpc_pic;

	for(i=0;i<MAX_NUM_REF-1;i++)
	{
		iter_pic = hvenc->reference_picture_buffer[i];
		rpc_pic = hvenc->reference_picture_buffer[i+1];
		if(iter_pic!=NULL && rpc_pic!=NULL)
		{
			if(rpc_pic->is_reference && rpc_pic->temp_info.poc != poc)
			{
				if(rpc_pic->sublayer >= layer)
					return FALSE;
			}
		}
	}

	return TRUE;
/*  TComPic* rpcPic;
  // loop through all pictures in the reference picture buffer
  TComList<TComPic*>::iterator iterPic = rcListPic.begin();
  while ( iterPic != rcListPic.end())
  {
    rpcPic = *(iterPic++);
    if(rpcPic->getSlice(0)->isReferenced() && rpcPic->getPOC() != getPOC())
    {
      if(rpcPic->getTLayer() >= getTLayer())
      {
        return false;
      }
    }
  }

  return true;
*/}

void hmr_slice_init(hvenc_engine_t* enc_engine, picture_t *currpict, slice_t *currslice)
{
	int idx_l1;
	int img_type = currpict->img2encode->img_type;
	currslice->qp =  enc_engine->pict_qp;
	currslice->poc = currpict->img2encode->temp_info.poc;
	currslice->gop_decode_order_idx = currpict->img2encode->temp_info.gop_decode_order_idx;
	currslice->sublayer = currpict->img2encode->sublayer;
	currslice->slice_temporal_layer_non_reference_flag = !currpict->img2encode->is_reference;
	currslice->sps = &enc_engine->hvenc->sps;
	currslice->pps = &enc_engine->hvenc->pps;
	currslice->slice_index = 0;
	currslice->curr_cu_address = currslice->first_cu_address = 0;
	currslice->last_cu_address = enc_engine->pict_total_ctu*enc_engine->num_partitions_in_cu;
	currslice->slice_temporal_mvp_enable_flag = enc_engine->hvenc->sps.temporal_mvp_enable_flag;
	currslice->deblocking_filter_disabled_flag = 0;//enabled
	currslice->sao_luma_flag = currslice->sps->sample_adaptive_offset_enabled_flag;
	currslice->sao_chroma_flag = currslice->sps->sample_adaptive_offset_enabled_flag;
	currslice->slice_loop_filter_across_slices_enabled_flag = 1;//disabled
	currslice->slice_beta_offset_div2 = currslice->pps->beta_offset_div2;
	currslice->slice_beta_offset_div2 = currslice->pps->beta_offset_div2;
	currslice->max_num_merge_candidates = enc_engine->num_merge_mvp_candidates;
	enc_engine->last_intra = enc_engine->hvenc->last_intra;
//	if((currslice->poc%enc_engine->intra_period)==0)
//	if(currslice->poc==0 || (enc_engine->intra_period!=0 && currslice->poc==(enc_engine->last_intra + enc_engine->intra_period) && img_type == IMAGE_AUTO) || (enc_engine->intra_period==0 && currslice->poc==0) || img_type == IMAGE_I)
	if(img_type == IMAGE_I)
	{
		enc_engine->last_intra = enc_engine->hvenc->last_intra;				//enc_engine->hvenc->last_intra = enc_engine->last_intra = currslice->poc;
		enc_engine->last_gop_reinit = enc_engine->hvenc->last_gop_reinit;	//enc_engine->hvenc->last_gop_reinit = enc_engine->last_gop_reinit = currslice->poc;
//		currpict->img2encode->img_type = IMAGE_I;
		currslice->slice_type = I_SLICE;
//		currslice->slice_temporal_layer_non_reference_flag = 0;
		currslice->is_dependent_slice = 0;
		currslice->nalu_type = get_nal_unit_type(enc_engine, currslice, currslice->poc);//NALU_CODED_SLICE_IDR;
//		currslice->sublayer = currpict->img2encode->sublayer;
		currslice->depth = 0;

#ifndef COMPUTE_AS_HM
/*		if(enc_engine->bitrate_mode != BR_FIXED_QP)//modulate avg qp for differential encoding
		{
			if(currslice->slice_type == I_SLICE && enc_engine->intra_period!=1)
				enc_engine->pict_qp = hmr_rc_compensate_qp_for_intra(enc_engine->avg_dist, enc_engine->pict_qp);		
		}
*/
#endif
	}
//	else if((enc_engine->num_b==0 && img_type == IMAGE_AUTO) || img_type == IMAGE_P || (enc_engine->intra_period==0 && enc_engine->num_b==0))
	else if(img_type == IMAGE_P)
	{
//		currpict->img2encode->img_type = IMAGE_P;
		currslice->slice_type = P_SLICE;
//		currslice->slice_temporal_layer_non_reference_flag = 0;
		currslice->is_dependent_slice = 0;
		currslice->nalu_type = get_nal_unit_type(enc_engine, currslice, currslice->poc);//NALU_CODED_SLICE_IDR;
//		currslice->sublayer = 0;
		currslice->depth = 0;	

	}
	//else if((enc_engine->num_b!=0 && img_type == IMAGE_AUTO) || img_type == IMAGE_B || enc_engine->intra_period==0)
	else if(img_type == IMAGE_B)
	{
//		currpict->img2encode->img_type = IMAGE_B;//IMAGE_P;
		currslice->slice_type = B_SLICE;//P_SLICE;
//		currslice->slice_temporal_layer_non_reference_flag = (currslice->sublayer>0)?1:0;
		currslice->is_dependent_slice = 0;
		currslice->nalu_type = get_nal_unit_type(enc_engine, currslice, currslice->poc);//NALU_CODED_SLICE_IDR;
//		currslice->sublayer = 0;
		currslice->depth = 0;	

	}

	currslice->qp = enc_engine->pict_qp;

	hmr_select_reference_picture_set(enc_engine->hvenc, currslice);

	if(currslice->nalu_type == NALU_CODED_SLICE_IDR_W_RADL || currslice->nalu_type == NALU_CODED_SLICE_IDR_N_LP)
	{
		enc_engine->hvenc->last_idr = currslice->poc;
	}
	enc_engine->last_idr = enc_engine->hvenc->last_idr;

	if(currslice->poc == 0)//first image?
	{
//		sublayer_non_reference = 0;		
	}

	switch(currslice->slice_type)
	{
		case I_SLICE:
		case P_SLICE:
		case B_SLICE:
		{
			currslice->referenced = 1;
		}
		break;
	}


//	currslice->num_ref_idx[REF_PIC_LIST_0] = min(enc_engine->num_ref_frames, currslice->ref_pic_set->num_pics);// currslice->ref_pic_set->num_negative_pics;//;
//	currslice->num_ref_idx[REF_PIC_LIST_1] = (currslice->slice_type==P_SLICE)?0:min(enc_engine->num_ref_frames, currslice->ref_pic_set->num_pics);//currslice->ref_pic_set->num_positive_pics;//enc_engine->num_refs_idx_active_list[REF_PIC_LIST_1];
	currslice->num_ref_idx[REF_PIC_LIST_0] = currslice->ref_pic_set->num_pics;// currslice->ref_pic_set->num_negative_pics;//;
	currslice->num_ref_idx[REF_PIC_LIST_1] = (currslice->slice_type==P_SLICE)?0:currslice->ref_pic_set->num_pics;//currslice->ref_pic_set->num_positive_pics;//enc_engine->num_refs_idx_active_list[REF_PIC_LIST_1];

	apply_reference_picture_set(enc_engine->hvenc, currslice);		

	if(currslice->slice_temporal_layer_non_reference_flag)
	{
	      if (currslice->nalu_type == NALU_CODED_SLICE_TRAIL_R && !(enc_engine->gop_size == 1 && currslice->slice_type == I_SLICE))
			  currslice->nalu_type = NALU_CODED_SLICE_TRAIL_N;

		  if(currslice->nalu_type == NALU_CODED_SLICE_RADL_R)
			currslice->nalu_type = NALU_CODED_SLICE_RADL_N;

		  if(currslice->nalu_type == NALU_CODED_SLICE_RASL_R)
			currslice->nalu_type = NALU_CODED_SLICE_RASL_N;
	}

	if(currslice->sublayer>0 && !(currslice->nalu_type == NALU_CODED_SLICE_RADL_N || currslice->nalu_type == NALU_CODED_SLICE_RADL_R   // Check if not a leading picture          
		|| currslice->nalu_type == NALU_CODED_SLICE_RASL_N || currslice->nalu_type == NALU_CODED_SLICE_RASL_R))
	{
		if(is_temporal_layer_switching_point(enc_engine->hvenc, currslice->poc, currslice->sublayer) || currslice->sps->temporal_id_nesting_flag)
		{
			if(currslice->slice_temporal_layer_non_reference_flag)
			{
				currslice->nalu_type = NALU_CODED_SLICE_TSA_N;
			}
			else
			{
				currslice->nalu_type = NALU_CODED_SLICE_TSA_R;
			}
		}	
	}          

	for (idx_l1 = 0; idx_l1 < currslice->num_ref_idx[REF_PIC_LIST_1]; idx_l1++ )
	{
		int idx_l0;
		currslice->list1_idx_to_list0_idx[idx_l1] = -1;//if list1_idx_to_list0_idx=-1, reference frames are different
		for ( idx_l0 = 0; idx_l0 < currslice->num_ref_idx[REF_PIC_LIST_0]; idx_l0++ )
		{
			if ( currslice->ref_pic_list[REF_PIC_LIST_0][idx_l0]->temp_info.poc == currslice->ref_pic_list[REF_PIC_LIST_1][idx_l1]->temp_info.poc )
			{
				currslice->list1_idx_to_list0_idx[idx_l1] = idx_l0;//list1_idx_to_list0_idx[ref_idx] shows the index in list0 that corresponds to and index in list1
				break;
			}
		}
	}

	currslice->mvd_l1_zero_flag = FALSE;//this flags indicates if REF_PIC_LIST_1 references should be taken into account. If they are different from REF_PIC_LIST_0 references
    if (currslice->slice_type == B_SLICE)
    {
		if(currslice->num_ref_idx[REF_PIC_LIST_0] == currslice->num_ref_idx[REF_PIC_LIST_1])
		{
			int i;
			currslice->mvd_l1_zero_flag = TRUE;		
			for ( i=0; i < currslice->num_ref_idx[REF_PIC_LIST_1]; i++ )
			{
				if (currslice->ref_poc_list[REF_PIC_LIST_1][i] != currslice->ref_poc_list[REF_PIC_LIST_0][i])
				{
					currslice->mvd_l1_zero_flag = FALSE;
					break;
				}
			}
		}
    }


	//init sao slice flags
	sao_decide_pic_params(enc_engine->slice_enabled, currslice->sao_luma_flag, currslice->sao_chroma_flag);// decidePicParams(sliceEnabled, pPic->getSlice(0)->getDepth()); 
	memset(enc_engine->sao_debug_mode, 0, sizeof(enc_engine->sao_debug_mode));
	memset(enc_engine->sao_debug_type, 0, sizeof(enc_engine->sao_debug_type));
}



void CuGetNeighbors(henc_thread_t* et, ctu_info_t* ctu)
{
	if(ctu->x[Y_COMP]==0)
		ctu->ctu_left = NULL;
	else
	{
		ctu->ctu_left = &et->enc_engine->ctu_info[ctu->ctu_number-1];
	}
	ctu->ctu_left_bottom = NULL;//en raster order este no existe. En wavefront si

	if(ctu->y[Y_COMP]==0)
	{
		ctu->ctu_top = NULL;	
		ctu->ctu_top_right = NULL;
		ctu->ctu_top_left = NULL;
	}
	else
	{
		ctu->ctu_top = &et->enc_engine->ctu_info[ctu->ctu_number-et->pict_width_in_ctu];	

		if(ctu->x[Y_COMP]==0)
			ctu->ctu_top_left = NULL;
		else
			ctu->ctu_top_left = &et->enc_engine->ctu_info[ctu->ctu_number-et->pict_width_in_ctu-1];	

		if(et->cu_current_y==0 || ((et->cu_current_x % et->pict_width_in_ctu) == (et->pict_width_in_ctu-1)))
			ctu->ctu_top_right = NULL;
		else
		{
			ctu->ctu_top_right = &et->enc_engine->ctu_info[ctu->ctu_number-et->pict_width_in_ctu+1];
		}
	}
}



int HOMER_enc_write_annex_b_output(nalu_t *nalu_out[], unsigned int num_nalus, encoder_in_out_t *vout)
{
	uint nalu_idx, bytes_written=0;
	uint8_t code[4] = {0x0,0x0,0x0,0x1};

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
	uint8_t *part_size_type = dst_ctu->part_size_type;
	uint8_t *pred_mode = dst_ctu->pred_mode;
	uint8_t *skipped = dst_ctu->skipped;
	uint8_t *merge = dst_ctu->merge;
	uint8_t *merge_idx = dst_ctu->merge_idx;
	memcpy(dst_ctu, src_ctu, sizeof(ctu_info_t));
	dst_ctu->part_size_type = part_size_type;
	dst_ctu->pred_mode = pred_mode;
	dst_ctu->skipped = skipped;
	dst_ctu->merge = merge;
	dst_ctu->merge_idx = merge_idx;
}


const uint8_t chroma_scale_conversion_table[58]=
{
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
  17,18,19,20,21,22,23,24,25,26,27,28,29,29,30,31,32,
  33,33,34,34,35,35,36,36,37,37,38,39,40,41,42,43,44,
  45,46,47,48,49,50,51
};


ctu_info_t* init_ctu(henc_thread_t* et)
{
	ctu_info_t *ctu;
	int ctu_width, ctu_height;
	
	ctu = &et->enc_engine->ctu_info[et->cu_current];//&et->curr_ctu_group_info[0];
	ctu->ctu_number = et->cu_current;
	ctu->x[Y_COMP] = et->cu_current_x*et->ctu_width[Y_COMP];
	ctu->y[Y_COMP] = et->cu_current_y*et->ctu_height[Y_COMP];
	ctu->x[U_COMP] = ctu->x[V_COMP] = et->cu_current_x*et->ctu_width[U_COMP];
	ctu->y[U_COMP] = ctu->y[V_COMP] = et->cu_current_y*et->ctu_height[U_COMP];
	ctu->size = et->max_cu_size;
	ctu->num_part_in_ctu = et->num_partitions_in_cu;
	ctu->num_part_in_ctu = et->num_partitions_in_cu;
//	ctu->partition_list = &et->partition_info[0];

	ctu_width = ((ctu->x[Y_COMP]+ctu->size)<et->pict_width[Y_COMP])?(ctu->size):et->pict_width[Y_COMP]-ctu->x[Y_COMP];
	ctu_height = ((ctu->y[Y_COMP]+ctu->size)<et->pict_height[Y_COMP])?(ctu->size):et->pict_height[Y_COMP]-ctu->y[Y_COMP];

	if(ctu_width!=ctu->size || ctu_height!=ctu->size)
	{
		int width_in_partitions = ctu_width>>2;
		int height_in_partitions = ctu_height>>2;
		int cu_size_in_partitions = ctu->size>>2;
//		if(height_in_partitions == cu_size_in_partitions)
//			height_in_partitions = cu_size_in_partitions-1;
//		else if(width_in_partitions == cu_size_in_partitions)
		height_in_partitions -= 1;
		ctu->last_valid_partition =	et->enc_engine->raster2abs_table[height_in_partitions*cu_size_in_partitions+width_in_partitions-1];
	}
	else
	{
		ctu->last_valid_partition = et->num_partitions_in_cu-1;
	}
	
	CuGetNeighbors(et, ctu);//raster order
	return ctu;
}

#define PRINTF //printf 
#define PRINTF_POST	//printf 
#define PRINTF_DEBLOCK //printf
#define PRINTF_SAO //printf
#define PRINTF_CTU_ENCODE //printf

void wfpp_encode_select_bitstream(henc_thread_t* et, ctu_info_t *ctu)
{
	int ctu_num = ctu->ctu_number;
	int ctu_x = ctu_num%et->pict_width_in_ctu;
	int ctu_y = ctu_num/et->pict_width_in_ctu;
	int ee_index = (ctu_y)%et->enc_engine->wfpp_num_threads;
	picture_t *currpict = &et->enc_engine->current_pict;
	slice_t *currslice = &currpict->slice;

	if(ctu_num==0)
	{
		PRINTF_CTU_ENCODE("ee_get0, ctu_num:%d, ee_index:%d - ", ctu_num, (2*ee_index));
		et->ec = &et->enc_engine->ec_list[ee_index];
		et->ee = et->enc_engine->ee_list[2*ee_index];
//		et->ec->bs = &et->enc_engine->bc_bs[ctu_y];
		et->ee->bs = &et->enc_engine->aux_bs[ctu_y];
		hmr_bitstream_init(et->ee->bs);

		//resetEntropy
		ee_start_entropy_model(et->ee, currslice);//Init CABAC contexts
		//cabac - reset binary encoder and entropy
		et->ee->ee_start(et->ee->b_ctx);
		et->ee->ee_reset_bits(et->ee->b_ctx);//ee_reset(&enc_engine->ee);
	}
	else if(et->wfpp_enable)
	{
		if(ctu_y>0 && ctu_x==0)
		{
			ptrswap(enc_env_t*, et->enc_engine->ee_list[(2*ee_index)], et->enc_engine->ee_list[(2*ee_index+et->enc_engine->num_ee-1)%et->enc_engine->num_ee]);//get inherited enviroment, leave current as avaliable for the previous thread of the list
			PRINTF_CTU_ENCODE("ptrswap, ctu_num:%d, ee_index_a:%d, ee_index_b:%d\r\n", ctu_num, (2*ee_index), (2*ee_index+et->enc_engine->num_ee-1)%et->enc_engine->num_ee);
		}
		PRINTF_CTU_ENCODE("ee_get1, ctu_num:%d, ee_index:%d - ", ctu_num, (2*ee_index));

		et->ec = &et->enc_engine->ec_list[ee_index];
//		et->ec->bs = &et->enc_engine->bc_bs[ctu_y];
		et->ee = et->enc_engine->ee_list[(2*ee_index)];
		et->ee->bs = &et->enc_engine->aux_bs[ctu_y];
		if(ctu_x==0)
		{
			hmr_bitstream_init(et->ee->bs);
			et->ee->ee_start(et->ee->b_ctx);
			et->ee->ee_reset_bits(et->ee->b_ctx);//ee_reset(&enc_engine->ee);
		}
	}

}


void wfpp_encode_ctu(henc_thread_t* et, ctu_info_t *ctu)
{
	int gcnt = 0;
	int bits_allocated;
	int ctu_num = ctu->ctu_number;
	int ctu_x = ctu_num%et->pict_width_in_ctu;
	int ctu_y = ctu_num/et->pict_width_in_ctu;
	int ee_index = (ctu_y)%et->enc_engine->wfpp_num_threads;
	picture_t *currpict = &et->enc_engine->current_pict;
	slice_t *currslice = &currpict->slice;

	bits_allocated = hmr_bitstream_bitcount(et->ee->bs);

	if(currslice->sps->sample_adaptive_offset_enabled_flag)
		ee_encode_sao(et, et->ee, currslice, ctu);

	ee_encode_ctu(et, et->ee, currslice, ctu, gcnt);
	PRINTF_CTU_ENCODE("ee_encode_ctu, ctu_num:%d\r\n", ctu_num);

	et->num_bits += hmr_bitstream_bitcount(et->ee->bs)-bits_allocated;

	if(ctu_x==1 && ctu_y+1 != et->pict_height_in_ctu)
	{
		if(et->wfpp_enable)
			ee_copy_entropy_model(et->ee->contexts, et->enc_engine->ee_list[(2*ee_index+1)%et->enc_engine->num_ee]->contexts);
		PRINTF_CTU_ENCODE("ee_copy, ctu_num:%d, ee_index_dst:%d\r\n", ctu_num, (2*ee_index+1)%et->enc_engine->num_ee);			
	}

	if((et->wfpp_enable && ctu_x+1==et->pict_width_in_ctu) || (!et->wfpp_enable) && (ctu_x+1==et->pict_total_ctu))
	{
		ee_end_slice(et->ee, currslice, ctu);
		PRINTF_CTU_ENCODE("ee_end_slice, ctu_num:%d\r\n", ctu_num);
	}

	et->num_encoded_ctus++;

}


void hmr_deblock_sao_pad_sync_ctu(henc_thread_t* et, slice_t *currslice, ctu_info_t* ctu)
{
	int ctu_num = ctu->ctu_number;
	cu_partition_info_t* aux_partition_info;
	ctu_info_t* filter_ctu;
	int is_inter_gop = (et->enc_engine->intra_period != 1)?1:0;
	int sao_enabled = currslice->sps->sample_adaptive_offset_enabled_flag;
	int search_window_in_ctus_x = (MOTION_SEARCH_RANGE_X+et->max_cu_size-1)>>et->max_cu_size_shift;
	int search_window_in_ctus_y = (MOTION_SEARCH_RANGE_Y+et->max_cu_size-1)>>et->max_cu_size_shift;
	int sem_post_ref_wnd_limit = (search_window_in_ctus_y)*et->pict_width_in_ctu+search_window_in_ctus_x+1;//(1+search_window_in_ctus_y)*et->pict_width_in_ctu+search_window_in_ctus_x;
	int ctu_num_index = ctu_num%et->pict_width_in_ctu;
	int ctu_num_vertical = ctu_num - (et->pict_width_in_ctu+1);//- et->pict_total_ctu;//take into account that intra prediction is done previous to deblocking the references
	int ctu_num_vertical_index = ctu_num_vertical%et->pict_width_in_ctu;
	int ctu_num_horizontal = ctu_num_vertical-1;//ctu_num - (et->pict_width_in_ctu+2);//  (1*et->pict_width_in_ctu+2);
	int ctu_num_padding = sao_enabled?(ctu_num_horizontal - (et->pict_width_in_ctu+1)):(ctu_num_horizontal - et->pict_width_in_ctu);//  (1*et->pict_width_in_ctu+2);
	int ctu_num_sao = ctu_num_padding;//only used if sao is enabled
	int ctu_num_sao_offset = ctu_num_sao - (et->pict_width_in_ctu+1);
	int ctu_num_post_semaphore = sao_enabled?(ctu_num_sao_offset - sem_post_ref_wnd_limit):(ctu_num_padding - sem_post_ref_wnd_limit);

	if(is_inter_gop)
	{
		if(sao_enabled)
		{
			if(ctu_num_vertical>=0 && (ctu_num_index)>=1)
			{
				filter_ctu = &et->enc_engine->ctu_info[ctu_num_vertical];
//				filter_ctu->partition_list = et->deblock_partition_info;
//				create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
				hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_VER);
				PRINTF_DEBLOCK("EDGE_VER_0, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);

				if(ctu_num_horizontal>=0 && (ctu_num_index)>=2)// && is_inter_gop)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_horizontal];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_HOR);
					PRINTF_DEBLOCK("EDGE_HOR_0, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);

					if(ctu_num_sao>=0 && (ctu_num_index)>=3)
					{
						filter_ctu = &et->enc_engine->ctu_info[ctu_num_sao];

						wfpp_encode_select_bitstream(et, filter_ctu);
						hmr_wpp_sao_ctu(et, currslice, filter_ctu);
						wfpp_encode_ctu(et, filter_ctu);
						PRINTF_SAO("SAO_0, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);

						if(ctu_num_sao_offset>=0 && (ctu_num_index)>=4)
						{
							filter_ctu = &et->enc_engine->ctu_info[ctu_num_sao_offset];
							sao_offset_ctu(et, filter_ctu, &filter_ctu->recon_params);
							reference_picture_border_padding_ctu(&et->enc_engine->curr_reference_frame->img, filter_ctu);
							PRINTF_SAO("SAO_OFFSET_0, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
							if(ctu_num_post_semaphore>=0 && (ctu_num_index)>=5+search_window_in_ctus_x)
							{
								int thread_idx = (ctu_num_post_semaphore/et->pict_width_in_ctu/*-(1+search_window_in_ctus_y)*/)%et->enc_engine->wfpp_num_threads;
								SEM_POST(et->enc_engine->thread[thread_idx]->synchro_signal[1]);
								et->enc_engine->dbg_num_posts[thread_idx]++;
								PRINTF_POST("SEM_POST_0, filter_ctu_num:%d, thread_signaled:%d, dbg_num_posts=%d\r\n", ctu_num_post_semaphore, thread_idx, et->enc_engine->dbg_num_posts[thread_idx]);
							}
						}
					}
				}
			}


			if((ctu_num_vertical_index+1) == et->pict_width_in_ctu-1 && (ctu_num+1) != et->pict_total_ctu && is_inter_gop)
			{
				int ctu_num_aux;
				int max_filter = ((ctu_num_vertical/et->pict_width_in_ctu)+1)*et->pict_width_in_ctu;
				for(ctu_num_aux=ctu_num_vertical+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_VER);
					PRINTF_DEBLOCK("EDGE_VER_1, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}
				for(ctu_num_aux=ctu_num_horizontal+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_HOR);
					PRINTF_DEBLOCK("EDGE_HOR_1, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_sao>0)
				{
					max_filter-=et->pict_width_in_ctu;
					for(ctu_num_aux=ctu_num_sao+1; ctu_num_aux<max_filter;ctu_num_aux++)
					{
						filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//						filter_ctu->partition_list = et->deblock_partition_info;
//						create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
						wfpp_encode_select_bitstream(et, filter_ctu);
						hmr_wpp_sao_ctu(et, currslice, filter_ctu);
						wfpp_encode_ctu(et, filter_ctu);
						PRINTF_SAO("SAO_1, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
					}

					if(ctu_num_sao_offset>0)
					{
						max_filter-=et->pict_width_in_ctu;
						for(ctu_num_aux=ctu_num_sao_offset+1; ctu_num_aux<max_filter;ctu_num_aux++)
						{
							filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
							sao_offset_ctu(et, filter_ctu, &filter_ctu->recon_params);
							reference_picture_border_padding_ctu(&et->enc_engine->curr_reference_frame->img, filter_ctu);
							PRINTF_SAO("SAO_OFFSET_1, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
						}

						if(ctu_num_post_semaphore>0)
						{
							max_filter-=(search_window_in_ctus_y)*et->pict_width_in_ctu;//(1+search_window_in_ctus_y)*et->pict_width_in_ctu;
							for(ctu_num_aux=ctu_num_post_semaphore+1; ctu_num_aux<max_filter;ctu_num_aux++)
							{
								int thread_idx = (ctu_num_aux/et->pict_width_in_ctu/*-(1+search_window_in_ctus_y)*/)%et->enc_engine->wfpp_num_threads;
								SEM_POST(et->enc_engine->thread[thread_idx]->synchro_signal[1]);			
								et->enc_engine->dbg_num_posts[thread_idx]++;
								PRINTF_POST("SEM_POST_1, filter_ctu_num:%d, thread_signaled:%d, dbg_num_posts=%d\r\n", ctu_num_aux, thread_idx, et->enc_engine->dbg_num_posts[thread_idx]);
							}
						}
					}
				}
			}


			if((ctu_num+1) == et->pict_total_ctu && is_inter_gop)
			{
				int ctu_num_aux;
				int max_filter = et->pict_total_ctu;//((ctu_num_vertical_index/et->pict_width_in_ctu)+1)*et->pict_width_in_ctu;

				if(ctu_num_vertical<0)
					ctu_num_vertical=-1;
				for(ctu_num_aux=ctu_num_vertical+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_VER);
					PRINTF_DEBLOCK("EDGE_VER_2, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_horizontal<0)
					ctu_num_horizontal=-1;
				for(ctu_num_aux=ctu_num_horizontal+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_HOR);
					PRINTF_DEBLOCK("EDGE_HOR_2, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_sao<0)
					ctu_num_sao=-1;
				for(ctu_num_aux=ctu_num_sao+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					wfpp_encode_select_bitstream(et, filter_ctu);
					hmr_wpp_sao_ctu(et, currslice, filter_ctu);
					wfpp_encode_ctu(et, filter_ctu);
					PRINTF_SAO("SAO_2, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_sao_offset<0)
					ctu_num_sao_offset=-1;
				for(ctu_num_aux=ctu_num_sao_offset+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
					sao_offset_ctu(et, filter_ctu, &filter_ctu->recon_params);
					reference_picture_border_padding_ctu(&et->enc_engine->curr_reference_frame->img, filter_ctu);
					PRINTF_SAO("SAO_OFFSET_2, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_post_semaphore<0)
					ctu_num_post_semaphore=-1;
				for(ctu_num_aux=ctu_num_post_semaphore+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					int thread_idx = (ctu_num_aux/et->pict_width_in_ctu/*-(1+search_window_in_ctus_y)*/)%et->enc_engine->wfpp_num_threads;
					SEM_POST(et->enc_engine->thread[thread_idx]->synchro_signal[1]);
					et->enc_engine->dbg_num_posts[thread_idx]++;
					PRINTF_POST("SEM_POST_2, filter_ctu_num:%d, thread_signaled:%d, dbg_num_posts=%d\r\n", ctu_num_aux, thread_idx, et->enc_engine->dbg_num_posts[thread_idx]);
				}
			}
		}
		else//if(!sao_enabled)
		{
			wfpp_encode_select_bitstream(et, ctu);
			wfpp_encode_ctu(et, ctu);
			if(ctu_num_vertical>=0 && (ctu_num_index)>=1)// && is_inter_gop)
			{
				filter_ctu = &et->enc_engine->ctu_info[ctu_num_vertical];
//				filter_ctu->partition_list = et->deblock_partition_info;
//				create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
				hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_VER);
				PRINTF_DEBLOCK("EDGE_VER_0, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);

				if(ctu_num_horizontal>=0 && (ctu_num_index)>=2)// && is_inter_gop)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_horizontal];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_HOR);
					PRINTF_DEBLOCK("EDGE_HOR_0, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);

					if(ctu_num_padding>=0 && (ctu_num_index)>=2)
					{
						filter_ctu = &et->enc_engine->ctu_info[ctu_num_padding];
						reference_picture_border_padding_ctu(&et->enc_engine->curr_reference_frame->img, filter_ctu);
						PRINTF_SAO("PADDING_0, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);

						if(ctu_num_post_semaphore>=0 && (ctu_num_index)>=3+search_window_in_ctus_x)
						{
							int thread_idx = (ctu_num_post_semaphore/et->pict_width_in_ctu/*-(1+search_window_in_ctus_y)*/)%et->enc_engine->wfpp_num_threads;
							SEM_POST(et->enc_engine->thread[thread_idx]->synchro_signal[1]);
							et->enc_engine->dbg_num_posts[thread_idx]++;
							PRINTF_POST("SEM_POST_0, thread:%d, filter_ctu_num:%d, thread_signaled:%d, dbg_num_posts=%d\r\n", et->index, ctu_num_post_semaphore, thread_idx, et->enc_engine->dbg_num_posts[thread_idx]);
						}
					}
				}
			}


			if((ctu_num_vertical_index+1) == et->pict_width_in_ctu-1 && (ctu_num+1) != et->pict_total_ctu && is_inter_gop)
			{
				int ctu_num_aux;
				int max_filter = ((ctu_num_vertical/et->pict_width_in_ctu)+1)*et->pict_width_in_ctu;
				for(ctu_num_aux=ctu_num_vertical+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_VER);
					PRINTF_DEBLOCK("EDGE_VER_1, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}
				for(ctu_num_aux=ctu_num_horizontal+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_HOR);
					PRINTF_DEBLOCK("EDGE_HOR_1, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_padding>0)
				{
					max_filter-=et->pict_width_in_ctu;
					for(ctu_num_aux=ctu_num_padding+1; ctu_num_aux<max_filter;ctu_num_aux++)
					{
						filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//						filter_ctu->partition_list = et->deblock_partition_info;
						reference_picture_border_padding_ctu(&et->enc_engine->curr_reference_frame->img, filter_ctu);
						PRINTF_SAO("PADDING_1, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
					}

					if(ctu_num_post_semaphore>0)
					{
						max_filter-=(search_window_in_ctus_y)*et->pict_width_in_ctu;
						for(ctu_num_aux=ctu_num_post_semaphore+1; ctu_num_aux<max_filter;ctu_num_aux++)
						{
							int thread_idx = (ctu_num_aux/et->pict_width_in_ctu/*-(1+search_window_in_ctus_y)*/)%et->enc_engine->wfpp_num_threads;
							SEM_POST(et->enc_engine->thread[thread_idx]->synchro_signal[1]);			
							et->enc_engine->dbg_num_posts[thread_idx]++;
							PRINTF_POST("SEM_POST_1, thread:%d, filter_ctu_num:%d, thread_signaled:%d, dbg_num_posts=%d\r\n", et->index, ctu_num_aux, thread_idx, et->enc_engine->dbg_num_posts[thread_idx]);
						}
					}
				}
			}


			if((ctu_num+1) == et->pict_total_ctu && is_inter_gop)
			{
				int ctu_num_aux;
				int max_filter = et->pict_total_ctu;//((ctu_num_vertical_index/et->pict_width_in_ctu)+1)*et->pict_width_in_ctu;

				if(ctu_num_vertical<0)
					ctu_num_vertical=-1;
				for(ctu_num_aux=ctu_num_vertical+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_VER);
					PRINTF_DEBLOCK("EDGE_VER_2, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_horizontal<0)
					ctu_num_horizontal=-1;
				for(ctu_num_aux=ctu_num_horizontal+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_HOR);
					PRINTF_DEBLOCK("EDGE_HOR_2, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_padding<0)
					ctu_num_padding=-1;
				for(ctu_num_aux=ctu_num_padding+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
					reference_picture_border_padding_ctu(&et->enc_engine->curr_reference_frame->img, filter_ctu);				
					PRINTF_SAO("PADDING_2, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_post_semaphore<0)
					ctu_num_post_semaphore=-1;
				for(ctu_num_aux=ctu_num_post_semaphore+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					int thread_idx = (ctu_num_aux/et->pict_width_in_ctu/*-(1+search_window_in_ctus_y)*/)%et->enc_engine->wfpp_num_threads;
					SEM_POST(et->enc_engine->thread[thread_idx]->synchro_signal[1]);
					et->enc_engine->dbg_num_posts[thread_idx]++;
					PRINTF_POST("SEM_POST_2, thread:%d, filter_ctu_num:%d, thread_signaled:%d, dbg_num_posts=%d\r\n", et->index, ctu_num_aux, thread_idx, et->enc_engine->dbg_num_posts[thread_idx]);
				}
			}
		}
	}
	else //intra
	{
		if(sao_enabled)
		{
			if(ctu_num_vertical>=0 && (ctu_num_index)>=1)
			{
				filter_ctu = &et->enc_engine->ctu_info[ctu_num_vertical];
//				filter_ctu->partition_list = et->deblock_partition_info;
//				create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
				hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_VER);
				PRINTF_DEBLOCK("EDGE_VER_0, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);

				if(ctu_num_horizontal>=0 && (ctu_num_index)>=2)// && is_inter_gop)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_horizontal];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_HOR);
					PRINTF_DEBLOCK("EDGE_HOR_0, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);

					if(ctu_num_sao>=0 && (ctu_num_index)>=3)
					{
						int thread_idx = (ctu_num_sao/et->pict_width_in_ctu/*-(1+search_window_in_ctus_y)*/)%et->enc_engine->wfpp_num_threads;
						filter_ctu = &et->enc_engine->ctu_info[ctu_num_sao];
						wfpp_encode_select_bitstream(et, filter_ctu);
						hmr_wpp_sao_ctu(et, currslice, filter_ctu);				
						wfpp_encode_ctu(et, filter_ctu);
						PRINTF_SAO("SAO_0, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
						SEM_POST(et->enc_engine->thread[thread_idx]->synchro_signal[1]);
						et->enc_engine->dbg_num_posts[thread_idx]++;
						PRINTF_POST("SEM_POST_0, filter_ctu_num:%d, thread_signaled:%d, dbg_num_posts=%d\r\n", ctu_num_sao, thread_idx, et->enc_engine->dbg_num_posts[thread_idx]);
					}
				}
			}


			if((ctu_num_vertical_index+1) == et->pict_width_in_ctu-1 && (ctu_num+1) != et->pict_total_ctu)
			{
				int ctu_num_aux;
				int max_filter = ((ctu_num_vertical/et->pict_width_in_ctu)+1)*et->pict_width_in_ctu;
				for(ctu_num_aux=ctu_num_vertical+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_VER);
					PRINTF_DEBLOCK("EDGE_VER_1, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}
				for(ctu_num_aux=ctu_num_horizontal+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_HOR);
					PRINTF_DEBLOCK("EDGE_HOR_1, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_sao>0)
				{
					max_filter-=et->pict_width_in_ctu;
					for(ctu_num_aux=ctu_num_sao+1; ctu_num_aux<max_filter;ctu_num_aux++)
					{
						int thread_idx = (ctu_num_aux/et->pict_width_in_ctu/*-(1+search_window_in_ctus_y)*/)%et->enc_engine->wfpp_num_threads;
						filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//						filter_ctu->partition_list = et->deblock_partition_info;
//						create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
						wfpp_encode_select_bitstream(et, filter_ctu);
						hmr_wpp_sao_ctu(et, currslice, filter_ctu);
						wfpp_encode_ctu(et, filter_ctu);
						PRINTF_SAO("SAO_1, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
						SEM_POST(et->enc_engine->thread[thread_idx]->synchro_signal[1]);			
						et->enc_engine->dbg_num_posts[thread_idx]++;
						PRINTF_POST("SEM_POST_1, filter_ctu_num:%d, thread_signaled:%d, dbg_num_posts=%d\r\n", ctu_num_aux, thread_idx, et->enc_engine->dbg_num_posts[thread_idx]);

					}
				}
			}


			if((ctu_num+1) == et->pict_total_ctu)
			{
				int ctu_num_aux;
				int max_filter = et->pict_total_ctu;//((ctu_num_vertical_index/et->pict_width_in_ctu)+1)*et->pict_width_in_ctu;

				if(ctu_num_vertical<0)
					ctu_num_vertical=-1;
				for(ctu_num_aux=ctu_num_vertical+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_VER);
					PRINTF_DEBLOCK("EDGE_VER_2, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_horizontal<0)
					ctu_num_horizontal=-1;
				for(ctu_num_aux=ctu_num_horizontal+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					hmr_deblock_filter_cu(et, currslice, filter_ctu, EDGE_HOR);
					PRINTF_DEBLOCK("EDGE_HOR_2, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
				}

				if(ctu_num_sao<0)
					ctu_num_sao=0;
				for(ctu_num_aux=ctu_num_sao+1; ctu_num_aux<max_filter;ctu_num_aux++)
				{
					int thread_idx = (ctu_num_aux/et->pict_width_in_ctu/*-(1+search_window_in_ctus_y)*/)%et->enc_engine->wfpp_num_threads;
					filter_ctu = &et->enc_engine->ctu_info[ctu_num_aux];
//					filter_ctu->partition_list = et->deblock_partition_info;
//					create_partition_ctu_neighbours(et, filter_ctu, filter_ctu->partition_list);//ctu->partition_list);//this call should be removed
					wfpp_encode_select_bitstream(et, filter_ctu);
					hmr_wpp_sao_ctu(et, currslice, filter_ctu);
					wfpp_encode_ctu(et, filter_ctu);
					PRINTF_SAO("SAO_2, filter_ctu_num:%d\r\n", filter_ctu->ctu_number);
					SEM_POST(et->enc_engine->thread[thread_idx]->synchro_signal[1]);
					et->enc_engine->dbg_num_posts[thread_idx]++;
					PRINTF_POST("SEM_POST_2, filter_ctu_num:%d, thread_signaled:%d, dbg_num_posts=%d\r\n", ctu_num_aux, thread_idx, et->enc_engine->dbg_num_posts[thread_idx]);

				}
			}
		}
		else//if(!sao_enabled)
		{
			int thread_idx = (ctu->ctu_number/et->pict_width_in_ctu/*-(1+search_window_in_ctus_y)*/)%et->enc_engine->wfpp_num_threads;
			wfpp_encode_select_bitstream(et, ctu);
			wfpp_encode_ctu(et, ctu);					
			SEM_POST(et->enc_engine->thread[thread_idx]->synchro_signal[1]);
		}
	}
}



#define PRINTF_SYNC //printf 

THREAD_RETURN_TYPE wfpp_encoder_thread(void *h)
{
	henc_thread_t* et = (henc_thread_t*)h;
	int gcnt=0;
	picture_t *currpict = &et->enc_engine->current_pict;
	slice_t *currslice = &currpict->slice;
	ctu_info_t* ctu;

	//printf("		+wfpp_encoder_thread %d\r\n", et->index);

	et->cu_current_x = 0;
	et->cu_current_y = et->index;

	ctu = &et->enc_engine->ctu_info[et->cu_current_y*et->pict_width_in_ctu];


	if(et->index==0)
	{
		et->ec = &et->enc_engine->ec_list[0];
		et->ee = et->enc_engine->ee_list[0];
		et->ee->bs = &et->enc_engine->aux_bs[0];
		hmr_bitstream_init(et->ee->bs);

		//resetEntropy
		ee_start_entropy_model(et->ee, currslice);//Init CABAC contexts
		//cabac - reset binary encoder and entropy
		et->ee->ee_start(et->ee->b_ctx);
		et->ee->ee_reset_bits(et->ee->b_ctx);//ee_reset(&enc_engine->ee);
	}

	while(et->cu_current < et->pict_total_ctu)//all ctus loop
	{
		int bits_allocated;
		et->cu_current = et->pict_width_in_ctu*(et->cu_current_y)+et->cu_current_x;
		et->cu_next = et->cu_current+min(1,et->pict_width_in_ctu-et->cu_current_x);

		if(et->cu_current_y == 0)// && ((et->cu_current_x & GRAIN_MASK) == 0))
		{
			SEM_POST(et->synchro_wait[0]);
			et->dbg_sem_post_cnt++;
			PRINTF_SYNC("SEM_POST1: ctu_num:%d, thread_id:%d, dbg_sem_post_cnt:%d\r\n", et->cu_current, et->index, et->dbg_sem_post_cnt);
		}
		if(et->enc_engine->num_encoded_frames == 0)// && ((et->cu_current_x & GRAIN_MASK) == 0))
		{
			SEM_POST(et->synchro_wait[1]);
		}

		{
			SEM_WAIT_MULTIPLE(et->synchro_wait, et->num_wait_sem);
		}

		ctu = init_ctu(et);

		//Prepare Memory
		mem_transfer_move_curr_ctu_group(et, et->cu_current_x, et->cu_current_y, ctu);	//move MBs from image to currMbWnd
		mem_transfer_intra_refs(et, ctu);//copy left and top info for intra prediction

		copy_ctu(ctu, et->ctu_rd);

//		bits_allocated = hmr_bitstream_bitcount(et->ee->bs);

		PROFILER_RESET(intra)

		//map spatial features and neighbours in recursive partition structure
		create_partition_ctu_neighbours(et, ctu, ctu->partition_list);


		if(currslice->slice_type != I_SLICE && !et->enc_engine->is_scene_change)// && (ctu->ctu_number & 0x1) == 0)
		{
			int ll;

			motion_inter(et, ctu);

			for(ll = 0; ll<et->num_partitions_in_cu;ll++)
			{
				if(ctu->pred_mode[ll]==INTRA_MODE)
					et->num_intra_partitions++;
			}
		}
		else
		{
			//make ctu intra prediction
			motion_intra(et, ctu, gcnt);
			et->num_intra_partitions += et->num_partitions_in_cu;
		}
		PROFILER_ACCUMULATE(intra)

		et->num_total_partitions += et->num_partitions_in_cu;
		et->num_total_ctus++;
		et->acc_qp += ctu->partition_list[0].qp;
		et->acc_dist += ctu->partition_list[0].distortion;
		ctu->distortion = ctu->partition_list[0].distortion;

		mem_transfer_decoded_blocks(et, ctu);


		wnd_copy(et->funcs->sse_copy_16_16, et->transform_quant_wnd[0], ctu->coeff_wnd);

#ifndef COMPUTE_AS_HM
		hmr_deblock_sao_pad_sync_ctu(et, currslice, ctu);
#endif
		if(et->cu_current_x>=2 && et->cu_current_y+1 != et->pict_height_in_ctu)//notify next wpp thread
		{
			SEM_POST(et->synchro_signal[0]);
			et->dbg_sem_post_cnt++;
			PRINTF_SYNC("SEM_POST2: ctu_num:%d, thread_id:%d, dbg_sem_post_cnt:%d\r\n", et->cu_current, et->index, et->dbg_sem_post_cnt);
		}


		et->cu_current_x++;

		//sync entropy contexts between wpp
		if(et->cu_current_x == 2 && et->cu_current_y+1 != et->pict_height_in_ctu)
		{
			SEM_POST(et->synchro_signal[0]);
			et->dbg_sem_post_cnt++;
			PRINTF_SYNC("SEM_POST3: ctu_num:%d, thread_id:%d, dbg_sem_post_cnt:%d\r\n", et->cu_current, et->index, et->dbg_sem_post_cnt);

		}

		//notify last synchronization as this line goes two ctus ahead from next line in wfpp
		if(et->cu_current_x==et->pict_width_in_ctu && et->cu_current_y+1 != et->pict_height_in_ctu)// && ((et->cu_current_x & GRAIN_MASK) == 0))
		{
			SEM_POST(et->synchro_signal[0]);
			et->dbg_sem_post_cnt++;
			PRINTF_SYNC("SEM_POST4: ctu_num:%d, thread_id:%d, dbg_sem_post_cnt:%d\r\n", et->cu_current, et->index, et->dbg_sem_post_cnt);
		}

		if(et->cu_current_x==et->pict_width_in_ctu)
		{
			et->cu_current_y+=et->wfpp_num_threads;
			et->cu_current_x=0;
		}

		et->cu_current = et->pict_width_in_ctu*(et->cu_current_y)+et->cu_current_x;
	}

	return THREAD_RETURN;
}

int HOMER_enc_encode(void* handle, encoder_in_out_t* input_frame)
{
	hvenc_enc_t* enc_engine = (hvenc_enc_t*)handle;
	put_frame_to_encode(enc_engine, input_frame);

	return 0;
}

int HOMER_enc_get_coded_frame(void* handle, encoder_in_out_t* output_frame, nalu_t *nalu_out[], unsigned int *nalu_list_size)
{
	hvenc_enc_t* hvenc = (hvenc_enc_t*)handle;
	*nalu_list_size = 0;

	if(get_num_elements(hvenc->output_hmr_container))
	{
		int comp, j, i, stride_src;
		uint16_t *src;
		uint8_t *dst;
		output_set_t* ouput_set;
		cont_get(hvenc->output_hmr_container, (void**)&ouput_set);
		memcpy(nalu_out, ouput_set->nalu_list, ouput_set->num_nalus*sizeof(ouput_set->nalu_list[0]));
		*nalu_list_size = ouput_set->num_nalus;
		output_frame->pts = ouput_set->pts;
		output_frame->image_type = ouput_set->image_type;
		if(output_frame->stream.streams[0]!=NULL && output_frame->stream.streams[1]!=NULL && output_frame->stream.streams[2]!=NULL)
		{
			for(comp=Y_COMP;comp<=V_COMP;comp++)
			{
				src = WND_DATA_PTR(uint16_t*, ouput_set->frame->img, comp);
				dst = output_frame->stream.streams[comp];
				stride_src = WND_STRIDE_2D(ouput_set->frame->img, comp);
				
				for(j=0;j<hvenc->pict_height[comp];j++)
				{
					for(i=0;i<hvenc->pict_width[comp];i++)
					{
						*dst++ = (uint8_t)src[i];						
					}
					src += stride_src;
				}
			}
		}
	}

	return 0;
}


void hmr_sao_encode_ctus_hm(hvenc_engine_t* enc_engine, slice_t *currslice)
{
	henc_thread_t *et = enc_engine->thread[0];
	int ctu_num;
	ctu_info_t* ctu = NULL;
	int cu_current_x = 0, cu_current_y = 0;
	for(ctu_num = 0;ctu_num < enc_engine->pict_total_ctu;ctu_num++)
	{
		int bits_allocated;// = hmr_bitstream_bitcount(et->ee->bs);
		int ee_index = cu_current_y%et->enc_engine->wfpp_num_threads;//et->index;

		ctu = &enc_engine->ctu_info[ctu_num];

		if(cu_current_x==0 && et->wfpp_enable)
		{
			if(cu_current_y > 0)
			{
				ptrswap(enc_env_t*, et->enc_engine->ee_list[(2*ee_index)], et->enc_engine->ee_list[(2*ee_index+et->enc_engine->num_ee-1)%et->enc_engine->num_ee]);//get inherited enviroment, leave current as avaliable for the previous thread of the list
			}

			et->ec = &et->enc_engine->ec_list[ee_index];
			et->ee = et->enc_engine->ee_list[(2*ee_index)];
			et->ee->bs = &et->enc_engine->aux_bs[cu_current_y];
			hmr_bitstream_init(et->ee->bs);
			et->ee->ee_start(et->ee->b_ctx);
			et->ee->ee_reset_bits(et->ee->b_ctx);//ee_reset(&enc_engine->ee);
		}

		bits_allocated = hmr_bitstream_bitcount(et->ee->bs);

		if(currslice->sps->sample_adaptive_offset_enabled_flag)
		{
			hmr_wpp_sao_ctu(enc_engine->thread[0], currslice, ctu);
			if(ctu_num>=(enc_engine->pict_width_in_ctu+1))
			{
				ctu_info_t* ctu_offset = &enc_engine->ctu_info[ctu_num-(enc_engine->pict_width_in_ctu+1)];
				sao_offset_ctu(enc_engine->thread[0], ctu_offset , &ctu_offset ->recon_params);
				reference_picture_border_padding_ctu(&enc_engine->thread[0]->enc_engine->curr_reference_frame->img, ctu_offset);
			}
			ee_encode_sao(et, et->ee, currslice, ctu);

			if(ctu_num+1 == enc_engine->pict_total_ctu)
			{
				int ctu_num_aux;
				for(ctu_num_aux = enc_engine->pict_total_ctu-(enc_engine->pict_width_in_ctu+1);ctu_num_aux < enc_engine->pict_total_ctu;ctu_num_aux++)
				{
					ctu_info_t* ctu_offset = &enc_engine->ctu_info[ctu_num_aux];
					sao_offset_ctu(enc_engine->thread[0], ctu_offset , &ctu_offset ->recon_params);
					reference_picture_border_padding_ctu(&enc_engine->curr_reference_frame->img, ctu_offset);
				}
			}
		}
		
		ee_encode_ctu(et, et->ee, currslice, ctu, 0);
		et->num_encoded_ctus++;
		et->num_bits += hmr_bitstream_bitcount(et->ee->bs)-bits_allocated;
		cu_current_x++;

		//sync entrophy contexts between wpp
		if(cu_current_x==2 && cu_current_y+1 != et->pict_height_in_ctu)
		{
			if(et->wfpp_enable)
				ee_copy_entropy_model(et->ee->contexts, et->enc_engine->ee_list[(2*ee_index+1)%et->enc_engine->num_ee]->contexts);
		}

		if(cu_current_x==et->pict_width_in_ctu)
		{
			if(et->wfpp_enable)
				ee_end_slice(et->ee, currslice, ctu);
			cu_current_y++;//=et->wfpp_num_threads;
			cu_current_x=0;
		}
	}

	if(!et->wfpp_enable)
	{
		ee_end_slice(et->ee, currslice, ctu);
	}
	et->num_encoded_ctus = 0;
}


#define INIT_SYNC_THREAD_CONTEXT(enc_engine, et)							\
		et->rd = enc_engine->rd;											\
		et->performance_mode = enc_engine->performance_mode;				\
		et->acc_dist = 0;													\
		et->cu_current = 0;													\
		et->cu_current_x = 0;												\
		et->cu_current_y = 0;												\
		et->num_intra_partitions = 0;										\
		et->num_total_partitions = 0;										\
		et->num_total_ctus = 0;												\
		et->dbg_sem_post_cnt = 0;


THREAD_RETURN_TYPE encoder_engine_thread(void *h)
{
	int avg_qp = 0.0;
	hvenc_engine_t* enc_engine = (hvenc_engine_t*)h;
	picture_t *currpict = &enc_engine->current_pict;
	slice_t *currslice = &currpict->slice;
	int n, i, num_threads;

	if(enc_engine->index==0)// && enc_engine->num_encoded_frames==0)
	{
		SEM_POST(enc_engine->output_wait);
		SEM_POST(enc_engine->input_wait);
	}

	while(enc_engine->hvenc->run)
	{
		output_set_t* ouput_sets;// = &enc_engine->hvenc->output_sets[enc_engine->num_encoded_frames & NUM_OUTPUT_NALUS_MASK];
		int		output_nalu_cnt = 0;
		int		nalu_list_size = NALU_SET_SIZE;
		nalu_t	**output_nalu_list;// = ouput_sets->nalu_list;
		int		engine;
		//Get into input mutex
		SEM_WAIT(enc_engine->input_wait);
		//get next image in decode order with pts and poc
		if(!get_frame_to_encode(enc_engine->hvenc, &enc_engine->current_pict.img2encode))//get next image to encode and init type
		{
			// we are closing the encoder..
			enc_engine->hvenc->run = FALSE;
			SEM_POST(enc_engine->input_signal);
			return THREAD_RETURN;
		}

		enc_engine->num_encoded_frames = enc_engine->hvenc->num_encoded_frames;
		enc_engine->hvenc->num_encoded_frames++;

		ouput_sets = &enc_engine->hvenc->output_sets[enc_engine->num_encoded_frames % NUM_OUTPUT_NALUS(enc_engine->hvenc->num_encoder_engines)];//relative to encoder
		output_nalu_list = ouput_sets->nalu_list;

		memset(output_nalu_list, 0, (nalu_list_size)*sizeof(output_nalu_list[0]));

		enc_engine->slice_nalu = &enc_engine->slice_nalu_list[enc_engine->num_encoded_frames_in_engine % STREAMS_PER_ENGINE];//relative to engine

		hmr_bitstream_init(&enc_engine->slice_nalu->bs);

		hmr_slice_init(enc_engine, &enc_engine->current_pict, &currpict->slice);

		//get free img for decoded blocks
		cont_get(enc_engine->hvenc->cont_empty_reference_wnds,(void**)&enc_engine->curr_reference_frame);
		enc_engine->curr_reference_frame->img_type = enc_engine->current_pict.img2encode->img_type;
		enc_engine->curr_reference_frame->temp_info = enc_engine->current_pict.img2encode->temp_info;//currslice->poc;//assign temporal info to decoding window for future use as reference
		enc_engine->curr_reference_frame->sublayer = enc_engine->current_pict.img2encode->sublayer;

		//reference prunning must be done in a selective way
		if(enc_engine->hvenc->reference_picture_buffer[enc_engine->hvenc->reference_list_index]!=NULL)
			cont_put(enc_engine->hvenc->cont_empty_reference_wnds,enc_engine->hvenc->reference_picture_buffer[enc_engine->hvenc->reference_list_index]);

		enc_engine->hvenc->reference_picture_buffer[enc_engine->hvenc->reference_list_index] = enc_engine->curr_reference_frame;
		enc_engine->hvenc->reference_list_index = (enc_engine->hvenc->reference_list_index+1)&MAX_NUM_REF_MASK;

		enc_engine->avg_dist = enc_engine->hvenc->avg_dist;

		if(enc_engine->bitrate_mode != BR_FIXED_QP)
		{
			if(currslice->poc==0)
				hmr_rc_init_seq(enc_engine);

			enc_engine->rc = enc_engine->hvenc->rc;
			hmr_rc_init_pic(enc_engine, &currpict->slice);
		}
		hmr_rd_init(enc_engine, &currpict->slice);


		for(n = 0; n<enc_engine->wfpp_num_threads;n++)
		{
			INIT_SYNC_THREAD_CONTEXT(enc_engine, enc_engine->thread[n]);
			//reset remafore
			SEM_RESET(enc_engine->thread[n]->synchro_wait[0])
		}

		SEM_POST(enc_engine->input_signal);		//Leave input mutex

		CREATE_THREADS((&enc_engine->hthreads[0]), wfpp_encoder_thread, enc_engine->thread, enc_engine->wfpp_num_threads)
		JOIN_THREADS(enc_engine->hthreads, enc_engine->wfpp_num_threads-1)

		//calc average distortion
		if(enc_engine->num_encoded_frames == 0 || currslice->slice_type != I_SLICE || enc_engine->intra_period==1)
		{
			enc_engine->avg_dist = 0;
			avg_qp = 0.0;
			for(n = 0;n<enc_engine->wfpp_num_threads;n++)
			{
				henc_thread_t* henc_th = enc_engine->thread[n];
		
				enc_engine->avg_dist+= henc_th->acc_dist;
				avg_qp+=henc_th->acc_qp;
			}
			enc_engine->avg_dist /= enc_engine->pict_total_ctu*enc_engine->num_partitions_in_cu;
			enc_engine->avg_dist = clip(enc_engine->avg_dist,.1,enc_engine->avg_dist);
			if(currslice->slice_type == I_SLICE)
				enc_engine->avg_dist*=1.5;
			else if(enc_engine->is_scene_change)
				enc_engine->avg_dist*=1.375;
		}
//		else if(currslice->slice_type == I_SLICE)
//			enc_engine->avg_dist/=1.5;

		enc_engine->hvenc->avg_dist = enc_engine->avg_dist;
		avg_qp = (avg_qp+(enc_engine->pict_total_ctu>>1))/enc_engine->pict_total_ctu;

#ifndef COMPUTE_AS_HM
		if(enc_engine->bitrate_mode != BR_FIXED_QP)//modulate avg qp for differential encoding
		{
			enc_engine->pict_qp = clip(avg_qp,/*MIN_QP*/1,MAX_QP);
			if(currslice->slice_type == I_SLICE && enc_engine->intra_period!=1)
				enc_engine->pict_qp = hmr_rc_compensate_qp_from_intra(enc_engine->avg_dist, enc_engine->pict_qp);		
		}
#endif

#ifdef COMPUTE_AS_HM
		if(enc_engine->intra_period>1)
			hmr_deblock_filter(enc_engine, currslice);

		hmr_sao_encode_ctus_hm(enc_engine, currslice);

		if(enc_engine->intra_period>1)
			reference_picture_border_padding(&enc_engine->curr_reference_frame->img);
#endif


		if(enc_engine->bitrate_mode != BR_FIXED_QP)
			hmr_rc_end_pic(enc_engine, currslice);

		enc_engine->hvenc->avg_dist = enc_engine->avg_dist;
		enc_engine->is_scene_change = 0;

		//sync to other modules
		for(engine = 0;engine<enc_engine->hvenc->num_encoder_engines;engine++)
		{
			hvenc_engine_t* phvenc_engine = enc_engine->hvenc->encoder_engines[engine];
			if(phvenc_engine->current_pict.slice.poc>enc_engine->current_pict.slice.poc)
			{
				phvenc_engine->rc.vbv_fullness = enc_engine->rc.vbv_fullness;
				phvenc_engine->rc.acc_avg = enc_engine->rc.acc_avg;
				phvenc_engine->rc.acc_rate= enc_engine->rc.acc_rate;
				if(!phvenc_engine->is_scene_change)
					phvenc_engine->avg_dist= enc_engine->avg_dist;
			}
		}




		//----------------------------end calc average statistics (distortion, qp)----------------------------------
		SEM_WAIT(enc_engine->output_wait);//Get output mutex
		
		if(currslice->nalu_type == NALU_CODED_SLICE_IDR_W_RADL)
		{
			hmr_bitstream_init(&enc_engine->hvenc->vps_nalu.bs);
			hmr_bitstream_init(&enc_engine->hvenc->sps_nalu.bs);
			hmr_bitstream_init(&enc_engine->hvenc->pps_nalu.bs);

			enc_engine->hvenc->vps_nalu.nal_unit_type = NALU_TYPE_VPS;
			enc_engine->hvenc->vps_nalu.temporal_id = enc_engine->hvenc->vps_nalu.rsvd_zero_bits = 0;
			output_nalu_list[output_nalu_cnt++] = &enc_engine->hvenc->vps_nalu;
			hmr_put_vps_header(enc_engine->hvenc);//vps header

			enc_engine->hvenc->sps_nalu.nal_unit_type = NALU_TYPE_SPS;
			enc_engine->hvenc->sps_nalu.temporal_id = enc_engine->hvenc->sps_nalu.rsvd_zero_bits = 0;
			output_nalu_list[output_nalu_cnt++] = &enc_engine->hvenc->sps_nalu;
			hmr_put_seq_header(enc_engine->hvenc);//seq header

			enc_engine->hvenc->pps_nalu.nal_unit_type = NALU_TYPE_PPS;
			enc_engine->hvenc->pps_nalu.temporal_id = enc_engine->hvenc->pps_nalu.rsvd_zero_bits = 0;
			output_nalu_list[output_nalu_cnt++] = &enc_engine->hvenc->pps_nalu;
			hmr_put_pic_header(enc_engine->hvenc);//pic header
		}
		//slice header
		enc_engine->slice_nalu->nal_unit_type = currslice->nalu_type;
		enc_engine->slice_nalu->temporal_id = enc_engine->slice_nalu->rsvd_zero_bits = 0;
		output_nalu_list[output_nalu_cnt++] = enc_engine->slice_nalu;

		if(currslice->poc==1)
		{
			int iiiii=0;
		}
		hmr_put_slice_header(enc_engine, currslice);//slice header
		if(enc_engine->wfpp_enable)
			hmr_slice_header_code_wfpp_entry_points(enc_engine);
		hmr_bitstream_rbsp_trailing_bits(&enc_engine->slice_bs);

		for(i=0;i<enc_engine->num_sub_streams;i++)
		{
			memcpy(&enc_engine->slice_bs.bitstream[enc_engine->slice_bs.streambytecnt], enc_engine->aux_bs[i].bitstream, enc_engine->aux_bs[i].streambytecnt);
			enc_engine->slice_bs.streambytecnt += enc_engine->aux_bs[i].streambytecnt;
		}

		//write the slice nalu
		hmr_bitstream_put_nal_unit_header(&enc_engine->slice_nalu->bs, currslice->nalu_type, currslice->sublayer, 0);
		hmr_bitstream_nalu_ebsp(&enc_engine->slice_bs,&enc_engine->slice_nalu->bs);

#ifdef WRITE_REF_FRAMES
		wnd_write2file(&enc_engine->curr_reference_frame->img);
#endif

//		enc_engine->num_encoded_frames++;
#ifdef DBG_TRACE_RESULTS
		{
			char stringI[] = "I";
			char stringP[] = "P";
			char stringB[] = "B";
			char *frame_type_str;
			uint poc = currpict->img2encode->temp_info.poc;
			frame_type_str=currpict->img2encode->img_type==IMAGE_I?stringI:currpict->img2encode->img_type==IMAGE_P?stringP:stringB;

			printf("\r\nengine:%d, POC:%d, %s, bits:%d,", enc_engine->index,poc, frame_type_str, enc_engine->slice_bs.streambytecnt*8);
			//printf("\r\nmodule:%d, frame:%d, %s, target:%.0f, bits:%d,", enc_engine->index,enc_engine->num_encoded_frames, frame_type_str, enc_engine->rc.target_pict_size, enc_engine->slice_bs.streambytecnt*8);
#ifdef COMPUTE_METRICS

			homer_psnr(&enc_engine->current_pict, &enc_engine->curr_reference_frame->img, enc_engine->pict_width, enc_engine->pict_height, enc_engine->current_psnr); 
			enc_engine->accumulated_psnr[0] += enc_engine->current_psnr[Y_COMP];
			enc_engine->accumulated_psnr[1] += enc_engine->current_psnr[U_COMP];
			enc_engine->accumulated_psnr[2] += enc_engine->current_psnr[V_COMP];

			printf("PSNRY: %.2f, PSNRU: %.2f,PSNRV: %.2f, ", enc_engine->current_psnr[Y_COMP], enc_engine->current_psnr[U_COMP], enc_engine->current_psnr[V_COMP]);
			printf("Average PSNRY: %.2f, PSNRU: %.2f,PSNRV: %.2f, ", enc_engine->accumulated_psnr[Y_COMP]/(enc_engine->num_encoded_frames+1), enc_engine->accumulated_psnr[U_COMP]/(enc_engine->num_encoded_frames+1), enc_engine->accumulated_psnr[V_COMP]/(enc_engine->num_encoded_frames+1));
#endif
			printf("vbv: %.2f, ", enc_engine->rc.vbv_fullness/enc_engine->rc.vbv_size);
//			printf("avg_dist: %.2f, ", enc_engine->avg_dist);
//			printf("sao_mode:[%d,%d,%d],sao_type:[%d,%d,%d,%d,%d] lambda:%.2f, ", enc_engine->sao_debug_mode[0],enc_engine->sao_debug_mode[1],enc_engine->sao_debug_mode[2],enc_engine->sao_debug_type[0],enc_engine->sao_debug_type[1],enc_engine->sao_debug_type[2],enc_engine->sao_debug_type[3],enc_engine->sao_debug_type[4], enc_engine->sao_lambdas[0]);
//			printf("rc.target_pict_size: %.2f", enc_engine->rc.target_pict_size);
//#ifndef COMPUTE_AS_HM
			printf("qp: %d, ", enc_engine->pict_qp);			
//#endif
//			printf("pts: %d, ", enc_engine->current_pict.img2encode->temp_info.pts);			

			if(currpict->img2encode->img_type!=IMAGE_I)
			{
				int list, i;
				
				for(list=0;list<2;list++)
				{
					printf("[L%d:", list);
					for(i=0;i<currslice->num_ref_idx[list];i++)
					{
						printf(" %d,", currslice->ref_pic_list[list][i]->temp_info.poc);						
					}
					printf("] ");
				}
			}

			fflush(stdout);
		}
#endif
		//prunning of references must be done in a selective way
//		if(enc_engine->reference_picture_buffer[enc_engine->reference_list_index]!=NULL)
//			cont_put(enc_engine->cont_empty_reference_wnds,enc_engine->reference_picture_buffer[enc_engine->reference_list_index]);

//#ifdef COMPUTE_AS_HM
//		reference_picture_border_padding(&enc_engine->curr_reference_frame->img);
//#endif
		//fill padding in reference picture
//		reference_picture_border_padding(&enc_engine->curr_reference_frame->img);
//		enc_engine->reference_picture_buffer[enc_engine->reference_list_index] = enc_engine->curr_reference_frame;
//		enc_engine->reference_list_index = (enc_engine->reference_list_index+1)&MAX_NUM_REF_MASK;

		ouput_sets->pts = enc_engine->current_pict.img2encode->temp_info.pts;
		ouput_sets->image_type = enc_engine->current_pict.img2encode->img_type;
		
		put_available_frame(enc_engine->hvenc, enc_engine->current_pict.img2encode);

		ouput_sets->num_nalus = output_nalu_cnt;
		ouput_sets->frame = enc_engine->curr_reference_frame;
		cont_put(enc_engine->hvenc->output_hmr_container, ouput_sets);

		enc_engine->num_encoded_frames_in_engine++;

		SEM_POST(enc_engine->output_signal);//Leave output mutex
	}

	return THREAD_RETURN;
}

