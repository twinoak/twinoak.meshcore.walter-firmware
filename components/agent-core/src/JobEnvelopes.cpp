#include <agent/JobEnvelopes.h>

#include <agent/Json.h>

#include <cstdio>
#include <cstring>
#include <new>

#include <agent/Iso8601.h>
#include <agent/JsonWriter.h>

namespace mm {

namespace {

void say(char* err, size_t cap, const char* text) {
  if (err == nullptr || cap == 0) return;
  std::snprintf(err, cap, "%s", text);
}

void copyText(const char* from, char* to, size_t cap) {
  if (from == nullptr) {
    to[0] = '\0';
    return;
  }
  std::snprintf(to, cap, "%s", from);
}

}  // namespace

const char* const kJobTypes[kJobTypeCount] = {"status",       "telemetry", "neighbours", "config.read",    "config.apply",
                                              "cli",          "trace",     "discover",   "advert.request", "clock.sync",
                                              "password.set", "acl.set",   "reboot"};

const char* passwordFieldName(PasswordField f) {
  switch (f) {
    case PasswordField::Admin: return "adminPassword";
    case PasswordField::Guest: return "guestPassword";
    case PasswordField::None:
    default: return "";
  }
}

bool isKnownJobType(const char* type) {
  if (type == nullptr) return false;
  for (int i = 0; i < kJobTypeCount; i++) {
    if (std::strcmp(type, kJobTypes[i]) == 0) return true;
  }
  return false;
}

const char* outcomeName(Outcome o) {
  switch (o) {
    case Outcome::Ok: return "ok";
    case Outcome::Unreachable: return "unreachable";
    case Outcome::Unauthorized: return "unauthorized";
    case Outcome::Unsupported: return "unsupported";
    case Outcome::Timeout: return "timeout";
    case Outcome::Failed:
    default: return "failed";
  }
}

namespace {

// JobTypes.CredentialClasses on the server, as a predicate.
bool typeAllowsClass(const char* type, CredentialClass c) {
  if (std::strcmp(type, "trace") == 0) return c == CredentialClass::None;
  // The three any login is answered. `neighbours` joined them in task p1.2-10, read from the
  // firmware this project pins: examples/simple_repeater/MyMesh.cpp at d9296435 guards
  // REQ_TYPE_GET_ACCESS_LIST with `sender->isAdmin()` (line 262) and REQ_TYPE_GET_NEIGHBOURS
  // with nothing at all (line 276), so a guest login gets the neighbour list. The server sends
  // the job at class `guest` for a guest-only pair since its task p4.7-94
  // (EXIT-NODES §7's table), and this board answered `unsupported` where the Pi agent answered
  // the list. `guest` with a grant on the repeater is still refused, below.
  if (std::strcmp(type, "status") == 0 || std::strcmp(type, "telemetry") == 0 ||
      std::strcmp(type, "neighbours") == 0) {
    return c == CredentialClass::Guest || c == CredentialClass::Acl || c == CredentialClass::Admin;
  }
  // `discover` at the guest door. The server sends the pairing step of a guest pair at class
  // `guest` since its task p4.7-94 (`JobTypes.CredentialClasses`: `Discover => [Guest, Admin]`),
  // because a guest login teaches the path like any other and a repeater whose owner never set a
  // guest password answers a blank one *at guest* (DECISIONS A-4, task p4.5-81) - and this board
  // answered `unsupported` where the .NET agent logged in, which ended meshmaster.dk's
  // association 9 `failed` on 11 October 2026 before anything went on the air. `admin` is still
  // the class a discover nobody chose one for gets (`LowestCredentialClass`), and `acl` and
  // `none` stay refused: at either the login carries no password, which is the ACL login, and a
  // repeater that has not granted our key an entry answers that with silence (task p3.5-40).
  if (std::strcmp(type, "discover") == 0) {
    return c == CredentialClass::Guest || c == CredentialClass::Admin;
  }
  // acl.set, config.read, and everything that writes: the admin password, and nothing less.
  return c == CredentialClass::Admin;
}

}  // namespace

bool canRun(const ClaimedJob& job, char* why, size_t whyCap) {
  if (!isKnownJobType(job.type)) {
    if (why != nullptr && whyCap > 0) {
      std::snprintf(why, whyCap, "no handler for job type '%s' in this firmware", job.type);
    }
    return false;
  }
  if (!typeAllowsClass(job.type, job.credentialClass)) {
    if (why != nullptr && whyCap > 0) {
      std::snprintf(why, whyCap, "a %s job cannot run at class %s on this firmware", job.type,
                    credentialClassName(job.credentialClass));
    }
    return false;
  }
  if (job.credentialClass == CredentialClass::Guest && job.hasAclLevel) {
    // The claim refuses to hand the guest password to an exit node that holds a grant on the
    // repeater (task p4.1-60), and so does this board when a claim carries both anyway: a guest
    // login from this key would rewrite the granted entry to guest.
    if (why != nullptr && whyCap > 0) {
      std::snprintf(why, whyCap,
                    "this exit node holds a grant on this repeater and will not log in with the guest password, "
                    "which would demote the entry (task p4.1-60)");
    }
    return false;
  }
  if (job.repeaterKey[0] == '\0') {
    if (why != nullptr && whyCap > 0) std::snprintf(why, whyCap, "the repeater has no public key; nothing to address");
    return false;
  }
  if (why != nullptr && whyCap > 0) why[0] = '\0';
  return true;
}

bool readsFirmwareVersion(const ClaimedJob& job) {
  if (std::strcmp(job.type, "status") != 0) return false;
  return job.credentialClass == CredentialClass::Acl && job.statusVersion;
}

bool sharesLogin(const ClaimedJob& a, const ClaimedJob& b) {
  if (a.id == b.id) return false;
  for (int i = 0; i < a.groupCount; i++) {
    if (a.groupWith[i] == b.id) return true;
  }
  for (int i = 0; i < b.groupCount; i++) {
    if (b.groupWith[i] == a.id) return true;
  }
  return false;
}

bool leaseLapsed(uint32_t leaseUntil, bool hasTime, uint32_t nowEpoch) {
  if (leaseUntil == 0 || !hasTime) return false;
  return nowEpoch >= leaseUntil;
}

size_t writeClaimRequest(int max, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("max");
  w.integer(max);
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

bool parseClaimResponse(char* json, size_t len, ClaimedJob* jobs, int max, int& count, uint32_t& serverTime, char* err,
                        size_t errCap) {
  count = 0;
  serverTime = 0;
  // Two jobs with a path, an idempotency key and a label is about a kilobyte; this is the one
  // reply of the protocol that is read rather than written here. The input is a `char*`, which
  // puts ArduinoJson in zero-copy mode: every string the document holds points into `json`
  // itself and the pool keeps no copy, so a password in the claim is in the caller's buffer and
  // in the job's credential and nowhere else once this returns.
  JsonDocument doc;
  const DeserializationError e = deserializeJson(doc, json, len);
  if (e) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "not JSON: %s", e.c_str());
    say(err, errCap, msg);
    return false;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    say(err, errCap, "not a JSON object");
    return false;
  }
  const char* when = root["serverTime"];
  if (when == nullptr || !parseUtc(when, serverTime)) {
    say(err, errCap, "missing or malformed: serverTime");
    return false;
  }
  JsonArrayConst list = root["jobs"];
  if (list.isNull()) {
    say(err, errCap, "missing: jobs");
    return false;
  }

  for (JsonObjectConst j : list) {
    if (count >= max) break;  // the server was asked for `max`; anything past it is not taken
    // Built in place, in the caller's slot: a job holds a password, and a local copy of it on
    // this stack frame would be one more thing to wipe.
    ClaimedJob& job = jobs[count];
    job.credential.wipe();
    new (&job) ClaimedJob();

    JsonVariantConst id = j["id"];
    if (!id.is<long long>()) {
      say(err, errCap, "a job has no id");
      return false;
    }
    job.id = id.as<long long>();

    const char* type = j["type"];
    if (type == nullptr) {
      say(err, errCap, "a job has no type");
      return false;
    }
    copyText(type, job.type, sizeof job.type);

    JsonObjectConst repeater = j["repeater"];
    if (repeater.isNull()) {
      say(err, errCap, "a job has no repeater");
      return false;
    }
    job.repeaterId = repeater["id"] | 0LL;
    copyText(repeater["label"], job.repeaterLabel, sizeof job.repeaterLabel);
    copyText(repeater["publicKey"], job.repeaterKey, sizeof job.repeaterKey);

    if (!parseCredentialClass(j["credentialClass"] | "none", job.credentialClass)) {
      say(err, errCap, "a job has a credential class this firmware does not know");
      return false;
    }

    const char* lease = j["leaseUntil"];
    if (lease == nullptr || !parseUtc(lease, job.leaseUntil)) {
      say(err, errCap, "a job has no leaseUntil");
      return false;
    }

    JsonObjectConst timeouts = j["timeouts"];
    if (!timeouts.isNull()) {
      job.perHopMs = timeouts["perHopMs"] | job.perHopMs;
      job.loginMs = timeouts["loginMs"] | job.loginMs;
    }

    JsonObjectConst credential = j["credential"];
    if (!credential.isNull()) {
      JsonVariantConst acl = credential["acl"];
      if (acl.is<int>()) {
        job.hasAclLevel = true;
        job.aclLevel = acl.as<int>();
      } else if (acl.is<bool>() && acl.as<bool>()) {
        // The pre-task-60 shape, `{"acl": true}`: a grant whose level the server did not say.
        job.hasAclLevel = true;
        job.aclLevel = kPermReadOnly;
      }
      // The passwords: the material of the job's class, straight from the buffer into the
      // job's fixed credential (Credentials.h), and the buffer is the caller's to wipe. The new
      // password of a password.set travels here too, never in the payload (task p4-55).
      job.credential.setAdminPassword(credential["adminPassword"].as<const char*>());
      job.credential.setGuestPassword(credential["guestPassword"].as<const char*>());
      job.credential.setNewPassword(credential["newPassword"].as<const char*>());
      copyText(credential["candidate"].as<const char*>(), job.credentialCandidate, sizeof job.credentialCandidate);
    }

    // The payload, for the two types whose payload says something. Anything else - `{}` for
    // the reads, a record this firmware does not know - is read by nobody.
    JsonObjectConst payload = j["payload"];
    if (!payload.isNull() && std::strcmp(job.type, "config.read") == 0) {
      JsonArrayConst keys = payload["keys"];
      if (!keys.isNull()) {
        for (JsonVariantConst k : keys) {
          const char* text = k.as<const char*>();
          if (text == nullptr || text[0] == '\0') continue;
          if (job.configKeyCount >= kMaxConfigKeys) {
            say(err, errCap, "a config.read asks for more keys than this firmware can read in one job");
            return false;
          }
          copyText(text, job.configKeys[job.configKeyCount], sizeof job.configKeys[0]);
          job.configKeyCount++;
        }
        job.hasConfigKeys = job.configKeyCount > 0;
      }
    }
    if (!payload.isNull() && std::strcmp(job.type, "acl.set") == 0) {
      const char* key = payload["publicKey"].as<const char*>();
      JsonVariantConst level = payload["level"];
      if (key != nullptr && key[0] != '\0' && level.is<int>()) {
        job.hasAclSet = true;
        copyText(key, job.aclSetKey, sizeof job.aclSetKey);
        for (char* p = job.aclSetKey; *p != '\0'; p++) {
          if (*p >= 'A' && *p <= 'F') *p = static_cast<char>(*p - 'A' + 'a');
        }
        job.aclSetLevel = level.as<int>();
      }
    }

    // The writes' payloads (task 05). A plan this firmware cannot hold is refused inside the
    // plan, by name, and the job is reported `failed` at no transaction rather than the whole
    // claim being refused: the other job of the claim is not this one's business.
    job.apply.clear();
    if (std::strcmp(job.type, "config.apply") == 0) {
      JsonArrayConst commands = payload.isNull() ? JsonArrayConst() : payload["commands"].as<JsonArrayConst>();
      if (commands.isNull() || commands.size() == 0) {
        job.apply.refuse("config.apply carries no plan: nothing to send");
      } else {
        for (JsonObjectConst c : commands) {
          if (!job.apply.addCommand(c["key"].as<const char*>(), c["command"].as<const char*>())) break;
          JsonArrayConst lines = c["lines"];
          if (lines.isNull()) continue;
          bool ok = true;
          for (JsonObjectConst l : lines) {
            JsonVariantConst expect = l["expect"];
            const char* expectText = expect.is<const char*>() ? expect.as<const char*>() : nullptr;
            if (!job.apply.addLine(l["line"].as<const char*>(), expectText, l["escape"] | false)) {
              ok = false;
              break;
            }
          }
          if (!ok) break;
        }
      }
      if (!payload.isNull()) copyText(payload["planHash"].as<const char*>(), job.apply.planHash, sizeof job.apply.planHash);
    }
    if (!payload.isNull() && std::strcmp(job.type, "cli") == 0) {
      const char* command = payload["command"].as<const char*>();
      if (command != nullptr) {
        while (*command == ' ' || *command == '\t') command++;
        size_t n = std::strlen(command);
        while (n > 0 && (command[n - 1] == ' ' || command[n - 1] == '\t' || command[n - 1] == '\r' || command[n - 1] == '\n')) n--;
        if (n > 0) {
          job.hasCliCommand = true;
          job.cliCommandTooLong = n > kMaxCliCommandLen;
          std::snprintf(job.cliCommand, sizeof job.cliCommand, "%.*s",
                        static_cast<int>(n > kMaxCliCommandLen ? kMaxCliCommandLen : n), command);
        }
      }
    }
    if (!payload.isNull() && std::strcmp(job.type, "advert.request") == 0) {
      job.advertFlood = payload["flood"] | true;
    }
    if (!payload.isNull() && std::strcmp(job.type, "status") == 0) {
      // A payload that says nothing is an older server, and that still means "read the version":
      // tests/fixtures/contracts/StatusPayload.json is `{}` for exactly that reason.
      job.statusVersion = payload["version"] | true;
    }
    if (!payload.isNull() && std::strcmp(job.type, "password.set") == 0) {
      const char* field = payload["field"].as<const char*>();
      if (field != nullptr && std::strcmp(field, "adminPassword") == 0) job.passwordField = PasswordField::Admin;
      if (field != nullptr && std::strcmp(field, "guestPassword") == 0) job.passwordField = PasswordField::Guest;
    }

    JsonObjectConst path = j["path"];
    if (!path.isNull()) {
      JsonArrayConst hops = path["hops"];
      if (!hops.isNull()) {
        job.hasPath = true;
        for (JsonVariantConst hop : hops) {
          const char* text = hop.as<const char*>();
          if (text == nullptr || job.pathCount >= kMaxClaimPathHops) continue;
          copyText(text, job.pathHops[job.pathCount], sizeof job.pathHops[0]);
          job.pathCount++;
        }
      }
    }

    copyText(j["idempotencyKey"], job.idempotencyKey, sizeof job.idempotencyKey);

    JsonArrayConst group = j["groupWith"];
    if (!group.isNull()) {
      for (JsonVariantConst other : group) {
        if (job.groupCount >= kMaxGroupWith) break;
        job.groupWith[job.groupCount++] = other.as<long long>();
      }
    }

    count++;
  }
  return true;
}

size_t writeJobResult(const JobReport& r, char* out, size_t cap) {
  JsonWriter w(out, cap);
  char when[24];
  w.beginObject();
  w.key("clientId");
  w.string(r.clientId);
  w.key("outcome");
  w.string(outcomeName(r.outcome));
  w.key("startedAt");
  formatUtc(r.startedAt, when, sizeof when);
  w.string(when);
  w.key("finishedAt");
  formatUtc(r.finishedAt, when, sizeof when);
  w.string(when);
  if (r.hasPathUsed) {
    w.key("pathUsed");
    w.beginArray();
    for (int i = 0; i < r.pathCount; i++) {
      w.comma();
      w.string(r.pathUsed[i]);
    }
    w.endArray();
  }
  if (r.hasHops) {
    w.key("hops");
    w.integer(r.hops);
  }
  if (r.hasRttMs) {
    w.key("rttMs");
    w.integer(r.rttMs);
  }
  w.key("transactions");
  w.integer(r.transactions);
  if (r.data != nullptr && r.data[0] != '\0') {
    w.key("data");
    w.raw(r.data);
  }
  if (r.log != nullptr) {
    // The **last** 4 000 characters, as the .NET runner bounds it: what a job did last is what
    // a reader of the job's page wants, and a job that went wrong says so at the end.
    w.key("log");
    const size_t n = std::strlen(r.log);
    w.string(n > kMaxJobLogChars ? r.log + (n - kMaxJobLogChars) : r.log, kMaxJobLogChars);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

// ---------------------------------------------------------------------------------------------
// extend and progress (task p1.5-15)

namespace {

// The `{jobId, <when>}` answer both calls give: the id as a number, the time as the server
// writes it, kept as sent and as parsed.
bool parseJobStamp(const char* json, size_t len, const char* whenKey, long long& jobId, char* when, size_t whenCap,
                   uint32_t& whenUnix, char* err, size_t errCap) {
  JsonDocument doc;
  const DeserializationError e = deserializeJson(doc, json, len);
  if (e) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "not JSON: %s", e.c_str());
    say(err, errCap, msg);
    return false;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) {
    say(err, errCap, "not a JSON object");
    return false;
  }
  JsonVariantConst id = root["jobId"];
  if (id.isNull() || !id.is<long long>()) {
    say(err, errCap, "missing or not a number: jobId");
    return false;
  }
  const char* text = root[whenKey];
  uint32_t epoch = 0;
  if (text == nullptr || !parseUtc(text, epoch)) {
    char msg[64];
    std::snprintf(msg, sizeof msg, "missing or malformed: %s", whenKey);
    say(err, errCap, msg);
    return false;
  }
  jobId = id.as<long long>();
  std::snprintf(when, whenCap, "%s", text);
  whenUnix = epoch;
  return true;
}

}  // namespace

size_t writeExtendRequest(int seconds, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("seconds");
  w.integer(seconds);
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

bool parseExtendReply(const char* json, size_t len, ExtendReply& out, char* err, size_t errCap) {
  ExtendReply r;
  if (!parseJobStamp(json, len, "leaseUntil", r.jobId, r.leaseUntil, sizeof r.leaseUntil, r.leaseUntilUnix, err,
                     errCap)) {
    return false;
  }
  out = r;
  return true;
}

size_t writeProgressReport(const char* dataJson, const char* note, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  if (dataJson != nullptr) {
    w.key("data");
    w.raw(dataJson);
  }
  if (note != nullptr) {
    w.key("note");
    w.string(note);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

bool parseProgressReply(const char* json, size_t len, ProgressReply& out, char* err, size_t errCap) {
  ProgressReply r;
  if (!parseJobStamp(json, len, "recordedAt", r.jobId, r.recordedAt, sizeof r.recordedAt, r.recordedAtUnix, err,
                     errCap)) {
    return false;
  }
  out = r;
  return true;
}

}  // namespace mm
