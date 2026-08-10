#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace onekvm::rtsp {

enum class Codec : std::uint8_t { kH264 = 1, kH265 = 2, kMJPEG = 3 };

struct Identity {
  std::string extension_id;
  std::string token;
};

struct VideoInfo {
  Codec codec = Codec::kH264;
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::uint32_t duration_us = 0;
};

struct VideoProfile {
  Codec codec = Codec::kH264;
  int fps = 60;
  double quality_factor = 1.0;
  int bitrate_kbps = 0;
  int initial_qp = 0;
  int min_qp = 0;
  int max_qp = 0;
  int gop = 0;
};

struct NalSpan {
  std::size_t offset = 0;
  std::size_t size = 0;
};

struct NalPacket {
  std::shared_ptr<void> owner;
  const std::uint8_t *data = nullptr;
  std::size_t size = 0;
  std::uint32_t duration_us = 0;
  bool end_access_unit = false;
  bool keyframe = false;
};

bool LoadIdentity(Identity &identity, std::string &error);
bool QueryVideoInfo(const std::string &socket_path, const Identity &identity,
                    Codec codec, VideoInfo &info, std::string &error);
std::vector<NalSpan> SplitAnnexB(const std::uint8_t *payload,
                                 std::size_t size);
std::vector<NalSpan> SplitAnnexB(const std::vector<std::uint8_t> &payload);
const char *CodecName(Codec codec);

class SessionReporter {
public:
  SessionReporter(const std::string &socket_path, const Identity &identity,
                  std::string client_ip, std::string client_user,
                  std::string client_transport, Codec codec,
                  std::string &error);
  ~SessionReporter();
  SessionReporter(const SessionReporter &) = delete;
  SessionReporter &operator=(const SessionReporter &) = delete;

  bool Ready() const { return socket_fd_ >= 0; }

private:
  int socket_fd_ = -1;
};

class MediaSubscription {
public:
  using FrameCallback = std::function<void(std::vector<NalPacket>)>;
  using NotifyCallback = std::function<void()>;
  using FatalCallback = std::function<void(std::string)>;

  MediaSubscription(std::string socket_path, Identity identity,
                    VideoProfile profile,
                    FrameCallback frame_callback,
                    NotifyCallback notify_callback,
                    FatalCallback fatal_callback);
  ~MediaSubscription();
  MediaSubscription(const MediaSubscription &) = delete;
  MediaSubscription &operator=(const MediaSubscription &) = delete;

  void Start();
  void Stop();
  void RequestKeyFrame();

private:
  void Run();

  std::string socket_path_;
  Identity identity_;
  VideoProfile profile_;
  FrameCallback frame_callback_;
  NotifyCallback notify_callback_;
  FatalCallback fatal_callback_;
  std::atomic<bool> stopping_{false};
  std::atomic<int> socket_fd_{-1};
  std::thread thread_;
};

} // namespace onekvm::rtsp
