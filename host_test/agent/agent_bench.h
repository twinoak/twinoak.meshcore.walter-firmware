// What the agent's tests share (task p4-05): the wake bench with the agent's half of a Walter on
// it (host_test/core/cycle/wake_bench.h, host_test/agent/agent_half.h), the scripted meshmaster.dk
// (host_test/agent/meshmaster_script.h), the unit's own repeater as the link's fixtures name it,
// jobs built with the library's own builder (`fake::Job`, support/Jobs.h: a claimed job's JSON,
// member for member the contract fixture's), a release served the way meshmaster.dk serves one,
// and a few readers of the JSON a result carries - without a parser, as the link-mesh tests read
// theirs.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <support/Jobs.h>

#include "core/cycle/wake_bench.h"
#include "core/image/crypto/sha256.h"
#include "walter_agent/firmware_jobs.h"

namespace bench {

// The unit's own repeater, as the link fixtures' IDENTITY answers it (spec/fixtures/link/
// link-v1.json: `TwinOak Risskov`).
constexpr const char* kOwnKeyHex = "7f3a9c02e1b44d9a0f6c2b7d8e9a1c3f5d7b9e0a2c4e6f8a0b1c2d3e4f5a6b7c";
constexpr const char* kOwnName = "TwinOak Risskov";
// A release's base route on the server (unit-protocol.md §4), and where its files are.
constexpr const char* kBase = "/agent/v1/firmware/7";
constexpr const char* kFilesDir = "files";

inline std::string sha256Hex(const std::vector<std::uint8_t>& bytes) {
  std::uint8_t digest[32];
  core::image::sha256(bytes.data(), bytes.size(), digest);
  const char digits[] = "0123456789abcdef";
  std::string hex;
  for (std::uint8_t b : digest) {
    hex += digits[b >> 4];
    hex += digits[b & 0x0F];
  }
  return hex;
}

// The file's name for a `use`, out of the pretty-printed manifest text (`name` follows `use` in a
// file entry; the manifest is spaced, as test_fetch reads it).
inline std::string fileName(const std::vector<std::uint8_t>& manifest, const char* use_value) {
  const std::string text(manifest.begin(), manifest.end());
  const std::size_t use = text.find(std::string("\"use\": \"") + use_value + "\"");
  REQUIRE(use != std::string::npos);
  const std::size_t name = text.find("\"name\": \"", use);
  REQUIRE(name != std::string::npos);
  const std::size_t begin = name + 9;
  return text.substr(begin, text.find('"', begin) - begin);
}
inline std::string swdName(const std::vector<std::uint8_t>& manifest) { return fileName(manifest, "swd"); }
inline std::string otaName(const std::vector<std::uint8_t>& manifest) { return fileName(manifest, "ota"); }

// A release on the scripted server as meshmaster.dk serves one: the manifest and its signature
// under the base, the file under `{base}/files/{name}`, every answer with a strong ETag and
// ranges honoured (the scripted server's own range logic). `manifest`/`sig` override the
// candidate's own, for a tampered manifest or another signature.
inline void serveRelease(sim::ScriptedServer& server, const Candidate& image, const std::string& base = kBase,
                         const std::vector<std::uint8_t>* manifest = nullptr, const std::string* sig = nullptr,
                         const std::vector<std::uint8_t>* file = nullptr, bool ota = false) {
  const std::vector<std::uint8_t>& m = manifest != nullptr ? *manifest : image.manifest;
  const std::string s = sig != nullptr ? *sig : image.signature;
  const std::vector<std::uint8_t>& f = file != nullptr ? *file : image.file;
  server.serve(base + "/manifest.json", m, "\"m\"");
  server.serve(base + "/manifest.json.sig", std::vector<std::uint8_t>(s.begin(), s.end()), "\"s\"");
  const std::string name = ota ? otaName(image.manifest) : swdName(image.manifest);
  server.serve(base + "/" + kFilesDir + "/" + name, f, "\"" + sha256Hex(f) + "\"");
}

// The four payloads, as FirmwareOfferPayload.json, FirmwareApplyPayload.json, FirmwareStatePayload.json
// and PowerCyclePayload.json have them.
inline std::string offerPayload(const Candidate& image, const char* product = agent::kProductRepeater,
                                const std::string& base = kBase, const std::string* expected = nullptr) {
  const std::string sha = expected != nullptr ? *expected : sha256Hex(image.file);
  return std::string("{\"releaseId\":7,\"product\":\"") + product + "\",\"version\":\"" + image.version +
         "\",\"slot\":0,\"base\":\"" + base + "\",\"expectedSha256\":\"" + sha +
         "\",\"size\":" + std::to_string(image.file.size()) + "}";
}
inline std::string applyPayload(const std::string& expected_hex, const char* product = agent::kProductRepeater,
                                bool first_install = false) {
  std::string out = std::string("{\"releaseId\":7,\"slot\":0,\"product\":\"") + product + "\"";
  if (!expected_hex.empty()) out += ",\"expectedSha256\":\"" + expected_hex + "\"";
  out += first_install ? ",\"firstInstall\":true}" : ",\"firstInstall\":false}";
  return out;
}
inline std::string powerCyclePayload(int off_seconds) { return "{\"offSeconds\":" + std::to_string(off_seconds) + "}"; }

// A job for the unit's own repeater at class `none`, as the server routes every `twinoak.*` job
// (unit-protocol.md §5). No lease named: the bench walks months, and a lease that lapsed would be
// a job reported `timeout` at no transaction rather than the job itself.
inline fake::Job ownJob(long long id, const char* type, const std::string& payload) {
  return fake::Job(id, type)
      .forRepeater(17, kOwnName, kOwnKeyHex)
      .atClass("none")
      .leasedUntil(0)
      .withPayload(payload)
      .asDirectNeighbour();
}

// One top-level scalar or string member of a JSON object, without a parser (the link-mesh tests'
// reader). A string's escapes are kept as they are; none of these fields has one.
inline std::string member(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t found = json.find(needle);
  if (found == std::string::npos) return "";
  std::size_t at = found + needle.size();
  if (json[at] == '"') {
    std::size_t end = at + 1;
    while (end < json.size() && json[end] != '"') end += json[end] == '\\' ? 2 : 1;
    return json.substr(at + 1, end - at - 1);
  }
  std::size_t end = at;
  while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']') end++;
  return json.substr(at, end - at);
}

// The whole object or array of a member, braces and all.
inline std::string objectOf(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t found = json.find(needle);
  if (found == std::string::npos) return "";
  std::size_t at = found + needle.size();
  if (json[at] != '{' && json[at] != '[') return "";
  int depth = 0;
  bool in_string = false;
  for (std::size_t i = at; i < json.size(); i++) {
    const char c = json[i];
    if (in_string) {
      if (c == '\\') i++;
      else if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') in_string = true;
    else if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') {
      if (--depth == 0) return json.substr(at, i - at + 1);
    }
  }
  return "";
}

// The keys of a JSON object at depth one, in order: what a result's shape is compared by.
inline std::vector<std::string> topKeys(const std::string& json) {
  std::vector<std::string> keys;
  int depth = 0;
  bool in_string = false;
  std::string current;
  bool reading_key = false;
  bool expect_key = false;
  for (std::size_t i = 0; i < json.size(); i++) {
    const char c = json[i];
    if (in_string) {
      if (c == '\\') {
        if (reading_key) current += json[i + 1];
        i++;
        continue;
      }
      if (c == '"') {
        in_string = false;
        if (reading_key) {
          keys.push_back(current);
          reading_key = false;
        }
        continue;
      }
      if (reading_key) current += c;
      continue;
    }
    if (c == '"') {
      in_string = true;
      if (depth == 1 && expect_key) {
        reading_key = true;
        current.clear();
        expect_key = false;
      }
      continue;
    }
    if (c == '{' || c == '[') {
      depth++;
      if (depth == 1 && c == '{') expect_key = true;
      continue;
    }
    if (c == '}' || c == ']') {
      depth--;
      continue;
    }
    if (depth == 1 && c == ',') expect_key = true;
  }
  return keys;
}

// A wake's knowledge for an exchange made by hand (the wake cycle fills it at step 8).
inline interfaces::BackendKnowledge knowledgeFor(core::power::Band band, bool radio_known = true,
                                                  std::uint16_t battery_mv = 4100) {
  interfaces::BackendKnowledge k{};
  k.uplink.up = true;
  k.radio.known = radio_known;
  k.identity.known = radio_known;
  k.wake.band = static_cast<std::uint8_t>(band);
  k.wake.next_interval_s = core::power::minutesFor(band) * 60u;
  k.wake.radio = static_cast<std::uint8_t>(core::supervisor::Health::kHealthy);
  k.battery_known = battery_mv != 0;
  k.battery_mv = battery_mv;
  return k;
}

// The wake bench with the agent on it: a chip for the radio to run from, the agent's world, and
// the server configured. `repeater` is what stands behind the RAK (the mesh tests give the
// library's own; the firmware jobs' tests need none).
struct AgentBench : WakeBench {
  explicit AgentBench(mm::IMesh* repeater = nullptr, bool with_chip = true)
      : WakeBench([=](WakeWorld& w) {
          if (with_chip) w.withChip();
          w.withAgent(repeater);
        }) {}

  MeshMasterScript& server() { return world.agent->server; }
  AgentHalf& half() { return *walter->agent; }

  // One exchange by hand, as step 8 makes it, with `seconds` to the deadline. The link has a
  // session first, as step 2 would have given it one.
  interfaces::Exchanged exchange(const interfaces::BackendKnowledge& known, std::uint32_t seconds,
                                 interfaces::BackendAnswer* out) {
    if (!walter->link.session().up) (void)walter->link.hello();
    *out = interfaces::BackendAnswer{};
    return half().backend.exchange(known, walter->clock.nowMs() + static_cast<std::uint64_t>(seconds) * 1000u, out);
  }

  // The last result the server took for a job, or "".
  std::string lastResult(long long job_id) {
    const auto it = server().results.find(job_id);
    if (it == server().results.end() || it->second.empty()) return "";
    return it->second.back().body;
  }
  // The result's `data` object.
  std::string lastData(long long job_id) { return objectOf(lastResult(job_id), "data"); }
};

}  // namespace bench
