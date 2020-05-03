#include "player.h"

#include <iostream>
#include <vector>
#include <stdexcept>
#include <string>

#include <assert.h>
#include "SDL.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h> // av_opt_set_channel_layout
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/log.h>
}

namespace
{
const int WIDTH = 640;
const int HEIGHT = 480;

using namespace std;

AVFrame wanted_frame;

struct AudioPacket
{
  AVPacketList* first, * last;
  int nb_packets, size;
  SDL_mutex* mutex;
  SDL_cond* cond;
};

AudioPacket audioq;
void audio_callback(void*, Uint8*, int);

#define printError(errorCode) \
  printErrorFunc(errorCode, __FILE__, __LINE__)

void printErrorFunc(int errorCode, const char* file, int line)
{
  char msg[256];
  av_strerror(errorCode, msg, sizeof msg);
  cout << "[" << file << ":" << line << "] Error: " << msg << endl;
}

// timeout after 3 seconds
bool someDataWasReceived = false;
int cb_open(void*)
{
  if(someDataWasReceived)
    return 0;

  static auto const TimeStart = SDL_GetTicks();

  if(SDL_GetTicks() - TimeStart > 3000)
  {
    printf("No input data was received.\n");
    return 1;
  }

  return 0;
}

void getAudioPacket(AudioPacket* q, AVPacket* pkt)
{
  SDL_LockMutex(q->mutex);

  while(1)
  {
    AVPacketList* pktl = q->first;

    if(pktl)
    {
      q->first = pktl->next;

      if(!q->first)
        q->last = nullptr;

      q->nb_packets--;
      q->size -= pktl->pkt.size;

      *pkt = pktl->pkt;
      av_free(pktl);
      break;
    }
    else
    {
      SDL_CondWait(q->cond, q->mutex);
    }
  }

  SDL_UnlockMutex(q->mutex);
}

int audio_decode_frame(AVCodecContext* aCodecCtx, uint8_t* audio_buf, int buf_size)
{
  static AVPacket pkt;
  static uint8_t* audio_pkt_data = nullptr;
  static int audio_pkt_size = 0;
  static AVFrame frame;

  SwrContext* swr_ctx = nullptr;

  while(1)
  {
    while(audio_pkt_size > 0)
    {
      int got_frame = 0;

      avcodec_send_packet(aCodecCtx, &pkt);
      avcodec_receive_frame(aCodecCtx, &frame);

      const int len1 = frame.pkt_size;

      if(len1 < 0)
      {
        audio_pkt_size = 0;
        break;
      }

      audio_pkt_data += len1;
      audio_pkt_size -= len1;
      int data_size = 0;

      if(got_frame)
      {
        int linesize = 1;
        data_size = av_samples_get_buffer_size(&linesize, aCodecCtx->channels, frame.nb_samples, aCodecCtx->sample_fmt, 1);
        assert(data_size <= buf_size);
        memcpy(audio_buf, frame.data[0], data_size);
      }

      if(frame.channels > 0 && frame.channel_layout == 0)
        frame.channel_layout = av_get_default_channel_layout(frame.channels);
      else if(frame.channels == 0 && frame.channel_layout > 0)
        frame.channels = av_get_channel_layout_nb_channels(frame.channel_layout);

      if(swr_ctx)
      {
        swr_free(&swr_ctx);
        swr_ctx = nullptr;
      }

      swr_ctx = swr_alloc_set_opts(nullptr, wanted_frame.channel_layout, (AVSampleFormat)wanted_frame.format, wanted_frame.sample_rate,
                                   frame.channel_layout, (AVSampleFormat)frame.format, frame.sample_rate, 0, nullptr);

      if(!swr_ctx || swr_init(swr_ctx) < 0)
      {
        cout << "swr_init failed" << endl;
        break;
      }

      int dst_nb_samples = (int)av_rescale_rnd(swr_get_delay(swr_ctx, frame.sample_rate) + frame.nb_samples,
                                               wanted_frame.sample_rate, wanted_frame.format, AV_ROUND_INF);
      int len2 = swr_convert(swr_ctx, &audio_buf, dst_nb_samples,
                             (const uint8_t**)frame.data, frame.nb_samples);

      if(len2 < 0)
      {
        cout << "swr_convert failed" << endl;
        break;
      }

      return wanted_frame.channels * len2 * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);

      if(data_size <= 0)
        continue;

      return data_size;
    }

    if(pkt.data)
      av_packet_unref(&pkt);

    getAudioPacket(&audioq, &pkt);

    audio_pkt_data = pkt.data;
    audio_pkt_size = pkt.size;
  }
}

void audio_callback(void* userdata, Uint8* stream, int len)
{
  AVCodecContext* aCodecCtx = (AVCodecContext*)userdata;

  static uint8_t buf[1024 * 1024];
  static unsigned int audio_buf_size = 0;
  static unsigned int audio_buf_index = 0;

  memset(stream, 0, len);

  while(len > 0)
  {
    if(audio_buf_index >= audio_buf_size)
    {
      const int audio_size = audio_decode_frame(aCodecCtx, buf, sizeof(buf));

      if(audio_size < 0)
      {
        audio_buf_size = 1024;
        memset(buf, 0, audio_buf_size);
      }
      else
        audio_buf_size = audio_size;

      audio_buf_index = 0;
    }

    int len1 = audio_buf_size - audio_buf_index;

    if(len1 > len)
      len1 = len;

    memcpy(stream, (uint8_t*)(buf + audio_buf_index), audio_buf_size);
    len -= len1;
    stream += len1;
    audio_buf_index += len1;
  }
}

///////////////////////////////////////////////////////////////////////////////

class Player
{
public:
  Player(Address address_) : address(address_), m_sock(0)
  {
    av_register_all(); // Ubuntu compat

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
      throw std::runtime_error("Can't init SDL");

    createDisplay();

    // send a first keepalive to establish the connection
    {
      const uint8_t pkt[] = { Op::KeepAlive };
      m_sock.send(address, pkt, sizeof pkt);
    }

    av_log_set_level(AV_LOG_VERBOSE);
    const int clientPort = m_sock.port();
    openUrl("udp://:" + std::to_string(clientPort));
  }

  ~Player()
  {
    av_free(m_decodedFrame);

    avcodec_close(pVideoCtx);
    avformat_close_input(&pFormatCtx);

    SDL_Quit();
  }

  void openUrl(std::string url)
  {
    avformat_network_init();
    pFormatCtx = avformat_alloc_context();

    {
      pFormatCtx->interrupt_callback.callback = cb_open;
      pFormatCtx->interrupt_callback.opaque = 0;
    }

    AVDictionary* opt = nullptr;

    av_dict_set(&opt, "analyzeduration", "8000000", 0);
    av_dict_set(&opt, "probesize", "8000000", 0);
    av_dict_set(&opt, "reuse_socket", "1", 0);
    av_dict_set(&opt, "buffer_size", "1048576", 0); // 1MB
    av_dict_set(&opt, "correct_ts_overflow", "1", 0);

    auto format = av_find_input_format("mpegts");
    if(!format)
      throw std::runtime_error("Can't find input format 'mpegts'");

    int res = avformat_open_input(&pFormatCtx, url.c_str(), format, &opt);
    assert(opt == nullptr);

    // check video opened
    if(res != 0)
    {
      printError(res);
      exit(-1);
    }

    // get video info
    res = avformat_find_stream_info(pFormatCtx, nullptr);

    if(res < 0)
    {
      printError(res);
      exit(-1);
    }

    chooseStreams();
    createDecoders();

    someDataWasReceived = true;

    m_decodedFrame = av_frame_alloc();
    assert(m_decodedFrame);
  }

  void init()
  {
    auto resampler = swr_alloc();

    if(resampler == nullptr)
      throw std::runtime_error("Failed to create audio resampler");

    // audio context
    av_opt_set_channel_layout(resampler, "in_channel_layout", pAudioCtx->channel_layout, 0);
    av_opt_set_channel_layout(resampler, "out_channel_layout", pAudioCtx->channel_layout, 0);
    av_opt_set_int(resampler, "in_sample_rate", pAudioCtx->sample_rate, 0);
    av_opt_set_int(resampler, "out_sample_rate", pAudioCtx->sample_rate, 0);
    av_opt_set_sample_fmt(resampler, "in_sample_fmt", pAudioCtx->sample_fmt, 0);
    av_opt_set_sample_fmt(resampler, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    int res = swr_init(resampler);

    if(res != 0)
      throw std::runtime_error("Failed to init audio resampler");

    SDL_AudioSpec wantedSpec {};
    wantedSpec.channels = pAudioCtx->channels;
    wantedSpec.freq = pAudioCtx->sample_rate;
    wantedSpec.format = AUDIO_S16SYS;
    wantedSpec.silence = 0;
    wantedSpec.samples = 1024;
    wantedSpec.userdata = pAudioCtx;
    wantedSpec.callback = audio_callback;

    if(SDL_OpenAudio(&wantedSpec, &audioSpec) < 0)
      throw std::runtime_error("Failed to init audio device");

    wanted_frame.format = AV_SAMPLE_FMT_S16;
    wanted_frame.sample_rate = audioSpec.freq;
    wanted_frame.channel_layout = av_get_default_channel_layout(audioSpec.channels);
    wanted_frame.channels = audioSpec.channels;

    initAudioPacket(&audioq);
    SDL_PauseAudio(0);
  }

  void run()
  {
    videoConverter = sws_getContext(pVideoCtx->width,
                                    pVideoCtx->height,
                                    pVideoCtx->pix_fmt,
                                    WIDTH,
                                    HEIGHT,
                                    AV_PIX_FMT_RGBA,
                                    SWS_BILINEAR,
                                    nullptr,
                                    nullptr,
                                    nullptr
                                    );
    assert(videoConverter);

    m_convertedPicture = av_frame_alloc();

    std::vector<uint8_t> buf2(avpicture_get_size(AV_PIX_FMT_RGBA, WIDTH, HEIGHT));
    // av_image_fill_arrays();
    avpicture_fill((AVPicture*)m_convertedPicture, buf2.data(), AV_PIX_FMT_RGBA, WIDTH, HEIGHT);

    int lastKeepAlive = 0;

    while(1)
    {
      {
        AVPacket packet {};

        if(av_read_frame(pFormatCtx, &packet) >= 0)
        {
          if(packet.stream_index == audioStream)
            processAudio(&audioq, packet);

          if(packet.stream_index == videoStream)
            processVideo(packet);
        }

        av_packet_unref(&packet);
      }

      SDL_Event evt;

      while(SDL_PollEvent(&evt))
      {
        if(evt.type == SDL_QUIT)
        {
          const uint8_t pkt[] = { Op::Disconnect };
          m_sock.send(address, pkt, sizeof pkt);
          return;
        }

        if(evt.type == SDL_KEYDOWN || evt.type == SDL_KEYUP)
        {
          if(evt.key.keysym.sym != SDLK_ESCAPE)
          {
            uint8_t pkt[1024];
            assert(sizeof evt.key < sizeof pkt);
            int len = 0;
            pkt[len++] = Op::KeyEvent;
            memcpy(pkt + 1, &evt.key, sizeof evt.key);
            len += sizeof evt.key;

            m_sock.send(address, pkt, len);
            lastKeepAlive = SDL_GetTicks();
          }
        }
      }

      if(SDL_GetTicks() - lastKeepAlive > 1000)
      {
        const uint8_t pkt[] = { Op::KeepAlive };
        m_sock.send(address, pkt, sizeof pkt);
        lastKeepAlive = SDL_GetTicks();
      }
    }
  }

  void createDisplay()
  {
    screen = SDL_CreateWindow("aceplayer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIDTH, HEIGHT, 0);
    assert(screen);

    renderer = SDL_CreateRenderer(screen, -1, 0);
    assert(renderer);

    m_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
    assert(m_texture);
  }

private:
  const Address address;
  Socket m_sock;
  int videoStream = -1;
  int audioStream = -1;

  AVFormatContext* pFormatCtx = nullptr;
  AVCodecContext* pVideoCtx = nullptr;
  AVCodecContext* pAudioCtx = nullptr;
  SwsContext* videoConverter = nullptr;
  SDL_AudioSpec audioSpec {};

  AVFrame* m_decodedFrame = nullptr;
  AVFrame* m_convertedPicture = nullptr;

  // display
  SDL_Window* screen;
  SDL_Renderer* renderer;
  SDL_Texture* m_texture;

  int chooseStreams()
  {
    videoStream = -1;
    audioStream = -1;

    for(int i = 0; i < (int)pFormatCtx->nb_streams; i++)
    {
      if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        videoStream = i;

      if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        audioStream = i;
    }

    assert(videoStream >= 0);
    assert(audioStream >= 0);

    return videoStream;
  }

  int createDecoders()
  {
    AVCodecParameters* pVideoParams = pFormatCtx->streams[videoStream]->codecpar;
    AVCodecParameters* pAudioParams = pFormatCtx->streams[audioStream]->codecpar;

    AVCodec* pVideoCodec = avcodec_find_decoder(pVideoParams->codec_id);
    assert(pVideoCodec);

    AVCodec* pAudioCodec = avcodec_find_decoder(pAudioParams->codec_id);
    assert(pAudioCodec);

    pVideoCtx = avcodec_alloc_context3(pVideoCodec);

    if(pVideoCtx == nullptr)
    {
      cout << "Bad video codec" << endl;
      exit(-1);
    }

    pAudioCtx = avcodec_alloc_context3(pAudioCodec);

    if(pAudioCtx == nullptr)
    {
      cout << "Bad audio codec" << endl;
      exit(-1);
    }

    int res = avcodec_parameters_to_context(pVideoCtx, pVideoParams);

    if(res < 0)
    {
      cout << "Failed to get video codec" << endl;
      avformat_close_input(&pFormatCtx);
      avcodec_free_context(&pVideoCtx);
      exit(-1);
    }

    res = avcodec_parameters_to_context(pAudioCtx, pAudioParams);

    if(res < 0)
    {
      cout << "Failed to get audio codec" << endl;
      avformat_close_input(&pFormatCtx);
      avcodec_free_context(&pVideoCtx);
      avcodec_free_context(&pAudioCtx);
      exit(-1);
    }

    res = avcodec_open2(pVideoCtx, pVideoCodec, nullptr);

    if(res < 0)
    {
      cout << "Failed to open video codec" << endl;
      exit(-1);
    }

    res = avcodec_open2(pAudioCtx, pAudioCodec, nullptr);

    if(res < 0)
    {
      cout << "Failed to open audio codec" << endl;
      exit(-1);
    }

    return 1;
  }

  void initAudioPacket(AudioPacket* q)
  {
    q->last = nullptr;
    q->first = nullptr;
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
  }

  int processAudio(AudioPacket* q, AVPacket& pkt)
  {
    AVPacket* newPkt = (AVPacket*)av_mallocz_array(1, sizeof(AVPacket));

    if(av_packet_ref(newPkt, &pkt) < 0)
      return -1;

    AVPacketList* pktl = (AVPacketList*)av_malloc(sizeof(AVPacketList));

    if(!pktl)
      return -1;

    pktl->pkt = *newPkt;
    pktl->next = nullptr;

    SDL_LockMutex(q->mutex);

    if(!q->last)
      q->first = pktl;
    else
      q->last->next = pktl;

    q->last = pktl;

    q->nb_packets++;
    q->size += newPkt->size;

    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);

    return 0;
  }

  void processVideo(AVPacket& packet)
  {
    int res = avcodec_send_packet(pVideoCtx, &packet);

    if(res < 0)
    {
      printError(res);
      return;
    }

    res = avcodec_receive_frame(pVideoCtx, m_decodedFrame);

    if(res < 0)
    {
      printError(res);
      return;
    }

    if(pVideoCtx->width < 0)
      return;

    sws_scale(videoConverter, m_decodedFrame->data, m_decodedFrame->linesize, 0, pVideoCtx->height, m_convertedPicture->data, m_convertedPicture->linesize);

    SDL_UpdateTexture(m_texture, nullptr, m_convertedPicture->data[0], m_convertedPicture->linesize[0]);
    SDL_RenderCopy(renderer, m_texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    SDL_UpdateWindowSurface(screen);
  }
};
}

void playerMain(Address serverAddr)
{
  Player player(serverAddr);

  player.init();
  player.run();
}

