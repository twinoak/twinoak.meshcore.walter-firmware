#include <agent/JobResults.h>

#include <agent/CliReplies.h>
#include <agent/Iso8601.h>
#include <agent/JsonWriter.h>

namespace mm {

int statusErrors(const StatusReading& s, ErrorCounter* out, int max) {
  int n = 0;
  if (n < max) out[n++] = {"eventsFull", s.errEvents};
  if (s.hasRecvErrors && n < max) out[n++] = {"recvErrors", static_cast<long long>(s.recvErrors)};
  if (s.hasRxAirtime && n < max) out[n++] = {"rxAirtimeS", static_cast<long long>(s.rxAirtimeS)};
  return n;
}

size_t writeStatusResult(const StatusReading& s, const ErrorCounter* errors, int errorCount, const char* firmware,
                         char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("batteryMv");
  w.integer(s.batteryMv);
  w.key("noiseFloorDbm");
  w.integer(s.noiseFloorDbm);
  w.key("lastRssi");
  w.integer(s.lastRssi);
  w.key("lastSnr");
  w.number(s.lastSnr);
  w.key("uptimeS");
  w.integer(s.uptimeS);
  w.key("txQueueLen");
  w.integer(s.txQueueLen);
  w.key("packets");
  w.beginObject();
  w.key("recv");
  w.integer(s.packetsRecv);
  w.key("sent");
  w.integer(s.packetsSent);
  w.key("recvFlood");
  w.integer(s.recvFlood);
  w.key("recvDirect");
  w.integer(s.recvDirect);
  w.key("sentFlood");
  w.integer(s.sentFlood);
  w.key("sentDirect");
  w.integer(s.sentDirect);
  w.endObject();
  w.key("airtimeS");
  w.integer(s.airtimeS);
  w.key("dups");
  w.beginObject();
  w.key("flood");
  w.integer(s.floodDups);
  w.key("direct");
  w.integer(s.directDups);
  w.endObject();
  if (errors != nullptr && errorCount > 0) {
    w.key("errors");
    w.beginObject();
    for (int i = 0; i < errorCount; i++) {
      w.key(errors[i].name);
      w.integer(errors[i].value);
    }
    w.endObject();
  }
  if (firmware != nullptr && firmware[0] != '\0') {
    w.key("firmware");
    w.string(firmware);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

size_t writeTelemetryResult(const LppReading* readings, int count, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("channels");
  w.beginArray();
  for (int i = 0; i < count; i++) {
    w.comma();
    w.beginObject();
    w.key("channel");
    w.integer(readings[i].channel);
    w.key("type");
    w.string(readings[i].type);
    w.key("value");
    w.number(readings[i].value);
    if (readings[i].unit != nullptr) {
      w.key("unit");
      w.string(readings[i].unit);
    }
    w.endObject();
  }
  w.endArray();
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

size_t writeNeighboursResult(const Neighbour* neighbours, int count, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("neighbours");
  w.beginArray();
  for (int i = 0; i < count; i++) {
    w.comma();
    w.beginObject();
    w.key("prefix");
    w.string(neighbours[i].prefix);
    w.key("lastHeardS");
    w.integer(neighbours[i].lastHeardS);
    w.key("snr");
    w.number(neighbours[i].snr);
    w.endObject();
  }
  w.endArray();
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

size_t writeTraceResult(const TraceHop* hops, int count, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("hops");
  w.beginArray();
  for (int i = 0; i < count; i++) {
    w.comma();
    w.beginObject();
    w.key("prefix");
    w.string(hops[i].prefix);
    if (hops[i].hasSnr) {
      w.key("snr");
      w.number(hops[i].snr);
    }
    w.endObject();
  }
  w.endArray();
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

size_t writeLoginBelowClassResult(const char* loggedInAs, const char* needed, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("loggedInAs");
  w.string(loggedInAs);
  w.key("needed");
  w.string(needed);
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

const char kLoginSilentAfterReachable[] = "silentAfterReachable";

size_t writeLoginSilentResult(const char* probeAnsweredAs, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("reason");
  w.string(kLoginSilentAfterReachable);
  if (probeAnsweredAs != nullptr) {
    w.key("probeAnsweredAs");
    w.string(probeAnsweredAs);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

const char kPathSourceLogin[] = "login";
const char kPathSourceDiscovery[] = "discovery";

size_t writeDiscoverResult(bool hasPath, const char* const* hops, int hopCount, const char* source, uint32_t learnedAt,
                           const char* loggedInAs, char* out, size_t cap) {
  JsonWriter w(out, cap);
  char when[24];
  w.beginObject();
  if (hasPath) {
    w.key("path");
    w.beginObject();
    w.key("hops");
    w.beginArray();
    for (int i = 0; i < hopCount; i++) {
      w.comma();
      w.string(hops[i]);
    }
    w.endArray();
    if (source != nullptr) {
      w.key("source");
      w.string(source);
    }
    if (learnedAt != 0) {
      w.key("learnedAt");
      formatUtc(learnedAt, when, sizeof when);
      w.string(when);
    }
    w.endObject();
  }
  if (loggedInAs != nullptr) {
    w.key("loggedInAs");
    w.string(loggedInAs);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

size_t writeAclSetResult(const char* publicKey, int level, bool confirmed, const char* reply, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("publicKey");
  w.string(publicKey);
  w.key("level");
  w.integer(level);
  w.key("confirmed");
  w.boolean(confirmed);
  if (reply != nullptr) {
    w.key("reply");
    w.string(reply);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

const char kPasswordSetAdminReplyRedacted[] = "password now: <set>";

size_t writeCliResult(const char* command, const char* reply, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("command");
  w.string(command);
  if (reply != nullptr) {
    w.key("reply");
    w.string(reply);
  }
  w.key("answered");
  w.boolean(reply != nullptr);
  w.key("isHardError");
  w.boolean(cliReplyIsHardError(reply));
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

size_t writeAdvertRequestResult(bool confirmed, const char* reply, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("confirmed");
  w.boolean(confirmed);
  if (reply != nullptr) {
    w.key("reply");
    w.string(reply);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

size_t writeClockSyncResult(uint32_t setTo, bool hasDriftS, long long driftS, bool confirmed, const char* reply, char* out,
                            size_t cap) {
  JsonWriter w(out, cap);
  char when[24];
  w.beginObject();
  w.key("setTo");
  formatUtc(setTo, when, sizeof when);
  w.string(when);
  if (hasDriftS) {
    w.key("driftS");
    w.integer(driftS);
  }
  w.key("confirmed");
  w.boolean(confirmed);
  if (reply != nullptr) {
    w.key("reply");
    w.string(reply);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

size_t writePasswordSetResult(const char* field, bool confirmed, bool sent, const char* reply, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("field");
  w.string(field != nullptr ? field : "");
  w.key("confirmed");
  w.boolean(confirmed);
  w.key("sent");
  w.boolean(sent);
  if (reply != nullptr) {
    w.key("reply");
    w.string(reply);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

size_t writeRebootResult(bool sent, const char* reply, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("sent");
  w.boolean(sent);
  if (reply != nullptr) {
    w.key("reply");
    w.string(reply);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

}  // namespace mm
