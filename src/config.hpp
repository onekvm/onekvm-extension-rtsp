#pragma once

#include <string>

namespace onekvm::rtsp {

struct Config {
  bool service_enabled = true;
  std::string listen_address = "127.0.0.1";
  int port = 8554;
  std::string path = "/onekvm";
  bool access_control_enabled = false;
  std::string username;
  std::string password;
  int fps = 60;
  std::string codec = "h264";
  double quality_factor = 1.0;
  int bitrate_kbps = 0;
  int initial_qp = 0;
  int min_qp = 0;
  int max_qp = 0;
  int gop = 0;
  std::string media_socket = "/run/onekvm/extension-media.sock";
};

bool LoadConfig(const std::string &path, bool allow_missing, Config &config,
                std::string &error);
bool ValidateConfig(Config &config, std::string &error);
std::string DefaultConfigPath();

} // namespace onekvm::rtsp
