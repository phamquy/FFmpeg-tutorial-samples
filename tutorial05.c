// tutorial05.c
// A pedagogical video player that really works!
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard,
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// Use
//
// gcc -o tutorial05 tutorial05.c -lavformat -lavcodec -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed,
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial05 myvideofile.mpg
//
// to play the video.

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavformat/avio.h>
#include <libavutil/avstring.h>

#include <SDL.h>
#include <SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>
#include <math.h>

#define SDL_AUDIO_BUFFER_SIZE 1024

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

#define VIDEO_PICTURE_QUEUE_SIZE 1

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;


typedef struct VideoPicture {
    SDL_Overlay *bmp;
    int width, height; /* source height & width */
    int allocated;
    double pts;
} VideoPicture;

typedef struct VideoState {
    
    AVFormatContext *pFormatCtx;
    int             videoStream, audioStream;
    
    double          audio_clock;
    AVStream        *audio_st;
    PacketQueue     audioq;
    uint8_t         audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int    audio_buf_size;
    unsigned int    audio_buf_index;
    AVPacket        audio_pkt;
    uint8_t         *audio_pkt_data;
    int             audio_pkt_size;
    int             audio_hw_buf_size;
    double          frame_timer;
    double          frame_last_pts;
    double          frame_last_delay;
    double          video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame
    AVStream        *video_st;
    PacketQueue     videoq;
    
    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int             pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex       *pictq_mutex;
    SDL_cond        *pictq_cond;
    
    SDL_Thread      *parse_tid;
    SDL_Thread      *video_tid;
    
    char            filename[1024];
    int             quit;
} VideoState;

SDL_Surface     *screen;

/* Since we only have one decoding thread, the Big Struct
 can be global in case we need it. */
VideoState *global_video_state;

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    
    AVPacketList *pkt1;
    if(av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    
    SDL_LockMutex(q->mutex);
    
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);
    
    SDL_UnlockMutex(q->mutex);
    return 0;
}
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;
    
    SDL_LockMutex(q->mutex);
    
    for(;;) {
        
        if(global_video_state->quit) {
            ret = -1;
            break;
        }
        
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
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
double get_audio_clock(VideoState *is) {
    double pts;
    int hw_buf_size, bytes_per_sec, n;
    
    pts = is->audio_clock; /* maintained in the audio thread */
    hw_buf_size = is->audio_buf_size - is->audio_buf_index;
    bytes_per_sec = 0;
    n = is->audio_st->codec->channels * 2;
    if(is->audio_st) {
        bytes_per_sec = is->audio_st->codec->sample_rate * n;
    }
    if(bytes_per_sec) {
        pts -= (double)hw_buf_size / bytes_per_sec;
    }
    return pts;
}

int audio_decode_frame(VideoState *vidState, uint8_t *audio_buf, int buf_size, double *pts_ptr) {
    
    int len1, data_size, n;
    AVPacket *pkt = &vidState->audio_pkt;
    static AVPacket pktTemp;
    double pts;
    static AVFrame *decoded_aframe;
    
    for(;;) {
        while(vidState->audio_pkt_size > 0) {
            int got_frame = 0;
            if (!decoded_aframe) {
                if (!(decoded_aframe = avcodec_alloc_frame())) {
                    fprintf(stderr, "out of memory\n");
                    exit(1);
                }
            } else
                avcodec_get_frame_defaults(decoded_aframe);
            
            data_size = buf_size;
            //            len1 = avcodec_decode_audio2(is->audio_st->codec,
            //                                         (int16_t *)audio_buf, &data_size,
            //                                         is->audio_pkt_data, is->audio_pkt_size);
            len1 = avcodec_decode_audio4(vidState->audio_st->codec, decoded_aframe, &got_frame, &pktTemp);
            
            if(len1 < 0) {
                /* if error, skip frame */
                vidState->audio_pkt_size = 0;
                break;
            }
            
            if (got_frame) {
                //printf("\nGot frame!");
                //printf("\nFrame data size: %d", sizeof(decoded_aframe->data[0]));
                data_size = av_samples_get_buffer_size(NULL, vidState->audio_st->codec->channels,
                                                       decoded_aframe->nb_samples,
                                                       vidState->audio_st->codec->sample_fmt, 1);
                if (data_size > buf_size) {
                    data_size = buf_size;
                }
                memcpy(audio_buf, decoded_aframe->data[0], data_size);
                
            }else{
                data_size = 0;
            }
            pktTemp.data += len1;
            pktTemp.size -= len1;
            vidState->audio_pkt_data += len1;
            vidState->audio_pkt_size -= len1;
            if(data_size <= 0) {
                /* No data yet, get more frames */
                continue;
            }
            
            pts = vidState->audio_clock;
            *pts_ptr = pts;
            n = 2 * vidState->audio_st->codec->channels; // number of bit per-sample (all channels)
            vidState->audio_clock += (double)data_size /
            (double)(n * vidState->audio_st->codec->sample_rate);
            
            /* We have data, return it and come back for more later */
            return data_size;
        }
        if(pkt->data)
            av_free_packet(pkt);
        
        if(vidState->quit) {
            return -1;
        }
        /* next packet */
        if(packet_queue_get(&vidState->audioq, pkt, 1) < 0) {
            return -1;
        }
        pktTemp.data = pkt->data;
        pktTemp.size = pkt->size;
        vidState->audio_pkt_data = pkt->data;
        vidState->audio_pkt_size = pkt->size;
        /* if update, update the audio clock w/pts */
        if(pkt->pts != AV_NOPTS_VALUE) {
            vidState->audio_clock = av_q2d(vidState->audio_st->time_base)*pkt->pts;
        }
        
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len) {
    
    VideoState *is = (VideoState *)userdata;
    int len1, audio_size;
    double pts;
    
    while(len > 0) {
        if(is->audio_buf_index >= is->audio_buf_size) {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf), &pts);
            if(audio_size < 0) {
                /* If error, output silence */
                is->audio_buf_size = 1024;
                memset(is->audio_buf, 0, is->audio_buf_size);
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if(len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *vidState) {
    
    SDL_Rect rect;
    VideoPicture *vp;
//    AVPicture pict;
    float aspect_ratio;
    int w, h, x, y;
//    int i;
    
    vp = &vidState->pictq[vidState->pictq_rindex];
    if(vp->bmp) {
        if(vidState->video_st->codec->sample_aspect_ratio.num == 0) {
            aspect_ratio = 0;
        } else {
            aspect_ratio = av_q2d(vidState->video_st->codec->sample_aspect_ratio) *
            vidState->video_st->codec->width / vidState->video_st->codec->height;
        }
        if(aspect_ratio <= 0.0) {
            aspect_ratio = (float)vidState->video_st->codec->width /
            (float)vidState->video_st->codec->height;
        }
        h = screen->h;
        w = ((int)rint(h * aspect_ratio)) & -3;
        if(w > screen->w) {
            w = screen->w;
            h = ((int)rint(w / aspect_ratio)) & -3;
        }
        x = (screen->w - w) / 2;
        y = (screen->h - h) / 2;
        
        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        SDL_DisplayYUVOverlay(vp->bmp, &rect);
    }
}

void video_refresh_timer(void *userdata) {
    
    VideoState *vidState = (VideoState *)userdata;
    VideoPicture *vp;
    double actual_delay, delay, sync_threshold, ref_clock, diff;
    
    if(vidState->video_st) {
        if(vidState->pictq_size == 0) {
            schedule_refresh(vidState, 1);
        } else {
            vp = &vidState->pictq[vidState->pictq_rindex];
            
            
            delay = vp->pts - vidState->frame_last_pts; /* the pts from last time */
            if(delay <= 0 || delay >= 1.0) {
                /** if incorrect delay, use previous one */
                delay = vidState->frame_last_delay;
            }
            /** save for next time */
            vidState->frame_last_delay = delay;
            vidState->frame_last_pts = vp->pts;
            
            /* update delay to sync to audio */
            ref_clock = get_audio_clock(vidState);
            diff = vp->pts - ref_clock;
            
            /** Skip or repeat the frame. Take delay into account
             FFPlay still doesn't "know if this is the best guess." */
            /// Make the greater gap to
            sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
            
            if(fabs(diff) < AV_NOSYNC_THRESHOLD) {
                if(diff <= -sync_threshold) { // video is behind?
                    delay = 0;
                } else if(diff >= sync_threshold) { // video is ahead
                    delay = 2 * delay;
                }
            }
            vidState->frame_timer += delay;
            /* computer the REAL delay */
            actual_delay = vidState->frame_timer - (av_gettime() / 1000000.0);
            if(actual_delay < 0.010) {
                /* Really it should skip the picture instead */
                actual_delay = 0.010;
            }
            
            
            schedule_refresh(vidState, (int)(actual_delay * 1000 + 0.5));
            /* show the picture! */
            video_display(vidState);
            
            /* update queue for next picture! */
            if(++vidState->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                vidState->pictq_rindex = 0;
            }
            SDL_LockMutex(vidState->pictq_mutex);
            vidState->pictq_size--;
            SDL_CondSignal(vidState->pictq_cond);
            SDL_UnlockMutex(vidState->pictq_mutex);
        }
    } else {
        schedule_refresh(vidState, 100);
    }
}

void alloc_picture(void *userdata) {
    
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;
    
    vp = &is->pictq[is->pictq_windex];
    if(vp->bmp) {
        // we already have one make another, bigger/smaller
        SDL_FreeYUVOverlay(vp->bmp);
    }
    // Allocate a place to put our YUV image on that screen
    vp->bmp = SDL_CreateYUVOverlay(is->video_st->codec->width,
                                   is->video_st->codec->height,
                                   SDL_YV12_OVERLAY,
                                   screen);
    vp->width = is->video_st->codec->width;
    vp->height = is->video_st->codec->height;
    
    SDL_LockMutex(is->pictq_mutex);
    vp->allocated = 1;
    SDL_CondSignal(is->pictq_cond);
    SDL_UnlockMutex(is->pictq_mutex);
    
}

int queue_picture(VideoState *vidState, AVFrame *pFrame, double pts) {
    
    VideoPicture *vp;
    int dst_pix_fmt;
    AVPicture pict;
    
    /* wait until we have space for a new pic */
    SDL_LockMutex(vidState->pictq_mutex);
    while(vidState->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
          !vidState->quit) {
        SDL_CondWait(vidState->pictq_cond, vidState->pictq_mutex);
    }
    SDL_UnlockMutex(vidState->pictq_mutex);
    
    if(vidState->quit)
        return -1;
    
    // windex is set to 0 initially
    vp = &vidState->pictq[vidState->pictq_windex];
    
    /* allocate or resize the buffer! */
    if(!vp->bmp ||
       vp->width != vidState->video_st->codec->width ||
       vp->height != vidState->video_st->codec->height) {
        SDL_Event event;
        
        vp->allocated = 0;
        /* we have to do it in the main thread */
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = vidState;
        SDL_PushEvent(&event);
        
        /* wait until we have a picture allocated */
        SDL_LockMutex(vidState->pictq_mutex);
        while(!vp->allocated && !vidState->quit) {
            SDL_CondWait(vidState->pictq_cond, vidState->pictq_mutex);
        }
        SDL_UnlockMutex(vidState->pictq_mutex);
        if(vidState->quit) {
            return -1;
        }
    }
    /* We have a place to put our picture on the queue */
    /* If we are skipping a frame, do we set this to null
     but still return vp->allocated = 1? */
    
    
    if(vp->bmp) {
        
        SDL_LockYUVOverlay(vp->bmp);
        
        dst_pix_fmt = PIX_FMT_YUV420P;
        /* point pict at the queue */
        
        pict.data[0] = vp->bmp->pixels[0];
        pict.data[1] = vp->bmp->pixels[2];
        pict.data[2] = vp->bmp->pixels[1];
        
        pict.linesize[0] = vp->bmp->pitches[0];
        pict.linesize[1] = vp->bmp->pitches[2];
        pict.linesize[2] = vp->bmp->pitches[1];
        
        // Convert the image into YUV format that SDL uses
        //        img_convert(&pict, dst_pix_fmt,
        //                    (AVPicture *)pFrame, is->video_st->codec->pix_fmt,
        //                    is->video_st->codec->width, is->video_st->codec->height);
        
        static struct SwsContext *img_convert_ctx;
        int w = vidState->video_st->codec->width;
        int h = vidState->video_st->codec->height;
        img_convert_ctx = sws_getContext(w, h, vidState->video_st->codec->pix_fmt,
                                         w, h, dst_pix_fmt,
                                         SWS_X, NULL, NULL, NULL);
        
        sws_scale(img_convert_ctx, (const uint8_t * const *)pFrame->data,
                  pFrame->linesize, 0, h,
                  pict.data, pict.linesize);
        SDL_UnlockYUVOverlay(vp->bmp);
        vp->pts = pts;
        
        /* now we inform our display thread that we have a pic ready */
        if(++vidState->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
            vidState->pictq_windex = 0;
        }
        SDL_LockMutex(vidState->pictq_mutex);
        vidState->pictq_size++;
        SDL_UnlockMutex(vidState->pictq_mutex);
    }
    return 0;
}

double synchronize_video(VideoState *vidState, AVFrame *src_frame, double pts) {
    
    double frame_delay;
    
    if(pts != 0) {
        /* if we have pts, set video clock to it */
        vidState->video_clock = pts;
    } else {
        /* if we aren't given a pts, set it to the clock */
        pts = vidState->video_clock;
    }
    /// update the video clock, if we are repeating a frame, adjust clock accordingly
    frame_delay = av_q2d(vidState->video_st->codec->time_base);
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5); // ???:why *0.5
    vidState->video_clock += frame_delay;
    
    return pts;
}
uint64_t global_video_pkt_pts = AV_NOPTS_VALUE;

/* These are called whenever we allocate a frame
 * buffer. We use this to store the global_pts in
 * a frame at the time it is allocated.
 */
int our_get_buffer(struct AVCodecContext *c, AVFrame *pic) {
    int ret = avcodec_default_get_buffer(c, pic);
    uint64_t *pts = av_malloc(sizeof(uint64_t));
    //printf("\t1st pkt's pts: %lld", global_video_pkt_pts);
    *pts = global_video_pkt_pts;
    pic->opaque = pts;
    return ret;
}
void our_release_buffer(struct AVCodecContext *c, AVFrame *pic) {
    if(pic) av_freep(&pic->opaque);
    avcodec_default_release_buffer(c, pic);
}

int video_thread(void *arg) {
    VideoState *vidState = (VideoState *)arg;
    AVPacket pkt1, *packet = &pkt1;
    //int len1;
    int frameFinished;
    AVFrame *pFrame;
    double pts;
    
    pFrame = avcodec_alloc_frame();
    
    for(;;) {
        if(packet_queue_get(&vidState->videoq, packet, 1) < 0) {
            // means we quit getting packets
            break;
        }
        pts = 0;
        
        /// Save global pts to be stored in pFrame in first call
        /** 
         (?) How to know it is the first call?
         (@) It actually be called everytime a new packet read, but 
         the our_get_buffer is called only when a new frame is about 
         to be created, it meant only first packet's pts is used.
         */
        
        //printf("\nPkt's dts: %lld",  packet->dts);
        global_video_pkt_pts = packet->pts;
        
        // Decode video frame
        // len1 = avcodec_decode_video(is->video_st->codec, pFrame, &frameFinished, packet->data, packet->size); -- Deprecated
        avcodec_decode_video2(vidState->video_st->codec, pFrame, &frameFinished, packet);

        /// Check custom pts value
        if (packet->dts == AV_NOPTS_VALUE
           && pFrame->opaque
           && *(uint64_t*)pFrame->opaque != AV_NOPTS_VALUE) {
            pts = *(uint64_t *)pFrame->opaque;
        } else if(packet->dts != AV_NOPTS_VALUE) {
            pts = packet->dts;
        
        } else {
            pts = 0;
        }
        pts *= av_q2d(vidState->video_st->time_base);
        
        // Did we get a video frame?
        if(frameFinished) {
            pts = synchronize_video(vidState, pFrame, pts);
            if(queue_picture(vidState, pFrame, pts) < 0) {
                break;
            }
        }
        av_free_packet(packet);
    }
    
    
    av_free(pFrame);
    return 0;
}

int stream_component_open(VideoState *is, int stream_index) {
    
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    SDL_AudioSpec wanted_spec, spec;
    
    if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }
    
    // Get a pointer to the codec context for the video stream
    codecCtx = pFormatCtx->streams[stream_index]->codec;
    
    if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
        // Set audio settings from codec info
        wanted_spec.freq = codecCtx->sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = codecCtx->channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = is;
        
        if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
            fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
            return -1;
        }
        is->audio_hw_buf_size = spec.size;
    }
    codec = avcodec_find_decoder(codecCtx->codec_id);
    
    if(!codec || (avcodec_open2(codecCtx, codec, NULL) < 0)) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    
    switch(codecCtx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audioStream = stream_index;
            is->audio_st = pFormatCtx->streams[stream_index];
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;
            memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
            packet_queue_init(&is->audioq);
            SDL_PauseAudio(0);
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->videoStream = stream_index;
            is->video_st = pFormatCtx->streams[stream_index];
            
            is->frame_timer = (double)av_gettime() / 1000000.0;
            is->frame_last_delay = 40e-3;
            
            packet_queue_init(&is->videoq);
            is->video_tid = SDL_CreateThread(video_thread, is);
            
            /**
             Add function to customize the memory allocating
             for a frame
            */
            codecCtx->get_buffer = our_get_buffer;
            codecCtx->release_buffer = our_release_buffer;
            break;
        default:
            break;
    }
}

int decode_interrupt_cb(void * ctx) {
    return (global_video_state && global_video_state->quit);
}

const AVIOInterruptCB int_cb = { decode_interrupt_cb, NULL };

int decode_thread(void *arg) {
    
    VideoState *vidState = (VideoState *)arg;
    AVFormatContext *pFormatCtx;
    AVPacket pkt1, *packet = &pkt1;
    
    int video_index = -1;
    int audio_index = -1;
    int i;
    
    vidState->videoStream=-1;
    vidState->audioStream=-1;
    
    global_video_state = vidState;
    // will interrupt blocking functions if we quit!
//    url_set_interrupt_cb(decode_interrupt_cb);
//    if(av_open_input_file(&pFormatCtx, vidState->filename, NULL, 0, NULL)!=0)
//        return -1; // Couldn't open file

    // will interrupt blocking functions if we quit! --> Deprecated, usind AVIOInterruptCB instead
    // url_set_interrupt_cb(decode_interrupt_cb);
    // avio_set_interrupt_cb(decode_interrupt_cb);
    
    pFormatCtx = avformat_alloc_context();
    pFormatCtx->interrupt_callback = int_cb;
    
    if (avio_open2(&pFormatCtx->pb, vidState->filename, AVIO_FLAG_READ, &pFormatCtx->interrupt_callback, NULL))
        return -1;
    
    // Open video file
    if (avformat_open_input(&pFormatCtx, vidState->filename, NULL, NULL)!=0)
        return -1; // Couldn't open file
    
    

    vidState->pFormatCtx = pFormatCtx;
    
    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information
    
    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, vidState->filename, 0);
    
    // Find the first video stream
    
    for(i=0; i<pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO &&
           video_index < 0) {
            video_index=i;
        }
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO &&
           audio_index < 0) {
            audio_index=i;
        }
    }
    if(audio_index >= 0) {
        stream_component_open(vidState, audio_index);
    }
    if(video_index >= 0) {
        stream_component_open(vidState, video_index);
    }   
    
    if(vidState->videoStream < 0 || vidState->audioStream < 0) {
        fprintf(stderr, "%s: could not open codecs\n", vidState->filename);
        goto fail;
    }
    
    // main decode loop
    
    for(;;) {
        if(vidState->quit) {
            break;
        }
        // seek stuff goes here
        if(vidState->audioq.size > MAX_AUDIOQ_SIZE ||
           vidState->videoq.size > MAX_VIDEOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }
        if(av_read_frame(vidState->pFormatCtx, packet) < 0) {
            //if(url_ferror(&pFormatCtx->pb) == 0)
            if(&pFormatCtx->pb && &pFormatCtx->pb->error)
            {
                SDL_Delay(100); /* no error; wait for user input */
                continue;
            } else {
                break;
            }
        }
        // Is this a packet from the video stream?
        if(packet->stream_index == vidState->videoStream) {
            packet_queue_put(&vidState->videoq, packet);
        } else if(packet->stream_index == vidState->audioStream) {
            packet_queue_put(&vidState->audioq, packet);
        } else {
            av_free_packet(packet);
        }
    }
    /* all done - wait for it */
    while(!vidState->quit) {
        SDL_Delay(100);
    }
    
fail:
    {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = vidState;
        SDL_PushEvent(&event);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    
    SDL_Event       event;
    
    VideoState      *vidState;
    
    vidState = av_mallocz(sizeof(VideoState));
    
    if(argc < 2) {
        fprintf(stderr, "Usage: test <file>\n");
        exit(1);
    }
    // Register all formats and codecs
    av_register_all();
    
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }
    
    // Make a screen to put our video
#ifndef __DARWIN__
    screen = SDL_SetVideoMode(640, 480, 0, 0);
#else
    screen = SDL_SetVideoMode(640, 480, 24, 0);
#endif
    if(!screen) {
        fprintf(stderr, "SDL: could not set video mode - exiting\n");
        exit(1);
    }
    
    av_strlcpy(vidState->filename, argv[1], sizeof(vidState->filename));
    
    vidState->pictq_mutex = SDL_CreateMutex();
    vidState->pictq_cond = SDL_CreateCond();
    
    schedule_refresh(vidState, 40);
    
    vidState->parse_tid = SDL_CreateThread(decode_thread, vidState);
    if(!vidState->parse_tid) {
        av_free(vidState);
        return -1;
    }
    for(;;) {
        
        SDL_WaitEvent(&event);
        switch(event.type) {
            case FF_QUIT_EVENT:
            case SDL_QUIT:
                vidState->quit = 1;
                SDL_Quit();
                exit(0);
                break;
            case FF_ALLOC_EVENT:
                alloc_picture(event.user.data1);
                break;
            case FF_REFRESH_EVENT:
                video_refresh_timer(event.user.data1);
                break;
            default:
                break;
        }
    }
    return 0;
    
}
