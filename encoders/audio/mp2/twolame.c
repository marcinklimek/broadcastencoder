/*****************************************************************************
 * twolame.c : twolame encoding functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 ******************************************************************************/

#include "common/common.h"
#include "encoders/audio/audio.h"
#include <twolame.h>
#include <libavutil/fifo.h>
#include <libavcodec/audioconvert.h>

#define MP2_AUDIO_BUFFER_SIZE 50000

static void *start_encoder( void *ptr )
{
    obe_aud_enc_params_t *enc_params = ptr;
    obe_t *h = enc_params->h;
    obe_encoder_t *encoder = enc_params->encoder;
    obe_raw_frame_t *raw_frame;
    obe_coded_frame_t *coded_frame;
    twolame_options *tl_opts = NULL;
    int output_size, frame_size, in_stride;
    int64_t cur_pts = -1;
    void *audio_buf = NULL;
    uint8_t *output_buf = NULL;
    AVAudioConvert *audio_conv = NULL;
    AVFifoBuffer *fifo = NULL;

    /* Lock the mutex until we verify parameters */
    pthread_mutex_lock( &encoder->encoder_mutex );

    tl_opts = twolame_init();
    if( !tl_opts )
    {
        fprintf( stderr, "[twolame] could load options" );
        pthread_mutex_unlock( &encoder->encoder_mutex );
        goto end;
    }

    /* TODO: setup bitrate reconfig, errors */
    twolame_set_bitrate( tl_opts, enc_params->bitrate );
    twolame_set_in_samplerate( tl_opts, enc_params->sample_rate );
    twolame_set_out_samplerate( tl_opts, enc_params->sample_rate );
    twolame_set_copyright( tl_opts, 1 );
    twolame_set_original( tl_opts, 1 );
    twolame_set_num_channels( tl_opts, enc_params->num_channels );
    twolame_set_error_protection( tl_opts, 1 );

    twolame_init_params( tl_opts );

    frame_size = twolame_get_framelength( tl_opts ) * enc_params->frames_per_pes;

    encoder->is_ready = 1;
    /* Broadcast because input and muxer can be stuck waiting for encoder */
    pthread_cond_broadcast( &encoder->encoder_cv );
    pthread_mutex_unlock( &encoder->encoder_mutex );

    output_buf = malloc( MP2_AUDIO_BUFFER_SIZE );
    if( !output_buf )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    int out_stride = av_get_bytes_per_sample( AV_SAMPLE_FMT_FLT );

    /* This works on "planar" audio so pretend it's just one audio plane */
    audio_conv = av_audio_convert_alloc( AV_SAMPLE_FMT_FLT, 1, enc_params->sample_format, 1, NULL, 0 );
    if( !audio_conv )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    /* Setup the output FIFO */
    fifo = av_fifo_alloc( frame_size );
    if( !fifo )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    while( 1 )
    {
        pthread_mutex_lock( &encoder->encoder_mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->encoder_mutex );
            break;
        }

        if( !encoder->num_raw_frames )
            pthread_cond_wait( &encoder->encoder_cv, &encoder->encoder_mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->encoder_mutex );
            break;
        }

        raw_frame = encoder->frames[0];
        pthread_mutex_unlock( &encoder->encoder_mutex );

        if( cur_pts == -1 )
            cur_pts = raw_frame->pts;

        in_stride = av_get_bytes_per_sample( raw_frame->sample_fmt );
        int num_samples = raw_frame->len / in_stride;
        int sample_bytes = num_samples * out_stride;
        int istride[6] = { in_stride };
        int ostride[6] = { out_stride };
        const void *ibuf[6] = { raw_frame->data };

        audio_buf = malloc( sample_bytes );
        if( !audio_buf )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto end;
        }
        void *obuf[6] = { audio_buf };

        if( av_audio_convert( audio_conv, obuf, ostride, ibuf, istride, num_samples ) < 0 )
        {
            syslog( LOG_ERR, "[lavf] Could not convert audio sample format\n" );
            goto end;
        }

        output_size = twolame_encode_buffer_float32_interleaved( tl_opts, audio_buf, raw_frame->num_samples, output_buf, MP2_AUDIO_BUFFER_SIZE );

        if( output_size < 0 )
        {
            syslog( LOG_ERR, "[twolame] Encode failed\n" );
            break;
        }

        free( audio_buf );
        audio_buf = NULL;

        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        remove_frame_from_encode_queue( encoder );

        if( av_fifo_realloc2( fifo, av_fifo_size( fifo ) + output_size ) < 0 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            break;
        }

        av_fifo_generic_write( fifo, output_buf, output_size, NULL );

        while( av_fifo_size( fifo ) >= frame_size )
        {
            coded_frame = new_coded_frame( encoder->stream_id, frame_size );
            if( !coded_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }
            av_fifo_generic_read( fifo, coded_frame->data, frame_size, NULL );
            coded_frame->pts = cur_pts;
            coded_frame->random_access = 1; /* Every frame output is a random access point */

            add_to_mux_queue( h, coded_frame );
            /* We need to generate PTS because frame sizes have changed */
            cur_pts += (double)MP2_NUM_SAMPLES * OBE_CLOCK * enc_params->frames_per_pes / enc_params->sample_rate;
        }
    }

end:
    if( output_buf )
        free( output_buf );

    if( audio_buf )
        free( audio_buf );

    if( audio_conv )
        av_audio_convert_free( audio_conv );

    if( fifo )
        av_fifo_free( fifo );

    if( tl_opts )
        twolame_close( &tl_opts );
    free( enc_params );

    return NULL;
}

const obe_aud_enc_func_t twolame_encoder = { start_encoder };
