#include <stdio.h>
#include <stdlib.h>
#include <iostream>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include "libavformat/avio.h"
#include "libavutil/buffer.h"
#include "libavutil/error.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_qsv.h"
#include "libavutil/mem.h"

#include "SDL.h"
}


//Refresh Event
#define SFM_REFRESH_EVENT  (SDL_USEREVENT + 1)

#define SFM_BREAK_EVENT  (SDL_USEREVENT + 2)

int thread_exit = 0;
int thread_pause = 0;

typedef struct DecodeContext {
	AVBufferRef *hw_device_ref;
} DecodeContext;

int sfp_refresh_thread(void *opaque)
{
	thread_exit = 0;
	thread_pause = 0;
	while (!thread_exit)
	{
		if (!thread_pause)
		{
			SDL_Event event;
			event.type = SFM_REFRESH_EVENT;
			SDL_PushEvent(&event);
		}
		SDL_Delay(40);
	}

	thread_exit = 0;
	thread_pause = 0;
	SDL_Event event;
	event.type = SFM_BREAK_EVENT;
	SDL_PushEvent(&event);
	return 0;
}


static  enum AVPixelFormat get_format(AVCodecContext *avctx, const enum AVPixelFormat *pix_fmts)
{
	while (*pix_fmts != AV_PIX_FMT_NONE) {
		if (*pix_fmts == AV_PIX_FMT_QSV) {
			DecodeContext *decode = (DecodeContext *)avctx->opaque;
			AVHWFramesContext  *frames_ctx;
			AVQSVFramesContext *frames_hwctx;
			int ret;

			/* create a pool of surfaces to be used by the decoder */
			avctx->hw_frames_ctx = av_hwframe_ctx_alloc(decode->hw_device_ref);
			if (!avctx->hw_frames_ctx)
				return AV_PIX_FMT_NONE;
			frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
			frames_hwctx = (AVQSVFramesContext *)frames_ctx->hwctx;

			frames_ctx->format = AV_PIX_FMT_QSV;
			frames_ctx->sw_format = avctx->sw_pix_fmt;
			frames_ctx->width = FFALIGN(avctx->coded_width, 32);
			frames_ctx->height = FFALIGN(avctx->coded_height, 32);
			frames_ctx->initial_pool_size = 32;

			frames_hwctx->frame_type = MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;

			ret = av_hwframe_ctx_init(avctx->hw_frames_ctx);
			if (ret < 0)
				return AV_PIX_FMT_NONE;

			return AV_PIX_FMT_QSV;
		}

		pix_fmts++;
	}

	fprintf(stderr, "The QSV pixel format not offered in get_format()\n");

	return AV_PIX_FMT_NONE;
}





int main(int argc, char* argv[])
{

	AVFormatContext* pFormatCtx;
	int i, videoIndex;
	AVCodecContext* pCodecCtx;
	AVCodec* pCodec;
	AVFrame* pFrame, *pFrameYUV, *sws_frame;
	unsigned char* out_buffer;
	AVPacket* packet;
	int y_size;
	int ret = -1, got_picture;
	struct SwsContext* img_convert_ctx;
	char filepath[] = "1.mp4";
	int screen_w = 0;
	int screen_h = 0;
	SDL_Window* screen;
	SDL_Renderer* sdlRenderer;
	SDL_Texture* sdlTexture;
	SDL_Rect sdlRect;
	DecodeContext decode = { NULL };
	AVCodecParserContext *parser;
	AVStream *video_st = NULL;


	SDL_Thread *video_tid;
	SDL_Event event;

	av_register_all();
	avformat_network_init();


	pFormatCtx = avformat_alloc_context();

	if (avformat_open_input(&pFormatCtx, filepath, NULL, NULL) != 0)
	{
		std::cout << "Coudldn't open input stream." << std::endl;
		return -1;
	}

	//ret = avformat_find_stream_info(pFormatCtx, NULL);
	//if (ret < 0)
	//{
	//	std::cout << "Counldn't find stream information." << std::endl;
	//	return -1;
	//}

	videoIndex = -1;

	for (i = 0; i < pFormatCtx->nb_streams; ++i)
	{
		AVStream *st = pFormatCtx->streams[i];
		
		if (st->codecpar->codec_id == AV_CODEC_ID_H264 && !video_st)
		{
			video_st = st;
			if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			{
				videoIndex = i;
			}
			
		}
		else
		{
			st->discard = AVDISCARD_ALL;
		}
	}

	if (!video_st) {
		fprintf(stderr, "No H.264 video stream in the input file\n");
		return -1;
	}

	if (videoIndex == -1)
	{
		std::cout << "didn't find a video stream." << std::endl;
		return -1;
	}


	ret = av_hwdevice_ctx_create(&decode.hw_device_ref, AV_HWDEVICE_TYPE_QSV, "auto", NULL, 0);
	if (ret < 0) {
		fprintf(stderr, "Cannot open the hardware device\n");
		return -1;
	}

	pCodec = avcodec_find_decoder_by_name("h264_qsv");
	if (!pCodec) {
		fprintf(stderr, "The QSV decoder is not present in libavcodec\n");
		return -1;
	}

	parser = av_parser_init(pCodec->id);
	if (!parser) {
		fprintf(stderr, "parser not found\n");
		return -1;
	}

	pCodecCtx = avcodec_alloc_context3(pCodec);
	if (!pCodecCtx)
	{
		ret = AVERROR(ENOMEM);
		return -1;
	}

	pCodecCtx->codec_id = AV_CODEC_ID_H264;

	if (video_st->codecpar->extradata_size) {
		pCodecCtx->extradata = (uint8_t *)av_mallocz(video_st->codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!pCodecCtx->extradata) {
			ret = AVERROR(ENOMEM);
			return -1;
		}
		memcpy(pCodecCtx->extradata, video_st->codecpar->extradata, video_st->codecpar->extradata_size);
		pCodecCtx->extradata_size = video_st->codecpar->extradata_size;
	}


	pCodecCtx->opaque = &decode;
	pCodecCtx->get_format = get_format;
	/*pCodecCtx->width = 1920;
	pCodecCtx->height = 1080;
	pCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;*/
	if (avcodec_open2(pCodecCtx, NULL, NULL) < 0)
	{
		std::cout << "could not open codec." << std::endl;
		return -1;
	}

	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();
	sws_frame = av_frame_alloc();

	out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_QSV, 1920, 1080, 1));
	av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer, AV_PIX_FMT_QSV, 1920, 1080, 1);

	packet = (AVPacket*)av_malloc(sizeof(AVPacket));

	std::cout << "==================File Information==========================" << std::endl;
	av_dump_format(pFormatCtx, 0, filepath, 0);

	std::cout << "-------------------------------------------------" << std::endl;
	//img_convert_ctx = sws_getContext(1920, 1080, AV_PIX_FMT_QSV, 1920, 1080, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);


	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_TIMER))
	{
		std::cout << "Could not initialize SDL" << std::endl;
		return -1;
	}

	screen_w = 1280;// pCodecCtx->width;
	screen_h = 720;// pCodecCtx->height;

	screen = SDL_CreateWindow("ffplayer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, screen_w, screen_h, SDL_WINDOW_OPENGL);

	if (!screen)
	{
		printf("SDL: could not create window - exiting:%s\n", SDL_GetError());
		return -1;
	}

	sdlRenderer = SDL_CreateRenderer(screen, -1, 0);

	sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_YVYU, SDL_TEXTUREACCESS_STREAMING, 1920, 1080);

	sdlRect.x = 0;
	sdlRect.y = 0;
	sdlRect.w = screen_w;
	sdlRect.h = screen_h;

	video_tid = SDL_CreateThread(sfp_refresh_thread, NULL, NULL);

	for (;;)
	{
		SDL_WaitEvent(&event);
		if (event.type == SFM_REFRESH_EVENT)
		{
			while (1)
			{
				ret = av_read_frame(pFormatCtx, packet);
				if (ret < 0)
				{
					if (ret == AVERROR_EOF)
					{
						avcodec_flush_buffers(pCodecCtx);
					}
					printf("av_read_frame %d %d\n", ret, AVERROR_EOF);
					break;
				}
				if (packet->stream_index == videoIndex)
				{

					ret = avcodec_send_packet(pCodecCtx, packet);
					if (ret < 0) {
						if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
						{
							avcodec_flush_buffers(pCodecCtx);
							fprintf(stderr, "Error during decoding %d %d\n", ret, AVERROR(EAGAIN));

						}
						fprintf(stderr, "Error during decoding\n");
						continue;
					}
					
					//while (ret >= 0)
					{
						ret = avcodec_receive_frame(pCodecCtx, pFrame);

						//ret = av_hwframe_transfer_data(sw_frame, pFrame, 0);


						//ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
						if (ret < 0) {
							if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
								if (ret == AVERROR_EOF)
								{
									avcodec_flush_buffers(pCodecCtx);
								}
								fprintf(stderr, "Error during decoding %d %d\n", ret, AVERROR(EAGAIN));
								continue;
							}
							else
							{
								break;
							}
						}
					
						//if (got_picture)

						ret = av_hwframe_transfer_data(sws_frame, pFrame, 0);
						if (ret < 0) {
							fprintf(stderr, "Error transferring the data to system memory\n");
							continue;
						}
						//sws_scale(img_convert_ctx, (const unsigned char* const*)sws_frame->data, sws_frame->linesize, 0, pCodecCtx->height,
						//	pFrameYUV->data, pFrameYUV->linesize);

						//SDL_UpdateYUVTexture(sdlTexture, &sdlRect,
						//	pFrameYUV->data[0], pFrameYUV->linesize[0],
						//	pFrameYUV->data[1], pFrameYUV->linesize[1],
						//	pFrameYUV->data[2], pFrameYUV->linesize[2]);
					/*	for (int i = 0; i < FF_ARRAY_ELEMS(pFrameYUV->data) && pFrameYUV->data[i]; i++)
						{
							for (int j = 0; j < (pFrameYUV->height >> (i > 0)); j++)
							{

							}
						}*/
							
								//avio_write(output_ctx, pFrameYUV->data[i] + j * sw_frame->linesize[i], sw_frame->width);

						SDL_UpdateTexture(sdlTexture, &sdlRect, sws_frame->data[0], sws_frame->linesize[0]);

						SDL_RenderClear(sdlRenderer);
						SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
						SDL_RenderPresent(sdlRenderer);
						SDL_Delay(30);
					}
					
					
				}

				av_free_packet(packet);
			}
		}
		else if (event.type == SDL_KEYDOWN) 
		{
			//Pause
			if (event.key.keysym.sym == SDLK_SPACE)
				thread_pause = !thread_pause;
		}
		else if (event.type == SDL_QUIT)
		{
			thread_exit = 1;
		}
		else if (event.type == SFM_BREAK_EVENT)
		{
			break;
		}
	}


	sws_freeContext(img_convert_ctx);
	SDL_Quit();
	av_frame_free(&pFrameYUV);
	av_frame_free(&pFrame);
	avcodec_close(pCodecCtx);
	avformat_close_input(&pFormatCtx);

	system("pause");
	return 0;
}