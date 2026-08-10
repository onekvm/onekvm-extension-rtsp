#include "config.hpp"
#include "protocol_ipc.hpp"
#include "rtsp_server.hpp"

#include <signal.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

char volatile exit_flag = 0;

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
  std::string fatal_error;
  if (!onekvm::rtsp::RunRTSPServer(config, identity, codec, exit_flag,
                                   fatal, fatal_error, error)) {
    std::cerr << "onekvm-protocol-rtsp: "
              << (fatal_error.empty() ? error : fatal_error) << '\n';
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) { return Run(argc, argv); }
