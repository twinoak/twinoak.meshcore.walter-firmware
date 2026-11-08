#include "sim/sim_rak.h"

#include <cstring>

namespace sim {

namespace link = core::link;

namespace {

void put8(std::vector<std::uint8_t>& into, std::uint8_t v) { into.push_back(v); }
void put16(std::vector<std::uint8_t>& into, std::uint16_t v) {
  into.push_back(static_cast<std::uint8_t>(v & 0xFF));
  into.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& into, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) into.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
std::uint32_t get32(const std::uint8_t* at) {
  return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
         (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}
std::vector<std::uint8_t> statusOnly(std::uint8_t status) { return std::vector<std::uint8_t>{status}; }

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string toHex(const std::uint8_t* bytes, std::size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 0x0F]);
  }
  return out;
}

void wipe(void* at, std::size_t size) {
  auto* p = static_cast<volatile std::uint8_t*>(at);
  for (std::size_t i = 0; i < size; ++i) p[i] = 0;
}

}  // namespace

SimRak::SimRak(fakes::FakeClock& clock, mm::IMesh& repeater)
    : own_radio_(clock), radio(own_radio_), clock_(clock), repeater_(repeater) {
  radio.on_request = [this](const link::Packet& packet, std::vector<std::uint8_t>* payload) {
    return onRequest(packet, payload);
  };
}

SimRak::SimRak(ScriptedRadio& existing, fakes::FakeClock& clock, mm::IMesh& repeater)
    : own_radio_(clock), radio(existing), clock_(clock), repeater_(repeater) {
  radio.on_request = [this](const link::Packet& packet, std::vector<std::uint8_t>* payload) {
    return onRequest(packet, payload);
  };
}

std::size_t SimRak::read(std::uint8_t* out, std::size_t max) {
  tick();
  return radio.read(out, max);
}

std::size_t SimRak::write(const std::uint8_t* data, std::size_t size) {
  tick();
  return radio.write(data, size);
}

// ---------------------------------------------------------------------------- the clock's work

namespace {
struct Busy {
  explicit Busy(bool& flag) : flag_(flag) { flag_ = true; }
  ~Busy() { flag_ = false; }
  bool& flag_;
};
}  // namespace

void SimRak::tick() {
  if (busy_) return;  // re-entered from inside the repeater's own wait: nothing to add
  const Busy busy(busy_);
  const std::uint64_t now = clock_.nowMs();
  // §6.1: a Walter that crashed must not leave a peer open.
  if (job_ && now - last_request_ms_ >= job_idle_end_ms) {
    forgetJob();
    ++jobs_timed_out;
  }
  if (await_.kind == Await::kNone) return;
  // The repeater behind this radio delivers on its own clock, which the harness moves with this
  // one: one millisecond of its wait is one look at whether the reply is due.
  const bool answered = await_.kind == Await::kText ? repeater_.awaitText(1, nullptr)
                                                    : repeater_.awaitReply(await_.tag, 1, nullptr);
  if (answered) {
    finishWait(true);
    return;
  }
  if (clock_.nowMs() - await_.sent_at_ms >= await_.timeout_ms) {
    finishWait(false);
  }
}

// §6.3: the outcome, reported exactly once as an event, kept in the record.
void SimRak::finishWait(bool answered) {
  record_ = Record{};
  record_.flood = await_.flood;
  record_.tag = await_.tag;
  record_.elapsed_ms = static_cast<std::uint32_t>(clock_.nowMs() - await_.sent_at_ms);
  if (answered) {
    record_.reply = repeater_.reply();
    switch (record_.reply.kind) {
      case mm::MeshReply::Kind::Response: record_.state = link::reply_state::kResponse; break;
      case mm::MeshReply::Kind::Text: record_.state = link::reply_state::kText; break;
      case mm::MeshReply::Kind::Trace: record_.state = link::reply_state::kTrace; break;
      case mm::MeshReply::Kind::None: record_.state = link::reply_state::kTimeout; break;
    }
    if (record_.reply.kind != mm::MeshReply::Kind::Text) record_.tag = record_.reply.tag;
    // A flooded send whose reply taught the repeater a path came back as a returned path.
    record_.via_path = await_.flood && repeater_.hasPath();
  } else {
    record_.state = link::reply_state::kTimeout;
    record_.elapsed_ms = await_.timeout_ms;
  }
  await_ = Awaiting{};
  if (lose_all_events || lose_events > 0) {
    if (lose_events > 0) --lose_events;
    ++events_lost;
    return;
  }
  radio.sendEventPayload(link::type::kEvMeshReply, encodeRecord(), event_delay_ms);
  ++events_sent;
}

// §6.3's record: state, tag, via_path, snr_q4, rssi, elapsed_ms, then the state's own fields.
std::vector<std::uint8_t> SimRak::encodeRecord() const {
  std::vector<std::uint8_t> out;
  put8(out, record_.state);
  put32(out, record_.tag);
  put8(out, record_.via_path ? 1 : 0);
  const bool heard = record_.state >= link::reply_state::kResponse;
  put8(out, static_cast<std::uint8_t>(heard ? reply_snr_q4 : 0));
  put16(out, static_cast<std::uint16_t>(heard ? reply_rssi : link::kRssiUnknown));
  put32(out, record_.elapsed_ms);
  const mm::MeshReply& r = record_.reply;
  switch (record_.state) {
    case link::reply_state::kResponse:
      out.insert(out.end(), r.data, r.data + r.len);
      break;
    case link::reply_state::kText:
      out.insert(out.end(), r.text, r.text + std::strlen(r.text));
      break;
    case link::reply_state::kTrace: {
      put8(out, r.traceFlags);
      const float q4 = r.traceFinalSnr * 4.0f;
      put8(out, static_cast<std::uint8_t>(static_cast<std::int8_t>(q4 < 0 ? q4 - 0.5f : q4 + 0.5f)));
      put8(out, r.traceHashBytes);
      out.insert(out.end(), r.traceHashes, r.traceHashes + r.traceHashBytes);
      put8(out, r.traceSnrCount);
      out.insert(out.end(), r.traceSnrs, r.traceSnrs + r.traceSnrCount);
      break;
    }
    default:
      break;
  }
  return out;
}

// §6.1's path, as the repeater holds it now.
std::vector<std::uint8_t> SimRak::encodePath() const {
  std::vector<std::uint8_t> out;
  if (!repeater_.hasPath()) {
    put8(out, link::path_mode::kNone);
    put8(out, 0);
    put8(out, 0);
    return out;
  }
  if (repeater_.pathHopCount() == 0) {
    put8(out, link::path_mode::kDirect);
    put8(out, 0);
    put8(out, 0);
    return out;
  }
  char hops[link::kMaxHopCount][8];
  const int n = repeater_.describePath(hops, link::kMaxHopCount);
  const std::size_t hash_size = n > 0 ? std::strlen(hops[0]) / 2 : 0;
  put8(out, link::path_mode::kHops);
  put8(out, static_cast<std::uint8_t>(hash_size));
  put8(out, static_cast<std::uint8_t>(n));
  for (int i = 0; i < n; ++i) {
    for (std::size_t b = 0; b < hash_size; ++b) {
      out.push_back(static_cast<std::uint8_t>((hexDigit(hops[i][b * 2]) << 4) | hexDigit(hops[i][b * 2 + 1])));
    }
  }
  return out;
}

bool SimRak::decodePath(const std::uint8_t* at, std::size_t size, std::uint8_t* mode, std::uint8_t* hash_size,
                        std::uint8_t* hop_count, std::vector<std::uint8_t>* hashes) const {
  if (size < 3) return false;
  *mode = at[0];
  *hash_size = at[1];
  *hop_count = at[2];
  hashes->clear();
  if (*mode == link::path_mode::kNone || *mode == link::path_mode::kDirect) {
    return *hash_size == 0 && *hop_count == 0 && size == 3;
  }
  if (*mode != link::path_mode::kHops) return false;
  if (*hash_size < link::kMinHashSize || *hash_size > link::kMaxHashSize) return false;
  if (*hop_count < link::kMinHopCount || *hop_count > link::kMaxHopCount) return false;
  const std::size_t n = static_cast<std::size_t>(*hash_size) * *hop_count;
  if (n > link::kMaxPathBytes || size != 3 + n) return false;
  hashes->assign(at + 3, at + 3 + n);
  return true;
}

void SimRak::forgetJob() {
  repeater_.endJob();
  job_ = false;
  await_ = Awaiting{};
  record_ = Record{};
}

void SimRak::restart(bool announce) {
  forgetJob();
  radio.restart(announce);
}

// MeshCore's unique-time source on this radio's clock: never the same second twice.
std::uint32_t SimRak::uniqueTime() {
  std::uint32_t now = radio.clockEpoch();
  if (now == 0) now = 1791720000u + static_cast<std::uint32_t>(clock_.nowMs() / 1000u);
  if (now <= last_unique_) return ++last_unique_;
  return last_unique_ = now;
}

// ---------------------------------------------------------------------------- the requests

bool SimRak::onRequest(const link::Packet& packet, std::vector<std::uint8_t>* payload) {
  const Busy busy(busy_);
  const std::uint8_t type = packet.type;
  const std::uint8_t* at = packet.payload.data;
  const std::size_t size = packet.payload.size;
  last_request_ms_ = clock_.nowMs();  // §6.1: any valid request restarts the idle count

  switch (type) {
    case link::type::kJobBegin: {
      if (size < link::kPubkeySize + 3) {
        *payload = statusOnly(link::status::kMalformed);
        ++refused_other;
        return true;
      }
      std::uint8_t mode, hash_size, hop_count;
      std::vector<std::uint8_t> hashes;
      if (!decodePath(at + link::kPubkeySize, size - link::kPubkeySize, &mode, &hash_size, &hop_count, &hashes)) {
        *payload = statusOnly(link::status::kMalformed);
        ++refused_other;
        return true;
      }
      // §6.1: ends any job in progress first, then makes this key the peer.
      await_ = Awaiting{};
      record_ = Record{};
      peer_hex_ = toHex(at, link::kPubkeySize);
      const bool begun = mode == link::path_mode::kHops
                             ? repeater_.beginJob(peer_hex_.c_str(), hashes.data(), hop_count, hash_size)
                             : repeater_.beginJob(peer_hex_.c_str(), nullptr, 0, 0);
      if (begun && mode == link::path_mode::kDirect) repeater_.setDirectPath();
      job_ = begun;
      ++jobs_begun;
      *payload = statusOnly(begun ? link::status::kOk : link::status::kFailed);
      return true;
    }
    case link::type::kJobEnd:
      forgetJob();
      ++jobs_ended;
      *payload = statusOnly(link::status::kOk);
      return true;
    case link::type::kPathSet: {
      if (!job_) {
        *payload = statusOnly(link::status::kState);
        ++refused_state;
        return true;
      }
      std::uint8_t mode, hash_size, hop_count;
      std::vector<std::uint8_t> hashes;
      if (!decodePath(at, size, &mode, &hash_size, &hop_count, &hashes)) {
        *payload = statusOnly(link::status::kMalformed);
        ++refused_other;
        return true;
      }
      if (mode == link::path_mode::kNone) {
        repeater_.forgetPath();
      } else if (mode == link::path_mode::kDirect) {
        repeater_.setDirectPath();
      } else {
        // The repeater behind this radio takes a path of hops with the job alone, so the job is
        // begun again with the same peer: the same peer, a new path, nothing awaited.
        await_ = Awaiting{};
        record_ = Record{};
        repeater_.beginJob(peer_hex_.c_str(), hashes.data(), hop_count, hash_size);
      }
      *payload = statusOnly(link::status::kOk);
      return true;
    }
    case link::type::kPathGet: {
      ++path_reads;
      *payload = statusOnly(link::status::kOk);
      const std::vector<std::uint8_t> path = encodePath();
      payload->insert(payload->end(), path.begin(), path.end());
      return true;
    }
    case link::type::kSendLogin:
    case link::type::kSendRequest:
    case link::type::kSendCli:
    case link::type::kSendDiscovery:
    case link::type::kSendTrace:
      *payload = send(packet);
      return true;
    case link::type::kReplyGet: {
      ++reply_gets;
      *payload = statusOnly(link::status::kOk);
      const std::vector<std::uint8_t> record = encodeRecord();
      payload->insert(payload->end(), record.begin(), record.end());
      return true;
    }
    case link::type::kMeshCounters: {
      ++counter_reads;
      const mm::MeshCounters& c = repeater_.counters();
      *payload = statusOnly(link::status::kOk);
      put32(*payload, c.rx);
      put32(*payload, client_tx() + forwards);
      put32(*payload, client_tx());
      put32(*payload, c.adverts);
      put32(*payload, c.advertsDropped);
      put32(*payload, obs_dropped);
      put16(*payload, static_cast<std::uint16_t>(noise_floor));
      put16(*payload, static_cast<std::uint16_t>(last_rssi));
      put8(*payload, static_cast<std::uint8_t>(last_snr_q4));
      return true;
    }
    case link::type::kObsPull: {
      // §7: `max` records wanted; as many as fit one payload and no more than `max`, oldest
      // first, removed from the ring as they go. `remaining` is what is left after this answer
      // and `dropped` is what was lost since the last pull.
      ++obs_pulls;
      const std::uint8_t max = size >= 1 ? at[0] : 0;
      // "The RAK returns as many as fit in one payload and no more than `max`": the bound that
      // decides is the payload's, because a record with a path and application data is far
      // bigger than the 45-byte smallest one link::kMaxObservations is counted from.
      std::size_t count = 0;
      std::size_t bytes = link::kObsPullHeadSize;
      while (count < observations.size() && count < max && count < link::kMaxObservations) {
        const Advert& next = observations[count];
        const std::size_t record = 45 + next.path.size() + next.app_data.size();
        if (bytes + record > link::kMaxPayload) break;
        bytes += record;
        ++count;
      }
      *payload = statusOnly(link::status::kOk);
      put8(*payload, static_cast<std::uint8_t>(observations.size() - count));
      put16(*payload, obs_dropped_since_pull);
      put8(*payload, static_cast<std::uint8_t>(count));
      for (std::size_t i = 0; i < count; ++i) {
        const Advert& a = observations.front();
        payload->insert(payload->end(), a.pubkey.begin(), a.pubkey.end());
        put32(*payload, a.adv_ts);
        put32(*payload, a.age_s);
        put8(*payload, static_cast<std::uint8_t>(a.snr_q4));
        put16(*payload, static_cast<std::uint16_t>(a.rssi));
        put8(*payload, a.path_len);
        payload->insert(payload->end(), a.path.begin(), a.path.end());
        put8(*payload, static_cast<std::uint8_t>(a.app_data.size()));
        payload->insert(payload->end(), a.app_data.begin(), a.app_data.end());
        observations.pop_front();
        ++obs_handed_out;
      }
      obs_dropped_since_pull = 0;
      return true;
    }
    case link::type::kCli: {
      if (size < link::kMinCliLine || size > link::kMaxCliLine) {
        *payload = statusOnly(link::status::kMalformed);
        ++refused_other;
        return true;
      }
      std::memcpy(cli_, at, size);
      cli_[size] = 0;
      const std::string line(cli_, size);
      // §5.7: the line is run; nothing of it is kept here but what the test asked to see. The
      // buffer is wiped whatever the line was - this end cannot know which line is a password.
      wipe(cli_, sizeof cli_);
      cli_lines.push_back(line);
      std::string reply = "Unknown command";
      if (console && !console(line, &reply)) {
        payload->clear();  // a line that restarts the node: no response, none owed
        return true;
      }
      if (reply.size() > link::kMaxCliReply) reply.resize(link::kMaxCliReply);
      *payload = statusOnly(link::status::kOk);
      payload->insert(payload->end(), reply.begin(), reply.end());
      return true;
    }
    case link::type::kLppGet: {
      if (size < 1) {
        *payload = statusOnly(link::status::kMalformed);
        ++refused_other;
        return true;
      }
      lpp_masks.push_back(at[0]);
      *payload = statusOnly(link::status::kOk);
      payload->insert(payload->end(), lpp.begin(), lpp.end());
      return true;
    }
    case link::type::kLocalRequest: {
      // §5.9: `req_type : u8`, then `data` : rest, 0 to 32 bytes. Nothing of the mesh client is
      // touched - no job, no packet, no client_tx.
      if (size < 1) {
        *payload = statusOnly(link::status::kMalformed);
        ++refused_other;
        return true;
      }
      const std::uint8_t req_type = at[0];
      const std::size_t len = size - 1;
      if (len > link::kMaxLocalRequestData) {
        *payload = statusOnly(link::status::kMalformed);
        ++refused_other;
        ++local_refused;
        return true;
      }
      LocalAsked asked;
      asked.req_type = req_type;
      asked.data.assign(at + 1, at + 1 + len);
      local_asked.push_back(asked);
      std::vector<std::uint8_t> answer;
      std::uint8_t status = link::status::kOk;
      if (!local_door) {
        status = link::status::kUnsupported;  // §5.9: a role with no request handler
      } else if (!link::local_req::isAnswered(req_type)) {
        status = link::status::kRange;  // telemetry is LPP_GET's, keep-alive is a session's
      } else if (!local_door(req_type, len == 0 ? nullptr : at + 1, len, &answer)) {
        status = link::status::kRange;  // the handler answered nothing: silence, over the mesh
      }
      if (status != link::status::kOk) {
        *payload = statusOnly(status);
        ++refused_other;
        ++local_refused;
        return true;
      }
      *payload = statusOnly(link::status::kOk);
      payload->insert(payload->end(), answer.begin(), answer.end());
      return true;
    }
    default:
      return false;  // §5's own, §7, and anything else: the fixtures answer
  }
}

// §6.2, the five sends. Every one begins with timeout_ms; every one is answered at once.
std::vector<std::uint8_t> SimRak::send(const link::Packet& packet) {
  const std::uint8_t type = packet.type;
  const std::uint8_t* at = packet.payload.data;
  const std::size_t size = packet.payload.size;
  if (size < 4) {
    ++refused_other;
    return statusOnly(link::status::kMalformed);
  }
  if (type != link::type::kSendTrace && !job_) {
    ++refused_state;
    return statusOnly(link::status::kState);
  }
  if (await_.kind != Await::kNone) {
    ++refused_busy;
    return statusOnly(link::status::kBusy);
  }
  const std::uint32_t timeout_ms = get32(at);
  const std::uint8_t* rest = at + 4;
  const std::size_t rest_size = size - 4;

  mm::SendResult result = mm::SendResult::Failed;
  std::uint32_t tag = 0;
  std::uint32_t sender_ts = 0;
  Await kind = Await::kResponse;
  bool flood_only = false;
  switch (type) {
    case link::type::kSendLogin: {
      if (rest_size > link::kMaxPassword) {
        ++refused_other;
        return statusOnly(link::status::kRange);
      }
      std::memcpy(password_, rest, rest_size);
      password_[rest_size] = 0;
      result = repeater_.sendLogin(reinterpret_cast<const char*>(password_), tag);
      // §6: wiped as soon as the packet is built, never stored, never printed.
      wipe(password_, sizeof password_);
      tag = 0;  // a login reply carries the peer's timestamp, not ours
      sender_ts = uniqueTime();
      break;
    }
    case link::type::kSendRequest: {
      if (rest_size < 1) {
        ++refused_other;
        return statusOnly(link::status::kMalformed);
      }
      result = repeater_.sendRequest(rest[0], rest + 1, static_cast<std::uint8_t>(rest_size - 1), tag);
      sender_ts = tag;
      break;
    }
    case link::type::kSendCli: {
      if (rest_size < 2 || rest_size - 1 > link::kMaxMeshCliText) {
        ++refused_other;
        return statusOnly(link::status::kMalformed);
      }
      const bool sensitive = (rest[0] & link::send_cli_flag::kSensitive) != 0;
      std::memcpy(cli_, rest + 1, rest_size - 1);
      cli_[rest_size - 1] = 0;
      result = repeater_.sendCli(cli_, sensitive, sender_ts);
      wipe(cli_, sizeof cli_);  // every send's buffer, flagged or not (the repeater's docs, §2f)
      tag = 0;  // a CLI reply carries no tag
      kind = Await::kText;
      break;
    }
    case link::type::kSendDiscovery:
      result = repeater_.sendDiscovery(tag);
      sender_ts = tag;
      flood_only = true;
      break;
    case link::type::kSendTrace: {
      if (rest_size < 2) {
        ++refused_other;
        return statusOnly(link::status::kMalformed);
      }
      const std::uint8_t hash_size = rest[0];
      if (!link::isTraceHashSize(hash_size)) {
        ++refused_other;
        return statusOnly(link::status::kRange);  // §6.2: a trace cannot express three-byte hashes
      }
      if (rest_size - 1 > link::kMaxPathBytes) {
        ++refused_other;
        return statusOnly(link::status::kRange);
      }
      result = repeater_.sendTrace(rest + 1, static_cast<std::uint8_t>(rest_size - 1), hash_size, tag);
      sender_ts = tag;
      kind = Await::kTrace;
      break;
    }
    default:
      break;
  }

  if (result == mm::SendResult::Failed) {
    ++refused_other;
    return statusOnly(link::status::kFailed);  // tried and failed: nothing on the air
  }
  const bool flood = flood_only || result == mm::SendResult::Flood;
  Sent one;
  one.type = type;
  one.flood = flood;
  one.timeout_ms = timeout_ms;
  one.tag = tag;
  one.at_ms = clock_.nowMs();
  sent.push_back(one);

  record_ = Record{};
  if (timeout_ms > 0) {
    // Sending and starting to wait are one step (§6.2).
    await_.kind = kind;
    await_.tag = tag;
    await_.timeout_ms = timeout_ms;
    await_.sent_at_ms = clock_.nowMs();
    await_.flood = flood;
    record_.state = link::reply_state::kPending;
    record_.tag = tag;
    record_.flood = flood;
  }

  std::vector<std::uint8_t> out = statusOnly(link::status::kOk);
  put8(out, flood ? link::send_result::kFlood : link::send_result::kDirect);
  put32(out, tag);
  put32(out, sender_ts);
  return out;
}

}  // namespace sim
