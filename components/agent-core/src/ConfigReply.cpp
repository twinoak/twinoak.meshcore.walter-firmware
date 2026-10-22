#include <agent/ConfigReply.h>

#include <agent/Json.h>

#include <cstdio>
#include <cstring>

#include <agent/Iso8601.h>

namespace mm {

namespace {

void say(char* err, size_t cap, const char* text) {
  if (err == nullptr || cap == 0) return;
  std::snprintf(err, cap, "%s", text);
}

bool takeInt(JsonObjectConst obj, const char* name, int& out, char* err, size_t cap) {
  JsonVariantConst v = obj[name];
  if (v.isNull() || !v.is<int>()) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "missing or not a whole number: %s", name);
    say(err, cap, msg);
    return false;
  }
  out = v.as<int>();
  return true;
}

bool takeBudget(JsonObjectConst obj, AgentBudget& out, char* err, size_t cap) {
  JsonObjectConst b = obj["budget"];
  if (b.isNull()) {
    say(err, cap, "missing: budget");
    return false;
  }
  return takeInt(b, "maxTransactionsPerMinute", out.maxTransactionsPerMinute, err, cap) &&
         takeInt(b, "minGapSeconds", out.minGapSeconds, err, cap);
}

// `epoch` and not `unix`: GCC predefines `unix` as 1 in every `gnu++` dialect on a Linux host, so
// a consumer compiling this file as `gnu++17` got fifteen errors here (the first consumer did -
// twinoak.meshcore.walter-firmware, its p4-01). tools/check-library-host.py greps the library for
// every such name now, and compiles it in both dialects, so the next one fails here.
bool takeTime(JsonObjectConst obj, char* text, size_t textCap, uint32_t& epoch, char* err, size_t cap) {
  const char* s = obj["serverTime"];
  if (s == nullptr) {
    say(err, cap, "missing: serverTime");
    return false;
  }
  std::snprintf(text, textCap, "%s", s);
  if (!parseUtc(s, epoch)) {
    say(err, cap, "serverTime is not a UTC timestamp");
    return false;
  }
  return true;
}

bool takeString(JsonObjectConst obj, const char* name, char* out, size_t outCap, char* err, size_t cap) {
  const char* s = obj[name];
  if (s == nullptr) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "missing: %s", name);
    say(err, cap, msg);
    return false;
  }
  std::snprintf(out, outCap, "%s", s);
  return true;
}

bool parseDocument(const char* json, size_t len, JsonDocument& doc, char* err, size_t cap) {
  const DeserializationError e = deserializeJson(doc, json, len);
  if (e) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "not JSON: %s", e.c_str());
    say(err, cap, msg);
    return false;
  }
  if (!doc.is<JsonObjectConst>()) {
    say(err, cap, "not a JSON object");
    return false;
  }
  return true;
}

}  // namespace

bool parseHeartbeatReply(const char* json, size_t len, HeartbeatReply& out, char* err, size_t errCap) {
  JsonDocument doc;
  if (!parseDocument(json, len, doc, err, errCap)) return false;
  JsonObjectConst obj = doc.as<JsonObjectConst>();

  HeartbeatReply r;
  if (!takeTime(obj, r.serverTime, sizeof r.serverTime, r.serverTimeUnix, err, errCap)) return false;
  if (!takeBudget(obj, r.budget, err, errCap)) return false;
  if (!takeInt(obj, "pollIntervalS", r.pollIntervalS, err, errCap)) return false;
  JsonVariantConst jw = obj["jobsWaiting"];
  if (jw.isNull() || !jw.is<bool>()) {
    say(err, errCap, "missing or not a boolean: jobsWaiting");
    return false;
  }
  r.jobsWaiting = jw.as<bool>();
  out = r;
  return true;
}

bool parseObservationsReply(const char* json, size_t len, ObservationsReply& out, char* err, size_t errCap) {
  JsonDocument doc;
  if (!parseDocument(json, len, doc, err, errCap)) return false;
  JsonObjectConst obj = doc.as<JsonObjectConst>();

  ObservationsReply r;
  if (!takeInt(obj, "accepted", r.accepted, err, errCap)) return false;
  JsonVariantConst dup = obj["duplicate"];
  if (dup.isNull() || !dup.is<bool>()) {
    say(err, errCap, "missing or not a boolean: duplicate");
    return false;
  }
  r.duplicate = dup.as<bool>();
  out = r;
  return true;
}

bool parseAgentConfig(const char* json, size_t len, AgentConfig& out, char* err, size_t errCap) {
  JsonDocument doc;
  if (!parseDocument(json, len, doc, err, errCap)) return false;
  JsonObjectConst obj = doc.as<JsonObjectConst>();

  AgentConfig c;
  JsonVariantConst id = obj["agentId"];
  if (id.isNull() || !id.is<long long>()) {
    say(err, errCap, "missing or not a whole number: agentId");
    return false;
  }
  c.agentId = id.as<long long>();
  if (!takeString(obj, "label", c.label, sizeof c.label, err, errCap)) return false;
  if (!takeString(obj, "transport", c.transport, sizeof c.transport, err, errCap)) return false;
  if (!takeBudget(obj, c.budget, err, errCap)) return false;
  if (!takeInt(obj, "pollIntervalS", c.pollIntervalS, err, errCap)) return false;
  if (!takeTime(obj, c.serverTime, sizeof c.serverTime, c.serverTimeUnix, err, errCap)) return false;
  out = c;
  return true;
}

}  // namespace mm
