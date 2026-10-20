#include "config.hpp"
#include "socket_activation.hpp"
#include "protocol_ipc.hpp"
#include "rtsp_server.hpp"

#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

EventLoopWatchVariable exit_flag{0};

void HandleSignal(int) { exit_flag = 1; }

struct Options {
  std::string config_path = onekvm::rtsp::DefaultConfigPath();
  bool health = false;
  std::string lifecycle;
};

bool ParseOptions(int argc, char **argv, Options &options, std::string &error) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--health") {
      options.health = true;
    } else if (argument == "--config" || argument == "--lifecycle") {
      if (++index >= argc) {
        error = std::string(argument) + " requires a value";
        return false;
      }
      if (argument == "--config")
        options.config_path = argv[index];
      else
        options.lifecycle = argv[index];
    } else {
      error = "unexpected argument: " + std::string(argument);
      return false;
    }
  }
  if (!options.lifecycle.empty() && options.lifecycle != "enable" &&
      options.lifecycle != "disable") {
    error = "unsupported lifecycle action " + options.lifecycle;
    return false;
  }
  return true;
}

int Run(int argc, char **argv) {
  Options options;
  std::string error;
  if (!ParseOptions(argc, argv, options, error)) {
    std::cerr << "onekvm-protocol-rtsp: " << error << '\n';
    return 2;
  }

  onekvm::rtsp::Config config;
  if (!onekvm::rtsp::LoadConfig(options.config_path, options.health, config,
                                error)) {
    std::cerr << "onekvm-protocol-rtsp: " << error << '\n';
    return 1;
  }
  if (options.health || !options.lifecycle.empty() || !config.service_enabled)
    return 0;

  int listen_fd = -1, listen_family = AF_UNSPEC;
  if (!onekvm::activation::TakeListener(listen_fd, listen_family, error)) {
    std::cerr << "socket activation: " << error << '\n';
    return 1;
  }
  if (listen_fd >= 0) {
    bool pending = false;
    if (!onekvm::activation::CheckPendingConnection(listen_fd, pending, error)) {
      std::cerr << "socket activation: " << error << '\n';
      return 1;
    }
    if (!pending)
      return 0;
  }

  onekvm::rtsp::Identity identity;
  if (!onekvm::rtsp::LoadIdentity(identity, error)) {
    std::cerr << "onekvm-protocol-rtsp: " << error << '\n';
    return 1;
  }
  onekvm::rtsp::VideoInfo info;
  const auto codec = config.codec == "h265" ? onekvm::rtsp::Codec::kH265
                                             : onekvm::rtsp::Codec::kH264;
  if (!onekvm::rtsp::QueryVideoInfo(config.media_socket, identity, codec, info,
                                    error)) {
    std::cerr << "onekvm-protocol-rtsp: query independent video stream: " << error
              << '\n';
    return 1;
  }
  if (!onekvm::rtsp::ValidateConfig(config, error)) {
    std::cerr << "onekvm-protocol-rtsp: " << error << '\n';
    return 1;
  }

  struct sigaction action{};
  action.sa_handler = HandleSignal;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, nullptr) < 0 ||
      sigaction(SIGTERM, &action, nullptr) < 0) {
    std::cerr << "onekvm-protocol-rtsp: install signal handler: "
              << std::strerror(errno) << '\n';
    return 1;
  }

  std::atomic<bool> fatal{false};
  std::atomic<bool> capability_monitor_stop{false};
  std::atomic<bool> audio_capability_changed{false};
  std::string fatal_error;
  std::thread capability_monitor([&] {
    while (!capability_monitor_stop.load()) {
      for (int step = 0; step < 10 && !capability_monitor_stop.load(); ++step)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (capability_monitor_stop.load())
        return;
      onekvm::rtsp::VideoInfo current;
      std::string query_error;
      if (onekvm::rtsp::QueryVideoInfo(config.media_socket, identity, codec,
                                       current, query_error) &&
          current.audio_available != info.audio_available) {
        audio_capability_changed.store(true);
        kill(getpid(), SIGTERM);
        return;
      }
    }
  });
  const bool ran = onekvm::rtsp::RunRTSPServer(
      config, identity, codec, info.audio_available, exit_flag, fatal,
      fatal_error, error, listen_fd, listen_family);
  capability_monitor_stop.store(true);
  capability_monitor.join();
  if (audio_capability_changed.load()) {
    std::cerr << "onekvm-protocol-rtsp: USB audio capability changed; restarting\n";
    return 1;
  }
  if (!ran) {
    std::cerr << "onekvm-protocol-rtsp: "
              << (fatal_error.empty() ? error : fatal_error) << '\n';
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) { return Run(argc, argv); }
