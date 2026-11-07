// The payloads of protocol 1.1, spec/LINK-PROTOCOL.md §5-§8. spec/tools/linkref.py's encode()
// and decode() are the reference and every case of spec/fixtures/link/link-v1.json and of
// link-v1.1.json is held against this file by
// host_test/core/link/codec/test_link_fixtures.cpp - in both directions it
// implements: a request's fields become its payload, a response's or an event's payload becomes
// its fields.
#include "core/link/codec/messages.h"

#include "core/link/codec/reader.h"
#include "core/link/codec/writer.h"

namespace core {
namespace link {
namespace {

// ------------------------------------------------------------------ the shared pieces, encoded

// §6.1. The three bytes are always there; the hashes only for kHops. A mode this firmware does
// not know is refused here as well as on the way in: it would mean sending a path the radio
// cannot read.
bool putPath(Writer& w, const Path& path) {
  if (path.mode == path_mode::kNone || path.mode == path_mode::kDirect) {
    w.u8(path.mode);
    w.u8(0);
    return w.u8(0);
  }
  if (path.mode != path_mode::kHops) {
    w.fail();
    return false;
  }
  const std::size_t expected = static_cast<std::size_t>(path.hash_size) * path.hop_count;
  if (path.hash_size < kMinHashSize || path.hash_size > kMaxHashSize ||
      path.hop_count < kMinHopCount || path.hop_count > kMaxHopCount ||
      path.hashes.size != expected || expected > kMaxPathBytes) {
    w.fail();
    return false;
  }
  w.u8(path.mode);
  w.u8(path.hash_size);
  w.u8(path.hop_count);
  return w.span(path.hashes);
}

// ------------------------------------------------------------------ the shared pieces, decoded

bool takePath(Reader& r, Path* path) {
  std::uint8_t hash_size = 0;
  std::uint8_t hop_count = 0;
  if (!r.u8(&path->mode) || !r.u8(&hash_size) || !r.u8(&hop_count)) {
    return false;
  }
  if (path->mode == path_mode::kNone || path->mode == path_mode::kDirect) {
    // §6.1 writes them as 0 for these modes and nothing reads them. They are not checked here:
    // the layout does not depend on them, and a byte this end ignores is not worth refusing a
    // whole response over.
    path->hash_size = 0;
    path->hop_count = 0;
    path->hashes = Bytes{};
    return true;
  }
  if (path->mode != path_mode::kHops) {
    r.fail();  // a mode whose layout is unknown: the rest of the payload cannot be read
    return false;
  }
  const std::size_t expected = static_cast<std::size_t>(hash_size) * hop_count;
  if (hash_size < kMinHashSize || hash_size > kMaxHashSize || hop_count < kMinHopCount ||
      hop_count > kMaxHopCount || expected > kMaxPathBytes) {
    r.fail();
    return false;
  }
  path->hash_size = hash_size;
  path->hop_count = hop_count;
  return r.span(expected, &path->hashes);
}

bool takeHelloBody(Reader& r, HelloBody* body) {
  const bool through_1_0 = r.u8(&body->proto_major) && r.u8(&body->proto_minor) &&
                           r.u8(&body->role) && r.u8(&body->slot) && r.u8(&body->max_payload) &&
                           r.u32(&body->caps) && r.u32(&body->boot_id) &&
                           r.str8(&body->fw_version) && r.str8(&body->board) &&
                           r.str8(&body->upstream);
  if (!through_1_0) {
    return false;
  }
  // §5.1: 1.1 put `band` at the end of the body, and what decides whether it is there is the
  // **body's own** `proto_minor` and not what this end asked for - a 1.0 RAK answers a 1.1
  // request with a 1.0 body. A minor beyond this one still ends with the fields it knows
  // (*Versions*), so anything past the band is left where it is and ignored.
  if (body->proto_minor < kBandMinor) {
    return true;
  }
  return r.str8(&body->band);
}

// §6.3. `state` selects the layout of the rest, so a state this firmware does not know is
// malformed: there is no way to tell a trace's hashes from a response's data without it.
bool takeReply(Reader& r, ReplyRecord* reply) {
  if (!r.u8(&reply->state) || !r.u32(&reply->tag) || !r.u8(&reply->via_path) ||
      !r.i8(&reply->snr_q4) || !r.i16(&reply->rssi) || !r.u32(&reply->elapsed_ms)) {
    return false;
  }
  switch (reply->state) {
    case reply_state::kNone:
    case reply_state::kPending:
    case reply_state::kTimeout:
      return true;  // the fixed part is the whole record
    case reply_state::kResponse:
      reply->data = r.rest();
      return r.ok();
    case reply_state::kText:
      reply->text = r.restText();
      return r.ok();
    case reply_state::kTrace: {
      std::uint8_t hash_bytes = 0;
      std::uint8_t snr_count = 0;
      return r.u8(&reply->trace_flags) && r.i8(&reply->final_snr_q4) && r.u8(&hash_bytes) &&
             r.span(hash_bytes, &reply->hashes) && r.u8(&snr_count) &&
             r.span(snr_count, &reply->snrs);
    }
    default:
      r.fail();
      return false;
  }
}

// §7. `path_len` is MeshCore's encoding: bits 7-6 the hash size minus one, bits 5-0 the hop
// count, so the path's length is implied and is checked against what is there.
bool takeObservation(Reader& r, Observation* obs) {
  if (!r.span(kPubkeySize, &obs->pubkey) || !r.u32(&obs->adv_ts) || !r.u32(&obs->age_s) ||
      !r.i8(&obs->snr_q4) || !r.i16(&obs->rssi) || !r.u8(&obs->path_len)) {
    return false;
  }
  const std::size_t path_bytes =
      static_cast<std::size_t>(pathHashSize(obs->path_len)) * pathHopCount(obs->path_len);
  if (!r.span(path_bytes, &obs->path)) {
    return false;
  }
  std::uint8_t app_len = 0;
  if (!r.u8(&app_len)) {
    return false;
  }
  if (app_len > kMaxAppData) {
    r.fail();  // §7: app_len is 0 to 32
    return false;
  }
  return r.span(app_len, &obs->app_data);
}

// §3. The status byte, and whether a body follows it. False: the payload has not even a status
// byte, which is malformed. *body is false when the status is not OK - the payload is then that
// one byte and nothing else.
bool takeStatus(Reader& r, std::uint8_t* status, bool* body) {
  if (!r.u8(status)) {
    return false;
  }
  *body = (*status == status::kOk);
  return true;
}

Encoded done(const Writer& w) {
  Encoded encoded;
  encoded.ok = w.ok();
  encoded.size = w.size();
  return encoded;
}

}  // namespace

// ---------------------------------------------------------------------------- requests

Encoded encode(const HelloRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  w.u8(request.proto_major);
  w.u8(request.proto_minor);
  w.u8(request.peer_kind);
  w.u8(request.max_payload);
  w.str8(request.fw_version);
  return done(w);
}

Encoded encode(const PingRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  w.u32(request.nonce);
  return done(w);
}

Encoded encode(const TimeSetRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  w.u32(request.epoch);
  w.u8(request.source);
  w.u8(request.flags);
  return done(w);
}

Encoded encode(const RebootRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  w.u8(request.mode);
  w.u16(request.delay_ms);
  return done(w);
}

Encoded encode(const SleepNoticeRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  w.u32(request.expect_back_s);
  return done(w);
}

Encoded encode(const CliRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  if (request.text.size < kMinCliLine || request.text.size > kMaxCliLine) {
    w.fail();  // §5.7: 1 to 160 bytes of UTF-8, no terminator
    return done(w);
  }
  w.text(request.text);
  return done(w);
}

Encoded encode(const LppGetRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  w.u8(request.perm_mask);
  return done(w);
}

Encoded encode(const JobBeginRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  if (request.pubkey.size != kPubkeySize) {
    w.fail();  // §6.1: a 32-byte MeshCore public key
    return done(w);
  }
  w.span(request.pubkey);
  putPath(w, request.path);
  return done(w);
}

Encoded encode(const PathSetRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  putPath(w, request.path);
  return done(w);
}

Encoded encode(const SendLoginRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  if (request.password.size > kMaxPassword) {
    w.fail();  // §6.2: 0 to 15 bytes
    return done(w);
  }
  w.u32(request.timeout_ms);
  w.span(request.password);
  return done(w);
}

Encoded encode(const SendRequestRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  w.u32(request.timeout_ms);
  w.u8(request.req_type);
  w.span(request.data);
  return done(w);
}

Encoded encode(const SendCliRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  if (request.text.size < kMinCliLine || request.text.size > kMaxMeshCliText) {
    w.fail();  // §6.2: 1 to 148 bytes
    return done(w);
  }
  w.u32(request.timeout_ms);
  w.u8(request.flags);
  w.text(request.text);
  return done(w);
}

Encoded encode(const SendDiscoveryRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  w.u32(request.timeout_ms);
  return done(w);
}

Encoded encode(const SendTraceRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  if (!isTraceHashSize(request.hash_size)) {
    w.fail();  // §6.2: 1, 2, 4 or 8 - a trace cannot express three-byte hashes (ERR_RANGE)
    return done(w);
  }
  w.u32(request.timeout_ms);
  w.u8(request.hash_size);
  w.span(request.hashes);
  return done(w);
}

Encoded encode(const ObsPullRequest& request, std::uint8_t* out, std::size_t out_size) {
  Writer w(out, out_size);
  w.u8(request.max);
  return done(w);
}

// ---------------------------------------------------------------------------- responses

bool decode(Bytes payload, StatusOnly* out) {
  Reader r(payload);
  bool body = false;
  return takeStatus(r, &out->status, &body);  // the body, if any, is nothing to this one
}

bool decode(Bytes payload, HelloResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return takeHelloBody(r, &out->body) && r.ok();
}

bool decode(Bytes payload, PingResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return r.u32(&out->nonce) && r.u32(&out->uptime_s);
}

bool decode(Bytes payload, StatusResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return r.u32(&out->uptime_s) && r.u8(&out->reset_reason) && r.u8(&out->time_state) &&
         r.u32(&out->epoch) && r.u16(&out->flags) && r.u8(&out->obs_pending) &&
         r.u16(&out->obs_dropped) && r.u16(&out->link_errors) && r.u8(&out->attention);
}

bool decode(Bytes payload, TimeSetResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return r.u32(&out->prev_epoch) && r.u32(&out->epoch) && r.u8(&out->time_state);
}

bool decode(Bytes payload, TimeGetResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return r.u32(&out->epoch) && r.u8(&out->time_state);
}

bool decode(Bytes payload, IdentityResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return r.span(kPubkeySize, &out->pubkey) && r.str8(&out->name);
}

bool decode(Bytes payload, CliResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  out->text = r.restText();  // §5.7: 0 to 160 bytes, and 0 is a legal reply
  return r.ok();
}

bool decode(Bytes payload, PowerGetResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  if (!r.u8(&out->valid)) {
    return false;
  }
  for (PowerChannel& channel : out->channels) {  // §5.8: the three channels follow one another
    if (!r.u16(&channel.bus_mv) || !r.i16(&channel.current_ma)) {
      return false;
    }
  }
  return true;
}

bool decode(Bytes payload, EnvGetResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return r.u8(&out->valid) && r.i16(&out->temp_centi) && r.u16(&out->rh_centi) &&
         r.u32(&out->pressure_pa);
}

bool decode(Bytes payload, LppGetResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  out->lpp = r.rest();
  return r.ok();
}

bool decode(Bytes payload, PathGetResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return takePath(r, &out->path);
}

bool decode(Bytes payload, SendResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return r.u8(&out->send) && r.u32(&out->tag) && r.u32(&out->sender_ts);
}

bool decode(Bytes payload, ReplyGetResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return takeReply(r, &out->reply);
}

bool decode(Bytes payload, ObsPullResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  if (!r.u8(&out->remaining) || !r.u16(&out->dropped) || !r.u8(&out->count)) {
    return false;
  }
  if (out->count > kMaxObservations) {
    return false;  // more records than a payload can hold: not a count this radio can have sent
  }
  for (std::uint8_t i = 0; i < out->count; ++i) {
    if (!takeObservation(r, &out->records[i])) {
      return false;
    }
  }
  return true;
}

bool decode(Bytes payload, MeshCountersResponse* out) {
  Reader r(payload);
  bool body = false;
  if (!takeStatus(r, &out->status, &body) || !body) {
    return r.ok();
  }
  return r.u32(&out->rx) && r.u32(&out->tx) && r.u32(&out->client_tx) && r.u32(&out->adverts) &&
         r.u32(&out->adverts_dropped) && r.u32(&out->obs_dropped) && r.i16(&out->noise_floor) &&
         r.i16(&out->last_rssi) && r.i8(&out->last_snr_q4);
}

// ---------------------------------------------------------------------------- events

bool decode(Bytes payload, EvBoot* out) {
  Reader r(payload);  // no status byte: §5.1's body behind the reset reason
  return r.u8(&out->reset_reason) && takeHelloBody(r, &out->body) && r.ok();
}

bool decode(Bytes payload, EvMeshReply* out) {
  Reader r(payload);
  return takeReply(r, &out->reply);
}

bool decode(Bytes payload, EvAttention* out) {
  Reader r(payload);
  return r.u8(&out->reason) && r.u16(&out->detail) && r.u8(&out->repeat);
}

}  // namespace link
}  // namespace core
