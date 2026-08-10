#pragma once

#include <atomic>
#include <string>

#include "config.hpp"
#include "protocol_ipc.hpp"

namespace onekvm::rtsp {

bool RunRTSPServer(const Config &config, const Identity &identity, Codec codec,
                   char volatile &exit_flag, std::atomic<bool> &fatal,
                   std::string &fatal_error, std::string &error);

} // namespace onekvm::rtsp
