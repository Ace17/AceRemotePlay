#define __STDC_CONSTANT_MACROS

#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cassert>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include "streamer.h"
#include "common/socket.h"

extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace
{
const char* averrorToString(int error)
{
  static char msg[256];
  av_strerror(error, msg, sizeof msg);
  return msg;
}

const int STREAM_FRAME_RATE = 30;
const int SAMPLE_RATE = 48000;

std::vector<uint8_t> rgbaFrame;

struct OutputStream
{
  AVStream* st;
  AVCodecContext* enc;

  int64_t nextPts;
  int sampleCount;

  AVFrame* raw_frame;
  AVFrame* converted_frame;

  float t = 0;

  struct SwsContext* sws_ctx;
  struct SwrContext* swr_ctx;
};

void write_frame(AVFormatContext* ctx, const AVRational* time_base, AVStream* s, AVPacket* pkt)
{
  av_packet_rescale_ts(pkt, *time_base, s->time_base);
  pkt->stream_index = s->index;

  int ret = av_interleaved_write_frame(ctx, pkt);

  if(ret < 0)
    throw std::runtime_error(std::string("av_interleaved_write_frame error: ") + averrorToString(ret));
}

AVCodec* add_stream(OutputStream* os, AVFormatContext* oc, AVCodecID codec_id, Size2i resolution)
{
  AVCodec* codec = avcodec_find_encoder(codec_id);

  if(!codec)
  {
    fprintf(stderr, "Could not find encoder for '%s'\n", avcodec_get_name(codec_id));
    exit(1);
  }

  os->st = avformat_new_stream(oc, nullptr);

  if(!os->st)
  {
    fprintf(stderr, "Could not allocate stream\n");
    exit(1);
  }

  os->st->id = oc->nb_streams - 1;
  AVCodecContext* c = avcodec_alloc_context3(codec);

  if(!c)
  {
    fprintf(stderr, "Could not alloc an encoding context\n");
    exit(1);
  }

  os->enc = c;
  switch(codec->type)
  {
  case AVMEDIA_TYPE_AUDIO:
    c->sample_fmt = codec->sample_fmts ?
      codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    c->bit_rate = 64000;
    c->sample_rate = SAMPLE_RATE;

    c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
    c->channel_layout = AV_CH_LAYOUT_STEREO;

    if(codec->channel_layouts)
    {
      c->channel_layout = codec->channel_layouts[0];

      for(int i = 0; codec->channel_layouts[i]; i++)
      {
        if(codec->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
          c->channel_layout = AV_CH_LAYOUT_STEREO;
      }
    }

    c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
    os->st->time_base = (AVRational) { 1, c->sample_rate };
    break;

  case AVMEDIA_TYPE_VIDEO:
    c->codec_id = codec_id;
    c->bit_rate = 400 * 1000;
    c->width = resolution.width;
    c->height = resolution.height;

    /* timebase: This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identical to 1. */
    os->st->time_base = (AVRational) { 1, STREAM_FRAME_RATE };
    c->time_base = os->st->time_base;

    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->thread_count = 1;

    // low latency
    c->max_b_frames = 0;
    c->gop_size = 12;

    break;

  default:
    break;
  }

  /* Some formats want stream headers to be separate. */
  if(oc->oformat->flags & AVFMT_GLOBALHEADER)
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  return codec;
}

/**************************************************************/
/* audio output */

AVFrame* alloc_audio_frame(enum AVSampleFormat sample_fmt, uint64_t channel_layout, int sample_rate, int nb_samples)
{
  AVFrame* frame = av_frame_alloc();
  int ret;

  if(!frame)
  {
    fprintf(stderr, "Error allocating an audio frame\n");
    exit(1);
  }

  frame->format = sample_fmt;
  frame->channel_layout = channel_layout;
  frame->sample_rate = sample_rate;
  frame->nb_samples = nb_samples;

  if(nb_samples)
  {
    ret = av_frame_get_buffer(frame, 0);

    if(ret < 0)
    {
      fprintf(stderr, "Error allocating an audio buffer\n");
      exit(1);
    }
  }

  return frame;
}

void open_audio(AVCodec* codec, OutputStream* os)
{
  AVCodecContext* c = os->enc;

  int ret = avcodec_open2(c, codec, nullptr);

  if(ret < 0)
    throw std::runtime_error(std::string("Could not open audio codec: ") + averrorToString(ret));

  int nb_samples;

  if(c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
    nb_samples = 10000;
  else
    nb_samples = c->frame_size;

  os->converted_frame = alloc_audio_frame(c->sample_fmt, c->channel_layout, c->sample_rate, nb_samples);
  assert(os->converted_frame);

  os->raw_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, c->channel_layout, c->sample_rate, nb_samples);
  assert(os->raw_frame);

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(os->st->codecpar, c);

  if(ret < 0)
  {
    fprintf(stderr, "Could not copy the stream parameters\n");
    exit(1);
  }

  /* create resampler context */
  os->swr_ctx = swr_alloc();

  if(!os->swr_ctx)
  {
    fprintf(stderr, "Could not allocate resampler context\n");
    exit(1);
  }

  /* set options */
  av_opt_set_int(os->swr_ctx, "in_channel_count", c->channels, 0);
  av_opt_set_int(os->swr_ctx, "in_sample_rate", c->sample_rate, 0);
  av_opt_set_sample_fmt(os->swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
  av_opt_set_int(os->swr_ctx, "out_channel_count", c->channels, 0);
  av_opt_set_int(os->swr_ctx, "out_sample_rate", c->sample_rate, 0);
  av_opt_set_sample_fmt(os->swr_ctx, "out_sample_fmt", c->sample_fmt, 0);

  /* initialize the resampling context */
  if((ret = swr_init(os->swr_ctx)) < 0)
  {
    fprintf(stderr, "Failed to initialize the resampling context\n");
    exit(1);
  }
}

/* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
 * 'nb_channels' channels. */
AVFrame* get_audio_frame(OutputStream* os)
{
  AVFrame* frame = os->raw_frame;
  int16_t* q = (int16_t*)frame->data[0];

  for(int j = 0; j < frame->nb_samples; j++)
  {
    int v = (int)(sin(os->t * 2 * 3.14 * 440) * 10000);

    for(int i = 0; i < os->enc->channels; i++)
      *q++ = v;

    os->t += 1.0 / SAMPLE_RATE;
  }

  frame->pts = os->nextPts;
  os->nextPts += frame->nb_samples;

  return frame;
}

void writeOneAudioFrame(AVFormatContext* oc, OutputStream* os)
{
  int ret;

  AVPacket pkt {};
  av_init_packet(&pkt);

  AVCodecContext* c = os->enc;

  AVFrame* frame = get_audio_frame(os);

  if(frame)
  {
    /* convert samples from native format to destination codec format, using the resampler */
    /* compute destination number of samples */
    const int dst_nb_samples = av_rescale_rnd(swr_get_delay(os->swr_ctx, c->sample_rate) + frame->nb_samples,
                                              c->sample_rate, c->sample_rate, AV_ROUND_UP);
    assert(dst_nb_samples == frame->nb_samples);

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally;
     * make sure we do not overwrite it here
     */
    ret = av_frame_make_writable(os->converted_frame);
    assert(ret >= 0);

    /* convert to destination format */
    ret = swr_convert(os->swr_ctx,
                      os->converted_frame->data, dst_nb_samples,
                      (const uint8_t**)frame->data, frame->nb_samples);

    if(ret < 0)
    {
      fprintf(stderr, "Error while converting\n");
      exit(1);
    }

    frame = os->converted_frame;

    frame->pts = av_rescale_q(os->sampleCount, (AVRational) { 1, c->sample_rate }, c->time_base);
    os->sampleCount += dst_nb_samples;
  }

  int got_packet;
  ret = avcodec_encode_audio2(c, &pkt, frame, &got_packet);

  if(ret < 0)
    throw std::runtime_error(std::string("Error encoding audio frame: ") + averrorToString(ret));

  if(got_packet)
    write_frame(oc, &c->time_base, os->st, &pkt);
}

/**************************************************************/
/* video output */

AVFrame* alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
  AVFrame* picture = av_frame_alloc();

  if(!picture)
    throw std::runtime_error("Can't alloc intermediate picture");

  picture->format = pix_fmt;
  picture->width = width;
  picture->height = height;

  int ret = av_frame_get_buffer(picture, 32);

  if(ret < 0)
    throw std::runtime_error("Can't alloc frame data");

  return picture;
}

void safe_av_opt_set(void* obj, const char* name, const char* sval, int ival)
{
  int ret = av_opt_set(obj, name, sval, ival);

  if(ret != 0)
    printf("WARNING: can't set '%s' to '%s'\n", name, sval);
}

void open_video(AVCodec* codec, OutputStream* os)
{
  AVCodecContext* c = os->enc;

  safe_av_opt_set(c->priv_data, "profile", "baseline", 0);
  safe_av_opt_set(c->priv_data, "tune", "zerolatency", 0);

  int ret = avcodec_open2(c, codec, nullptr);

  if(ret < 0)
  {
    fprintf(stderr, "Could not open video codec: %s\n", averrorToString(ret));
    exit(1);
  }

  os->converted_frame = alloc_picture(c->pix_fmt, c->width, c->height);
  assert(os->converted_frame);

  os->raw_frame = alloc_picture(AV_PIX_FMT_RGBA, c->width, c->height);
  assert(os->raw_frame);

  // copy the stream parameters to the muxer
  ret = avcodec_parameters_from_context(os->st->codecpar, c);

  if(ret < 0)
  {
    fprintf(stderr, "Could not copy the stream parameters\n");
    exit(1);
  }
}

AVFrame* get_video_frame(OutputStream* os)
{
  AVCodecContext* c = os->enc;

  /* when we pass a frame to the encoder, it may keep a reference to it
   * internally; make sure we do not overwrite it here */
  int ret = av_frame_make_writable(os->converted_frame);
  assert(ret >= 0);

  {
    /* as we only generate a YUV420P picture, we must convert it
     * to the codec pixel format if needed */
    if(!os->sws_ctx)
    {
      os->sws_ctx = sws_getContext(c->width, c->height,
                                   AV_PIX_FMT_RGBA,
                                   c->width, c->height,
                                   c->pix_fmt,
                                   SWS_BICUBIC, nullptr, nullptr, nullptr);

      if(!os->sws_ctx)
      {
        fprintf(stderr, "Could not initialize the conversion context\n");
        exit(1);
      }
    }

    memcpy(os->raw_frame->data[0], rgbaFrame.data(), rgbaFrame.size());

    int stride = os->raw_frame->linesize[0];
    uint8_t* srcData[] = { os->raw_frame->data[0] + stride * (c->height - 1) };
    const int srcStride[] = { -stride };

    sws_scale(os->sws_ctx,
              srcData,
              srcStride,
              0,
              c->height,
              os->converted_frame->data,
              os->converted_frame->linesize);
  }

  os->converted_frame->pts = os->nextPts++;

  return os->converted_frame;
}

void writeOneVideoFrame(AVFormatContext* oc, OutputStream* os)
{
  int got_packet = 0;
  AVPacket pkt {};

  AVCodecContext* c = os->enc;

  AVFrame* frame = get_video_frame(os);

  av_init_packet(&pkt);

  int ret = avcodec_encode_video2(c, &pkt, frame, &got_packet);

  if(ret < 0)
    throw std::runtime_error(std::string("Error encoding video frame: ") + averrorToString(ret));

  if(got_packet)
    write_frame(oc, &c->time_base, os->st, &pkt);
}

void close_stream(OutputStream* os)
{
  avcodec_free_context(&os->enc);
  av_frame_free(&os->converted_frame);
  av_frame_free(&os->raw_frame);
  sws_freeContext(os->sws_ctx);
  swr_free(&os->swr_ctx);
}

struct StreamingClient
{
  StreamingClient(Address address_, Size2i resolution, Socket* sock) :
    m_address(address_),
    m_sock(*sock),
    m_streamerThread(&StreamingClient::streamerThread, this, resolution)
  {
  }

  ~StreamingClient()
  {
    m_keepGoing = false;
    m_streamerThread.join();
    printf("Client left\n");
  }

  void touch()
  {
    m_lastSeenTime = SDL_GetTicks();
  }

  bool isExpired() const
  {
    return dead || SDL_GetTicks() - m_lastSeenTime > 5000;
  }

  bool dead = false;
  int m_lastSeenTime = 0;

private:
  void streamerThread(Size2i resolution)
  {
    try
    {
      safeStreamerThread(resolution);
    }
    catch(const std::exception& e)
    {
      fprintf(stderr, "Client thread error: %s\n", e.what());
      dead = true;
    }
  }

  static int sendTsPacketsToNetwork(void* opaque, uint8_t* data, int len)
  {
    return ((StreamingClient*)opaque)->sendTsPacketsToNetwork((const uint8_t*)data, len);
  }

  int sendTsPacketsToNetwork(const uint8_t* data, int len)
  {
    assert(len % 188 == 0);
    m_sock.send(m_address, data, len);
    return len;
  }

  void safeStreamerThread(Size2i resolution)
  {
    av_log_set_level(AV_LOG_VERBOSE);
    const std::string filename = "udp://" + m_address.toString();

    auto const fmt = av_guess_format("mpegts", nullptr, nullptr);
    assert(fmt);

    AVFormatContext* oc;
    avformat_alloc_output_context2(&oc, fmt, nullptr, nullptr);

    if(!oc)
      throw std::runtime_error("Can't open output context");

    OutputStream videoStream {};
    OutputStream audioStream {};

    auto video_codec = add_stream(&videoStream, oc, AV_CODEC_ID_H264, resolution);
    auto audio_codec = add_stream(&audioStream, oc, AV_CODEC_ID_AAC, {});

    open_video(video_codec, &videoStream);
    open_audio(audio_codec, &audioStream);

    av_dump_format(oc, 0, nullptr, 1);

    const auto SIZE = 1024 * 1024;

    oc->pb = avio_alloc_context((unsigned char*)av_malloc(SIZE), SIZE, 1, this, nullptr, &StreamingClient::sendTsPacketsToNetwork, nullptr);
    oc->flags |= AVFMT_FLAG_CUSTOM_IO;

    if(!oc->pb)
      throw std::runtime_error("Can't allocate avio");

    AVDictionary* opt = nullptr;
    av_dict_set(&opt, "vsync", "0", 0);

    int ret = avformat_write_header(oc, &opt);

    if(ret < 0)
      throw std::runtime_error(std::string("Error occurred when opening output stream: ") + averrorToString(ret));

    int queuedAudioTime = 0;
    int queuedVideoTime = 0;

    int lastTicks = SDL_GetTicks();

    while(m_keepGoing)
    {
      const int nowTicks = SDL_GetTicks();
      const int deltaTicks = nowTicks - lastTicks;
      lastTicks = nowTicks;

      queuedVideoTime += deltaTicks;
      queuedAudioTime += deltaTicks;

      if(queuedVideoTime > 0)
      {
        writeOneVideoFrame(oc, &videoStream);
        queuedVideoTime -= 1000 / STREAM_FRAME_RATE;
      }
      else if(queuedAudioTime > 0)
      {
        writeOneAudioFrame(oc, &audioStream);
        queuedAudioTime -= 1000 * (1024 / SAMPLE_RATE);
      }
      else
      {
        SDL_Delay(1);
      }
    }

    av_write_trailer(oc);

    close_stream(&videoStream);
    close_stream(&audioStream);

    av_free(oc->pb);

    avformat_free_context(oc);
  }

  const Address m_address;
  Socket& m_sock;
  std::thread m_streamerThread;
  bool m_keepGoing = true;
};

struct AddressLess
{
  bool operator () (const Address& a, const Address& b)
  {
    if(a.address < b.address)
      return true;

    if(a.address > b.address)
      return false;

    return a.port < b.port;
  }
};

struct StreamingServer : IStreamingServer
{
  StreamingServer(Size2i resolution) :
    m_sock(SERVER_PORT),
    m_resolution(resolution),
    m_thread(&StreamingServer::serverThread, this)
  {
    rgbaFrame.resize(resolution.width * resolution.height * 4);
    printf("[StreamingServer] started (%dx%d)\n", resolution.width, resolution.height);
    printf("[StreamingServer] waiting for connections ...\n");
  }

  ~StreamingServer()
  {
    m_keepGoing = false;
    m_thread.join();
    printf("[StreamingServer] stopped\n");
  }

  bool processOnePacket()
  {
    char pkt[2048];
    Address addr;
    int n = m_sock.recv(addr, pkt, sizeof pkt);

    if(n <= 0)
      return false; // no packet

    {
      auto iClient = m_clients.find(addr);

      if(iClient == m_clients.end())
      {
        printf("Received connection: %s\n", addr.toString().c_str());
        m_clients[addr] = std::make_unique<StreamingClient>(addr, m_resolution, &m_sock);
      }

      m_clients[addr]->touch();
    }

    const int op = pkt[0];
    switch(op)
    {
    case Op::KeyEvent:
      {
        SDL_Event evt {};
        memcpy(&evt.key, pkt + 1, sizeof evt.key);

        {
          std::unique_lock<std::mutex> lock(m_eventMutex);
          eventQueue.push_back(evt);
        }
        break;
      }
    case Op::Disconnect:
      {
        m_clients[addr]->dead = true;
        break;
      }
    }

    return true;
  }

  void serverThread()
  {
    while(m_keepGoing)
    {
      while(processOnePacket())
      {
      }

      removeDeadClients();
      SDL_Delay(1);
    }
  }

  void removeDeadClients()
  {
    for(auto i = m_clients.begin(), last = m_clients.end(); i != last;)
    {
      if(i->second->isExpired())
      {
        i = m_clients.erase(i);
      }
      else
      {
        ++i;
      }
    }
  }

  // IStreamingServer implementation
  void push(std::vector<uint8_t> const& pixels) override
  {
    assert(pixels.size() == rgbaFrame.size());
    rgbaFrame = pixels;
  }

  bool pollEvent(SDL_Event& event) override
  {
    std::unique_lock<std::mutex> lock(m_eventMutex);

    if(eventQueue.empty())
      return false;

    event = eventQueue.front();
    eventQueue.erase(eventQueue.begin());
    return true;
  }

  Socket m_sock;
  const Size2i m_resolution;
  bool m_keepGoing = true;
  std::thread m_thread;
  std::mutex m_eventMutex;
  std::vector<SDL_Event> eventQueue;
  std::map<Address, std::unique_ptr<StreamingClient>, AddressLess> m_clients;
};
}

std::unique_ptr<IStreamingServer> createServer(Size2i resolution)
{
  return std::make_unique<StreamingServer>(resolution);
}

