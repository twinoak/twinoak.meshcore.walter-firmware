#include "core/update/tls_check.h"

namespace core {
namespace update {

UplinkVerdict TlsHandshakeCheck::check() {
  if (host_[0] == 0) return UplinkVerdict::kNotConfigured;
  const std::uint64_t deadline = clock_.nowMs() + kHandshakeDeadlineMs;
  if (trust_ != nullptr) {
    const net::TrustOutcome t = trust_->ensure(deadline);
    if (t == net::TrustOutcome::kFailed || t == net::TrustOutcome::kNotBegun) {
      last_ = interfaces::TlsOpen::kTlsFailed;
      return UplinkVerdict::kFailed;
    }
  }
  last_ = socket_.open(host_, port_, net::rootForHost(host_), deadline);
  socket_.close();
  return last_ == interfaces::TlsOpen::kOpen ? UplinkVerdict::kReached : UplinkVerdict::kFailed;
}

}  // namespace update
}  // namespace core
