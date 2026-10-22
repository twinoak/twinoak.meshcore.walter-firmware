// Random bits, as core/ and components/agent/ see them (task p4-04): thirty-two of them, from
// whatever the board has. On the ESP32-S3 that is `esp_random()`, which the ESP-IDF documents as
// a true random number generator whenever an RF subsystem is enabled (platform/entropy);
// host_test/ gives a seeded xorshift so that a failure is the same failure twice.
//
// It exists because the agent library asks for it: `mm::IPlatform::random()` is "thirty-two
// random bits: client ids, the backoff's jitter, a request's nonce. It does not have to be
// cryptographic; it does have to differ between two units that booted together"
// (components/agent-core/include/agent/IPlatform.h). Nothing in this firmware signs or encrypts
// with it - the one thing that would, the release's signature, is Ed25519 verification and needs
// no randomness at all - so the contract is deliberately the library's and not more.
#pragma once

#include <cstdint>

namespace interfaces {

class Entropy {
 public:
  virtual ~Entropy() = default;

  // Thirty-two bits. Never blocks: a caller is in the middle of building a request.
  virtual std::uint32_t bits32() = 0;
};

}  // namespace interfaces
