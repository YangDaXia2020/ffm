// dxva2_test.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavfilter/avfilter.h"
#include "libavutil/opt.h"
#include "libavfilter/buffersrc.h"
#include "libavfilter/buffersink.h"
#include "libswscale/swscale.h"
}

/*
威盾视频监控主程序
2019年12月19日
2023 修改自动结束，不再自动重启
zlf
*/

#include  "main.h"
#include <windows.h>          // for HANDLE
#include <process.h>          // for _beginthread()
#include <string>
#include <Auth.h>


using namespace std;
camera maincameral;
camera camera1;
camera camera2;
camera camera3;
//相机切换状态，用于动态切换相机
int cameraState[] = { 1,1,1,1,1,1 };
//轨迹
AVFrame** currentFrame = NULL;
//输出流上下文
AVFormatContext* ic = NULL;
HANDLE hMutexSend = NULL;//互斥量  图像队列互斥锁 
BOOL ServerState = true;
BOOL issuccess = false;
typedef  void(__stdcall* LogCallBack)(const char* msg);
LogCallBack BackCallFun;
DWORD audio_time = 0;
DWORD video_time = 0;
//视频总pts
int64_t vps = 0;

//需要重置
BOOL needReset=false;

unsigned __stdcall GetVideoMain(LPVOID lpParam)
{
RESTART:

	camera p = maincameral;
	unsigned int i;
	int ret;
	int video_st_index = -1;
	int audio_st_index = -1;
	AVPacket pkt;
	AVStream* st = NULL;
	AVDictionary* optionsDict = NULL;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;
	//视频参数
	AVStream* pVst;
	AVCodecContext* pVideoCodecCtx = NULL;
	AVFrame* pFrame = av_frame_alloc();
	AVFormatContext* ifmt_ctx = avformat_alloc_context();
	SwsContext* img_convert_ctx = NULL;
	AVCodec* pVideoCodec = NULL;
	//降低延迟
	ifmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;
	av_dict_set(&optionsDict, "rtsp_transport", "tcp", 0); //采用tcp传输
	av_dict_set(&optionsDict, "stimeout", "2000000", 0); //如果没有设置stimeout，那么把ipc网线拔掉，av_read_frame会阻塞（时间单位是微妙）
	//av_dict_set(&optionsDict, "max_delay", "500", 0);
	ret = avformat_open_input(&ifmt_ctx, p.rtspUrl, NULL, &optionsDict);
	if (ret != 0)
	{
		Sleep(1000);
		BackCallFun("超过2秒未连接到主摄像机avformat_open_input");
		goto EXIT;
	}

	for (i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		st = ifmt_ctx->streams[i];
		switch (st->codec->codec_type)
		{
		case AVMEDIA_TYPE_AUDIO: audio_st_index = i; break;
		case AVMEDIA_TYPE_VIDEO: video_st_index = i; break;
		default: break;
		}
	}
	if (-1 == video_st_index) {
		BackCallFun("主摄像机未找到视频流");
		goto EXIT;
	}
	pVst = ifmt_ctx->streams[video_st_index];
	pVideoCodecCtx = pVst->codec;
	pVideoCodec = avcodec_find_decoder(pVideoCodecCtx->codec_id);
	if (pVideoCodec == NULL)
	{
		BackCallFun("主摄像机未找到任何可用的解码器");
		goto EXIT;
	}
	if (avcodec_open2(pVideoCodecCtx, pVideoCodec, NULL) < 0)
	{
		BackCallFun("初始化音视频解码器失败");
		goto EXIT;
	}

	while (ServerState&& !needReset)
	{
		int read_status;
		do
		{
			read_status = av_read_frame(ifmt_ctx, &pkt);
		} while (read_status == AVERROR(EAGAIN));
		if (read_status < 0)
		{
			BackCallFun("没有读取出任何帧数据0");
		}
		else
		{
			if (pkt.stream_index == video_st_index)
			{
				int got_picture = 0;
				int bytes_used = avcodec_decode_video2(pVideoCodecCtx, pFrame, &got_picture, &pkt);

				if (got_picture)
				{
					SwsContext* scaleCtx = sws_getContext(pVideoCodecCtx->width, pVideoCodecCtx->height, AV_PIX_FMT_YUV420P,
						p.width, p.height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
					if (scaleCtx == NULL)
					{
						BackCallFun("图像缩放初始化失败");
						break;
					}
					if (sws_scale(scaleCtx,
						pFrame->data, pFrame->linesize,
						0,
						pFrame->height,
						currentFrame[0]->data, currentFrame[0]->linesize) <= 0)
					{
						BackCallFun("图像缩放失败");
						break;
					}
					sws_freeContext(scaleCtx);
					currentFrame[0]->pkt_dts = pkt.dts;
					currentFrame[0]->pkt_pts = pkt.pts;
					currentFrame[0]->pkt_duration = pkt.duration;
					currentFrame[0]->pkt_pos = pkt.pos;
					video_time = GetTickCount();
				}
			}
			else if (pkt.stream_index == audio_st_index)
			{
				if (ifmt_ctx != NULL)
				{
					if (pkt.pts != AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE)
					{
						WaitForSingleObject(hMutexSend, INFINITE);
						audio_time = GetTickCount64();
						if (ic != NULL && issuccess)
						{
							try
							{
								ret = av_interleaved_write_frame(ic, &pkt);
							}
							catch (const std::exception&)
							{

							}

						}
						ReleaseMutex(hMutexSend);
					}
				}
			}
			av_packet_unref(&pkt);
		}
	}
EXIT:
	avcodec_close(pVideoCodecCtx);
	avformat_close_input(&ifmt_ctx);
	avformat_free_context(ifmt_ctx);
	av_frame_free(&pFrame);
	av_free_packet(&pkt);
	if (ServerState)
	{
		needReset = false;
		cameraState[camera1.index] = 1;
		goto RESTART;
	}
	return 0;
}

unsigned __stdcall GetVideoChangDiOne(LPVOID lpParam)
{
RESTART:
	camera p;
	p.height = camera1.height;
	p.width = camera1.width;
	p.rtspUrl = camera1.rtspUrl;
	p.index = camera1.index;
	unsigned int i;
	int ret;
	int video_st_index = -1;
	int audio_st_index = -1;
	AVPacket pkt;
	AVStream* st = NULL;
	AVDictionary* optionsDict = NULL;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;
	//视频参数
	AVStream* pVst;
	AVCodecContext* pVideoCodecCtx = NULL;
	AVFrame* pFrame = av_frame_alloc();
	AVFormatContext* ifmt_ctx = avformat_alloc_context();
	SwsContext* img_convert_ctx = NULL;
	AVCodec* pVideoCodec = NULL;
	uint8_t* m_yuvBuffer = NULL;

	//降低延迟
	ifmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;
	av_dict_set(&optionsDict, "rtsp_transport", "tcp", 0); //采用tcp传输
	av_dict_set(&optionsDict, "stimeout", "2000000", 0); //如果没有设置stimeout，那么把ipc网线拔掉，av_read_frame会阻塞（时间单位是微妙）
	//av_dict_set(&optionsDict, "max_delay", "500", 0);
	ret = avformat_open_input(&ifmt_ctx, p.rtspUrl, NULL, &optionsDict);
	BackCallFun(p.rtspUrl);
	if (ret != 0)
	{
		Sleep(1000);
		BackCallFun("超过2秒未连接到场地1摄像机avformat_open_input");
		goto EXIT;
	}
	for (i = 0; i < ifmt_ctx->nb_streams; i++) {// dump information
		av_dump_format(ifmt_ctx, i, p.rtspUrl, 0);
	}


	for (i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		st = ifmt_ctx->streams[i];
		switch (st->codec->codec_type)
		{
		case AVMEDIA_TYPE_AUDIO: audio_st_index = i; break;
		case AVMEDIA_TYPE_VIDEO: video_st_index = i; break;
		default: break;
		}
	}
	if (-1 == video_st_index) {
		BackCallFun("场地1摄像机未找到视频流");
		goto EXIT;
	}
	pVst = ifmt_ctx->streams[video_st_index];
	pVideoCodecCtx = pVst->codec;
	pVideoCodec = avcodec_find_decoder(pVideoCodecCtx->codec_id);
	if (pVideoCodec == NULL)
	{
		BackCallFun("场地1摄像机未找到任何可用的解码器");
		goto EXIT;
	}
	if (avcodec_open2(pVideoCodecCtx, pVideoCodec, NULL) < 0)
	{
		BackCallFun("初始化音视频解码器失败");
		goto EXIT;
	}



	while (cameraState[camera1.index] && ServerState)
	{
		int read_status;
		do
		{
			read_status = av_read_frame(ifmt_ctx, &pkt);
		} while (read_status == AVERROR(EAGAIN));
		if (read_status < 0)
		{
			BackCallFun("没有读取出任何帧数据1");
			av_free_packet(&pkt);
			break;
		}
		if (pkt.stream_index == video_st_index)
		{
			int got_picture = 0;
			int bytes_used = avcodec_decode_video2(pVideoCodecCtx, pFrame, &got_picture, &pkt);
			if (got_picture)
			{
				SwsContext* scaleCtx = sws_getContext(pVideoCodecCtx->width, pVideoCodecCtx->height, AV_PIX_FMT_YUV420P,
					p.width, p.height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
				if (scaleCtx == NULL)
				{
					BackCallFun("图像缩放初始化失败");
					break;
				}
				if (sws_scale(scaleCtx,
					pFrame->data, pFrame->linesize,
					0,
					pFrame->height,
					currentFrame[1]->data, currentFrame[1]->linesize) <= 0)
				{
					BackCallFun("图像缩放失败");
					break;
				}
				sws_freeContext(scaleCtx);
			}
		}
		av_packet_unref(&pkt);
	}
EXIT:
	avcodec_close(pVideoCodecCtx);
	avformat_close_input(&ifmt_ctx);
	avformat_free_context(ifmt_ctx);
	av_frame_free(&pFrame);
	av_free_packet(&pkt);
	av_freep(&pkt);
	if (ServerState)
	{
		cameraState[camera1.index] = 1;
		goto RESTART;
	}
	return 0;
}

unsigned __stdcall GetVideoChangDiTwo(LPVOID lpParam)
{
RESTART:
	camera p;
	p.height = camera2.height;
	p.width = camera2.width;
	p.rtspUrl = camera2.rtspUrl;
	p.index = camera2.index;
	unsigned int i;
	int ret;
	int video_st_index = -1;
	int audio_st_index = -1;
	AVPacket pkt;
	AVStream* st = NULL;
	AVDictionary* optionsDict = NULL;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;
	//视频参数
	AVStream* pVst;
	AVCodecContext* pVideoCodecCtx = NULL;
	AVFrame* pFrame = av_frame_alloc();
	AVFormatContext* ifmt_ctx = avformat_alloc_context();
	SwsContext* img_convert_ctx = NULL;
	AVCodec* pVideoCodec = NULL;

	uint8_t* m_yuvBuffer = NULL;

	//降低延迟
	ifmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;
	av_dict_set(&optionsDict, "rtsp_transport", "tcp", 0); //采用tcp传输
	av_dict_set(&optionsDict, "stimeout", "2000000", 0); //如果没有设置stimeout，那么把ipc网线拔掉，av_read_frame会阻塞（时间单位是微妙）
	//av_dict_set(&optionsDict, "max_delay", "500", 0);
	ret = avformat_open_input(&ifmt_ctx, p.rtspUrl, NULL, &optionsDict);
	BackCallFun(p.rtspUrl);
	if (ret != 0)
	{
		Sleep(1000);
		BackCallFun("超过2秒未连接到场地2摄像机avformat_open_input");
		goto EXIT;
	}

	//很重要，每次打开摄像头都会检测抽取一段到缓存分析格式，这里设置抽取的大小和时间，降低打开摄像头的延迟
	//ifmt_ctx->probesize = 500 * 1024;
	//ifmt_ctx->max_analyze_duration = 0.1 * AV_TIME_BASE;//AV_TIME_BASE是定义的时间标准，代表1秒
	//if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {// Get information on the input file (number of streams etc.).
	//	BackCallFun("场地2摄像机分析视频编码内容失败avformat_find_stream_info");
	//	goto EXIT;
	//}

	for (i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		st = ifmt_ctx->streams[i];
		switch (st->codec->codec_type)
		{
		case AVMEDIA_TYPE_AUDIO: audio_st_index = i; break;
		case AVMEDIA_TYPE_VIDEO: video_st_index = i; break;
		default: break;
		}
	}
	if (-1 == video_st_index) {
		BackCallFun("场地2摄像机未找到视频流");
		goto EXIT;
	}
	pVst = ifmt_ctx->streams[video_st_index];
	pVideoCodecCtx = pVst->codec;
	pVideoCodec = avcodec_find_decoder(pVideoCodecCtx->codec_id);
	if (pVideoCodec == NULL)
	{
		BackCallFun("场地2摄像机未找到任何可用的解码器");
		goto EXIT;
	}
	if (avcodec_open2(pVideoCodecCtx, pVideoCodec, NULL) < 0)
	{
		BackCallFun("初始化音视频解码器失败");
		goto EXIT;
	}
	while (cameraState[camera2.index] && ServerState)
	{
		int read_status;
		do
		{
			read_status = av_read_frame(ifmt_ctx, &pkt);
		} while (read_status == AVERROR(EAGAIN));
		if (read_status < 0)
		{
			BackCallFun("没有读取出任何帧数据2");
			av_free_packet(&pkt);
			break;
		}
		if (pkt.stream_index == video_st_index)
		{
			int got_picture = 0;
			int bytes_used = avcodec_decode_video2(pVideoCodecCtx, pFrame, &got_picture, &pkt);
			if (got_picture)
			{
				SwsContext* scaleCtx = sws_getContext(pVideoCodecCtx->width, pVideoCodecCtx->height, AV_PIX_FMT_YUV420P,
					p.width, p.height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
				if (scaleCtx == NULL)
				{
					BackCallFun("图像缩放初始化失败");
					break;
				}
				if (sws_scale(scaleCtx,
					pFrame->data, pFrame->linesize,
					0,
					pFrame->height,
					currentFrame[2]->data, currentFrame[2]->linesize) <= 0)
				{
					BackCallFun("图像缩放失败");
					break;
				}
				sws_freeContext(scaleCtx);
			}
		}
		av_packet_unref(&pkt);
	}

EXIT:
	avcodec_close(pVideoCodecCtx);
	avformat_close_input(&ifmt_ctx);
	avformat_free_context(ifmt_ctx);
	av_frame_free(&pFrame);
	av_free_packet(&pkt);
	av_freep(&pkt);

	if (ServerState)
	{
		cameraState[camera2.index] = 1;
		goto RESTART;
	}
	return 0;
}

unsigned __stdcall GetVideoChangDiThress(LPVOID lpParam)
{
RESTART:
	camera p;
	p.height = camera3.height;
	p.width = camera3.width;
	p.rtspUrl = camera3.rtspUrl;
	p.index = camera3.index;
	unsigned int i;
	int ret;
	int video_st_index = -1;
	int audio_st_index = -1;
	AVPacket pkt;
	AVStream* st = NULL;
	AVDictionary* optionsDict = NULL;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;
	//视频参数
	AVStream* pVst;
	AVCodecContext* pVideoCodecCtx = NULL;
	AVFrame* pFrame = av_frame_alloc();
	AVFormatContext* ifmt_ctx = avformat_alloc_context();
	SwsContext* img_convert_ctx = NULL;
	AVCodec* pVideoCodec = NULL;

	uint8_t* m_yuvBuffer = NULL;

	//降低延迟
	ifmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;
	av_dict_set(&optionsDict, "rtsp_transport", "tcp", 0); //采用tcp传输
	av_dict_set(&optionsDict, "stimeout", "2000000", 0); //如果没有设置stimeout，那么把ipc网线拔掉，av_read_frame会阻塞（时间单位是微妙）
	//av_dict_set(&optionsDict, "max_delay", "500", 0);
	ret = avformat_open_input(&ifmt_ctx, p.rtspUrl, NULL, &optionsDict);
	BackCallFun(p.rtspUrl);
	if (ret != 0)
	{
		Sleep(1000);
		BackCallFun("超过2秒未连接到场地3摄像机avformat_open_input");
		goto EXIT;
	}

	//很重要，每次打开摄像头都会检测抽取一段到缓存分析格式，这里设置抽取的大小和时间，降低打开摄像头的延迟
	//ifmt_ctx->probesize = 500 * 1024;
	//ifmt_ctx->max_analyze_duration = 0.1 * AV_TIME_BASE;//AV_TIME_BASE是定义的时间标准，代表1秒
	//if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {// Get information on the input file (number of streams etc.).
	//	BackCallFun("场地2摄像机分析视频编码内容失败avformat_find_stream_info");
	//	goto EXIT;
	//}

	for (i = 0; i < ifmt_ctx->nb_streams; i++)
	{
		st = ifmt_ctx->streams[i];
		switch (st->codec->codec_type)
		{
		case AVMEDIA_TYPE_AUDIO: audio_st_index = i; break;
		case AVMEDIA_TYPE_VIDEO: video_st_index = i; break;
		default: break;
		}
	}
	if (-1 == video_st_index) {
		BackCallFun("场地3摄像机未找到视频流");
		goto EXIT;
	}
	pVst = ifmt_ctx->streams[video_st_index];
	pVideoCodecCtx = pVst->codec;
	pVideoCodec = avcodec_find_decoder(pVideoCodecCtx->codec_id);
	if (pVideoCodec == NULL)
	{
		BackCallFun("场地3摄像机未找到任何可用的解码器");
		goto EXIT;
	}
	if (avcodec_open2(pVideoCodecCtx, pVideoCodec, NULL) < 0)
	{
		BackCallFun("初始化音视频解码器失败");
		goto EXIT;
	}
	while (cameraState[camera3.index] && ServerState)
	{
		int read_status;
		do
		{
			read_status = av_read_frame(ifmt_ctx, &pkt);
		} while (read_status == AVERROR(EAGAIN));
		if (read_status < 0)
		{
			BackCallFun("没有读取出任何帧数据3");
			av_free_packet(&pkt);
			break;
		}
		if (pkt.stream_index == video_st_index)
		{
			int got_picture = 0;
			int bytes_used = avcodec_decode_video2(pVideoCodecCtx, pFrame, &got_picture, &pkt);
			if (got_picture)
			{
				SwsContext* scaleCtx = sws_getContext(pVideoCodecCtx->width, pVideoCodecCtx->height, AV_PIX_FMT_YUV420P,
					p.width, p.height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
				if (scaleCtx == NULL)
				{
					BackCallFun("图像缩放初始化失败");
					break;
				}
				if (sws_scale(scaleCtx,
					pFrame->data, pFrame->linesize,
					0,
					pFrame->height,
					currentFrame[3]->data, currentFrame[3]->linesize) <= 0)
				{
					BackCallFun("图像缩放失败");
					break;
				}
				sws_freeContext(scaleCtx);
			}
		}
		av_packet_unref(&pkt);
	}

EXIT:
	avcodec_close(pVideoCodecCtx);
	avformat_close_input(&ifmt_ctx);
	avformat_free_context(ifmt_ctx);
	av_frame_free(&pFrame);
	av_free_packet(&pkt);
	av_freep(&pkt);

	if (ServerState)
	{
		cameraState[camera3.index] = 1;
		goto RESTART;
	}
	return 0;
}

//发送包到上下文
int sendRtsp(char* outUrl, int inWidth, int inHeight)
{
	//编码器上下文
	AVCodecContext* vc = NULL;
	//编码器上下文（audio）
	AVCodecContext* audioContext = NULL;
	//添加视频流 
	AVStream* vs = NULL;
	int fps = 20;
	//输出的数据结构
	AVFrame* yuv = NULL;
	AVPacket pack;
	AVPacket packvideo;
	int err22 = 0;

	try
	{
		BackCallFun(outUrl);
		int ret = 0;
		//找到视频编码器
		AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!codec)
		{
			BackCallFun("未找到视频编码器");
			goto EXIT;
		}
		//找到音频编码器
		AVCodec* codecAudio = avcodec_find_encoder(AV_CODEC_ID_PCM_ALAW);
		if (!codecAudio)
		{
			BackCallFun("未找到音频编码器");
			goto EXIT;
		}
		//创建视频编码器上下文
		vc = avcodec_alloc_context3(codec);
		//创建音频编码器上下文
		audioContext = avcodec_alloc_context3(codecAudio);
		if (!vc || !audioContext)
		{
			BackCallFun("创建音视频编码器上下文");
			goto EXIT;
		}

		vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_LOW_DELAY;
		vc->codec_id = codec->id;
		vc->pix_fmt = AV_PIX_FMT_YUV420P;
		vc->codec_type = AVMEDIA_TYPE_VIDEO;
		vc->width = inWidth;
		vc->height = inHeight;
		vc->framerate = { fps, 1 };
		vc->time_base = { 1,fps };
		//vc->bit_rate = 512000;
		//vc->rc_max_rate = 50000;//最大码流偏差
		//vc->rc_min_rate = 50000;//最小码流偏差
		av_opt_set(vc->priv_data, "preset", "ultrafast", 0);    //快速编码，但会损失质量
		//av_opt_set(vc->priv_data, "tune", "zerolatency", 0);  //适用于快速编码和低延迟流式传输,但是会出现绿屏
		av_opt_set_int(vc->priv_data, "crf", 50, 0);  //0-51
		vc->gop_size = 50;
		vc->max_b_frames = 0;
		vc->qmin = 16;
		vc->qmax = 26;
		vc->refs = 2;	//运动补偿
		vc->mb_decision = 1;
		vc->scenechange_threshold = 40; //场景切换检测阈
		vc->delay = 0;

		//Audio: pcm_alaw, 8000 Hz, 1 channels, s16, 64 kb / s
		audioContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; //全局参数
		audioContext->sample_fmt = AV_SAMPLE_FMT_S16;
		audioContext->codec_id = codecAudio->id;
		audioContext->codec_type = AVMEDIA_TYPE_AUDIO;
		audioContext->channels = 1;
		audioContext->channel_layout = av_get_default_channel_layout(1);
		audioContext->sample_rate = 8000;

		//d 打开编码器上下文
		ret = avcodec_open2(vc, 0, 0);
		if (ret != 0)
		{
			char buf[1024] = { 0 };
			av_strerror(ret, buf, sizeof(buf) - 1);
			BackCallFun(buf);
			BackCallFun("编码器打开失败");
			goto EXIT;
		}
		//输出封装器和视频流配置
		//a 创建输出封装器上下文
		ret = avformat_alloc_output_context2(&ic, NULL, "rtsp", outUrl);
		if (ret != 0 || !ic)
		{
			char buf[1024] = { 0 };
			av_strerror(ret, buf, sizeof(buf) - 1);
			BackCallFun(buf);
			BackCallFun("创建输出视频头失败");
			goto EXIT;
		}
		//b 添加视频流 
		vs = avformat_new_stream(ic, NULL);
		if (!vs)
		{
			BackCallFun("添加视频流失败");
			goto EXIT;
		}
		avcodec_parameters_from_context(vs->codecpar, vc);
		//b 添加音频流 
		vs = avformat_new_stream(ic, NULL);
		if (!vs)
		{
			BackCallFun("添加音频流失败");
			goto EXIT;
		}
		avcodec_parameters_from_context(vs->codecpar, audioContext);

		///打开rtsp 的网络输出IO
		ret = avio_open(&ic->pb, outUrl, 0);
		av_dump_format(ic, 0, outUrl, 1);
		if (ret != 0)
		{
			char buf[1024] = { 0 };
			av_strerror(ret, buf, sizeof(buf) - 1);
			BackCallFun("打开输出rtsp服务失败");
			BackCallFun(std::to_string(ret).c_str());
			goto EXIT;
		}
		//检测视频等待时间
		ic->max_interleave_delta = 0;
		//写入封装头
		AVDictionary* dict = NULL;
		av_dict_set(&dict, "stimeout", std::to_string(5000000).c_str(), 0);
		av_dict_set(&dict, "rtsp_transport", "tcp", 0);
		av_dict_set(&dict, "muxdelay", "0", 0);
		av_dict_set(&dict, "preset", "ultrafast", 0);
		av_dict_set(&dict, "tune", "zerolatency", 0);
		av_dict_set(&dict, "thread_queue_size", "8", 0);
		//av_dict_set(&dict, "crf", "50", 0);
		av_dict_set(&dict, "dts_delta_threshold", "100000", 0);
		av_dict_set(&dict, "rtbufsize", "0", 0);
		av_dict_set(&dict, "start_time_realtime", 0, 0);


		av_dict_set(&dict, "sample_rate", "8000", 0); // 使用audioDict来设置音频参数
		av_dict_set(&dict, "sample_fmt", "s16", 0);
		av_dict_set(&dict, "channels", "1", 0);

		ret = avformat_write_header(ic, &dict);
		if (ret != 0)
		{
			char buf[1024] = { 0 };
			av_strerror(ret, buf, sizeof(buf) - 1);
			BackCallFun("写入头文件失败");

			BackCallFun(std::to_string(ret).c_str());
			goto EXIT;
		}
		memset(&pack, 0, sizeof(pack));
		memset(&packvideo, 0, sizeof(packvideo));
		issuccess = true;


		yuv = av_frame_alloc();
		int nDstSize = avpicture_get_size(AV_PIX_FMT_YUV420P, inWidth, inHeight);
		uint8_t* dstbuf = new uint8_t[nDstSize];
		avpicture_fill((AVPicture*)yuv, dstbuf, AV_PIX_FMT_YUV420P, inWidth, inHeight);

		yuv->width = inWidth;
		yuv->height = inHeight;
		yuv->format = AV_PIX_FMT_YUV420P;
		memset(yuv->data[0], 0, inWidth * inHeight);
		memset(yuv->data[1], 0x80, inWidth * inHeight / 4);
		memset(yuv->data[2], 0x80, inWidth * inHeight / 4);
		int minRateTime = 0;

		BackCallFun("初始化完成，压入视频流");
		//分配每画面帧的内存
		while (ServerState)
		{
			try
			{
				DWORD start_time = ::GetTickCount64();

				int videoSleep = GetTickCount64() - video_time;
				if (videoSleep > 10000)
				{
					BackCallFun("超过10秒没有收到主驾驶视频");
					goto EXIT;
				}

				if (currentFrame[0]->pkt_pts > vps)
				{
					minRateTime = 0;
					vps = currentFrame[0]->pkt_pts;
					yuv->pts = vps;
					pack.stream_index = vs->index;
					pack.pts = vps;
					pack.dts = vps;
					pack.duration = currentFrame[0]->pkt_duration;
					pack.pos = currentFrame[0]->pkt_pos;
					//开始合并
					int nYIndex = 0;
					int nUVIndex = 0;
					int FRAMEHEIGTH = currentFrame[0]->height;
					int i = 0;
					for (i = 0; i < FRAMEHEIGTH; i++)
					{
						//Y  
						memcpy(yuv->data[0] + i * inWidth, currentFrame[0]->data[0] + nYIndex * currentFrame[0]->width, currentFrame[0]->width);
						memcpy(yuv->data[0] + currentFrame[1]->width + i * inWidth, currentFrame[1]->data[0] + nYIndex * currentFrame[1]->width, currentFrame[1]->width);
						memcpy(yuv->data[0] + 2 * currentFrame[2]->width + i * inWidth, currentFrame[2]->data[0] + nYIndex * currentFrame[2]->width, currentFrame[2]->width);
						memcpy(yuv->data[0] + 3 * currentFrame[2]->width + i * inWidth, currentFrame[3]->data[0] + nYIndex * currentFrame[3]->width, currentFrame[3]->width);
						memcpy(yuv->data[0] + (i + FRAMEHEIGTH) * inWidth, currentFrame[4]->data[0] + nYIndex * inWidth, inWidth);
						nYIndex++;

						if (i < FRAMEHEIGTH / 2)
						{
							memcpy(yuv->data[1] + i * inWidth / 2, currentFrame[0]->data[1] + nUVIndex * currentFrame[0]->width / 2, currentFrame[0]->width / 2);
							memcpy(yuv->data[2] + i * inWidth / 2, currentFrame[0]->data[2] + nUVIndex * currentFrame[0]->width / 2, currentFrame[0]->width / 2);

							memcpy(yuv->data[1] + currentFrame[1]->width / 2 + i * inWidth / 2, currentFrame[1]->data[1] + nUVIndex * currentFrame[1]->width / 2, currentFrame[1]->width / 2);
							memcpy(yuv->data[2] + currentFrame[1]->width / 2 + i * inWidth / 2, currentFrame[1]->data[2] + nUVIndex * currentFrame[1]->width / 2, currentFrame[1]->width / 2);

							memcpy(yuv->data[1] + 2 * currentFrame[2]->width / 2 + i * inWidth / 2, currentFrame[2]->data[1] + nUVIndex * currentFrame[2]->width / 2, currentFrame[2]->width / 2);
							memcpy(yuv->data[2] + 2 * currentFrame[2]->width / 2 + i * inWidth / 2, currentFrame[2]->data[2] + nUVIndex * currentFrame[2]->width / 2, currentFrame[2]->width / 2);

							memcpy(yuv->data[1] + 3 * currentFrame[3]->width / 2 + i * inWidth / 2, currentFrame[3]->data[1] + nUVIndex * currentFrame[3]->width / 2, currentFrame[3]->width / 2);
							memcpy(yuv->data[2] + 3 * currentFrame[3]->width / 2 + i * inWidth / 2, currentFrame[3]->data[2] + nUVIndex * currentFrame[3]->width / 2, currentFrame[3]->width / 2);

							memcpy(yuv->data[1] + (i + FRAMEHEIGTH / 2) * inWidth / 2, currentFrame[4]->data[1] + nUVIndex * inWidth / 2, inWidth / 2);
							memcpy(yuv->data[2] + (i + FRAMEHEIGTH / 2) * inWidth / 2, currentFrame[4]->data[2] + nUVIndex * inWidth / 2, inWidth / 2);
							nUVIndex++;
						}
					}
					ret = avcodec_send_frame(vc, yuv);
					if (ret == 0)
					{
						ret = avcodec_receive_packet(vc, &pack);
					}
					if (ret == 0 && pack.size > 0)
					{
						WaitForSingleObject(hMutexSend, INFINITE);
						if (ic)
						{
							int audioSleep = GetTickCount64() - audio_time;
							if (audioSleep > 10000)
							{
								BackCallFun("超过10秒没有收到音频");
								goto EXIT;
							}

							ret = av_interleaved_write_frame(ic, &pack);
							if (ret < 0)
							{
								err22++;
							}
							else
							{
								err22 = 0;
							}

							if (ret < 0 && err22 > 100)
							{
								BackCallFun("写入流数据连续失败超过5秒");
								goto EXIT;
							}
						}
						ReleaseMutex(hMutexSend);
					}
					av_free_packet(&pack);
				}
				else
				{
					if (minRateTime > 160)
					{
						BackCallFun("超过8秒帧pts过小");
						goto EXIT;
					}
					minRateTime++;
				}
				DWORD stop_time = ::GetTickCount64();
				int sleepTime = stop_time - start_time;
				//20帧  保证每次循环在50ms内
				if (sleepTime <= (1000 / fps))
				{
					Sleep(1000 / fps - sleepTime);
				}
			}
			catch (const std::exception& e)
			{
				BackCallFun(e.what());
				BackCallFun("合成帧异常");
				goto EXIT;
			}
		}

	}
	catch (const std::exception& e)
	{
		BackCallFun(e.what());
		BackCallFun("初始化sender失败");
		goto EXIT;
	}

EXIT:
	issuccess = false;
	ReleaseMutex(hMutexSend);
	if (yuv)
	{
		av_frame_free(&yuv);
	}

	if (vc || audioContext)
	{
		avcodec_close(vc);
		avcodec_free_context(&vc);
		avcodec_free_context(&audioContext);
	}
	if (ic)
	{
		avio_closep(&ic->pb);
		avformat_close_input(&ic);
		avformat_free_context(ic);
	}
	if (ServerState)
	{
		return 0;
	}
	else
	{
		return 1;
	}

}


extern "C" _declspec(dllexport) int startServerFfmpeg(char* key, char* outUlr, camera * cameraInfo, int width, int height, LogCallBack logCallBack)
{
	try
	{
		BackCallFun = logCallBack;
		//char msg[64] = {};
		//int p = CheckSN(key, msg);
		//if (p == -1)
		//{
		//	std::string code = "授权码错误,标识编码：" + GetHardCode();
		//	BackCallFun(code.c_str());
		//	return -1;
		//}
		//else if(p == 0)
		//{
		//	std::string code = "授权即将到期,标识编码：" + GetHardCode();

		//	//授权即将到期
		//	BackCallFun(code.c_str());
		//}

		currentFrame = (AVFrame**)av_malloc(5 * sizeof(AVFrame*));

		currentFrame[0] = av_frame_alloc();
		int nDstSize = 0;
		if (cameraInfo[0].width > 0 && cameraInfo[0].height > 0)
		{
			nDstSize = avpicture_get_size(AV_PIX_FMT_YUV420P, cameraInfo[0].width, cameraInfo[0].height);
			uint8_t* dstbuf0 = new uint8_t[nDstSize];
			avpicture_fill((AVPicture*)currentFrame[0], dstbuf0, AV_PIX_FMT_YUV420P, cameraInfo[0].width, cameraInfo[0].height);
			memset(currentFrame[0]->data[0], 0, cameraInfo[0].width * cameraInfo[0].height);
			memset(currentFrame[0]->data[1], 0x80, cameraInfo[0].width * cameraInfo[0].height / 4);
			memset(currentFrame[0]->data[2], 0x80, cameraInfo[0].width * cameraInfo[0].height / 4);
		}
		currentFrame[0]->width = cameraInfo[0].width;
		currentFrame[0]->height = cameraInfo[0].height;
		currentFrame[0]->format = AV_PIX_FMT_YUV420P;
		currentFrame[0]->pts = 0;


		currentFrame[1] = av_frame_alloc();
		if (cameraInfo[1].width > 0 && cameraInfo[1].height > 0)
		{
			nDstSize = avpicture_get_size(AV_PIX_FMT_YUV420P, cameraInfo[1].width, cameraInfo[1].height);
			uint8_t* dstbuf1 = new uint8_t[nDstSize];
			avpicture_fill((AVPicture*)currentFrame[1], dstbuf1, AV_PIX_FMT_YUV420P, cameraInfo[1].width, cameraInfo[1].height);
			memset(currentFrame[1]->data[0], 0, cameraInfo[1].width * cameraInfo[1].height);
			memset(currentFrame[1]->data[1], 0x80, cameraInfo[1].width * cameraInfo[1].height / 4);
			memset(currentFrame[1]->data[2], 0x80, cameraInfo[1].width * cameraInfo[1].height / 4);
		}
		currentFrame[1]->width = cameraInfo[1].width;
		currentFrame[1]->height = cameraInfo[1].height;
		currentFrame[1]->format = AV_PIX_FMT_YUV420P;
		currentFrame[1]->pts = 0;


		currentFrame[2] = av_frame_alloc();
		if (cameraInfo[2].width > 0 && cameraInfo[2].height > 0)
		{
			nDstSize = avpicture_get_size(AV_PIX_FMT_YUV420P, cameraInfo[2].width, cameraInfo[2].height);
			uint8_t* dstbuf2 = new uint8_t[nDstSize];
			avpicture_fill((AVPicture*)currentFrame[2], dstbuf2, AV_PIX_FMT_YUV420P, cameraInfo[2].width, cameraInfo[2].height);
			memset(currentFrame[2]->data[0], 0, cameraInfo[2].width * cameraInfo[2].height);
			memset(currentFrame[2]->data[1], 0x80, cameraInfo[2].width * cameraInfo[2].height / 4);
			memset(currentFrame[2]->data[2], 0x80, cameraInfo[2].width * cameraInfo[2].height / 4);
		}
		currentFrame[2]->width = cameraInfo[2].width;
		currentFrame[2]->height = cameraInfo[2].height;
		currentFrame[2]->format = AV_PIX_FMT_YUV420P;
		currentFrame[2]->pts = 0;

		currentFrame[3] = av_frame_alloc();
		if (cameraInfo[3].width > 0 && cameraInfo[3].height > 0)
		{
			nDstSize = avpicture_get_size(AV_PIX_FMT_YUV420P, cameraInfo[3].width, cameraInfo[3].height);
			uint8_t* dstbuf2 = new uint8_t[nDstSize];
			avpicture_fill((AVPicture*)currentFrame[3], dstbuf2, AV_PIX_FMT_YUV420P, cameraInfo[3].width, cameraInfo[3].height);
			memset(currentFrame[3]->data[0], 0, cameraInfo[3].width * cameraInfo[3].height);
			memset(currentFrame[3]->data[1], 0x80, cameraInfo[3].width * cameraInfo[3].height / 4);
			memset(currentFrame[3]->data[2], 0x80, cameraInfo[3].width * cameraInfo[3].height / 4);
		}
		currentFrame[3]->width = cameraInfo[3].width;
		currentFrame[3]->height = cameraInfo[3].height;
		currentFrame[3]->format = AV_PIX_FMT_YUV420P;
		currentFrame[3]->pts = 0;


		//轨迹图片初始化
		currentFrame[4] = av_frame_alloc();
		int picWidth = cameraInfo[0].width + cameraInfo[1].width + cameraInfo[2].width + cameraInfo[3].width;
		int piHeight = cameraInfo[0].height;
		if (picWidth > 0 && piHeight > 0)
		{
			nDstSize = avpicture_get_size(AV_PIX_FMT_YUV420P, picWidth, piHeight);
			uint8_t* dstbuf3 = new uint8_t[nDstSize];
			avpicture_fill((AVPicture*)currentFrame[4], dstbuf3, AV_PIX_FMT_YUV420P, picWidth, cameraInfo[0].height);
			memset(currentFrame[4]->data[0], 0, picWidth * piHeight);
			memset(currentFrame[4]->data[1], 0x80, picWidth * piHeight / 4);
			memset(currentFrame[4]->data[2], 0x80, picWidth * piHeight / 4);
		}
		currentFrame[4]->width = picWidth;
		currentFrame[4]->height = piHeight;
		currentFrame[4]->format = AV_PIX_FMT_YUV420P;
		currentFrame[4]->pts = 0;


		//接收互斥锁
		hMutexSend = CreateMutex(NULL, FALSE, NULL);
		//绑定回调函数
		//注册所有的封装器

		//注册所有的编解码器
		avcodec_register_all();
		av_register_all();
		//注册所有网络协议
		avformat_network_init();
		//主驾视频开启
		maincameral = cameraInfo[0];
		if (cameraInfo[0].width > 0)
			_beginthreadex(NULL, 0, GetVideoMain, &cameraInfo[0], 0, NULL);
		Sleep(1000);
		//场地1视频开启
		camera1 = cameraInfo[1];
		if (cameraInfo[1].width > 0)
			_beginthreadex(NULL, 0, GetVideoChangDiOne, &cameraInfo[1], 0, NULL);
		Sleep(1000);
		//场地2视频开启
		camera2 = cameraInfo[2];
		if (cameraInfo[2].width > 0)
			_beginthreadex(NULL, 0, GetVideoChangDiTwo, &cameraInfo[2], 0, NULL);
		Sleep(1000);
		//场地2视频开启
		camera3 = cameraInfo[3];
		if (cameraInfo[3].width > 0)
			_beginthreadex(NULL, 0, GetVideoChangDiThress, &cameraInfo[3], 0, NULL);
		//开启输出流，发送rtsp数据
		while (ServerState)
		{

			Sleep(2000);
			//视频总pts
			vps = 0;
			//音频刷新时间
			audio_time = 0;
			//视频刷新时间
			video_time = 0;
			int result = sendRtsp(outUlr, width, height);
			if (result == 1)
			{
				break;
			}
			needReset = true;
			BackCallFun("restart");
		}



		return 0;

	}
	catch (const std::exception&)
	{
		return -100;
	}
}

//切换摄像头
extern "C" _declspec(dllexport) int changeCamera(camera cameraInfo)
{
	try
	{
		switch (cameraInfo.index)
		{
		case 1:
			memmove(camera1.rtspUrl, cameraInfo.rtspUrl, strlen(cameraInfo.rtspUrl));
			break;
		case 2:
			memmove(camera2.rtspUrl, cameraInfo.rtspUrl, strlen(cameraInfo.rtspUrl));
			break;
		case 3:
			memmove(camera3.rtspUrl, cameraInfo.rtspUrl, strlen(cameraInfo.rtspUrl));
			break;
		default:
			break;
		}
		//打开新摄像头
		cameraState[cameraInfo.index] = 0;
		return 1;
	}
	catch (const std::exception&)
	{
		return 0;
	}


}

//发送图片
extern "C" _declspec(dllexport) int sendPicture(byte * ImageBuffer, int imageWidth, int imageHeight, int imageStride)
{
	try
	{
		if (currentFrame != NULL)
		{

			int h = 0;
			AVFrame* rgbFrame = av_frame_alloc();
			avpicture_fill((AVPicture*)rgbFrame, ImageBuffer, AV_PIX_FMT_BGR24, imageWidth, imageHeight);
			SwsContext* sws_ctx = sws_getContext(
				imageWidth, imageHeight, AV_PIX_FMT_BGR24,
				imageWidth, imageHeight, AV_PIX_FMT_YUV420P,
				SWS_BICUBIC, NULL, NULL, NULL);
			h = sws_scale(sws_ctx, rgbFrame->data, rgbFrame->linesize, 0, imageHeight, currentFrame[4]->data, currentFrame[4]->linesize);
			sws_freeContext(sws_ctx);
			av_frame_free(&rgbFrame);
		}
		return 0;

	}
	catch (const std::exception&)
	{
		return -1;
	}
}


//停止服务
extern "C" _declspec(dllexport) int stopServer()
{
	ServerState = false;
	Sleep(1000);
	return 1;
}
