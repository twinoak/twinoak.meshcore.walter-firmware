// The HTTPS client's bench (task p3-02): a clock whose delays take no time, the scripted server
// behind the fake TLS socket, the firmware's own HTTP/1.1 over it (sim/fake_http.h), and the
// client and the download on top. Shared by the client's, the download's, the fetch's and the
// console's tests.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/net/client.h"
#include "core/net/download.h"
#include "fakes/capture.h"
#include "fakes/fake_clock.h"
#include "sim/fake_http.h"

namespace bench {

// A body into a vector.
class MemorySink final : public interfaces::HttpBodySink {
 public:
  std::vector<std::uint8_t> bytes;
  bool piece(const std::uint8_t* data, std::size_t size) override {
    bytes.insert(bytes.end(), data, data + size);
    return true;
  }
  std::string text() const { return std::string(bytes.begin(), bytes.end()); }
};

// A download's sink into a vector: held() is its size, restart() empties it.
class MemoryDownloadSink final : public core::net::DownloadSink {
 public:
  std::vector<std::uint8_t> bytes;
  int restarts = 0;
  bool refuse_restart = false;
  bool refuse_writes = false;
  std::uint32_t held() override { return static_cast<std::uint32_t>(bytes.size()); }
  bool write(const std::uint8_t* data, std::size_t size) override {
    if (refuse_writes) return false;
    bytes.insert(bytes.end(), data, data + size);
    return true;
  }
  bool restart() override {
    if (refuse_restart) return false;
    ++restarts;
    bytes.clear();
    return true;
  }
};

// Bytes that are not a repeating pattern, so a byte fetched twice or a range misplaced shows.
inline std::vector<std::uint8_t> patternBytes(std::size_t n, std::uint32_t seed = 1) {
  std::vector<std::uint8_t> out(n);
  std::uint32_t x = seed * 2654435761u + 12345u;
  for (std::size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    out[i] = static_cast<std::uint8_t>(x >> 24);
  }
  return out;
}

struct NetBench {
  fakes::FakeClock clock;
  sim::FakeHttp http{clock};
  core::net::Client client{http, clock};
  core::net::Download download{client, clock};
  fakes::CaptureOut trace;

  sim::ScriptedServer& server() { return http.server; }
  const sim::ScriptedServer& server() const { return http.server; }
  sim::FakeTlsSocket& socket() { return http.socket; }

  // The requests the server saw with this path.
  int requestsTo(const std::string& path) const {
    int n = 0;
    for (const sim::SeenRequest& r : http.server.requests) {
      if (r.path == path) ++n;
    }
    return n;
  }
};

inline bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

}  // namespace bench
