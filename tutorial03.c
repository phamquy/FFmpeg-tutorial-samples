// tutorial03.c
// A pedagogical video player that will stream through every video frame as fast as it can
// and play audio (out of sync).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard,
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// Use
//
// gcc -o tutorial03 tutorial03.c -lavformat -lavcodec -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed,
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial03 myvideofile.mpg
//
// to play the stream on your screen.


#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>

#define SDL_AUDIO_BUFFER_SIZE 1024


#pragma mark - Packet Queue Impelementation (for Audio stream)
typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;

int quit = 0;

void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

/**
 SDL_LockMutex() locks the mutex in the queue so we can add something to it, 
 and then SDL_CondSignal() sends a signal to our get function (if it is 
 waiting) through our condition variable to tell it that there is data and 
 it can proceed, then unlocks the mutex to let it go.
 */
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
        
        if(quit) {
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


#pragma mark - Audio decoding functions
int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {
    static AVFrame *decoded_aframe;
    static AVPacket pkt, pktTemp;
//    static uint8_t *audio_pkt_data = NULL;
//    static int audio_pkt_size = 0;
    
    int len1, data_size;
    
    for(;;) {
        
        while(pktTemp.size > 0)
        {
            int got_frame = 0;
            
            if (!decoded_aframe) {
                if (!(decoded_aframe = avcodec_alloc_frame())) {
                    fprintf(stderr, "out of memory\n");
                    exit(1);
                }
            } else
                avcodec_get_frame_defaults(decoded_aframe);

            //data_size = buf_size; /// ????
            len1 = avcodec_decode_audio4(aCodecCtx, decoded_aframe, &got_frame, &pktTemp);
            
            
            /// Check if 
            if (len1 < 0) {
                pktTemp.size = 0;
                break; // skip packet
            }
            
            
            if (got_frame) {
                printf("\nGot frame!");
                //printf("\nFrame data size: %d", sizeof(decoded_aframe->data[0]));
                data_size = av_samples_get_buffer_size(NULL, aCodecCtx->channels,
                                                       decoded_aframe->nb_samples,
                                                       aCodecCtx->sample_fmt, 1);
                if (data_size > buf_size) {
                    data_size = buf_size;
                }
                memcpy(audio_buf, decoded_aframe->data[0], data_size);
                
            }else{
                data_size = 0;
            }
                
            printf("\nData size %d", data_size);
            pktTemp.data += len1;
            pktTemp.size -= len1;
            
            if (data_size <= 0) {
                continue;
            }
            
            return data_size;
/* Deprecated
            data_size = buf_size;
            len1 = avcodec_decode_audio2(aCodecCtx, (int16_t *)audio_buf, &data_size,
                                         audio_pkt_data, audio_pkt_size);
            if(len1 < 0) {
                // if error, skip frame
                audio_pkt_size = 0;
                break;
            }
            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            if(data_size <= 0) {
                // No data yet, get more frames
                continue;
            }
            // We have data, return it and come back for more later
            return data_size;
*/
        }
        
        
        if(pkt.data)
            av_free_packet(&pkt);
        
        if(quit)
        {
            return -1;
        }
        
        
        /// Get packet from queue
        if(packet_queue_get(&audioq, &pkt, 1) < 0)
        {
            return -1;
        }
            
        
        av_init_packet(&pktTemp);
        
        pktTemp.data = pkt.data;
        pktTemp.size = pkt.size;
    }
}

/**
 This call back function is called from thread that spawned by SDL_openAudio()
 userdata: pointer to audio codec context
 stream: buffer that we need to write data to
 len: size of the the buffer
 */
void audio_callback(void *userdata, Uint8 *stream, int len) {
    
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len1, audio_size;
    
    static uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;
    
    while(len > 0) {
        /// Need to read more audio data
        if(audio_buf_index >= audio_buf_size) {
            /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if(audio_size < 0) {
                /* If error, output silence */
                audio_buf_size = 1024; // arbitrary?
                memset(audio_buf, 0, audio_buf_size);
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        
        len1 = audio_buf_size - audio_buf_index;
        if(len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}


#pragma mark - Main function

int main(int argc, char *argv[]) {
    AVFormatContext *pFormatCtx;
    int             i, videoStream, audioStream;
    AVCodecContext  *pCodecCtx;
    AVCodec         *pCodec;
    AVFrame         *pFrame;
    AVPacket        packet;
    int             frameFinished;
    static struct SwsContext *img_convert_ctx;
    
    AVCodecContext  *aCodecCtx;
    AVCodec         *aCodec;
    
    SDL_Overlay     *bmp;
    SDL_Surface     *screen;
    SDL_Rect        rect;
    SDL_Event       event;
    SDL_AudioSpec   wanted_spec, spec;
    
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
    
    // Open video file
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0)
        return -1; // Couldn't open file
    
    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information
    
    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, argv[1], 0);
    
    /// Find the first video and audio stream
    videoStream=-1;
    audioStream=-1;
    for(i=0; i<pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
           videoStream < 0) {
            videoStream=i;
        }
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO &&
           audioStream < 0) {
            audioStream=i;
        }
    }
    if(videoStream==-1)
        return -1; // Didn't find a video stream
    if(audioStream==-1)
        return -1;
    
    aCodecCtx=pFormatCtx->streams[audioStream]->codec;

    /// Set audio settings from codec info
    wanted_spec.freq = aCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = aCodecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback; ///<----- Callback to feed audio data
    wanted_spec.userdata = aCodecCtx;
    
    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }
    
    /// Find audio codec
    aCodec = avcodec_find_decoder(aCodecCtx->codec_id);
    if(!aCodec) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    
    /// Open codec
    if (avcodec_open2(aCodecCtx, aCodec, NULL) < 0)
        return -1;
    
    // audio_st = pFormatCtx->streams[index]
    packet_queue_init(&audioq);
    SDL_PauseAudio(0);
    
    // Get a pointer to the codec context for the video stream
    pCodecCtx=pFormatCtx->streams[videoStream]->codec;
    
    // Find the decoder for the video stream
    pCodec=avcodec_find_decoder(pCodecCtx->codec_id);
    if(pCodec==NULL) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1; // Codec not found
    }
    // Open codec
    if(avcodec_open2(pCodecCtx, pCodec, NULL)<0)
        return -1; // Could not open codec
    
    // Allocate video frame
    pFrame=avcodec_alloc_frame();
    
    // Make a screen to put our video
    
#ifndef __DARWIN__
    screen = SDL_SetVideoMode(pCodecCtx->width, pCodecCtx->height, 0, 0);
#else
    screen = SDL_SetVideoMode(pCodecCtx->width, pCodecCtx->height, 24, 0);
#endif
    if(!screen) {
        fprintf(stderr, "SDL: could not set video mode - exiting\n");
        exit(1);
    }
    
    // Allocate a place to put our YUV image on that screen
    bmp = SDL_CreateYUVOverlay(pCodecCtx->width,
                               pCodecCtx->height,
                               SDL_YV12_OVERLAY,
                               screen);
    

    int w = pCodecCtx->width;
    int h = pCodecCtx->height;
    img_convert_ctx = sws_getContext(w, h, pCodecCtx->pix_fmt,
                                     w, h, PIX_FMT_YUV420P,
                                     SWS_BICUBIC, NULL, NULL, NULL);
    
    // Read frames and save first five frames to disk
    i=0;
    while(av_read_frame(pFormatCtx, &packet)>=0) {
        /// Is this a packet from the video stream?
        if(packet.stream_index==videoStream) {
            // Decode video frame
            avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
          
            // Did we get a video frame?
            if(frameFinished) {
                SDL_LockYUVOverlay(bmp);
                
                AVPicture pict;
                pict.data[0] = bmp->pixels[0];
                pict.data[1] = bmp->pixels[2];
                pict.data[2] = bmp->pixels[1];
                
                pict.linesize[0] = bmp->pitches[0];
                pict.linesize[1] = bmp->pitches[2];
                pict.linesize[2] = bmp->pitches[1];
                
                // Convert the image into YUV format that SDL uses
                sws_scale(img_convert_ctx, (const uint8_t * const *)pFrame->data,
                          pFrame->linesize, 0, pCodecCtx->height,
                          pict.data, pict.linesize);
                
                SDL_UnlockYUVOverlay(bmp);
                
                rect.x = 0;
                rect.y = 0;
                rect.w = pCodecCtx->width;
                rect.h = pCodecCtx->height;
                SDL_DisplayYUVOverlay(bmp, &rect);
                av_free_packet(&packet);
            }
        /// Is this a packet from audio streams?
        } else if(packet.stream_index == audioStream) {
            packet_queue_put(&audioq, &packet);
        } else {
            av_free_packet(&packet);
        }
        
        /// Free the packet that was allocated by av_read_frame
        SDL_PollEvent(&event);
        
        switch(event.type) {
            case SDL_QUIT:
                quit = 1;
                SDL_Quit();
                exit(0);
                break;
            default:
                break;
        }
        
    }
    
    // Free the YUV frame
    av_free(pFrame);
    
    // Close the codec
    avcodec_close(pCodecCtx);
    
    // Close the video file
    avformat_close_input(&pFormatCtx);
    
    return 0;
}
