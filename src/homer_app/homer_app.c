/*****************************************************************************
* homer_app.c : homerHEVC example application
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
#include <stdlib.h>
//#include <process.h>
#include <malloc.h>
#include <math.h>
#include <homer_hevc_enc_api.h>
#include <string.h>

#ifdef _MSC_VER
#include <Windows.h>
#else
#include <sys/time.h>
#endif

//#define FILE_IN  "//home//juan//Patrones//720p5994_parkrun_ter.yuv"
#define FILE_IN  "C:\\Patrones\\720p5994_parkrun_ter.yuv"
//#define FILE_IN  "C:\\Patrones\\demo_multiple64_384x256_v2.yuv"
//#define FILE_IN  "C:\\Patrones\\1080p_pedestrian_area.yuv"
#define FILE_OUT  "output.265"//"output_32_.265"

#define HOR_SIZE	1280//1920//1280//(2*192)//1280//720//(2*192)//(192+16)//720//320//720
#define VER_SIZE	720//1080//720//(2*128)//720//576//(2*128)//(128+16)//320//576


#ifdef _MSC_VER
unsigned int get_ms()
{
	return GetTickCount();
}
#else
unsigned int get_ms()
{
	struct timeval tv;
	if(gettimeofday(&tv, NULL) != 0)
			return 0;

	return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}
#endif


char file_in_name[256];
char file_out_name[256];


void print_help()
{
	printf("\r\nhomer_app [-option] [value]...\r\n");

	printf("options:\r\n");
	printf("-h: \t\t\t help\r\n");
	printf("-i: \t\t\t input yuv file\r\n");
	printf("-o: \t\t\t output 265 file\r\n");
	printf("-widthxheight: \t\t default = 1280x720\r\n");
	printf("-cu_size: \t\t cu size[16,32 or 64], default = 64\r\n");
	printf("-qp: \t\t\t fixed qp[0-51], default = 32\r\n");
	printf("-n_wpp_threads: \t 0:no wpp, >0-number of wpp threads, default = 1\r\n");	
	printf("-max_intra_pred_depth: \t [0-4], default = 4\r\n");
	printf("-max_intra_tr_depth: \t [0-4], default = 4\r\n");
	printf("-sign_hiding: \t\t 0=off, 1=on, default = 1\r\n");
	printf("-performance_mode: \t 0=full computation, 1=fast , 2= ultra fast\r\n");
	printf("-rd: \t\t\t 0=off, 1=full rd , 2= fast rd\r\n");
	printf("-n_frames: \t\t\t default 40\r\n");

	printf("\r\nexamples:\r\n\r\n");
	printf("homer_app -i /home/juan/Patrones/720p5994_parkrun_ter.yuv -o output0.265 -widthxheight 1280x720 -n_wpp_threads 10 -performance_mode 2 -rd_mode 2 -n_frames 40\r\n");
	printf("homer_app -i /home/juan/Patrones/720p5994_parkrun_ter.yuv -o output0.265 -widthxheight 1280x720 -n_wpp_threads 10 -performance_mode 1 -rd_mode 2 -n_frames 40\r\n");
	printf("homer_app -i /home/juan/Patrones/720p5994_parkrun_ter.yuv -o output0.265 -widthxheight 1280x720 -n_wpp_threads 10 -performance_mode 1 -rd_mode 1 -n_frames 40\r\n");
}




void parse_args(int argc, char* argv[], HVENC_Cfg *cfg, int *num_frames)
{
	int args_parsed = 0;
	args_parsed = 1;

/*	if(argc==1)
	{
		printf ("\r\ntype -h for extended help");
		exit(0);
	}
*/
	while(args_parsed<argc)
	{
		if(strcmp(argv[args_parsed] , "-h")==0)//input
		{
			print_help();
			exit(0);
		}
		else if(strcmp(argv[args_parsed] , "-i")==0 && args_parsed+1<argc)//input
		{
			args_parsed++;
			strcpy(file_in_name, argv[args_parsed++]);
		}
		else if(strcmp(argv[args_parsed], "-o")==0 && args_parsed+1<argc)//output
		{
			args_parsed++;
			strcpy(file_out_name, argv[args_parsed++]);
		}
		else if(strcmp(argv[args_parsed], "-widthxheight")==0 && args_parsed+1<argc)//720x576, 1280x720, 1920x1080.... Multiple of 16
		{
			args_parsed++;
			sscanf( argv[args_parsed++], "%dx%d", &cfg->width, &cfg->height);
		}
		else if(strcmp(argv[args_parsed], "-cu_size")==0 && args_parsed+1<argc)//cu_size: 64, 32, 16
		{
			args_parsed++;
			sscanf( argv[args_parsed++], "%d", &cfg->cu_size);
		}
		else if(strcmp(argv[args_parsed], "-qp")==0 && args_parsed+1<argc)//quant
		{
			args_parsed++;
			sscanf( argv[args_parsed++], "%d", &cfg->qp );
		}
		else if(strcmp(argv[args_parsed], "-n_wpp_threads")==0 && args_parsed+1<argc)//number of threads
		{
			args_parsed++;
			sscanf( argv[args_parsed++], "%d", &cfg->wfpp_num_threads );
			if(cfg->wfpp_num_threads==0)
				cfg->wfpp_enable = 0;
			else
				cfg->wfpp_enable = 1;
		}
		else if(strcmp(argv[args_parsed], "-max_intra_pred_depth")==0 && args_parsed+1<argc)//depth of intra prediction, default 4
		{
			args_parsed++;
			sscanf( argv[args_parsed++], "%d", &cfg->max_pred_partition_depth);
		}
		else if(strcmp(argv[args_parsed], "-max_intra_tr_depth")==0 && args_parsed+1<argc)//transform of intra prediction, default 4
		{
			args_parsed++;
			sscanf( argv[args_parsed++], "%d", &cfg->max_intra_tr_depth);
		}
		else if(strcmp(argv[args_parsed], "-max_inter_tr_depth")==0 && args_parsed+1<argc)//transform of inter prediction, default 4
		{
			args_parsed++;

			sscanf( argv[args_parsed++], "%d", &cfg->max_inter_tr_depth);
		}
		else if(strcmp(argv[args_parsed], "-sign_hiding")==0 && args_parsed+1<argc)//sign_hiding, default 1
		{
			args_parsed++;
			sscanf( argv[args_parsed++], "%d", &cfg->sign_hiding);
		}
		else if(strcmp(argv[args_parsed], "-performance_mode")==0 && args_parsed+1<argc)//performance_mode:	0 = accurate+high quality, 1-quality-performance-balaced 2= fast				//0=FULL_COMPUTATION, 1=SELECTIVE_FULL_COMPUTATION, 2=FAST_PRED_MODE
		{
			args_parsed++;
			sscanf( argv[args_parsed++], "%d", &cfg->performance_mode);
		}
		else if(strcmp(argv[args_parsed], "-rd_mode")==0 && args_parsed+1<argc)//rd_mode: 0=OFF, 1=FULL_RD, 2=FAST_RD
		{
			args_parsed++;
			sscanf( argv[args_parsed++], "%d", &cfg->rd_mode);
		}
		else if(strcmp(argv[args_parsed], "-n_frames")==0 && args_parsed+1<argc)//num_frames
		{
			args_parsed++;
			sscanf( argv[args_parsed++], "%d", num_frames);
		}
	}
}



int main (int argc, char **argv)
{
	int totalbits=0;
	unsigned int msInit=0, msTotal=0;
	int bCoding = 1;
	int input_frames = 0, encoded_frames = 0;
	FILE *infile, *outfile;
	int num_frames = 40;

	unsigned char *frame[3];
	stream_t stream;
	encoder_in_out_t in, out;

	void *pEncoder;

	nalu_t  *nalu_out[8];
	unsigned int num_nalus = 8;

	HVENC_Cfg	HmrCfg;



	strcpy(file_in_name, FILE_IN);
	strcpy(file_out_name, FILE_OUT);

	HmrCfg.size = sizeof(HmrCfg);
	HmrCfg.width = HOR_SIZE;
	HmrCfg.height = VER_SIZE;
	HmrCfg.profile = PROFILE_MAIN;
	HmrCfg.M = 1;
	HmrCfg.N = 1;
	HmrCfg.qp = 32;
	HmrCfg.frame_rate = 25;
	HmrCfg.num_ref_frames = 2;
	HmrCfg.cu_size = 64;
	HmrCfg.max_pred_partition_depth = 4;
	HmrCfg.max_intra_tr_depth = 4;
	HmrCfg.max_inter_tr_depth = 3;
	HmrCfg.wfpp_enable = 1;
	HmrCfg.wfpp_num_threads = 10;
	HmrCfg.sign_hiding = 1;
	HmrCfg.rd_mode = 2;			//0 no rd, 1 similar to HM, 2 fast
	HmrCfg.performance_mode = 2;//0 full computation(HM), 1 = fast decission (rd=1 or rd=2), 2 = ultra fast decission (rd=2)

	parse_args(argc, argv, &HmrCfg, &num_frames);


	if(!(infile = fopen(file_in_name, "rb")))
	{
		printf("Error opening input file: %s\r\n", file_in_name);
		exit(0);
	}

	if((outfile = fopen(file_out_name, "wb")) == NULL)
	{
		printf("Error opening output file: %s\r\n", file_out_name);
		exit(0);
	}

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	memset(&stream, 0, sizeof(stream));



	stream.streams[0] = (unsigned char *)calloc(HmrCfg.width*HmrCfg.height, 1);
	stream.streams[1] = (unsigned char *)calloc(HmrCfg.width*HmrCfg.height>>1,1);
	stream.streams[2] = (unsigned char *)calloc(HmrCfg.width*HmrCfg.height>>1,1);

	out.stream.streams[0] = (unsigned char *)calloc(0x8000000,1);

	pEncoder = HOMER_enc_init();

	HOMER_enc_control(pEncoder,HENC_SETCFG,&HmrCfg);

	msInit = get_ms();
	while(bCoding)
	{
		frame[0] = (unsigned char*)stream.streams[0];
		frame[1] = (unsigned char*)stream.streams[1];
		frame[2] = (unsigned char*)stream.streams[2];

		fread(frame[0],HmrCfg.width,HmrCfg.height,infile);
		fread(frame[1],HmrCfg.width>>1,HmrCfg.height>>1,infile);
		fread(frame[2],HmrCfg.width>>1,HmrCfg.height>>1,infile);

		in.stream = stream;

		if(bCoding)
		{
			num_nalus = 8;
			HOMER_enc_encode(pEncoder, in.stream.streams);//, nalu_out, &num_nalus);
			printf("\r\ninput_frame %d introducida en HOMER_enc_encode", input_frames);
			fflush(stdout);
			input_frames++;

//			encoder_thread(pEncoder);

			HOMER_enc_get_coded_frame(pEncoder, nalu_out, &num_nalus);
			if(num_nalus>0)
			{
				HOMER_enc_write_annex_b_output(nalu_out, num_nalus, &out);
				fwrite(out.stream.streams[0], sizeof(unsigned char), out.stream.data_size[0], outfile);
				fflush(outfile);
				totalbits+=out.stream.data_size[0];
				encoded_frames++;
			}
			if(encoded_frames==num_frames)
			{
				msTotal += get_ms()-msInit;
				printf("\r\n%d frames en %d milisegundos: %f fps", encoded_frames, msTotal, 1000.0*(encoded_frames)/(double)msTotal);

				HOMER_enc_close(pEncoder);
				bCoding = 0;
			}
		}

		if(!bCoding)
			break;
	}

	fclose(infile);
	fclose(outfile);

	Sleep(1000000);

	return 0;
}