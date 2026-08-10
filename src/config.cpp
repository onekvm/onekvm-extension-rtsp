#include "config.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string_view>

namespace onekvm::rtsp {
namespace {

constexpr std::size_t kMaxConfigBytes = 1U << 20;

bool ReadFile(const std::string &path, bool allow_missing, bool &missing,
              std::string &data, std::string &error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (allow_missing && errno == ENOENT) {
      missing = true;
      return true;
    }
    error = "open protocol configuration " + path + ": " + std::strerror(errno);
    return false;
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::size_t>(size) > kMaxConfigBytes) {
    error = "protocol configuration exceeds 1 MiB";
    return false;
  }
  input.seekg(0, std::ios::beg);
  data.assign(std::istreambuf_iterator<char>(input),
              std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) {
    error = "read protocol configuration " + path;
    return false;
  }
  return true;
}

bool GetBoolean(json_object *root, const char *name, bool &value,
                std::string &error) {
  json_object *object = nullptr;
  if (!json_object_object_get_ex(root, name, &object))
    return true;
  if (!json_object_is_type(object, json_type_boolean)) {
    error = std::string("configuration field ") + name + " must be boolean";
    return false;
  }
  value = json_object_get_boolean(object) != 0;
  return true;
}

bool GetInteger(json_object *root, const char *name, int &value,
                std::string &error) {
  json_object *object = nullptr;
  if (!json_object_object_get_ex(root, name, &object))
    return true;
  if (!json_object_is_type(object, json_type_int)) {
    error = std::string("configuration field ") + name + " must be integer";
    return false;
  }
  const auto parsed = json_object_get_int64(object);
  if (parsed < std::numeric_limits<int>::min() ||
      parsed > std::numeric_limits<int>::max()) {
    error = std::string("configuration field ") + name + " is out of range";
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

bool GetNumber(json_object *root, const char *name, double &value,
               std::string &error) {
  json_object *object = nullptr;
  if (!json_object_object_get_ex(root, name, &object))
    return true;
  if (!json_object_is_type(object, json_type_double) &&
      !json_object_is_type(object, json_type_int)) {
    error = std::string("configuration field ") + name + " must be a number";
    return false;
  }
  value = json_object_get_double(object);
  return true;
}

bool GetString(json_object *root, const char *name, std::string &value,
               std::string &error) {
  json_object *object = nullptr;
  if (!json_object_object_get_ex(root, name, &object))
    return true;
  if (!json_object_is_type(object, json_type_string)) {
    error = std::string("configuration field ") + name + " must be string";
    return false;
  }
  value = json_object_get_string(object);
  return true;
}

std::string NormalizePath(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n/");
  const auto last = value.find_last_not_of(" \t\r\n/");
  if (first == std::string::npos || last == std::string::npos)
    return "/onekvm";
  return "/" + value.substr(first, last - first + 1);
}

} // namespace

bool LoadConfig(const std::string &path, bool allow_missing, Config &config,
                std::string &error) {
  std::string data;
  bool missing = false;
  if (!ReadFile(path, allow_missing, missing, data, error))
    return false;
  if (missing)
    return ValidateConfig(config, error);

  json_tokener *tokener = json_tokener_new_ex(32);
  if (tokener == nullptr) {
    error = "allocate JSON parser";
    return false;
  }
  json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
  json_object *root = json_tokener_parse_ex(tokener, data.data(),
                                            static_cast<int>(data.size()));
  const auto parse_error = json_tokener_get_error(tokener);
  auto parse_end = json_tokener_get_parse_end(tokener);
  json_tokener_free(tokener);
  while (parse_end < data.size() &&
         std::isspace(static_cast<unsigned char>(data[parse_end]))) {
    ++parse_end;
  }
  if (parse_error != json_tokener_success || root == nullptr ||
      parse_end != data.size()) {
    if (root != nullptr)
      json_object_put(root);
    error = "decode protocol configuration " + path + ": invalid JSON";
    return false;
  }
  if (!json_object_is_type(root, json_type_object)) {
    json_object_put(root);
    error = "protocol configuration must be an object";
    return false;
  }

  static const std::set<std::string_view> allowed = {
      "service_enabled", "listen_address", "port", "path",
      "access_control_enabled", "username", "password", "codec",
      "fps", "quality_factor", "bitrate_kbps", "initial_qp", "min_qp",
      "max_qp", "gop", "media_socket"};
  json_object_object_foreach(root, key, value) {
    (void)value;
    if (!allowed.contains(key)) {
      error = std::string("unknown protocol configuration field ") + key;
      json_object_put(root);
      return false;
    }
  }

  const bool parsed =
      GetBoolean(root, "service_enabled", config.service_enabled, error) &&
      GetString(root, "listen_address", config.listen_address, error) &&
      GetInteger(root, "port", config.port, error) &&
      GetString(root, "path", config.path, error) &&
      GetBoolean(root, "access_control_enabled",
                 config.access_control_enabled, error) &&
      GetString(root, "username", config.username, error) &&
      GetString(root, "password", config.password, error) &&
      GetInteger(root, "fps", config.fps, error) &&
      GetString(root, "codec", config.codec, error) &&
      GetNumber(root, "quality_factor", config.quality_factor, error) &&
      GetInteger(root, "bitrate_kbps", config.bitrate_kbps, error) &&
      GetInteger(root, "initial_qp", config.initial_qp, error) &&
      GetInteger(root, "min_qp", config.min_qp, error) &&
      GetInteger(root, "max_qp", config.max_qp, error) &&
      GetInteger(root, "gop", config.gop, error) &&
      GetString(root, "media_socket", config.media_socket, error);
  json_object_put(root);
  return parsed && ValidateConfig(config, error);
}

bool ValidateConfig(Config &config, std::string &error) {
  config.path = NormalizePath(config.path);
  std::transform(config.codec.begin(), config.codec.end(), config.codec.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  if (config.port < 1 || config.port > 65535) {
    error = "RTSP port must be between 1 and 65535";
    return false;
  }
  if (config.fps < 1 || config.fps > 60) {
    error = "RTSP FPS must be between 1 and 60";
    return false;
  }
  if (config.listen_address.empty() || config.listen_address.size() > 64) {
    error = "RTSP listen address must contain 1 to 64 characters";
    return false;
  }
  if (config.access_control_enabled &&
      (config.username.empty() || config.password.empty())) {
    error = "RTSP username and password are required when access control is enabled";
    return false;
  }
  if (config.codec != "h264" && config.codec != "h265") {
    error = "RTSP codec must be H.264 or H.265, got " + config.codec;
    return false;
  }
  if (config.quality_factor < 0 || config.quality_factor > 1) {
    error = "RTSP quality factor must be between 0 and 1";
    return false;
  }
  if (config.bitrate_kbps < 0 || config.bitrate_kbps > 100000 ||
      config.initial_qp < 0 || config.initial_qp > 51 ||
      config.min_qp < 0 || config.min_qp > 51 ||
      config.max_qp < 0 || config.max_qp > 51 ||
      config.gop < 0 || config.gop > 600) {
    error = "RTSP encoder setting is out of range";
    return false;
  }
  if (config.min_qp != 0 && config.max_qp != 0 &&
      config.min_qp > config.max_qp) {
    error = "RTSP minimum QP exceeds maximum QP";
    return false;
  }
  if (config.bitrate_kbps != 0 &&
      (config.initial_qp != 0 || config.min_qp != 0 || config.max_qp != 0)) {
    error = "RTSP bitrate and QP overrides are mutually exclusive";
    return false;
  }
  if (!std::filesystem::path(config.media_socket).is_absolute()) {
    error = "RTSP media socket path must be absolute";
    return false;
  }
  return true;
}

std::string DefaultConfigPath() {
  const char *root = std::getenv("ONEKVM_EXTENSION_CONFIG");
  return root != nullptr && root[0] != '\0'
             ? std::string(root) + "/config.json"
             : "/run/onekvm/extensions/rtsp/config.json";
}

} // namespace onekvm::rtsp
