#pragma warning(disable:4996) 
#pragma warning(disable:4819) 
#pragma warning(disable:4018) 

#include <stdio.h>  
#include <stdlib.h>  

#include<windows.h>

extern "C"
{
//ffmpeg
//libavcodec    - 编码解码器
//libavdevice   - 输入输出设备的支持
//libavfilter   - 视音频滤镜支持
//libavformat   - 视音频等格式的解析
//libavutil     - 工具库
//libpostproc   - 后期效果处理
//libswscale    - 图像颜色、尺寸转换
#include "libavcodec/avcodec.h"			//解码
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"			//缩放
#include "libavutil/avutil.h"	
#include "libswresample/swresample.h"
#include "libavutil/dict.h"  
#include "libavutil/imgutils.h"  
#include "libavutil/time.h"	

//SDL2.0
#include <SDL/SDL.h>
#include <SDL/SDL_main.h>
}

#include <iostream>  
using namespace std;  
//#define VIDEO_PICTURE_QUEUE_SIZE 3
//#define SUBPICTURE_QUEUE_SIZE 16
//#define SAMPLE_QUEUE_SIZE 9
//#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

#define MAX_QUEUE_SIZE (10 * 1024 * 1024)
#define MIN_FRAMES 25

#define MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio  

#define _BUG        1 
#ifdef _BUG
//1) __VA_ARGS__ 是一个可变参数的宏
//2) __FILE__ 宏在预编译时会替换成当前的源文件名
//3) __LINE__宏在预编译时会替换成当前的行号
//4) __FUNCTION__宏在预编译时会替换成当前的函数名称
#define debug(...)                                                      \
        {                                                               \
            fprintf(stderr, "[debug][%s:%d] ", __FUNCTION__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                               \
        }
//#define debug(fmt, ...) printf((fmt), ##__VA_ARGS__);
#endif

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    MyAVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct Decoder {
    AVPacket pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
} Decoder;

typedef struct VideoState {
	SDL_Thread *read_tid;		//read thread
	SDL_Thread *disaudio_tid;	//display audio thread
	SDL_Thread *disvideo_tid;	//display video thread
	SDL_Thread *respsnse_tid;	//respsnse thread
	SDL_Thread *gui_tid;		//gui thread

	int abort_request;
	int paused;
	int seek_req;
	int64_t seek_pos;

	AVFormatContext *ic;

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

	double audio_clock;
	double last_audio_clock;

	int audio_stream;
	AVStream *audio_st;
	PacketQueue audioq;
	int audio_volume;	
	int sample_rate;
	int channels;
	int64_t channel_layout;
	struct SwrContext *swr_ctx;

	double video_clock; // pts of last decoded frame / predicted pts of next decoded frame
	double last_video_clock;

	int video_stream;
	AVStream *video_st;
	PacketQueue videoq;
	//int width, height, xleft, ytop;
	struct SwsContext *img_convert_ctx;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;

	int eof;
	char *filename;

	double frame_timer; 
	double frame_last_delay;

	SDL_cond *continue_read_thread;
}VideoState;

struct _SDLStruct{
    SDL_Window *screen;   
    SDL_Renderer* sdlRenderer;  
    SDL_Texture* sdlTexture;  
    SDL_Rect sdlRect;  	
}SDLStruct;

AVPacket flush_pkt;

//Output YUV420P data as a file   
#define OUTPUT_YUV420P 0
//Open H264   
#define H264	0

void print_error(const char *filename, int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf_ptr);
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;

    if (q->abort_request)
       return -1;

    pkt1 = (MyAVPacketList *)av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->duration += pkt1->pkt.duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt);
    SDL_UnlockMutex(q->mutex);

    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;
    return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            q->duration -= pkt1->pkt.duration;
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

//static void decoder_abort(Decoder *d, FrameQueue *fq)
//{
//    packet_queue_abort(d->queue);
////    frame_queue_signal(fq);
//    SDL_WaitThread(d->decoder_tid, NULL);
//    d->decoder_tid = NULL;
//    packet_queue_flush(d->queue);
//}

static void decoder_destroy(Decoder *d) {
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static int decoder_start(Decoder *d, int (*fn)(void *), void *arg)
{
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThread(fn, "decoder", arg);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
}

//for the sync
//double get_clock(Clock *c){}
static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void stream_component_close(VideoState *is, unsigned int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
//        decoder_abort(&is->auddec, &is->sampq);
//        SDL_CloseAudio();
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
 //       av_freep(&is->audio_buf1);
 //       is->audio_buf1_size = 0;
 //       is->audio_buf = NULL;

        break;
    case AVMEDIA_TYPE_VIDEO:
//        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
//		decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

static void stream_close(VideoState *is)
{
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, NULL);

    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
/*
	frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);
*/
    SDL_DestroyCond(is->continue_read_thread);
	sws_freeContext(is->img_convert_ctx);
	av_free(is->filename);
/*    
	if (is->vis_texture)
        SDL_DestroyTexture(is->vis_texture);
    if (is->vid_texture)
        SDL_DestroyTexture(is->vid_texture);
    if (is->sub_texture)
        SDL_DestroyTexture(is->sub_texture);
 */
	av_free(is);
}

static void do_exit(VideoState *is)
{
    if (is) {
        stream_close(is);
    }
	avformat_network_deinit();
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);	
}

static int init_SDL_Video(int screen_w, int screen_h)
{
    if(SDL_Init(SDL_INIT_VIDEO)) {    
        debug( "Could not initialize SDL - %s\n", SDL_GetError());   
        return -1;  
    }  

    if(SDL_WasInit(SDL_INIT_VIDEO)){
        debug("SDL_INIT_VIDEO\n");  
	}else {
		debug("Quit Subsystem VIDEO \n");  
	}

    SDLStruct.sdlRect.x = 0;  
    SDLStruct.sdlRect.y = 0;  
    SDLStruct.sdlRect.w = screen_w;		
    SDLStruct.sdlRect.h = screen_h;		

	//SDL 2.0 Support for multiple windows  
    SDLStruct.screen = SDL_CreateWindow("Simplest ffmpeg player's Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,  
        screen_w, screen_h,  
        SDL_WINDOW_OPENGL);  
    if(!SDLStruct.screen) {    
        debug("SDL: could not create window - exiting:%s\n",SDL_GetError());    
        return -1;  
    }  

	SDLStruct.sdlRenderer = SDL_CreateRenderer(SDLStruct.screen, -1, 0);    
	//IYUV: Y + U + V  (3 planes)
	//YV12: Y + V + U  (3 planes) 

	SDLStruct.sdlTexture = SDL_CreateTexture(SDLStruct.sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, screen_w, screen_h);

	return 0;
}

static int SDL_reflesh(_SDLStruct SDLStruct, const void *pixels, int pitch)
{
	//？？？？
	#if 1  
	SDL_UpdateTexture( SDLStruct.sdlTexture, NULL, pixels, pitch);    
	#else  
	SDL_UpdateYUVTexture(sdlTexture, &sdlRect,  
	pFrameYUV->data[0], pFrameYUV->linesize[0],  
	pFrameYUV->data[1], pFrameYUV->linesize[1],  
	pFrameYUV->data[2], pFrameYUV->linesize[2]);  
	#endif                      
	SDL_RenderClear( SDLStruct.sdlRenderer );    
	SDL_RenderCopy( SDLStruct.sdlRenderer, SDLStruct.sdlTexture,  NULL, &SDLStruct.sdlRect);    
	SDL_RenderPresent( SDLStruct.sdlRenderer );

	return 0;
}

static int H264_filter(AVBitStreamFilterContext* bsfc, AVCodecContext *pCodecCtx, AVPacket *pkt)
{
	if(av_bitstream_filter_filter(bsfc, pCodecCtx, NULL, 
								&pkt->data, &pkt->size, 
								pkt->data, pkt->size, 
								0) < 0){
	debug("av_bitstream_filter_filter continue\n");
	av_free(pkt->data);
	av_packet_unref(pkt);
	}

	return 0;
}

static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub)
{
	int ret = AVERROR(EAGAIN);

	for(;;) {
		AVPacket pkt;
		if (d->queue->serial == d->pkt_serial) {
            if (d->queue->abort_request)
                return -1;
            switch (d->avctx->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0) {
                        //if (decoder_reorder_pts == -1) {
                        //    frame->pts = frame->best_effort_timestamp;
                        //} else if (!decoder_reorder_pts) {
                        //    frame->pts = frame->pkt_dts;
                        //}
                    }
                    break;

                case AVMEDIA_TYPE_AUDIO:
                    ret = avcodec_receive_frame(d->avctx, frame);
                    //if (ret >= 0) {
                    //    AVRational tb = (AVRational){1, frame->sample_rate};
                    //    if (frame->pts != AV_NOPTS_VALUE)
                    //        frame->pts = av_rescale_q(frame->pts, av_codec_get_pkt_timebase(d->avctx), tb);
                    //    else if (d->next_pts != AV_NOPTS_VALUE)
                    //        frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                    //    if (frame->pts != AV_NOPTS_VALUE) {
                    //        d->next_pts = frame->pts + frame->nb_samples;
                    //        d->next_pts_tb = tb;
                    //    }
                    //}
                    break;
            }
            if (ret == AVERROR_EOF) { //End of file
                d->finished = d->pkt_serial;
                avcodec_flush_buffers(d->avctx);
                return 0;
            }
            if (ret >= 0)		//解码一帧成功
                return 1;
        } while (ret != AVERROR(EAGAIN));	//EAGAIN 请再试一次	

		do {
			if (d->queue->nb_packets == 0)
				SDL_CondSignal(d->empty_queue_cond);
			if (d->packet_pending) {
				av_packet_move_ref(&pkt, &d->pkt);
				d->packet_pending = 0;
			} else {
				if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
					return -1;
			}
		} while (d->queue->serial != d->pkt_serial);
        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(d->avctx);
            d->finished = 0;
            d->next_pts = d->start_pts;
            d->next_pts_tb = d->start_pts_tb;
        } else {
            if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
                av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                d->packet_pending = 1;
                av_packet_move_ref(&d->pkt, &pkt);
            }		
			av_packet_unref(&pkt);
		}
	}
}

static double compute_frame_delay(VideoState *is)
{
	double delay, sync_threshold, ref_clock, diff;

	delay = is->video_clock - is->last_video_clock;	//视频帧的两帧间隔
	if (delay <= 0 || delay >= 10.0) { 
		 delay = is->frame_last_delay;	
	} else {
		is->frame_last_delay = delay;
	}
	is->last_video_clock = is->video_clock;

	ref_clock = is->audio_clock; 
	diff = is->video_clock - ref_clock;				//当前视频和音频的间隔
	
	sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, diff));
	if (diff <= -sync_threshold) {   /*当前视频帧落后于主时钟源*/	
		delay = FFMAX(0, delay + diff);
	} else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
		delay = delay + diff;
	}else if (diff >= sync_threshold) {
		delay = 2 * delay;			/*采取加倍延时*/
	}

	return delay;
}

//Output PCM  
#define OUTPUT_PCM 0

//Buffer:  
//|-----------|-------------|  
//chunk-------pos---len-----|  
static  Uint8  *audio_chunk;   
static  Uint32  audio_len;   
static  Uint8  *audio_pos; 
/* Audio Callback 
 * The audio function callback takes the following parameters:  
 * stream: A pointer to the audio buffer to be filled  
 * len: The length (in bytes) of the audio buffer  
 *  
*/   
void  fill_audio(void *udata, Uint8 *stream, int len){   
    SDL_memset(stream, 0, len);  
    if(audio_len==0)  return;   
  
    len=(len>audio_len?audio_len:len);   /*  Mix  as  much  data  as  possible  */   
  
    SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME);  
    audio_pos += len;   
    audio_len -= len;   
}   

static int init_SDL_Audio(int out_sample_rate, uint64_t out_ch_layout,int out_nb_samples)
{
    //Init  
    if(SDL_Init(SDL_INIT_AUDIO)) { debug("init SDL - %s\n", SDL_GetError()); return -1; }

	if(SDL_WasInit(SDL_INIT_AUDIO)) { 
		debug("SDL_INIT_AUDIO\n");  
	} else {
		debug("Quit Subsystem AUDIO \n");  
	}

	int out_channels=av_get_channel_layout_nb_channels(out_ch_layout);  
	debug("init_SDL_Audio out_channels = %d\n", out_channels);

    //SDL_AudioSpec 
	SDL_AudioSpec wanted_spec; 
    wanted_spec.freq = out_sample_rate;   
	wanted_spec.format = AUDIO_S16SYS;			//固定输出signed 16bit pcm 
    wanted_spec.channels = out_channels;   
    wanted_spec.silence = 0;   
    wanted_spec.samples = out_nb_samples;   
    wanted_spec.callback = fill_audio;   
//    wanted_spec.userdata = pCodecCtx;   

    if (SDL_OpenAudio(&wanted_spec, NULL) < 0){   
        debug("can't open audio.\n");   
        return -1;   
    }  

    SDL_PauseAudio(0);		 //Play  

	return 0;
}

static int pic_reflesh(VideoState *is, AVFrame *pFrame, AVFrame *pFrameYUV, AVPacket *pkt)
{
	double pts = 0;
	//display
	sws_scale(is->img_convert_ctx, (const uint8_t* const*)pFrame->data, pFrame->linesize, 0, is->viddec.avctx->height,   
		pFrameYUV->data, pFrameYUV->linesize); 
		
	//获取解码的视频帧时间
	if(pkt->pts!= AV_NOPTS_VALUE) {
		pts = av_frame_get_best_effort_timestamp(pFrame);
	} else {
		pts = 0;
	}
	pts *=av_q2d(is->video_st->time_base);
	is->video_clock = pts;

	if(pts == 0.0) {
		//按照默认帧率播放
		double frameRate = av_q2d(is->video_st->avg_frame_rate);
		frameRate += pFrame->repeat_pict * (frameRate * 0.5);
		Sleep((1/frameRate*1000));
	} else {			
		double delay = compute_frame_delay(is);
		Sleep((unsigned long)(delay*1000));
	}		

	SDL_reflesh(SDLStruct, pFrameYUV->data[0], pFrameYUV->linesize[0]);	

	return 0;
}

static int disvideo_thread(void *arg)
{	
	VideoState *is = (VideoState *)arg;	
	AVPacket pkt1, *pkt = &pkt1;
	int ret = -1;
	int got_picture = -1;

	if(!is){
		debug("arg is NULL\n");
		return NULL;
	}

	AVCodecContext *pCodecCtx;
	AVFrame *pFrame, *pFrameYUV;

	unsigned char *out_buffer;  	  

	pFrame		= av_frame_alloc();  
	pFrameYUV	= av_frame_alloc(); 
	pCodecCtx	= is->viddec.avctx;
	if(!pCodecCtx){
		debug("stream_open fail ;is->Vavctx = NULL\n");
	}
	is->img_convert_ctx = sws_getCachedContext(is->img_convert_ctx,
                pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height,
                AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
	
	out_buffer= (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P,  pCodecCtx->width, pCodecCtx->height, 1));  
    av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1); 

	init_SDL_Video(pCodecCtx->width, pCodecCtx->height);

#if OUTPUT_YUV420P 
	FILE *fp_yuv;
    fp_yuv=fopen("output.yuv","wb+");    
#endif 

#if H264
	FILE *fp_264;
	fp_264=fopen("output.h264","wb+");
	AVBitStreamFilterContext* bsfc =  av_bitstream_filter_init("h264_mp4toannexb");
#endif
	for(;;) {  
		//decode
		got_picture = decoder_decode_frame(&is->viddec, pFrame, NULL);
		if (got_picture < 0) {
			debug("decoder_decode_frame fail\n");  
			//return -1; 解码已经出错，退出线程或警告
		} else if(got_picture == 0) {
			continue;
		}else {
			//display
			ret = pic_reflesh(is, pFrame, pFrameYUV, pkt);			

			#if OUTPUT_YUV420P  
			y_size=pCodecCtx->width*pCodecCtx->height;    
			fwrite(pFrameYUV->data[0],1,y_size,fp_yuv);    //Y   
			fwrite(pFrameYUV->data[1],1,y_size/4,fp_yuv);  //U  
			fwrite(pFrameYUV->data[2],1,y_size/4,fp_yuv);  //V  
			#endif  	

			#if H264			
			H264_filter(bsfc, pCodecCtx, pkt);
			fwrite (pkt->data , pkt->size, 1, fp_264);
			#endif	
		}
	}
#if 0
	//flush decoder  
    //FIX: Flush Frames remained in Codec  
    while (1) {  
        ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, pkt);  
        if (ret < 0)  
            break;  
        if (!got_picture)  
            break;  
        sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height,   
            pFrameYUV->data, pFrameYUV->linesize);  

		#if OUTPUT_YUV420P  
        int y_size=pCodecCtx->width*pCodecCtx->height;    
        fwrite(pFrameYUV->data[0],1,y_size,fp_yuv);    //Y   
        fwrite(pFrameYUV->data[1],1,y_size/4,fp_yuv);  //U  
        fwrite(pFrameYUV->data[2],1,y_size/4,fp_yuv);  //V  
		#endif  

		#if OPEN_SDL
        //SDL---------------------------  
        SDL_UpdateTexture( sdlTexture, &sdlRect, pFrameYUV->data[0], pFrameYUV->linesize[0] );    
        SDL_RenderClear( sdlRenderer );    
        SDL_RenderCopy( sdlRenderer, sdlTexture,  NULL, &sdlRect);    
        SDL_RenderPresent( sdlRenderer );    
        //SDL End-----------------------
		#endif 
        //Delay 40ms  
        SDL_Delay(40);  
    }  
#endif
	debug("disvideo_thread end\n");

#if OUTPUT_YUV420P
	fclose (fp_yuv);
#endif

#if H264
	fclose (fp_264);
	av_bitstream_filter_close(bsfc);
#endif
//    avcodec_close(pCodecCtx);  
	
    av_frame_free(&pFrameYUV);  
    av_frame_free(&pFrame);  

	return 0;
}

static int disaudio_thread(void *arg)
{
	VideoState *is = (VideoState *)arg;	
	AVPacket pkt1, *pkt = &pkt1;
	int ret = -1;
	int got_picture = -1;

	if(!is){ debug("arg is NULL\n");return NULL; }

	AVCodecContext  *pCodecCtx = is->auddec.avctx;  
	AVFrame *pFrame = av_frame_alloc();

	//重采样设置选项
    enum AVSampleFormat in_sample_fmt	= pCodecCtx->sample_fmt;	//输入的采样格式    
    int in_sample_rate					= pCodecCtx->sample_rate;	//输入的采样率
    uint64_t in_ch_layout				= pCodecCtx->channel_layout;//输入的声道布局
	debug("in_sample_fmt = %d\n", pCodecCtx->sample_fmt);
	debug("in_sample_rate = %d\n", pCodecCtx->sample_rate);
	debug("in_ch_layout = %d\n\n", pCodecCtx->channel_layout);

	enum AVSampleFormat out_sample_fmt	= AV_SAMPLE_FMT_S16;		//输出的采样格式 16bit PCM		AV_SAMPLE_FMT_FLT	AV_SAMPLE_FMT_S16
    int out_sample_rate					= 48000;					//输出的采样率   44100  48000  
    uint64_t out_ch_layout				= AV_CH_LAYOUT_MONO;		//输出的声道布局	AV_CH_LAYOUT_STEREO	AV_CH_LAYOUT_MONO
	debug("out_sample_fmt = %d\n", out_sample_fmt);
	debug("out_sample_rate = %d\n", out_sample_rate);
	debug("out_ch_layout = %d\n", out_ch_layout);

	int out_channels	= av_get_channel_layout_nb_channels(out_ch_layout);		//获取输出的声道个数	
	int out_nb_samples	= pCodecCtx->frame_size;								//Number of samples per channel in an audio frame.
	debug("out_channels = %d\n", out_channels);					
	debug("out_nb_samples = %d\n\n", out_nb_samples);

    //Out Buffer Size  
    int out_buffer_size	= av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);    
    uint8_t *out_buffer	= (uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE * 2);			//存储pcm数据

	//Swr    
	is->swr_ctx = swr_alloc_set_opts(NULL, out_ch_layout, out_sample_fmt, out_sample_rate, in_ch_layout, in_sample_fmt,
				in_sample_rate, 0, NULL);
	if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {        
		swr_free(&is->swr_ctx);
        return -1;	
	}

	init_SDL_Audio(out_sample_rate, out_ch_layout, out_nb_samples);

	int count = -1;
#if OUTPUT_PCM  
	FILE *fp_pcm = NULL; 
    fp_pcm = fopen("output.pcm", "wb"); 
#endif  	
	int size = av_get_bytes_per_sample(pCodecCtx->sample_fmt);			//获取音频每次采样的byte数
	//	AVBitStreamFilterContext* bsfc =  av_bitstream_filter_init("h264_mp4toannexb");
	for(;;)
	{
		//decode
		if ((got_picture = decoder_decode_frame(&is->auddec, pFrame, NULL)) < 0) {
			debug("decoder_decode_frame fail\n");
			//return -1; 解码已经出错，退出线程或警告
		} else if(0 == got_picture) {
			continue;
		} else {
		//display
			//将正在播放的音频时间点记录下来作为基准
			is->audio_clock = pFrame->pkt_pts * av_q2d(is->audio_st->time_base);
	
			swr_convert(is->swr_ctx, &out_buffer, MAX_AUDIO_FRAME_SIZE, (const uint8_t **)pFrame->data , pFrame->nb_samples);                    
		#if OUTPUT_PCM
			fwrite (out_buffer , out_buffer_size, 1, fp_pcm);						
		#endif

			while(audio_len>0)//Wait until finish  
				SDL_Delay(1);   
 
			//Set audio buffer (PCM data)  
			audio_chunk = (Uint8 *) out_buffer;   
			//Audio buffer length  
			audio_len = out_buffer_size;  
			audio_pos = audio_chunk; 			
		}
	}

	return 0;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, unsigned int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
	AVCodec *codec;
	int sample_rate, nb_channels;
	int64_t channel_layout;
	int ret = 0;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

	avctx = avcodec_alloc_context3(NULL);
	if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;

	codec = avcodec_find_decoder(avctx->codec_id);

	avctx->codec_id = codec->id;

	if ((ret = avcodec_open2(avctx, codec, NULL)) < 0) {
		debug("avcodec_open2 fail\n");
        goto fail;
    }

	is->eof = 0;
	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
	switch (avctx->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
		is->sample_rate = sample_rate    = avctx->sample_rate;
        is->channels	= nb_channels    = avctx->channels;
        is->channel_layout = channel_layout = avctx->channel_layout;
		//debug(" bit_rate = %d \r\n", avctx->bit_rate);  
		//debug(" sample_rate = %d \r\n", avctx->sample_rate);  
		//debug(" channels = %d \r\n", avctx->channels);  
		//debug(" code_name = %s \r\n", avctx->codec->name);  
		//debug(" block_align = %d\n",avctx->block_align);

		is->audio_stream = stream_index;
		is->audio_st = ic->streams[stream_index];
		debug("stream_component_open is->Aavctx = 0x%d\n", avctx);
		decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
        if ((ret = decoder_start(&is->auddec, disaudio_thread, is)) < 0)
            goto out;
		break;

	case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
        if ((ret = decoder_start(&is->viddec, disvideo_thread, is)) < 0)
            goto out;	
		break;

	case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

		decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
        if ((ret = decoder_start(&is->subdec, disvideo_thread, is)) < 0)
            goto out;
		break;

    default:
        break;
	}
	goto out;

fail:
    avcodec_free_context(&avctx);
out:
//    av_dict_free(&opts);

    return ret;
}

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg)
{
    VideoState *is = (VideoState *)arg;
    AVFormatContext *ic = NULL;
    int err, ret;
    int st_index[AVMEDIA_TYPE_NB];
	AVPacket pkt1, *pkt = &pkt1;

	is->video_stream = is->audio_stream = is->subtitle_stream = -1;
	is->eof = 0;

	SDL_mutex *wait_mutex = SDL_CreateMutex();
    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    //is->last_video_stream = is->video_stream = -1;
    //is->last_audio_stream = is->audio_stream = -1;
    //is->last_subtitle_stream = is->subtitle_stream = -1;
	is->video_stream = -1;
	is->audio_stream = -1;
	is->subtitle_stream = -1;
	is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

	err = avformat_open_input(&ic, is->filename, NULL, NULL);
    if (err < 0) {
		print_error(is->filename, err);
        ret = -1;
        goto fail;
    }

	is->ic = ic;
	err = avformat_find_stream_info(ic, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_WARNING,
                "%s: could not find codec parameters\n", is->filename);
        ret = -1;
        goto fail;
    }

	av_dump_format(ic, 0, is->filename, false);

    st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                            st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);

    st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                            st_index[AVMEDIA_TYPE_AUDIO],
                            st_index[AVMEDIA_TYPE_VIDEO],
                            NULL, 0);

    st_index[AVMEDIA_TYPE_SUBTITLE] =
        av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                            st_index[AVMEDIA_TYPE_SUBTITLE],
                            (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                st_index[AVMEDIA_TYPE_AUDIO] :
                                st_index[AVMEDIA_TYPE_VIDEO]),
                            NULL, 0);

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }
    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }
	debug("ic->nb_streams = %d \n", ic->nb_streams);
	debug("is->videostream = %d \n", is->video_stream);
	debug("is->audiostream = %d \n", is->audio_stream);

	for (;;) {
		if (is->abort_request)
            break;

        /* if the queue are full, no need to read more */
        if ( is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE) {
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }

		ret = av_read_frame(ic, pkt);
		if(ret < 0){
			debug("av_read_frame fail ret= %d\n", ret);
			if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
				if (is->video_stream >= 0)
					packet_queue_put_nullpacket(&is->videoq, is->video_stream);
				if (is->audio_stream >= 0)
					packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
				if (is->subtitle_stream >= 0)
					packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
				is->eof = 1;
			}
            if (ic->pb && ic->pb->error)
                break;
			//why lock
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
           continue;
		}else {
			is->eof = 0;	
		}

        if (pkt->stream_index == is->audio_stream) {
           packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
		}
	}
	debug("read_thread end\n");

 fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

	SDL_DestroyMutex(wait_mutex);
	return 0;
}


static VideoState *stream_open(const char *filename)
{
	VideoState *is;
    is = (VideoState *)av_mallocz(sizeof(VideoState));
	if (!is){
		debug("is = NULL \n");
		return NULL;
	}
        
    is->filename = av_strdup(filename);
	if (!is->filename){
		debug("is->filename = NULL \n");
		goto fail;
	}
		
	    /* start video display */
#if 0
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;
#endif

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

	init_clock(&is->vidclk, &is->videoq.serial);
	init_clock(&is->audclk, &is->audioq.serial);
	init_clock(&is->extclk, &is->extclk.serial);

	is->read_tid = SDL_CreateThread(read_thread, "read_thread", is);
	if (!is->read_tid) {
		av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
        stream_close(is);
        return NULL;
    }

	return is;
}

int main(int argc, char * argv[])
{
	int ret = -1;
	VideoState *is = NULL;
	char *input_filename = "PGM[17-12-01-11-24-53].mp4";	//jhzc.mp4 
    
	av_register_all();
    avformat_network_init();

	av_init_packet(&flush_pkt);
	flush_pkt.data = (uint8_t *)&flush_pkt;

	is = stream_open(input_filename);	//等待ffmpeg初始化完成				

    if (!is) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
    }

	system("pause");
	SDL_Quit();  
	//	av_free(input_filename);
	return 0;
}

