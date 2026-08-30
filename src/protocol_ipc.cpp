#include "protocol_ipc.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <string_view>

namespace onekvm::rtsp {
namespace {

constexpr std::size_t kHeaderSize = 40;
constexpr std::size_t kMaxPayloadBytes = 8U << 20;
constexpr std::string_view kMagic = "OKVF";
constexpr std::string_view kRingMagic = "OKVR";
constexpr std::uint8_t kRingHandshake = 'R';
constexpr std::uint32_t kRingVersion = 1;
constexpr std::uint32_t kRingHeaderSize = 64;
constexpr std::uint32_t kRingSlotHeaderSize = 64;
constexpr std::uint32_t kRingStateFree = 0;
constexpr std::uint32_t kRingStateReady = 2;
constexpr std::uint32_t kRingStateReading = 3;

struct Frame {
  std::uint64_t sequence = 0;
  Codec codec;
  std::uint32_t duration_us;
  std::uint16_t width;
  std::uint16_t height;
  bool keyframe;
  std::shared_ptr<void> owner;
  const std::uint8_t *data = nullptr;
  std::size_t size = 0;
};

std::uint16_t ReadBE16(const std::uint8_t *data) {
  return static_cast<std::uint16_t>((data[0] << 8U) | data[1]);
}

std::uint32_t ReadBE32(const std::uint8_t *data) {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) | data[3];
}

std::uint16_t ReadLE16(const std::uint8_t *data) {
  return static_cast<std::uint16_t>(data[0] | (data[1] << 8U));
}

std::uint32_t ReadLE32(const std::uint8_t *data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint64_t ReadLE64(const std::uint8_t *data) {
  return static_cast<std::uint64_t>(ReadLE32(data)) |
         (static_cast<std::uint64_t>(ReadLE32(data + 4)) << 32U);
}

struct RingMapping {
  int memory_fd = -1;
  int event_fd = -1;
  std::uint8_t *memory = nullptr;
  std::size_t size = 0;
  std::uint32_t slot_count = 0;
  std::uint32_t slot_capacity = 0;
  std::uint32_t stride = 0;

  ~RingMapping() {
    if (memory != nullptr)
      munmap(memory, size);
    if (event_fd >= 0)
      close(event_fd);
    if (memory_fd >= 0)
      close(memory_fd);
  }
};

struct RingLease {
  std::shared_ptr<RingMapping> ring;
  std::uint32_t *state = nullptr;

  ~RingLease() {
    if (state != nullptr)
      __atomic_store_n(state, kRingStateFree, __ATOMIC_RELEASE);
  }
};

bool IsCodec(std::uint8_t value) { return value >= 1 && value <= 4; }

bool WriteAll(int fd, std::string_view data, std::string &error) {
  while (!data.empty()) {
    const auto written = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      error = std::string("write media subscription: ") + std::strerror(errno);
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

bool ReadAll(int fd, void *target, std::size_t size, std::string &error) {
  auto *output = static_cast<std::uint8_t *>(target);
  while (size != 0) {
    const auto received = recv(fd, output, size, 0);
    if (received < 0 && errno == EINTR)
      continue;
    if (received == 0) {
      error = "media subscription closed";
      return false;
    }
    if (received < 0) {
      error = std::string("read media subscription: ") + std::strerror(errno);
      return false;
    }
    output += received;
    size -= static_cast<std::size_t>(received);
  }
  return true;
}

int ConnectUnix(const std::string &path, int timeout_ms, std::string &error) {
  if (path.size() >= sizeof(sockaddr_un::sun_path)) {
    error = "media socket path is too long";
    return -1;
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    error = std::string("create media socket: ") + std::strerror(errno);
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) <
          0 &&
      errno != EINPROGRESS) {
    error = std::string("connect media socket: ") + std::strerror(errno);
    close(fd);
    return -1;
  }
  pollfd descriptor{fd, POLLOUT, 0};
  int polled;
  do {
    polled = poll(&descriptor, 1, timeout_ms);
  } while (polled < 0 && errno == EINTR);
  int socket_error = 0;
  socklen_t error_size = sizeof(socket_error);
  if (polled <= 0 ||
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) < 0 ||
      socket_error != 0) {
    error = polled == 0
                ? "connect media socket: timed out"
                : std::string("connect media socket: ") +
                      std::strerror(socket_error == 0 ? errno : socket_error);
    close(fd);
    return -1;
  }
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  return fd;
}

std::string SubscriptionJSON(const Identity &identity, std::string_view video,
                             const VideoProfile *profile = nullptr,
                             bool shared_ring = false,
                             bool track_session = true) {
  std::string request =
      "{\"version\":1,\"extension_id\":\"" + identity.extension_id +
      "\",\"token\":\"" + identity.token + "\",\"video\":\"" +
      std::string(video) + "\"";
  if (profile != nullptr) {
    request += ",\"codec\":\"" + std::string(CodecName(profile->codec)) + "\"";
    if (video != "info") {
      if (profile->fps > 0)
        request += ",\"fps\":" + std::to_string(profile->fps);
      request += ",\"quality_factor\":" +
                 std::to_string(profile->quality_factor);
      if (profile->bitrate_kbps > 0)
        request += ",\"bitrate_kbps\":" + std::to_string(profile->bitrate_kbps);
      if (profile->initial_qp > 0)
        request += ",\"initial_qp\":" + std::to_string(profile->initial_qp);
      if (profile->min_qp > 0)
        request += ",\"min_qp\":" + std::to_string(profile->min_qp);
      if (profile->max_qp > 0)
        request += ",\"max_qp\":" + std::to_string(profile->max_qp);
      if (profile->gop > 0)
        request += ",\"gop\":" + std::to_string(profile->gop);
    }
  }
  if (video == "encoded")
    request += ",\"audio\":\"opus\"";
  if (shared_ring)
    request += ",\"transport\":\"shm-ring-v1\"";
  if (!track_session)
    request += ",\"track_session\":false";
  return request + "}\n";
}

std::string JSONString(std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() + 2);
  result.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\b':
      result += "\\b";
      break;
    case '\f':
      result += "\\f";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (character < 0x20) {
        result += "\\u00";
        result.push_back(hex[character >> 4U]);
        result.push_back(hex[character & 0x0fU]);
      } else {
        result.push_back(static_cast<char>(character));
      }
    }
  }
  result.push_back('"');
  return result;
}

std::string SessionJSON(const Identity &identity, std::string_view client_ip,
                        std::string_view client_user,
                        std::string_view client_transport, Codec codec) {
  std::string request =
      "{\"version\":1,\"extension_id\":" + JSONString(identity.extension_id) +
      ",\"token\":" + JSONString(identity.token) +
      ",\"video\":\"session\",\"codec\":" + JSONString(CodecName(codec)) +
      ",\"client_ip\":" + JSONString(client_ip) +
      ",\"client_transport\":" + JSONString(client_transport);
  if (!client_user.empty())
    request += ",\"client_user\":" + JSONString(client_user);
  return request + "}\n";
}

bool ReceiveSharedRing(int fd, std::shared_ptr<RingMapping> &result,
                       std::string &error) {
  std::uint8_t marker = 0;
  iovec vector{&marker, 1};
  std::array<char, CMSG_SPACE(sizeof(int) * 2)> control{};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1;
  message.msg_control = control.data();
  message.msg_controllen = control.size();
  ssize_t received;
  do {
    received = recvmsg(fd, &message, MSG_CMSG_CLOEXEC);
  } while (received < 0 && errno == EINTR);
  if (received != 1 || marker != kRingHandshake) {
    error = received == 0 ? "shared media ring connection closed"
                          : "invalid shared media ring handshake";
    return false;
  }
  std::array<int, 2> descriptors{-1, -1};
  for (cmsghdr *header = CMSG_FIRSTHDR(&message); header != nullptr;
       header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
      continue;
    const auto bytes = header->cmsg_len - CMSG_LEN(0);
    if (bytes < sizeof(int) * descriptors.size())
      continue;
    std::memcpy(descriptors.data(), CMSG_DATA(header),
                sizeof(int) * descriptors.size());
    break;
  }
  if (descriptors[0] < 0 || descriptors[1] < 0) {
    error = "shared media ring descriptors are missing";
    for (const int descriptor : descriptors)
      if (descriptor >= 0)
        close(descriptor);
    return false;
  }
  struct stat status {};
  if (fstat(descriptors[0], &status) != 0 ||
      status.st_size < static_cast<off_t>(kRingHeaderSize)) {
    error = "invalid shared media ring size";
    close(descriptors[0]);
    close(descriptors[1]);
    return false;
  }
  auto *memory = static_cast<std::uint8_t *>(
      mmap(nullptr, static_cast<std::size_t>(status.st_size),
           PROT_READ | PROT_WRITE, MAP_SHARED, descriptors[0], 0));
  if (memory == MAP_FAILED) {
    error = std::string("map shared media ring: ") + std::strerror(errno);
    close(descriptors[0]);
    close(descriptors[1]);
    return false;
  }
  auto ring = std::make_shared<RingMapping>();
  ring->memory_fd = descriptors[0];
  ring->event_fd = descriptors[1];
  ring->memory = memory;
  ring->size = static_cast<std::size_t>(status.st_size);
  ring->slot_count = ReadLE32(memory + 8);
  ring->slot_capacity = ReadLE32(memory + 12);
  ring->stride = ReadLE32(memory + 16);
  const auto header_size = ReadLE32(memory + 20);
  const bool layout_valid =
      std::equal(kRingMagic.begin(), kRingMagic.end(), memory) &&
      ReadLE32(memory + 4) == kRingVersion && header_size == kRingHeaderSize &&
      ring->slot_count >= 2 && ring->slot_count <= 16 &&
      ring->slot_capacity > 0 && ring->slot_capacity <= kMaxPayloadBytes &&
      ring->stride >= kRingSlotHeaderSize + ring->slot_capacity &&
      ring->slot_count <=
          (ring->size - header_size) / static_cast<std::size_t>(ring->stride);
  if (!layout_valid) {
    error = "invalid shared media ring layout";
    return false;
  }
  result = std::move(ring);
  return true;
}

Frame TakeNextFrame(const std::shared_ptr<RingMapping> &ring) {
  // Encoded frames are interdependent, so consume ready slots in sequence
  // order. Unlike the MJPEG/VNC path, never discard a newer ready slot merely
  // because notifications were coalesced.
  for (int attempt = 0; attempt < 2; ++attempt) {
    int selected = -1;
    std::uint64_t oldest_sequence = std::numeric_limits<std::uint64_t>::max();
    for (std::uint32_t index = 0; index < ring->slot_count; ++index) {
      const auto offset = kRingHeaderSize + index * ring->stride;
      auto *state = reinterpret_cast<std::uint32_t *>(ring->memory + offset);
      if (__atomic_load_n(state, __ATOMIC_ACQUIRE) != kRingStateReady)
        continue;
      const auto sequence = ReadLE64(ring->memory + offset + 8);
      if (selected < 0 || sequence < oldest_sequence) {
        selected = static_cast<int>(index);
        oldest_sequence = sequence;
      }
    }
    if (selected < 0) {
      if (attempt == 0) {
        auto *notified = reinterpret_cast<std::uint32_t *>(ring->memory + 24);
        __atomic_store_n(notified, 0U, __ATOMIC_RELEASE);
        continue;
      }
      return {};
    }
    const auto selected_offset =
        kRingHeaderSize + static_cast<std::uint32_t>(selected) * ring->stride;
    auto *selected_state =
        reinterpret_cast<std::uint32_t *>(ring->memory + selected_offset);
    std::uint32_t expected = kRingStateReady;
    if (!__atomic_compare_exchange_n(selected_state, &expected,
                                     kRingStateReading, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      continue;
    const auto *header = ring->memory + selected_offset;
    const auto payload_size = ReadLE32(header + 4);
    const auto width = ReadLE16(header + 28);
    const auto height = ReadLE16(header + 30);
    const auto codec = static_cast<Codec>(header[32]);
    const bool audio = codec == Codec::kOpus;
    if (payload_size == 0 || payload_size > ring->slot_capacity ||
        !IsCodec(header[32]) || (!audio && (width == 0 || height == 0))) {
      __atomic_store_n(selected_state, kRingStateFree, __ATOMIC_RELEASE);
      return {};
    }
    auto lease = std::make_shared<RingLease>();
    lease->ring = ring;
    lease->state = selected_state;
    return Frame{ReadLE64(header + 8), codec, ReadLE32(header + 24), width,
                 height, header[33] != 0, lease, header + kRingSlotHeaderSize,
                 payload_size};
  }
  return {};
}

bool ReadFrame(int fd, Frame &frame, std::string &error) {
  std::array<std::uint8_t, kHeaderSize> header{};
  if (!ReadAll(fd, header.data(), header.size(), error))
    return false;
  if (!std::equal(kMagic.begin(), kMagic.end(), header.begin()) ||
      header[4] != 1 || !IsCodec(header[5])) {
    error = "invalid protocol frame header";
    return false;
  }
  const auto length = ReadBE32(header.data() + 32);
  if (length > kMaxPayloadBytes) {
    error = "invalid protocol frame payload length";
    return false;
  }
  frame.sequence =
      (static_cast<std::uint64_t>(ReadBE32(header.data() + 8)) << 32U) |
      ReadBE32(header.data() + 12);
  frame.codec = static_cast<Codec>(header[5]);
  frame.duration_us = ReadBE32(header.data() + 24);
  frame.width = ReadBE16(header.data() + 28);
  frame.height = ReadBE16(header.data() + 30);
  frame.keyframe = (header[6] & 1U) != 0;
  auto payload = std::make_shared<std::vector<std::uint8_t>>(length);
  frame.owner = payload;
  frame.data = payload->data();
  frame.size = payload->size();
  return length == 0 || ReadAll(fd, payload->data(), length, error);
}

void CloseOwnedSocket(std::atomic<int> &socket_fd, int fd) {
  int expected = fd;
  if (socket_fd.compare_exchange_strong(expected, -1))
    close(fd);
}

} // namespace

bool LoadIdentity(Identity &identity, std::string &error) {
  const char *id = std::getenv("ONEKVM_EXTENSION_ID");
  const char *credentials = std::getenv("CREDENTIALS_DIRECTORY");
  if (id == nullptr || id[0] == '\0' || credentials == nullptr ||
      credentials[0] == '\0') {
    error = "extension identity is unavailable";
    return false;
  }
  static const std::regex id_pattern("^[a-z0-9][a-z0-9-]{1,62}$");
  if (!std::regex_match(id, id_pattern)) {
    error = "extension identity is invalid";
    return false;
  }
  std::ifstream input(std::filesystem::path(credentials) / "onekvm-token");
  if (!input) {
    error = "read extension credential";
    return false;
  }
  std::string token((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  token.erase(
      std::remove_if(token.begin(), token.end(),
                     [](unsigned char value) { return std::isspace(value); }),
      token.end());
  if (token.size() != 64 ||
      !std::all_of(token.begin(), token.end(),
                   [](unsigned char value) { return std::isxdigit(value); })) {
    error = "extension credential has invalid format";
    return false;
  }
  identity = Identity{id, std::move(token)};
  return true;
}

bool QueryVideoInfo(const std::string &socket_path, const Identity &identity,
                    Codec codec, VideoInfo &info, std::string &error) {
  const int fd = ConnectUnix(socket_path, 3000, error);
  if (fd < 0)
    return false;
  VideoProfile profile;
  profile.codec = codec;
  if (!WriteAll(fd, SubscriptionJSON(identity, "info", &profile), error)) {
    close(fd);
    return false;
  }
  Frame frame{};
  const bool read = ReadFrame(fd, frame, error);
  close(fd);
  if (!read)
    return false;
  if (frame.size != 0 || frame.width == 0 || frame.height == 0 ||
      frame.duration_us == 0 || frame.codec == Codec::kMJPEG) {
    error = "invalid protocol video metadata";
    return false;
  }
  info = VideoInfo{frame.codec, frame.width, frame.height, frame.duration_us};
  return true;
}

std::vector<NalSpan> SplitAnnexB(const std::uint8_t *payload,
                                 std::size_t size) {
  const auto find_start = [payload, size](std::size_t from,
                                          std::size_t &prefix) {
    for (std::size_t index = from; index + 3 <= size; ++index) {
      if (payload[index] != 0 || payload[index + 1] != 0)
        continue;
      if (payload[index + 2] == 1) {
        prefix = 3;
        return index;
      }
      if (index + 4 <= size && payload[index + 2] == 0 &&
          payload[index + 3] == 1) {
        prefix = 4;
        return index;
      }
    }
    prefix = 0;
    return size;
  };

  std::size_t prefix = 0;
  const auto first = find_start(0, prefix);
  if (first == size) {
    return size == 0 ? std::vector<NalSpan>{}
                     : std::vector<NalSpan>{{0, size}};
  }
  std::vector<NalSpan> result;
  std::size_t start = first + prefix;
  while (start < size) {
    std::size_t next_prefix = 0;
    const auto next = find_start(start, next_prefix);
    std::size_t end = next;
    if (next == size) {
      while (end > start && payload[end - 1] == 0)
        --end;
    }
    if (end > start)
      result.push_back({start, end - start});
    if (next == size)
      break;
    start = next + next_prefix;
  }
  return result;
}

std::vector<NalSpan> SplitAnnexB(const std::vector<std::uint8_t> &payload) {
  return SplitAnnexB(payload.data(), payload.size());
}

const char *CodecName(Codec codec) {
  switch (codec) {
  case Codec::kH264:
    return "h264";
  case Codec::kH265:
    return "h265";
  case Codec::kMJPEG:
    return "mjpeg";
  case Codec::kOpus:
    return "opus";
  }
  return "unknown";
}

SessionReporter::SessionReporter(const std::string &socket_path,
                                 const Identity &identity,
                                 std::string client_ip,
                                 std::string client_user,
                                 std::string client_transport, Codec codec,
                                 std::string &error) {
  socket_fd_ = ConnectUnix(socket_path, 3000, error);
  if (socket_fd_ < 0)
    return;
  if (!WriteAll(socket_fd_,
                SessionJSON(identity, client_ip, client_user,
                            client_transport, codec),
                error)) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
}

SessionReporter::~SessionReporter() {
  if (socket_fd_ >= 0)
    close(socket_fd_);
}

MediaSubscription::MediaSubscription(std::string socket_path, Identity identity,
                                     VideoProfile profile,
                                     FrameCallback frame_callback,
                                     AudioCallback audio_callback,
                                     NotifyCallback notify_callback,
                                     FatalCallback fatal_callback)
    : socket_path_(std::move(socket_path)), identity_(std::move(identity)),
      profile_(profile), frame_callback_(std::move(frame_callback)),
      audio_callback_(std::move(audio_callback)),
      notify_callback_(std::move(notify_callback)),
      fatal_callback_(std::move(fatal_callback)) {}

MediaSubscription::~MediaSubscription() { Stop(); }

void MediaSubscription::Start() {
  if (thread_.joinable())
    return;
  stopping_.store(false);
  thread_ = std::thread(&MediaSubscription::Run, this);
}

void MediaSubscription::Stop() {
  stopping_.store(true);
  const int fd = socket_fd_.load();
  if (fd >= 0)
    shutdown(fd, SHUT_RDWR);
  if (thread_.joinable())
    thread_.join();
}

void MediaSubscription::RequestKeyFrame() {
  const int fd = socket_fd_.load();
  if (fd < 0)
    return;
  std::string error;
  (void)WriteAll(fd, "{\"request\":\"keyframe\"}\n", error);
}

void MediaSubscription::Run() {
  auto backoff = std::chrono::milliseconds(100);
  bool shared_ring_supported = true;
  while (!stopping_.load()) {
    std::string error;
    const int fd = ConnectUnix(socket_path_, 3000, error);
    if (fd < 0) {
      if (!stopping_.load())
        std::this_thread::sleep_for(backoff);
      backoff = std::min(backoff * 2, std::chrono::milliseconds(2000));
      continue;
    }
    socket_fd_.store(fd);
    const bool request_shared_ring = shared_ring_supported;
    if (!WriteAll(fd, SubscriptionJSON(identity_, "encoded", &profile_,
                                        request_shared_ring, false),
                  error)) {
      CloseOwnedSocket(socket_fd_, fd);
      continue;
    }
    backoff = std::chrono::milliseconds(100);
    std::shared_ptr<RingMapping> ring;
    if (request_shared_ring && !ReceiveSharedRing(fd, ring, error)) {
      shared_ring_supported = false;
      CloseOwnedSocket(socket_fd_, fd);
      continue;
    }
    std::uint64_t last_sequence = 0;
    bool waiting_for_keyframe = request_shared_ring;
    while (!stopping_.load()) {
      Frame frame{};
      if (ring != nullptr) {
        frame = TakeNextFrame(ring);
        if (frame.owner == nullptr) {
          std::array<pollfd, 2> descriptors{
              pollfd{ring->event_fd, POLLIN, 0},
              pollfd{fd, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0}};
          int polled;
          do {
            polled = poll(descriptors.data(), descriptors.size(), -1);
          } while (polled < 0 && errno == EINTR);
          if (polled < 0 ||
              (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            break;
          if ((descriptors[0].revents & POLLIN) == 0)
            continue;
          std::uint64_t notifications = 0;
          ssize_t read_size;
          do {
            read_size = read(ring->event_fd, &notifications,
                             sizeof(notifications));
          } while (read_size < 0 && errno == EINTR);
          if (read_size != static_cast<ssize_t>(sizeof(notifications)))
            continue;
          // A coalesced event may cover several ready slots. The next loop
          // iteration drains them in order before sleeping again.
          continue;
        }
      } else if (!ReadFrame(fd, frame, error)) {
        break;
      }
      if (frame.codec == Codec::kOpus) {
        if (audio_callback_) {
          auto copy = std::make_shared<std::vector<std::uint8_t>>(
              frame.data, frame.data + frame.size);
          audio_callback_({copy, copy->data(), copy->size(), frame.duration_us,
                           true, false});
          notify_callback_();
        }
        continue;
      }
      if (ring != nullptr) {
        if (last_sequence != 0 && frame.sequence != last_sequence + 1) {
          waiting_for_keyframe = true;
          if (!frame.keyframe)
            (void)WriteAll(fd, "{\"request\":\"keyframe\"}\n", error);
        }
        last_sequence = frame.sequence;
      }
      if (waiting_for_keyframe) {
        if (!frame.keyframe)
          continue;
        waiting_for_keyframe = false;
      }
      if (frame.codec != profile_.codec) {
        fatal_callback_(std::string("independent video codec changed from ") +
                        CodecName(profile_.codec) + " to " + CodecName(frame.codec));
        stopping_.store(true);
        break;
      }
      const auto spans = SplitAnnexB(frame.data, frame.size);
      if (spans.empty())
        continue;
      std::vector<NalPacket> packets;
      packets.reserve(spans.size());
      for (std::size_t index = 0; index < spans.size(); ++index) {
        packets.push_back({frame.owner, frame.data + spans[index].offset,
                           spans[index].size, frame.duration_us,
                           index + 1 == spans.size(), frame.keyframe});
      }
      frame_callback_(std::move(packets));
      notify_callback_();
    }
    CloseOwnedSocket(socket_fd_, fd);
    if (!stopping_.load())
      std::this_thread::sleep_for(backoff);
  }
}

} // namespace onekvm::rtsp
