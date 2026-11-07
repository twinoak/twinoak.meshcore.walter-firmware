#include "walter_agent/link_mesh.h"

namespace agent {

namespace {

void zeroBytes(void* at, std::size_t size) {
  auto* out = static_cast<volatile std::uint8_t*>(at);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = 0;
  }
}

void copyBytes(const void* from, void* to, std::size_t size) {
  const auto* in = static_cast<const std::uint8_t*>(from);
  auto* out = static_cast<std::uint8_t*>(to);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = in[i];
  }
}

std::uint32_t saturate(std::uint32_t value) {
  return value == 0xFFFFFFFFu ? value : value + 1;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

constexpr char kHexDigits[] = "0123456789abcdef";

// ---------------------------------------------------------------- the local door's login reply
//
// What MyMesh::handleLoginReq answers an admin login with, after the four-byte tag
// (repeater-v1.17.1 at d9296435, examples/simple_repeater/MyMesh.cpp:135-144): RESP_SERVER_LOGIN_OK,
// the legacy keep-alive field, is_admin, the entry's permissions, a four-byte random blob and
// FIRMWARE_VER_LEVEL - nine bytes. The three bytes that say *admin* are the companion fixture's
// login-success.hex (host_test/agent-core/tests/fixtures/companion): byte 1 is_admin = 1, byte 11
// permissions = 3 (PERM_ACL_ADMIN, ClientACL.h:5-9) and byte 12 FIRMWARE_VER_LEVEL = 2; a host
// test holds these constants to that file. The blob is zero: it exists to make the packet's hash
// unique on the air, and this reply is never on the air.
constexpr std::uint8_t kLocalLoginReply[] = {
    0x00,  // RESP_SERVER_LOGIN_OK (BaseChatMesh.h:21)
    0x00,  // keep-alive / 16, the legacy field
    0x01,  // is_admin
    0x03,  // permissions: PERM_ACL_ADMIN
    0x00, 0x00, 0x00, 0x00,  // the random blob
    0x02,  // FIRMWARE_VER_LEVEL (MyMesh.cpp:44)
};

}  // namespace

LinkMesh::LinkMesh(lc::Client& client, interfaces::Clock& clock) : client_(client), clock_(clock) {
  client_.subscribeRestart(this);
}

// ---------------------------------------------------------------------------- the unit's own key

void LinkMesh::setOwnKey(const std::uint8_t* key) {
  if (key == nullptr) {
    clearOwnKey();
    return;
  }
  copyBytes(key, own_key_, kKeyLen);
  own_key_known_ = true;
}

void LinkMesh::clearOwnKey() {
  own_key_known_ = false;
  zeroBytes(own_key_, sizeof own_key_);
}

// ---------------------------------------------------------------------------- the link's answers

template <typename Response>
bool LinkMesh::note(const lc::Answer<Response>& answer) {
  link_counters_.requests = saturate(link_counters_.requests);
  last_fault_ = answer.fault;
  last_status_ = answer.status;
  if (answer.ok()) {
    return true;
  }
  if (answer.fault == lc::Fault::kRefused) {
    link_counters_.refused = saturate(link_counters_.refused);
  } else {
    link_counters_.faults = saturate(link_counters_.faults);
  }
  return false;
}

bool LinkMesh::refreshCounters() {
  const lc::Answer<link::MeshCountersResponse> answer = client_.meshCounters();
  link_counters_.counter_reads = saturate(link_counters_.counter_reads);
  if (!note(answer)) {
    return false;
  }
  // §6.4 against the library's MeshCounters: `tx` is `client_tx` - one per send and nothing
  // else, the figure the runner's transaction count is held against - and never the radio's own
  // `tx`, which counts forwards. rxByType has nothing to be filled from (a specification gap).
  const link::MeshCountersResponse& m = answer.message;
  counters_.rx = m.rx;
  counters_.tx = m.client_tx;
  counters_.adverts = m.adverts;
  counters_.advertsDropped = m.adverts_dropped;
  radio_tx_ = m.tx;
  // §6.4's `obs_dropped`: what the RAK's **observation** ring lost, which is the library's
  // `observationsDropped` one hop earlier (task p4-04, walter_agent/observations.h). Kept beside
  // the library's counters and not in them: the library's figure is its own ring's, and the two
  // are added together in exactly one place.
  radio_obs_dropped_ = m.obs_dropped;
  return true;
}

link::Path LinkMesh::path() const {
  link::Path out;
  out.mode = path_.mode;
  if (path_.mode == link::path_mode::kHops) {
    out.hash_size = path_.hash_size;
    out.hop_count = path_.hop_count;
    out.hashes = link::Bytes{path_.hashes, static_cast<std::size_t>(path_.hash_size) * path_.hop_count};
  }
  return out;
}

void LinkMesh::cachePath(const link::Path& path) {
  path_ = PathCache{};
  path_.mode = path.mode;
  if (path.mode != link::path_mode::kHops) {
    return;
  }
  const std::size_t size = static_cast<std::size_t>(path.hash_size) * path.hop_count;
  if (path.hashes.data == nullptr || path.hashes.size < size || size > sizeof path_.hashes) {
    path_.mode = link::path_mode::kNone;  // not a path this end can hold: as good as none
    return;
  }
  path_.hash_size = path.hash_size;
  path_.hop_count = path.hop_count;
  copyBytes(path.hashes.data, path_.hashes, size);
}

// §6.1, PATH_GET: the path the job holds now - a returned path changes it (§6.3), so this is how
// the route a flood discovered is read.
void LinkMesh::refreshPath() {
  const lc::Answer<link::PathGetResponse> answer = client_.pathGet();
  link_counters_.path_reads = saturate(link_counters_.path_reads);
  if (note(answer)) {
    cachePath(answer.message.path);
  }
}

void LinkMesh::pathSet(std::uint8_t mode) {
  link::Path path;
  path.mode = mode;
  if (job_.local) {
    cachePath(path);  // the UART is the path: nothing to tell the RAK
    return;
  }
  const lc::Answer<link::StatusOnly> answer = client_.pathSet(path);
  if (note(answer)) {
    cachePath(path);
  }
}

// ---------------------------------------------------------------------------- the pump

void LinkMesh::loop() {
  client_.poll();
  takeMeshReplies();
}

// Every core type, when the radio lends this end its mesh client: §6.2 has a request for each of
// the five sends the thirteen are made of, SEND_TRACE included (§6.2, §6.3 state 5). Without a
// session, or without MESH_CLIENT in it, nothing can be carried and nothing is advertised - which
// for a unit whose radio has no identity yet is the truth (tasks/phase-4/FOLLOWUPS.md, [04]).
bool LinkMesh::supports(const char* jobType) const {
  (void)jobType;
  // Two conditions, and the list in the heartbeat is what the server may route by, so this is
  // also "what a claim may hand over" (agent/AgentLoop.cpp's `buildHeartbeat`):
  //
  //   the session carries MESH_CLIENT   a radio that cannot put a packet on the air carries no
  //                                     mesh job, and a radio with no session carries nothing;
  //   this end knows its own key        task p8-04. Until `setOwnKey` has it, `beginJob` cannot
  //                                     tell a job for the unit's own repeater from one for a
  //                                     stranger, and the local door below stays shut: the job
  //                                     would be addressed **over the air to the radio holding
  //                                     the conversation**, which is what happened on
  //                                     6 November (tasks/phase-6/FOLLOWUPS.md). A type not
  //                                     advertised is a job the server does not send, which is
  //                                     better than one claimed and then refused.
  return client_.hasCapability(link::cap::kMeshClient) && own_key_known_;
}

// ---------------------------------------------------------------------------- the job

bool LinkMesh::parseKey(const char* hex, std::uint8_t* out) {
  if (hex == nullptr) return false;
  for (std::size_t i = 0; i < kKeyLen; ++i) {
    const int high = hexDigit(hex[i * 2]);
    if (high < 0) return false;
    const int low = hexDigit(hex[i * 2 + 1]);
    if (low < 0) return false;
    out[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return hex[kKeyHexLen] == 0;
}

bool LinkMesh::sameKey(const std::uint8_t* a, const std::uint8_t* b) {
  for (std::size_t i = 0; i < kKeyLen; ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

bool LinkMesh::beginJob(const char* publicKeyHex, const std::uint8_t* pathHashes, std::uint8_t hopCount,
                        std::uint8_t hashSize) {
  std::uint8_t key[kKeyLen];
  if (!parseKey(publicKeyHex, key)) {
    return false;  // not 64 hex characters: no identity to address (docs/library.md §3.2)
  }
  // End any job in progress first (IMesh). A mesh job that becomes a mesh job needs nothing: §6.1
  // says JOB_BEGIN ends the job in progress itself. A mesh job that becomes a local one is ended,
  // so that the RAK is not left holding a peer it will only forget after 120 s.
  const bool local = own_key_known_ && sameKey(key, own_key_);
  if (job_.begun && !job_.local && local) {
    note(client_.jobEnd());
  }
  await_ = Await::kNone;
  resolved_ = false;
  local_held_ = false;
  clearReply();
  job_ = Job{};
  copyBytes(key, job_.peer, kKeyLen);

  if (local) {
    job_.begun = true;
    job_.local = true;
    link::Path direct;
    direct.mode = link::path_mode::kDirect;  // the UART: zero hops
    cachePath(direct);
    link_counters_.local_jobs = saturate(link_counters_.local_jobs);
    return true;
  }

  link::Path path;
  if (pathHashes != nullptr && hopCount >= link::kMinHopCount && hopCount <= link::kMaxHopCount &&
      hashSize >= link::kMinHashSize && hashSize <= link::kMaxHashSize &&
      static_cast<std::size_t>(hopCount) * hashSize <= link::kMaxPathBytes) {
    path.mode = link::path_mode::kHops;
    path.hash_size = hashSize;
    path.hop_count = hopCount;
    path.hashes = link::Bytes{pathHashes, static_cast<std::size_t>(hopCount) * hashSize};
  }
  const lc::Answer<link::StatusOnly> answer = client_.jobBegin(link::Bytes{job_.peer, kKeyLen}, path);
  // A link that did not take the job is not "a key that is not 64 hex characters": the job is
  // begun as far as the library is concerned and the sends that follow are refused by the RAK
  // (ERR_STATE), which the library reports as a failed job. The key is addressable; the link is
  // what is not there.
  job_.begun = note(answer);
  cachePath(job_.begun ? path : link::Path{});
  return true;
}

void LinkMesh::endJob() {
  if (job_.begun && !job_.local) {
    note(client_.jobEnd());
  }
  job_ = Job{};
  path_ = PathCache{};
  await_ = Await::kNone;
  resolved_ = false;
  local_held_ = false;
  clearReply();
  // One MESH_COUNTERS a job: what counters() answers the library and the console between jobs,
  // and the figure the runner's transaction count is held against.
  refreshCounters();
}

void LinkMesh::setDirectPath() {
  if (!job_.begun) return;
  pathSet(link::path_mode::kDirect);
}

void LinkMesh::forgetPath() {
  if (!job_.begun) return;
  pathSet(link::path_mode::kNone);
}

bool LinkMesh::hasPath() const { return path_.mode != link::path_mode::kNone; }

std::uint8_t LinkMesh::pathHopCount() const {
  return path_.mode == link::path_mode::kHops ? path_.hop_count : 0;
}

int LinkMesh::describePath(char hops[][8], int max) const {
  if (path_.mode != link::path_mode::kHops || hops == nullptr || max <= 0) return 0;
  int n = path_.hop_count;
  if (n > max) n = max;
  for (int i = 0; i < n; ++i) {
    const std::uint8_t* hash = &path_.hashes[static_cast<std::size_t>(i) * path_.hash_size];
    std::size_t at = 0;
    for (std::uint8_t b = 0; b < path_.hash_size && at + 2 < 8; ++b) {
      hops[i][at++] = kHexDigits[hash[b] >> 4];
      hops[i][at++] = kHexDigits[hash[b] & 0x0F];
    }
    hops[i][at] = 0;
  }
  return n;
}

// ---------------------------------------------------------------------------- the five sends

void LinkMesh::clearReply() {
  reply_ = mm::MeshReply{};
  last_record_ = LastRecord{};
}

mm::SendResult LinkMesh::afterSend(const lc::Answer<link::SendResponse>& answer, Await kind, std::uint32_t* tag,
                                   std::uint32_t* sender_ts) {
  clearReply();
  await_ = Await::kNone;
  resolved_ = false;
  if (!note(answer)) {
    return mm::SendResult::Failed;
  }
  const link::SendResponse& m = answer.message;
  if (tag != nullptr) *tag = m.tag;
  if (sender_ts != nullptr) *sender_ts = m.sender_ts;
  if (m.send == link::send_result::kNothingSent) {
    link_counters_.nothing_sent = saturate(link_counters_.nothing_sent);
    return mm::SendResult::Failed;
  }
  link_counters_.sends = saturate(link_counters_.sends);
  await_ = kind;
  await_tag_ = m.tag;
  await_flood_ = m.send == link::send_result::kFlood;
  last_record_.flood = await_flood_;
  return await_flood_ ? mm::SendResult::Flood : mm::SendResult::Direct;
}

mm::SendResult LinkMesh::sendLogin(const char* password, std::uint32_t& tag) {
  tag = 0;  // §6.2: a login reply carries the peer's timestamp, not ours
  if (job_.local) {
    return localLogin();
  }
  // At most 15 bytes go (kMaxPassword: what the firmware at the other end keeps), copied into
  // the one buffer of this object that ever holds a password, and wiped before this returns. The
  // client wipes its request and frame buffers after the exchange (`sensitive`).
  std::size_t n = 0;
  while (password != nullptr && password[n] != 0 && n < link::kMaxPassword) {
    secret_[n] = static_cast<std::uint8_t>(password[n]);
    ++n;
  }
  const lc::Answer<link::SendResponse> answer =
      client_.sendLogin(kSendTimeoutCeilingMs, link::Bytes{secret_, n});
  zeroBytes(secret_, sizeof secret_);
  return afterSend(answer, Await::kResponse, nullptr, nullptr);
}

mm::SendResult LinkMesh::sendRequest(std::uint8_t reqType, const std::uint8_t* data, std::uint8_t len,
                                     std::uint32_t& tag) {
  tag = 0;
  if (job_.local) {
    return localRequest(reqType);
  }
  const link::Bytes body{len == 0 ? nullptr : data, len};
  const lc::Answer<link::SendResponse> answer = client_.sendRequest(kSendTimeoutCeilingMs, reqType, body);
  return afterSend(answer, Await::kResponse, &tag, nullptr);
}

mm::SendResult LinkMesh::sendCli(const char* text, bool sensitive, std::uint32_t& timestamp) {
  timestamp = 0;
  const std::size_t size = link::textLength(text);
  if (job_.local) {
    return localCli(text, size, sensitive);
  }
  if (size < 1 || size > link::kMaxMeshCliText) {
    return mm::SendResult::Failed;  // §6.2: 1 to 148 bytes, and the RAK would say ERR_MALFORMED
  }
  const std::uint8_t flags = sensitive ? link::send_cli_flag::kSensitive : 0;
  const lc::Answer<link::SendResponse> answer =
      client_.sendCli(kSendTimeoutCeilingMs, flags, link::Text{text, size});
  // §6.2: the timestamp the RAK put in the packet is what the library gets back - this end
  // supplies none, ever.
  return afterSend(answer, Await::kText, nullptr, &timestamp);
}

mm::SendResult LinkMesh::sendDiscovery(std::uint32_t& tag) {
  tag = 0;
  if (job_.local) {
    link_counters_.local_unserved = saturate(link_counters_.local_unserved);
    return mm::SendResult::Failed;  // a flood to oneself discovers nothing
  }
  const lc::Answer<link::SendResponse> answer = client_.sendDiscovery(kSendTimeoutCeilingMs);
  return afterSend(answer, Await::kResponse, &tag, nullptr);
}

mm::SendResult LinkMesh::sendTrace(const std::uint8_t* roundTripHashes, std::uint8_t hashBytes, std::uint8_t hashSize,
                                   std::uint32_t& tag) {
  tag = 0;
  if (job_.local) {
    link_counters_.local_unserved = saturate(link_counters_.local_unserved);
    return mm::SendResult::Failed;  // a radio cannot trace a round trip to itself
  }
  if (roundTripHashes == nullptr || hashBytes == 0 || hashBytes > link::kMaxPathBytes ||
      !link::isTraceHashSize(hashSize)) {
    return mm::SendResult::Failed;  // §6.2: three-byte hashes are ERR_RANGE; refused before a byte goes
  }
  const lc::Answer<link::SendResponse> answer =
      client_.sendTrace(kSendTimeoutCeilingMs, hashSize, link::Bytes{roundTripHashes, hashBytes});
  return afterSend(answer, Await::kTrace, &tag, nullptr);
}

// ---------------------------------------------------------------------------- the reply

bool LinkMesh::recordFrom(const link::ReplyRecord& record) {
  clearReply();
  last_record_.state = record.state;
  last_record_.via_path = record.via_path != 0;
  last_record_.snr_q4 = record.snr_q4;
  last_record_.rssi = record.rssi;
  last_record_.elapsed_ms = record.elapsed_ms;
  last_record_.flood = await_flood_;
  reply_.tag = record.tag;
  switch (record.state) {
    case link::reply_state::kResponse: {
      // §6.3: the response after its four tag bytes, which is what the library's codec reads.
      reply_.kind = mm::MeshReply::Kind::Response;
      std::size_t n = record.data.size;
      if (n > mm::kMeshReplyDataCap) n = mm::kMeshReplyDataCap;
      if (n > 0) copyBytes(record.data.data, reply_.data, n);
      reply_.len = static_cast<std::uint8_t>(n);
      return true;
    }
    case link::reply_state::kText: {
      reply_.kind = mm::MeshReply::Kind::Text;
      std::size_t n = record.text.size;
      if (n > mm::kMaxCliReplyLen) n = mm::kMaxCliReplyLen;
      if (n > 0) copyBytes(record.text.data, reply_.text, n);
      reply_.text[n] = 0;
      return true;
    }
    case link::reply_state::kTrace: {
      // §6.3 state 5 as MeshCore delivers a completed trace: the flags byte, this node's SNR on
      // the packet home, the hop hashes and one SNR a hop (quarter-decibels, as the library keeps
      // them).
      reply_.kind = mm::MeshReply::Kind::Trace;
      reply_.traceFlags = record.trace_flags;
      reply_.traceFinalSnr = static_cast<float>(record.final_snr_q4) / 4.0f;
      std::size_t hashes = record.hashes.size;
      if (hashes > mm::kMeshPathCap) hashes = mm::kMeshPathCap;
      if (hashes > 0) copyBytes(record.hashes.data, reply_.traceHashes, hashes);
      reply_.traceHashBytes = static_cast<std::uint8_t>(hashes);
      std::size_t snrs = record.snrs.size;
      if (snrs > mm::kMeshPathCap) snrs = mm::kMeshPathCap;
      if (snrs > 0) copyBytes(record.snrs.data, reply_.traceSnrs, snrs);
      reply_.traceSnrCount = static_cast<std::uint8_t>(snrs);
      return true;
    }
    default:
      return false;  // kTimeout: the wait is over and there is nothing to read
  }
}

bool LinkMesh::takeMeshReplies() {
  lc::QueuedEvent event;
  bool resolved = false;
  while (client_.takeEvent(link::type::kEvMeshReply, &event)) {
    link_counters_.events = saturate(link_counters_.events);
    link::EvMeshReply decoded;
    if (!link::decode(event.view(), &decoded)) {
      link_counters_.malformed = saturate(link_counters_.malformed);
      continue;
    }
    if (await_ == Await::kNone || resolved_) {
      // A reply this end had given up on, or a second event for one wait: dropped as the next
      // send would drop it (§6.3). Its bytes are read by nothing.
      link_counters_.late_replies = saturate(link_counters_.late_replies);
      continue;
    }
    if (decoded.reply.state < link::reply_state::kTimeout) {
      continue;  // §6.3 sends an event for states 2 to 5 only; anything else is not an outcome
    }
    recordFrom(decoded.reply);
    resolved_ = true;
    resolved = true;
  }
  return resolved;
}

// §6.1: JOB_BEGIN again, same peer, the path this end holds - which forgets any awaited or held
// reply on the RAK's side, so that the next send is not ERR_BUSY for a wait this end has already
// given up on.
void LinkMesh::cancelWait() {
  link_counters_.cancelled = saturate(link_counters_.cancelled);
  const lc::Answer<link::StatusOnly> answer = client_.jobBegin(link::Bytes{job_.peer, kKeyLen}, path());
  if (!note(answer)) {
    job_.begun = false;
  }
}

bool LinkMesh::await(bool text, std::uint32_t tag, std::uint32_t timeoutMs, void (*pump)()) {
  if (job_.local) {
    return localAwait(text, tag);
  }
  if (await_ == Await::kNone) {
    return false;  // nothing was sent, or the radio restarted since
  }
  // The runner's own timeout, plus the margin a reply that reached the RAK at the last moment
  // needs to cross the UART. The event is what is waited for; the clock is this end's.
  const std::uint64_t deadline = clock_.nowMs() + timeoutMs + kReplyMarginMs;
  for (;;) {
    client_.poll();
    takeMeshReplies();
    if (resolved_ || await_ == Await::kNone) {
      break;
    }
    if (pump != nullptr) {
      pump();
    }
    if (resolved_ || await_ == Await::kNone) {
      break;
    }
    if (clock_.nowMs() >= deadline) {
      break;
    }
    clock_.delayMs(kPollMs);
  }
  if (await_ == Await::kNone) {
    return false;  // the radio restarted under the wait (onRadioRestart): nothing to ask for
  }
  if (!resolved_) {
    // §6.3: the event did not come. Ask: a lost event costs one request, never a lost reply.
    const lc::Answer<link::ReplyGetResponse> answer = client_.replyGet();
    link_counters_.reply_gets = saturate(link_counters_.reply_gets);
    if (note(answer)) {
      const link::ReplyRecord& record = answer.message.reply;
      if (record.state >= link::reply_state::kTimeout) {
        link_counters_.lost_events = saturate(link_counters_.lost_events);
        recordFrom(record);
        resolved_ = true;
      } else if (record.state == link::reply_state::kPending) {
        recordFrom(record);  // the record's own fields (how long it has waited), for the console
        cancelWait();        // the RAK would wait on; this end will not
      }
      // kNone: nothing is held - the job ended or the radio restarted - and there is nothing to end.
    }
  }
  const bool resolved = resolved_;
  await_ = Await::kNone;
  resolved_ = false;
  if (!resolved) {
    return false;
  }
  // What came, against what was awaited: a text for a CLI line, a response or a trace for the
  // rest, and this tag (0 is "the next reply from the peer"). The RAK matches these too (§6.2);
  // one that still differs is a late reply of another exchange and is not this one.
  const bool is_text = reply_.kind == mm::MeshReply::Kind::Text;
  if (reply_.kind == mm::MeshReply::Kind::None || is_text != text ||
      (tag != 0 && reply_.tag != tag)) {
    if (reply_.kind != mm::MeshReply::Kind::None) {
      link_counters_.mismatched = saturate(link_counters_.mismatched);
    }
    clearReply();
    return false;
  }
  if (last_record_.flood) {
    refreshPath();  // §6.3: a returned path is learned, and PATH_GET is how this end reads it
  }
  return true;
}

bool LinkMesh::awaitReply(std::uint32_t tag, std::uint32_t timeoutMs, void (*pump)()) {
  return await(false, tag, timeoutMs, pump);
}

bool LinkMesh::awaitText(std::uint32_t timeoutMs, void (*pump)()) { return await(true, 0, timeoutMs, pump); }

// ---------------------------------------------------------------------------- the radio restarted

void LinkMesh::onRadioRestart(std::uint32_t boot_id, std::uint8_t reset_reason) {
  (void)boot_id;
  (void)reset_reason;
  if (!job_.begun || job_.local) {
    return;  // a local job is this end's own; the RAK held nothing for it
  }
  link_counters_.restarts = saturate(link_counters_.restarts);
  job_.begun = false;
  path_ = PathCache{};
  await_ = Await::kNone;
  resolved_ = false;
}

// ---------------------------------------------------------------------------- the local door

// A login to the unit's own repeater: the link has console privileges (§5.7), so the answer is
// what an admin login is answered with, and no packet goes anywhere. Without a session that has
// CLI in it there is no repeater to speak to, and the login is silent - which the library reads
// as unreachable, which is what a radio that does not answer is.
mm::SendResult LinkMesh::localLogin() {
  clearReply();
  local_held_ = false;
  if (!client_.hasCapability(link::cap::kCli)) {
    link_counters_.local_silent = saturate(link_counters_.local_silent);
    return mm::SendResult::Direct;
  }
  reply_.kind = mm::MeshReply::Kind::Response;
  reply_.tag = 0;
  copyBytes(kLocalLoginReply, reply_.data, sizeof kLocalLoginReply);
  reply_.len = sizeof kLocalLoginReply;
  last_record_.state = link::reply_state::kResponse;
  local_held_ = true;
  return mm::SendResult::Direct;
}

mm::SendResult LinkMesh::localRequest(std::uint8_t reqType) {
  clearReply();
  local_held_ = false;
  if (reqType == mm::kReqTypeGetTelemetry) {
    // §5.8 LPP_GET: the Cayenne-LPP bytes the node would answer a mesh telemetry request with at
    // these permissions - exactly what a mesh client logged in as admin would have been told.
    const lc::Answer<link::LppGetResponse> answer = client_.lppGet(kLocalLppPermissions);
    link_counters_.local_requests = saturate(link_counters_.local_requests);
    if (!note(answer)) {
      return mm::SendResult::Failed;
    }
    reply_.kind = mm::MeshReply::Kind::Response;
    std::size_t n = answer.message.lpp.size;
    if (n > mm::kMeshReplyDataCap) n = mm::kMeshReplyDataCap;
    if (n > 0) copyBytes(answer.message.lpp.data, reply_.data, n);
    reply_.len = static_cast<std::uint8_t>(n);
    last_record_.state = link::reply_state::kResponse;
    local_held_ = true;
    return mm::SendResult::Direct;
  }
  if (reqType == mm::kReqTypeGetAccessList) {
    // No request on this link yields the ACL's bytes, and the library's own rule for the access
    // list is that its silence is *no ACL in this snapshot, no evidence either way* - the one
    // key whose silence a reachable repeater guarantees (JobRunner::readAccessList). So the
    // request is "sent" and nothing answers it, and a config.read of the unit's own repeater
    // keeps its fifteen other keys.
    link_counters_.local_silent = saturate(link_counters_.local_silent);
    return mm::SendResult::Direct;
  }
  // Status (REQ 0x01, RepeaterStats as bytes), neighbours (0x06, the paged binary list) and
  // owner-info (0x07): CommonCLI.cpp at d9296435 has `stats-*` and `neighbors` lines, which
  // print text and not these structs, and nothing of §5 carries them. Failed, which the library
  // reports as a failed job with *the packet could not be built*; the report lists each as the
  // input to a link 1.1 request.
  link_counters_.local_unserved = saturate(link_counters_.local_unserved);
  return mm::SendResult::Failed;
}

mm::SendResult LinkMesh::localCli(const char* text, std::size_t size, bool sensitive) {
  clearReply();
  local_held_ = false;
  if (size < link::kMinCliLine || size > link::kMaxCliLine) {
    return mm::SendResult::Failed;  // §5.7: 1 to 160 bytes
  }
  // §5.7: the line, run by the radio's own interpreter as if typed on its console. A sensitive
  // line (one made from a password) is wiped from the client's buffers after the exchange, as a
  // SEND_CLI flagged sensitive is.
  const lc::Answer<link::CliResponse> answer = client_.cli(link::Text{text, size}, sensitive);
  link_counters_.local_requests = saturate(link_counters_.local_requests);
  if (answer.ok()) {
    note(answer);
    reply_.kind = mm::MeshReply::Kind::Text;
    std::size_t n = answer.message.text.size;
    if (n > mm::kMaxCliReplyLen) n = mm::kMaxCliReplyLen;
    if (n > 0) copyBytes(answer.message.text.data, reply_.text, n);
    reply_.text[n] = 0;
    last_record_.state = link::reply_state::kText;
    local_held_ = true;
    return mm::SendResult::Direct;
  }
  if (answer.fault == lc::Fault::kLinkDown) {
    // §5.7: some lines restart the node and never answer (`reboot`), and silence after one of
    // those is the expected outcome - the line went, and nothing came back. The library reads a
    // silent CLI line the same way (one send per command, never retried).
    note(answer);
    link_counters_.local_silent = saturate(link_counters_.local_silent);
    return mm::SendResult::Direct;
  }
  note(answer);
  return mm::SendResult::Failed;
}

bool LinkMesh::localAwait(bool text, std::uint32_t tag) {
  if (!local_held_) {
    return false;
  }
  local_held_ = false;
  const bool is_text = reply_.kind == mm::MeshReply::Kind::Text;
  if (is_text != text || (tag != 0 && reply_.tag != tag)) {
    clearReply();
    return false;
  }
  return true;
}

}  // namespace agent
