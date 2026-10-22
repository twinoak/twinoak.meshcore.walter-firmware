#include <agent/ServerPolicy.h>

#include <agent/Backoff.h>

namespace mm {

ServerAction actionFor(bool reachable, int httpStatus, uint32_t retryAfterS) {
  if (!reachable) return ServerAction::BackOff;
  if (httpStatus >= 200 && httpStatus < 300) return ServerAction::Ok;
  if (httpStatus == 401) return ServerAction::Halt;
  // A 429's wait is Retry-After whether the header came or not: the server is rate-limiting and
  // kRetryAfterWhenUnstatedMs is the safe guess. A 5xx is believed only when it said something -
  // without a header there is nothing to honour and the doubling below is the answer.
  if (httpStatus == 429) return ServerAction::WaitRetryAfter;
  if (saysWhenToComeBack(httpStatus) && retryAfterS > 0) return ServerAction::WaitRetryAfter;
  // The 4xx the .NET agent calls Definitive (ServerAnswer.Definitive in Uplink.cs): the request
  // itself is wrong, or the resource is gone, and sending it again would only be wrong again.
  // 403 is here too - an exit node the owner paused is handed nothing, and saying so twice a
  // second helps nobody - and 413, which a batch too large for the server earns.
  if (httpStatus == 400 || httpStatus == 403 || httpStatus == 404 || httpStatus == 409 || httpStatus == 410 ||
      httpStatus == 413 || httpStatus == 422) {
    return ServerAction::Discard;
  }
  // Everything else - a 5xx, a 408, a status no version of this server has sent yet - is worth
  // trying again later.
  return ServerAction::BackOff;
}

bool saysWhenToComeBack(int httpStatus) {
  // 503 is what a deploy of meshmaster.dk answers while the new build starts, 502 and 504 what a
  // reverse proxy in front of it answers for the same seconds. All three carry Retry-After when
  // the server knows how long it will be, and that is a better number than any doubling.
  return httpStatus == 429 || httpStatus == 502 || httpStatus == 503 || httpStatus == 504;
}

uint32_t retryAfterMs(uint32_t retryAfterS) {
  if (retryAfterS == 0) return kRetryAfterWhenUnstatedMs;
  const uint32_t ms = retryAfterS > kMaxRetryAfterMs / 1000u ? kMaxRetryAfterMs : retryAfterS * 1000u;
  return ms;
}

uint32_t maxBackoffMs(bool linkProven) { return linkProven ? kMaxBackoffWhenLinkUpMs : kMaxBackoffMs; }

uint32_t nextBackoffMs(uint32_t previousMs, bool linkProven) {
  // The sequence is the one both backoffs of this firmware share (agent/Backoff.h); what is
  // waited is this number through withJitter(), so two boards that failed together drift apart.
  // A link proven while the board was already deep in the sequence drops the wait to the lower
  // ceiling at the next failure rather than keeping the step it had reached.
  return nextBackoffMs(previousMs, kFirstBackoffMs, maxBackoffMs(linkProven));
}

}  // namespace mm
