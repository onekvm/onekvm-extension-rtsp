#include "rtsp_server.hpp"

#include <BasicUsageEnvironment.hh>
#include <GroupsockHelper.hh>
#include <H264VideoRTPSink.hh>
#include <H264VideoStreamDiscreteFramer.hh>
#include <H265VideoRTPSink.hh>
#include <H265VideoStreamDiscreteFramer.hh>
#include <SimpleRTPSink.hh>
#include <liveMedia.hh>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace onekvm::rtsp {
namespace {

constexpr std::size_t kMaxQueuedAccessUnits = 2;
constexpr std::size_t kMaxQueuedAudioPackets = 8;
constexpr unsigned kEstimatedBitrateKbps = 20000;
constexpr unsigned kEstimatedAudioBitrateKbps = 128;
constexpr unsigned kAuxSDPChecks = 30;

std::string ClientAddress(const sockaddr_storage &address) {
  char host[NI_MAXHOST]{};
  socklen_t size = sizeof(address);
  if (address.ss_family == AF_INET)
    size = sizeof(sockaddr_in);
  else if (address.ss_family == AF_INET6)
    size = sizeof(sockaddr_in6);
  if (getnameinfo(reinterpret_cast<const sockaddr *>(&address), size, host,
                  sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0)
    return {};
  return host;
}

int CreateListenSocket(const std::string &address, int port, int &family,
                       std::string &error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo *addresses = nullptr;
  const auto service = std::to_string(port);
  const int resolved =
      getaddrinfo(address.c_str(), service.c_str(), &hints, &addresses);
  if (resolved != 0) {
    error =
        std::string("resolve RTSP listen address: ") + gai_strerror(resolved);
    return -1;
  }

  int listen_fd = -1;
  for (auto *candidate = addresses; candidate != nullptr;
       candidate = candidate->ai_next) {
    const int fd = socket(candidate->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      continue;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (candidate->ai_family == AF_INET6) {
      setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    }
    if (bind(fd, candidate->ai_addr, candidate->ai_addrlen) == 0 &&
        listen(fd, 20) == 0) {
      const int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      listen_fd = fd;
      family = candidate->ai_family;
      break;
    }
    close(fd);
  }
  freeaddrinfo(addresses);
  if (listen_fd < 0) {
    error = std::string("listen RTSP: ") + std::strerror(errno);
  }
  return listen_fd;
}

class TrackedRTSPClientConnection;
class TrackedRTSPClientSession;

class BoundRTSPServer final : public RTSPServer {
public:
  static BoundRTSPServer *Create(UsageEnvironment &env, const Config &config,
                                 const Identity &identity, Codec codec,
                                 UserAuthenticationDatabase *auth,
                                 std::string &error) {
    int family = AF_UNSPEC;
    const int socket_fd = CreateListenSocket(config.listen_address, config.port,
                                             family, error);
    if (socket_fd < 0)
      return nullptr;
    Port server_port(static_cast<portNumBits>(config.port));
    return new BoundRTSPServer(env, family == AF_INET ? socket_fd : -1,
                               family == AF_INET6 ? socket_fd : -1, server_port,
                               auth, config, identity, codec);
  }

  const Config &ServerConfig() const { return config_; }
  const Identity &ServerIdentity() const { return identity_; }
  Codec ServerCodec() const { return codec_; }

private:
  BoundRTSPServer(UsageEnvironment &env, int ipv4_socket, int ipv6_socket,
                  Port port, UserAuthenticationDatabase *auth,
                  const Config &config, const Identity &identity, Codec codec)
      : RTSPServer(env, ipv4_socket, ipv6_socket, port, auth, 65),
        config_(config), identity_(identity), codec_(codec) {}

  GenericMediaServer::ClientConnection *
  createNewClientConnection(int client_socket,
                            sockaddr_storage const &client_address) override;
  GenericMediaServer::ClientSession *
  createNewClientSession(u_int32_t session_id) override;

  Config config_;
  Identity identity_;
  Codec codec_;
};

class TrackedRTSPClientConnection final
    : public RTSPServer::RTSPClientConnection {
public:
  const std::string &ClientIP() const { return client_ip_; }

private:
  friend class BoundRTSPServer;
  TrackedRTSPClientConnection(BoundRTSPServer &server, int client_socket,
                              sockaddr_storage const &client_address)
      : RTSPClientConnection(server, client_socket, client_address),
        client_ip_(ClientAddress(client_address)) {}

  std::string client_ip_;
};

class TrackedRTSPClientSession final : public RTSPServer::RTSPClientSession {
private:
  friend class BoundRTSPServer;
  TrackedRTSPClientSession(BoundRTSPServer &server, u_int32_t session_id)
      : RTSPClientSession(server, session_id), server_(server) {}

  void handleCmd_PLAY(RTSPServer::RTSPClientConnection *connection,
                      ServerMediaSubsession *subsession,
                      char const *full_request) override {
    RTSPClientSession::handleCmd_PLAY(connection, subsession, full_request);
    if (reporter_ != nullptr || fOurServerMediaSession == nullptr ||
        fNumStreamStates == 0)
      return;
    auto *tracked = dynamic_cast<TrackedRTSPClientConnection *>(connection);
    if (tracked == nullptr || tracked->ClientIP().empty())
      return;
    const auto &config = server_.ServerConfig();
    std::string error;
    auto reporter = std::make_unique<SessionReporter>(
        config.media_socket, server_.ServerIdentity(), tracked->ClientIP(),
        config.access_control_enabled ? config.username : "",
        usesTCPTransport() ? "tcp" : "udp", server_.ServerCodec(), error);
    if (reporter->Ready()) {
      reporter_ = std::move(reporter);
    } else {
      envir() << "onekvm-protocol-rtsp: report client session: "
              << error.c_str() << "\n";
    }
  }

  void handleCmd_PAUSE(RTSPServer::RTSPClientConnection *connection,
                       ServerMediaSubsession *subsession) override {
    reporter_.reset();
    RTSPClientSession::handleCmd_PAUSE(connection, subsession);
  }

  void handleCmd_TEARDOWN(RTSPServer::RTSPClientConnection *connection,
                          ServerMediaSubsession *subsession) override {
    reporter_.reset();
    RTSPClientSession::handleCmd_TEARDOWN(connection, subsession);
  }

  BoundRTSPServer &server_;
  std::unique_ptr<SessionReporter> reporter_;
};

GenericMediaServer::ClientConnection *
BoundRTSPServer::createNewClientConnection(
    int client_socket, sockaddr_storage const &client_address) {
  return new TrackedRTSPClientConnection(*this, client_socket, client_address);
}

GenericMediaServer::ClientSession *
BoundRTSPServer::createNewClientSession(u_int32_t session_id) {
  return new TrackedRTSPClientSession(*this, session_id);
}

class EncodedHub {
public:
  EncodedHub(const Config &config, const Identity &identity, Codec codec,
             std::atomic<bool> &fatal, std::string &fatal_error)
      : fatal_(fatal), fatal_error_(fatal_error) {
    const VideoProfile profile{codec, config.fps, config.quality_factor,
                               config.bitrate_kbps, config.initial_qp,
                               config.min_qp, config.max_qp, config.gop};
    subscription_ = std::make_unique<MediaSubscription>(
        config.media_socket, identity, profile,
        [this](std::vector<NalPacket> packets) { EnqueueVideo(std::move(packets)); },
        [this](NalPacket packet) { EnqueueAudio(std::move(packet)); },
        [this]() { Notify(); },
        [this](std::string message) {
          fatal_error_ = std::move(message);
          fatal_.store(true);
          kill(getpid(), SIGTERM);
        });
  }

  EncodedHub(const EncodedHub &) = delete;
  EncodedHub &operator=(const EncodedHub &) = delete;

  ~EncodedHub() { subscription_->Stop(); }

  void Retain() {
    bool start = false;
    {
      std::lock_guard lock(mutex_);
      start = refs_++ == 0;
    }
    if (start)
      subscription_->Start();
  }

  void Release() {
    bool stop = false;
    {
      std::lock_guard lock(mutex_);
      if (refs_ > 0)
        stop = --refs_ == 0;
      if (stop) {
        video_.clear();
        audio_.clear();
        queued_access_units_ = 0;
      }
    }
    if (stop)
      subscription_->Stop();
  }

  void BindVideo(UsageEnvironment *env, EventTriggerId trigger, void *client) {
    std::lock_guard lock(mutex_);
    video_env_ = env;
    video_trigger_ = trigger;
    video_client_ = client;
  }

  void ClearVideo() {
    std::lock_guard lock(mutex_);
    video_env_ = nullptr;
    video_trigger_ = 0;
    video_client_ = nullptr;
  }

  void BindAudio(UsageEnvironment *env, EventTriggerId trigger, void *client) {
    std::lock_guard lock(mutex_);
    audio_env_ = env;
    audio_trigger_ = trigger;
    audio_client_ = client;
  }

  void ClearAudio() {
    std::lock_guard lock(mutex_);
    audio_env_ = nullptr;
    audio_trigger_ = 0;
    audio_client_ = nullptr;
  }

  bool PopVideo(NalPacket &packet) {
    std::lock_guard lock(mutex_);
    if (video_.empty())
      return false;
    packet = std::move(video_.front());
    video_.pop_front();
    if (packet.end_access_unit && queued_access_units_ > 0)
      --queued_access_units_;
    return true;
  }

  bool PopAudio(NalPacket &packet) {
    std::lock_guard lock(mutex_);
    if (audio_.empty())
      return false;
    packet = std::move(audio_.front());
    audio_.pop_front();
    return true;
  }

  void RequestKeyFrame() { subscription_->RequestKeyFrame(); }

private:
  void Notify() {
    std::lock_guard lock(mutex_);
    if (video_env_ != nullptr)
      video_env_->taskScheduler().triggerEvent(video_trigger_, video_client_);
    if (audio_env_ != nullptr)
      audio_env_->taskScheduler().triggerEvent(audio_trigger_, audio_client_);
  }

  void EnqueueVideo(std::vector<NalPacket> packets) {
    if (packets.empty())
      return;
    std::lock_guard lock(mutex_);
    const bool keyframe = packets.front().keyframe;
    if (keyframe) {
      video_.clear();
      queued_access_units_ = 0;
    } else if (queued_access_units_ >= kMaxQueuedAccessUnits &&
               !video_.empty() && video_.front().keyframe) {
      return;
    }
    while (queued_access_units_ >= kMaxQueuedAccessUnits && !video_.empty()) {
      const bool end = video_.front().end_access_unit;
      video_.pop_front();
      if (end)
        --queued_access_units_;
    }
    for (auto &packet : packets)
      video_.push_back(std::move(packet));
    ++queued_access_units_;
  }

  void EnqueueAudio(NalPacket packet) {
    if (packet.data == nullptr || packet.size == 0)
      return;
    std::lock_guard lock(mutex_);
    while (audio_.size() >= kMaxQueuedAudioPackets)
      audio_.pop_front();
    audio_.push_back(std::move(packet));
  }

  std::atomic<bool> &fatal_;
  std::string &fatal_error_;
  std::unique_ptr<MediaSubscription> subscription_;
  std::mutex mutex_;
  int refs_ = 0;
  std::deque<NalPacket> video_;
  std::deque<NalPacket> audio_;
  std::size_t queued_access_units_ = 0;
  UsageEnvironment *video_env_ = nullptr;
  EventTriggerId video_trigger_ = 0;
  void *video_client_ = nullptr;
  UsageEnvironment *audio_env_ = nullptr;
  EventTriggerId audio_trigger_ = 0;
  void *audio_client_ = nullptr;
};

class QueueSource final : public FramedSource {
public:
  static QueueSource *Create(UsageEnvironment &env, EncodedHub &hub) {
    return new QueueSource(env, hub);
  }

  bool LastNalEndsAccessUnit() const { return last_nal_ends_access_unit_; }

  void RequestKeyFrame() { hub_.RequestKeyFrame(); }

protected:
  QueueSource(UsageEnvironment &env, EncodedHub &hub)
      : FramedSource(env), hub_(hub) {
    event_trigger_ = envir().taskScheduler().createEventTrigger(DeliverFrame0);
    hub_.BindVideo(&envir(), event_trigger_, this);
    hub_.Retain();
  }

  ~QueueSource() override {
    hub_.ClearVideo();
    envir().taskScheduler().deleteEventTrigger(event_trigger_);
    hub_.Release();
  }

private:
  static void DeliverFrame0(void *client_data) {
    static_cast<QueueSource *>(client_data)->DeliverFrame();
  }

  void doGetNextFrame() override { DeliverFrame(); }

  void DeliverFrame() {
    if (!isCurrentlyAwaitingData())
      return;
    NalPacket packet;
    if (!hub_.PopVideo(packet))
      return;
    fFrameSize = static_cast<unsigned>(
        std::min<std::size_t>(packet.size, fMaxSize));
    fNumTruncatedBytes = static_cast<unsigned>(packet.size - fFrameSize);
    std::memcpy(fTo, packet.data, fFrameSize);
    gettimeofday(&fPresentationTime, nullptr);
    fDurationInMicroseconds =
        packet.duration_us == 0 ? 16667 : packet.duration_us;
    last_nal_ends_access_unit_ = packet.end_access_unit;
    FramedSource::afterGetting(this);
  }

  EncodedHub &hub_;
  EventTriggerId event_trigger_ = 0;
  bool last_nal_ends_access_unit_ = false;
};

class OpusSource final : public FramedSource {
public:
  static OpusSource *Create(UsageEnvironment &env, EncodedHub &hub) {
    return new OpusSource(env, hub);
  }

protected:
  OpusSource(UsageEnvironment &env, EncodedHub &hub)
      : FramedSource(env), hub_(hub) {
    event_trigger_ = envir().taskScheduler().createEventTrigger(DeliverFrame0);
    hub_.BindAudio(&envir(), event_trigger_, this);
    hub_.Retain();
  }

  ~OpusSource() override {
    hub_.ClearAudio();
    envir().taskScheduler().deleteEventTrigger(event_trigger_);
    hub_.Release();
  }

private:
  static void DeliverFrame0(void *client_data) {
    static_cast<OpusSource *>(client_data)->DeliverFrame();
  }

  void doGetNextFrame() override { DeliverFrame(); }

  void DeliverFrame() {
    if (!isCurrentlyAwaitingData())
      return;
    NalPacket packet;
    if (!hub_.PopAudio(packet))
      return;
    fFrameSize = static_cast<unsigned>(
        std::min<std::size_t>(packet.size, fMaxSize));
    fNumTruncatedBytes = static_cast<unsigned>(packet.size - fFrameSize);
    std::memcpy(fTo, packet.data, fFrameSize);
    gettimeofday(&fPresentationTime, nullptr);
    fDurationInMicroseconds =
        packet.duration_us == 0 ? 20000 : packet.duration_us;
    FramedSource::afterGetting(this);
  }

  EncodedHub &hub_;
  EventTriggerId event_trigger_ = 0;
};

class H264AccessUnitFramer final : public H264VideoStreamDiscreteFramer {
public:
  static H264AccessUnitFramer *Create(UsageEnvironment &env,
                                      QueueSource *source) {
    return new H264AccessUnitFramer(env, source);
  }

  void RequestKeyFrame() { source_->RequestKeyFrame(); }

protected:
  H264AccessUnitFramer(UsageEnvironment &env, QueueSource *source)
      : H264VideoStreamDiscreteFramer(env, source, False, False),
        source_(source) {}

  Boolean nalUnitEndsAccessUnit(u_int8_t) override {
    return source_->LastNalEndsAccessUnit() ? True : False;
  }

private:
  QueueSource *source_;
};

class H265AccessUnitFramer final : public H265VideoStreamDiscreteFramer {
public:
  static H265AccessUnitFramer *Create(UsageEnvironment &env,
                                      QueueSource *source) {
    return new H265AccessUnitFramer(env, source);
  }

  void RequestKeyFrame() { source_->RequestKeyFrame(); }

protected:
  H265AccessUnitFramer(UsageEnvironment &env, QueueSource *source)
      : H265VideoStreamDiscreteFramer(env, source, False, False),
        source_(source) {}

  Boolean nalUnitEndsAccessUnit(u_int8_t) override {
    return source_->LastNalEndsAccessUnit() ? True : False;
  }

private:
  QueueSource *source_;
};

class OneKVMSubsession final : public OnDemandServerMediaSubsession {
public:
  static OneKVMSubsession *Create(UsageEnvironment &env, Codec codec,
                                  EncodedHub &hub) {
    return new OneKVMSubsession(env, codec, hub);
  }

protected:
  OneKVMSubsession(UsageEnvironment &env, Codec codec, EncodedHub &hub)
      : OnDemandServerMediaSubsession(env, True), codec_(codec), hub_(hub) {}

  ~OneKVMSubsession() override { delete[] aux_sdp_line_; }

  FramedSource *createNewStreamSource(unsigned,
                                      unsigned &estimated_bitrate) override {
    estimated_bitrate = kEstimatedBitrateKbps;
    auto *source = QueueSource::Create(envir(), hub_);
    if (codec_ == Codec::kH264)
      return H264AccessUnitFramer::Create(envir(), source);
    return H265AccessUnitFramer::Create(envir(), source);
  }

  RTPSink *createNewRTPSink(Groupsock *groupsock, unsigned char payload_type,
                            FramedSource *) override {
    if (codec_ == Codec::kH264) {
      return H264VideoRTPSink::createNew(envir(), groupsock, payload_type);
    }
    return H265VideoRTPSink::createNew(envir(), groupsock, payload_type);
  }

  void startStream(
      unsigned client_session_id, void *stream_token,
      TaskFunc *rtcp_rr_handler, void *rtcp_rr_handler_client_data,
      unsigned short &rtp_sequence, unsigned &rtp_timestamp,
      ServerRequestAlternativeByteHandler *alternative_byte_handler,
      void *alternative_byte_handler_client_data) override {
    OnDemandServerMediaSubsession::startStream(
        client_session_id, stream_token, rtcp_rr_handler,
        rtcp_rr_handler_client_data, rtp_sequence, rtp_timestamp,
        alternative_byte_handler, alternative_byte_handler_client_data);
    auto *source = getStreamSource(stream_token);
    if (source == nullptr)
      return;
    if (codec_ == Codec::kH264) {
      static_cast<H264AccessUnitFramer *>(source)->RequestKeyFrame();
    } else {
      static_cast<H265AccessUnitFramer *>(source)->RequestKeyFrame();
    }
  }

  char const *getAuxSDPLine(RTPSink *sink, FramedSource *source) override {
    if (aux_sdp_line_ != nullptr)
      return aux_sdp_line_;
    if (dummy_sink_ == nullptr) {
      dummy_sink_ = sink;
      done_flag_ = 0;
      checks_remaining_ = kAuxSDPChecks;
      dummy_sink_->startPlaying(*source, AfterPlayingDummy, this);
      CheckForAuxSDPLine(this);
    }
    envir().taskScheduler().doEventLoop(&done_flag_);
    return aux_sdp_line_;
  }

private:
  static void AfterPlayingDummy(void *client_data) {
    auto *self = static_cast<OneKVMSubsession *>(client_data);
    self->envir().taskScheduler().unscheduleDelayedTask(self->nextTask());
    self->done_flag_ = 1;
  }

  static void CheckForAuxSDPLine(void *client_data) {
    auto *self = static_cast<OneKVMSubsession *>(client_data);
    self->nextTask() = nullptr;
    const char *line = nullptr;
    if (self->aux_sdp_line_ != nullptr) {
      self->done_flag_ = 1;
    } else if (self->dummy_sink_ != nullptr &&
               (line = self->dummy_sink_->auxSDPLine()) != nullptr) {
      self->aux_sdp_line_ = strDup(line);
      self->dummy_sink_ = nullptr;
      self->done_flag_ = 1;
    } else if (self->checks_remaining_-- > 0) {
      self->nextTask() = self->envir().taskScheduler().scheduleDelayedTask(
          100000, CheckForAuxSDPLine, self);
    } else {
      self->dummy_sink_ = nullptr;
      self->done_flag_ = 1;
    }
  }

  Codec codec_;
  EncodedHub &hub_;
  char *aux_sdp_line_ = nullptr;
  EventLoopWatchVariable done_flag_{0};
  RTPSink *dummy_sink_ = nullptr;
  unsigned checks_remaining_ = 0;
};

class OneKVMAudioSubsession final : public OnDemandServerMediaSubsession {
public:
  static OneKVMAudioSubsession *Create(UsageEnvironment &env, EncodedHub &hub) {
    return new OneKVMAudioSubsession(env, hub);
  }

protected:
  OneKVMAudioSubsession(UsageEnvironment &env, EncodedHub &hub)
      : OnDemandServerMediaSubsession(env, True), hub_(hub) {}

  FramedSource *createNewStreamSource(unsigned,
                                      unsigned &estimated_bitrate) override {
    estimated_bitrate = kEstimatedAudioBitrateKbps;
    return OpusSource::Create(envir(), hub_);
  }

  RTPSink *createNewRTPSink(Groupsock *groupsock, unsigned char payload_type,
                            FramedSource *) override {
    return SimpleRTPSink::createNew(envir(), groupsock, payload_type, 48000,
                                    "audio", "OPUS", 2, False);
  }

private:
  EncodedHub &hub_;
};

} // namespace

bool RunRTSPServer(const Config &config, const Identity &identity, Codec codec,
                   EventLoopWatchVariable &exit_flag, std::atomic<bool> &fatal,
                   std::string &fatal_error, std::string &error) {
  auto *scheduler = BasicTaskScheduler::createNew();
  if (scheduler == nullptr) {
    error = "create LIVE555 task scheduler";
    return false;
  }
  auto *environment = BasicUsageEnvironment::createNew(*scheduler);
  if (environment == nullptr) {
    delete scheduler;
    error = "create LIVE555 usage environment";
    return false;
  }

  std::unique_ptr<UserAuthenticationDatabase> auth;
  if (config.access_control_enabled) {
    auth = std::make_unique<UserAuthenticationDatabase>();
    auth->addUserRecord(config.username.c_str(), config.password.c_str());
  }
  auto *server = BoundRTSPServer::Create(*environment, config, identity, codec,
                                         auth.get(), error);
  if (server == nullptr) {
    environment->reclaim();
    delete scheduler;
    return false;
  }

  std::string stream_name = config.path;
  while (!stream_name.empty() && stream_name.front() == '/')
    stream_name.erase(0, 1);
  EncodedHub hub(config, identity, codec, fatal, fatal_error);
  auto *session =
      ServerMediaSession::createNew(*environment, stream_name.c_str(),
                                    stream_name.c_str(), "OneKVM");
  session->addSubsession(OneKVMSubsession::Create(*environment, codec, hub));
  session->addSubsession(OneKVMAudioSubsession::Create(*environment, hub));
  server->addServerMediaSession(session);
  OutPacketBuffer::maxSize = 2U << 20;

  environment->taskScheduler().doEventLoop(&exit_flag);
  Medium::close(server);
  environment->reclaim();
  delete scheduler;
  return !fatal.load();
}

} // namespace onekvm::rtsp
