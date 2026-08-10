#include "protocol_ipc.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool ExpectSpans(const std::vector<std::uint8_t> &payload,
                 const std::vector<onekvm::rtsp::NalSpan> &expected) {
  const auto actual = onekvm::rtsp::SplitAnnexB(payload);
  if (actual.size() != expected.size())
    return false;
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (actual[index].offset != expected[index].offset ||
        actual[index].size != expected[index].size) {
      return false;
    }
  }
  return true;
}

} // namespace

int main() {
  if (!ExpectSpans({0, 0, 0, 1, 0x67, 1, 0, 0, 1, 0x68, 2}, {{4, 2}, {9, 2}})) {
    std::cerr << "failed to split four- and three-byte Annex-B prefixes\n";
    return 1;
  }
  if (!ExpectSpans({0x65, 1, 2}, {{0, 3}})) {
    std::cerr << "failed to preserve a discrete NAL unit\n";
    return 1;
  }
  if (!ExpectSpans({0, 0, 1, 0x65, 1, 0, 0}, {{3, 2}})) {
    std::cerr << "failed to trim Annex-B trailing zeros\n";
    return 1;
  }
  return 0;
}
