/*
 * sendvideo.c
 *
 *  Created on: May 15, 2016
 *      Author: amyznikov
 */
#include "sendvideo.h"
#include "ffmpeg.h"
#include "pthread_wait.h"
#include "cclist.h"
#include "ffplay-java-api.h"
#include "debug.h"


struct ff_output_stream {
  char * server;
  char * opts;
  ff_output_stream_event_callback events_callback;
  void * cookie;
  pthread_t pid;
  pthread_wait_t lock;
  struct ccfifo p, q;
  int cx, cy, pxfmt;
  ff_output_stream_state state;
  int64_t firstpts;
  int status;
  int reason;
  bool interrupt:1;
};



static void ctx_lock(ff_output_stream * ctx) {
  pthread_wait_lock(&ctx->lock);
}

static void ctx_unlock(ff_output_stream * ctx) {
  pthread_wait_unlock(&ctx->lock);
}

static int ctx_wait(ff_output_stream * ctx, int tmo) {
  return pthread_wait(&ctx->lock, tmo);
}

static void ctx_signal(ff_output_stream * ctx) {
  pthread_wait_broadcast(&ctx->lock);
}

static int ctx_interrupt_callback(void * arg)
{
  return ((ff_output_stream *) arg)->interrupt;
}


static void set_output_stream_state(ff_output_stream * ctx, ff_output_stream_state state, int reason, bool lock)
{
  if ( lock ) {
    ctx_lock(ctx);
  }

  ctx->state = state;

  if ( !ctx->reason ) {
    ctx->reason = reason;
  }

  if ( ctx->events_callback.stream_state_changed ) {
    ctx->events_callback.stream_state_changed(ctx->cookie, ctx, ctx->state, reason);
  }

  ctx_signal(ctx);

  if ( lock ) {
    ctx_unlock(ctx);
  }
}


static int create_video_stream(AVCodecContext ** cctx, AVFormatContext * oc, const AVCodec * codec,
    const AVDictionary * options, int cx, int cy, int pixfmt)
{
  AVDictionary * codec_opts = NULL;
  AVDictionaryEntry * e = NULL;
  AVStream * os = NULL;

  int bitrate = 128000;
  int gop_size = 25;

  int status = 0;

  * cctx = NULL;

  /// Filter codec opts

  if ( (e = av_dict_get(options, "-b:v", NULL, 0)) ) {
    if ( (bitrate = (int) av_strtod(e->value, NULL)) < 1000 ) {
      PDBG("Bad output bitrate specified: %s", e->value);
      status = AVERROR(EINVAL);
      goto end;
    }
  }

  if ( (e = av_dict_get(options, "-g", NULL, 0)) ) {
    if ( sscanf(e->value, "%d", &gop_size) != 1 || gop_size < 1 ) {
      PDBG("Bad output gop size specified: %s", e->value);
      status = AVERROR(EINVAL);
      goto end;
    }
  }

  if ( (status = ffmpeg_filter_codec_opts(options, codec, AV_OPT_FLAG_ENCODING_PARAM, &codec_opts)) ) {
    PERROR("ffmpeg_filter_codec_opts('%s') fails", codec->name);
    goto end;
  }


  /// Open encoder

  if ( !(*cctx = avcodec_alloc_context3(codec)) ) {
    PDBG("avcodec_alloc_context3('%s') fails", codec->name);
    status = AVERROR(ENOMEM);
    goto end;
  }

  (*cctx)->time_base = (AVRational) { 1, 1000 };
  (*cctx)->pix_fmt = pixfmt;
  (*cctx)->width = cx;
  (*cctx)->height = cy;
  (*cctx)->bit_rate = bitrate;
  (*cctx)->gop_size = gop_size;
  (*cctx)->me_range = 1;
  (*cctx)->qmin = 1;
  (*cctx)->qmax = 32;
  (*cctx)->flags |= CODEC_FLAG_GLOBAL_HEADER;


  if ( (status = avcodec_open2(*cctx, codec, &codec_opts)) ) {
    PDBG("avcodec_open2('%s') fails: %s", codec->name, av_err2str(status));
    goto end;
  }

  /// Alloc stream

  if ( !(os = avformat_new_stream(oc, codec)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( (status = avcodec_parameters_from_context(os->codecpar, *cctx)) < 0 ) {
    PDBG("avcodec_parameters_from_context('%s') fails: %s", codec->name, av_err2str(status));
    goto end;
  }

  os->time_base = (*cctx)->time_base;

end:

  av_dict_free(&codec_opts);

  return status;
}


static void * output_stream_thread(void * arg)
{
  struct ff_output_stream * ctx = arg;
  JNIEnv * env = NULL;

  AVDictionary * opts = NULL;
  AVDictionaryEntry * e = NULL;

  const char * format_name = "matroska";
  AVOutputFormat * oformat = NULL;

  const char * vcodec_name = "libx264";
  const AVCodec * vcodec = NULL;
  AVCodecContext * vcodec_ctx = NULL;

  AVFormatContext * oc = NULL;
  AVStream * os = NULL;

  struct frm * frm;
  AVFrame * fyuyv = NULL;
  AVFrame * fnv21 = NULL;
  struct SwsContext * sws = NULL;
  int cx, cy;

  AVPacket pkt;
  int gotpkt;

  bool write_header_ok = false;

  int status;


  PDBG("ENTER");

  java_attach_current_thread(&env);




  cx = ctx->cx;
  cy = ctx->cy;

  av_init_packet(&pkt);
  pkt.data = NULL, pkt.size = 0;


  /// Parse command line

  if ( (status = av_dict_parse_string(&opts, ctx->opts, " \t", " \t", 0)) ) {
    PERROR("av_dict_parse_string() fails: %s", av_err2str(status));
    goto end;
  }



  /// Get output format

  if ( (e = av_dict_get(opts, "-f", NULL, 0)) ) {
    format_name = e->value;
  }
  if ( !(oformat = av_guess_format(format_name, NULL, NULL)) ) {
    status = AVERROR_MUXER_NOT_FOUND;
    PERROR("av_guess_format('%s') fails: %s", format_name, av_err2str(status));
    goto end;
  }



  /// Get output codec

  if ( (e = av_dict_get(opts, "-c:v", NULL, 0)) ) {
    vcodec_name = e->value;
  }
  if ( !(vcodec = avcodec_find_encoder_by_name(vcodec_name)) ) {
    status = AVERROR_ENCODER_NOT_FOUND;
    PERROR("avcodec_find_encoder_by_name('%s') fails: %s", vcodec_name, av_err2str(status));
    goto end;
  }




  /// Alloc frame buffers

  if ( (status = ffmpeg_create_video_frame(&fnv21, AV_PIX_FMT_NV21, ctx->cx, ctx->cy, 0)) ) {
    PERROR("ffmpeg_create_video_frame(fnv21) fails: %s", av_err2str(status));
    goto end;
  }

  if ( (status = ffmpeg_create_video_frame(&fyuyv, AV_PIX_FMT_YUV420P, ctx->cx, ctx->cy, 32)) ) {
    PERROR("ffmpeg_create_video_frame(fyuyv) fails: %s", av_err2str(status));
    goto end;
  }

  if ( !(sws = sws_getContext(cx, cy, AV_PIX_FMT_NV21, cx, cy, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL)) ) {
    PERROR("sws_getContext() fails");
    goto end;
  }




  /// Alloc output context

  if ( (status = avformat_alloc_output_context2(&oc, oformat, NULL, ctx->server)) < 0 ) {
    PERROR("avformat_alloc_output_context2() fails: %s", av_err2str(status));
    goto end;
  }

  oc->interrupt_callback.callback = ctx_interrupt_callback;
  oc->interrupt_callback.opaque = ctx;



  /// Create video stream
  if ( (status = create_video_stream(&vcodec_ctx, oc, vcodec, opts, cx, cy, AV_PIX_FMT_YUV420P)) ) {
    PERROR("create_video_stream('%s') fails: %s", vcodec->name, av_err2str(status));
    goto end;
  }


  /// Start server connection

  set_output_stream_state(ctx, ff_output_stream_connecting, 0, true);


  PDBG("C avio_open('%s')", ctx->server);

  if ( (status = avio_open(&oc->pb, ctx->server, AVIO_FLAG_WRITE)) < 0 ) {
    PCRITICAL("avio_open(%s) fails: %s", ctx->server, av_err2str(status));
    goto end;
  }

  PDBG("R avio_open('%s')", ctx->server);

  set_output_stream_state(ctx, ff_output_stream_established, 0, true);

  /* Write the stream header */
  PDBG("C avformat_write_header('%s')", ctx->server);

  if ( (status = avformat_write_header(oc, NULL)) < 0 ) {
    PERROR("avformat_write_header() fails: %s", av_err2str(status));
    goto end;
  }

  PDBG("R avformat_write_header('%s')", ctx->server);

  write_header_ok = true;

  ctx_lock(ctx);

  while ( status >= 0 ) {

    frm = NULL;

    while ( !ctx->interrupt && !(frm = ccfifo_ppop(&ctx->q)) ) {
      ctx_wait(ctx, -1);
    }

    if ( ctx->interrupt ) {
      status = AVERROR_EXIT;
      if ( frm ) {
        ccfifo_ppush(&ctx->p, frm);
      }
      break;
    }

    ctx_unlock(ctx);

    fyuyv->pts = frm->pts;
    gotpkt = false;

    if ( (status = av_image_fill_arrays(fnv21->data, fnv21->linesize, frm->data, fnv21->format, cx, cy, 1)) <= 0 ) {
      PERROR("av_image_fill_arrays() fails: %s", av_err2str(status));
    }
    else if ( (status = sws_scale(sws, (const uint8_t * const*)fnv21->data, fnv21->linesize, 0, cy, fyuyv->data, fyuyv->linesize)) < 0 ) {
      PERROR("sws_scale() fails: %s", av_err2str(status));
    }
    else if ( (status = avcodec_encode_video2(vcodec_ctx, &pkt, fyuyv, &gotpkt)) < 0 ) {
      PERROR("avcodec_encode_video2() fails: %s", av_err2str(status));
    }
    else if ( gotpkt ) {

      pkt.stream_index = 0;

      os = oc->streams[0];

      if ( vcodec_ctx->time_base.num != os->time_base.num || vcodec_ctx->time_base.den != os->time_base.den ) {
        av_packet_rescale_ts(&pkt, vcodec_ctx->time_base, os->time_base);
      }

      if ( (status = av_write_frame(oc, &pkt)) < 0 ) {
        PERROR("av_write_frame() fails: status=%d %s", status, av_err2str(status));
      }

      av_packet_unref(&pkt);
    }

    ctx_lock(ctx);

    if ( frm ) {
      ccfifo_ppush(&ctx->p, frm);
    }
  }

  ctx_unlock(ctx);

end:

  PDBG("C set_output_stream_state(disconnecting, status=%d)", status);
  set_output_stream_state(ctx, ff_output_stream_disconnecting, status, true);

  if ( write_header_ok && (status = av_write_trailer(oc)) ) {
    PERROR("av_write_trailer() fails: %s", av_err2str(status));
  }

  if ( vcodec_ctx ) {
    if ( avcodec_is_open(vcodec_ctx) ) {
      avcodec_close(vcodec_ctx);
    }
    avcodec_free_context(&vcodec_ctx);
  }

  if ( oc ) {
    avio_closep(&oc->pb);
    avformat_free_context(oc);
  }

  if ( sws ) {
    sws_freeContext(sws);
  }

  av_dict_free(&opts);

  set_output_stream_state(ctx, ff_output_stream_idle, status, true);

  java_deatach_current_thread();

  PDBG("LEAVE");

  return NULL;
}



ff_output_stream * create_output_stream(const create_output_stream_args * args)
{
  ff_output_stream * ctx = NULL;
  struct frm * frm;
  size_t frmsize;

  bool fok = false;


  if ( !args || args->cx < 2 || args->cy < 2 || !args->pxfmt || !args->server || !*args->server) {
    errno = EINVAL;
    goto end;
  }

  if ( !(ctx = av_mallocz(sizeof(*ctx))) ) {
    goto end;
  }

  if ( !(ccfifo_init(&ctx->p, 10, sizeof(struct frm*))) ) {
    goto end;
  }

  if ( !(ccfifo_init(&ctx->q, 10, sizeof(struct frm*))) ) {
    goto end;
  }

  ctx->cx = args->cx;
  ctx->cy = args->cy;
  ctx->pxfmt = args->pxfmt;
  ctx->server = av_strdup(args->server);

  if ( args->opts && *args->opts ) {
    ctx->opts = av_strdup(args->opts);
  }

  if ( args->events_callback ) {
    ctx->events_callback = *args->events_callback;
    ctx->cookie = args->cookie;
  }

  ctx->state = ff_output_stream_idle;
  ctx->status = 0;

  frmsize = offsetof(struct frm, data) + FRAME_DATA_SIZE(ctx->cx, ctx->cy);
  for ( uint i = 0; i < 10; ++i ) {
    if ( !(frm = av_mallocz(frmsize)) ) {
      goto end;
    }
    ccfifo_ppush(&ctx->p, frm);
  }


  fok = true;

end:

  if ( !fok && ctx ) {

    while ( (frm = ccfifo_ppop(&ctx->p)) ) {
      av_free(frm);
    }

    ccfifo_cleanup(&ctx->p);
    ccfifo_cleanup(&ctx->q);

    av_free(ctx->server);
    av_free(ctx->opts);

    av_free(ctx), ctx = NULL;
  }

  return ctx;
}


void destroy_output_stream(ff_output_stream * ctx)
{
  if ( ctx ) {

    struct frm * frm;

    if ( ctx->pid ) {
      pthread_join(ctx->pid, NULL);
    }

    while ( (frm = ccfifo_ppop(&ctx->p)) ) {
      av_free(frm);
    }

    while ( (frm = ccfifo_ppop(&ctx->q)) ) {
      av_free(frm);
    }

    ccfifo_cleanup(&ctx->p);
    ccfifo_cleanup(&ctx->q);

    av_free(ctx->server);
    av_free(ctx->opts);
    av_free(ctx);
  }
}



bool start_output_stream(ff_output_stream * ctx)
{
  int status = -1;

  ctx_lock(ctx);

  if ( ctx->pid != 0 || ctx->state != ff_output_stream_idle ) {
    errno = EALREADY;
    goto end;
  }


  set_output_stream_state(ctx, ff_output_stream_starting, 0, false);

  if ( (status = pthread_create(&ctx->pid, NULL, output_stream_thread, ctx)) ) {
    set_output_stream_state(ctx, ff_output_stream_idle, errno = status, false);
  }
  else {

    while ( ctx->state == ff_output_stream_starting ) {
      ctx_wait(ctx, -1);
    }

    if ( ctx->state != ff_output_stream_connecting && ctx->state != ff_output_stream_established ) {
      errno = ctx->reason;
      pthread_join(ctx->pid, NULL);
      ctx->pid = 0;
      status = -1;
    }
  }

end:

  ctx_unlock(ctx);

  return status == 0;
}


void stop_output_stream(ff_output_stream * ctx)
{
  PDBG("ENTER");

  ctx_lock(ctx);

  ctx->interrupt = true;
  ctx_signal(ctx);

  while ( ctx->state != ff_output_stream_idle ) {
    PDBG("WAIT STATE");
    ctx_wait(ctx, -1);
  }

  ctx_unlock(ctx);

  PDBG("LEAVE");
}

ff_output_stream_state get_output_stream_state(const ff_output_stream * ctx)
{
  return ctx->state;
}

void * get_output_stream_cookie(const ff_output_stream * ctx)
{
  return ctx->cookie;
}

size_t get_output_frame_data_size(const ff_output_stream * ctx)
{
  return FRAME_DATA_SIZE(ctx->cx, ctx->cy);
}

struct frm * pop_output_frame(ff_output_stream * ctx)
{
  struct frm * frm;
  ctx_lock(ctx);
  frm = ccfifo_ppop(&ctx->p);
  ctx_unlock(ctx);
  return frm;
}

void push_output_frame(ff_output_stream * ctx, struct frm * frm)
{
  ctx_lock(ctx);

  if ( !ctx->firstpts ) {
    ctx->firstpts = frm->pts;
  }

  frm->pts -= ctx->firstpts;
  ccfifo_ppush(&ctx->q, frm);

  ctx_signal(ctx);
  ctx_unlock(ctx);
}