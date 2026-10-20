#pragma once

#include <atomic>
#include <string>

#include <UsageEnvironment.hh>

#include "config.hpp"
#include "protocol_ipc.hpp"

namespace onekvm::rtsp {

bool RunRTSPServer(const Config &config, const Identity &identity, Codec codec,
                   bool audio_available,
                   EventLoopWatchVariable &exit_flag, std::atomic<bool> &fatal,
                   std::string &fatal_error, std::string &error,
                   int listen_fd = -1, int listen_family = 0);

} // namespace onekvm::rtsp
