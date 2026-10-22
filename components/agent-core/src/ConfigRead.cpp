#include <agent/ConfigRead.h>

#include <cstdio>
#include <cstring>

#include <agent/JsonWriter.h>

namespace mm {

// `ConfigReadPayload.DefaultKeys`, in its order and complete: `agc.reset.interval` is the
// fourteenth, before `regions` and `acl`, which is where CONFIGURATION §2's own paragraph on it
// puts it ("it is on the default read list as the fourteenth key"). It is not a fixture - the
// contract fixtures carry a `ConfigReadPayload` with a three-key list and not the default - so
// `test_config_read` pins this against the literal and names the file it came from.
const char* const kDefaultConfigKeys[kDefaultConfigKeyCount] = {
    "name",     "lat",  "lon",           "advert.interval", "flood.advert.interval", "repeat",  "dutycycle", "flood.max",
    "flood.max.unscoped", "tx", "path.hash.mode", "multi.acks", "allow.read.only", "agc.reset.interval", "regions",
    "acl"};

const char* const kConfigReadExtras[kConfigReadExtraCount] = {"ver", "board"};

const char kAclKey[] = "acl";
const char kRegionsKey[] = "regions";

const char kRegionListCommand[] = "region";
const char kRegionDefaultCommand[] = "region default";
const char kRegionTruncatedLine[] =
    "regions: the listing filled the firmware's 160-character reply buffer, so it is not the whole map and no "
    "region hierarchy is stored from this read (RegionMap::exportTo)";

const char kAclFullLine[] = "acl: 23 entries, the firmware's reply is full and may be truncated";
const char kAclSilentLine[] =
    "acl: no reply \xE2\x80\x94 an ACL of 24 entries or more, or a firmware without the request, is not answered "
    "(spec/EXIT-NODES.md \xC2\xA7"
    "4.3)";
const char kAclUnreadableLine[] = "acl: the reply's bytes are not a list of 7-byte entries this agent can read";

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

const char kDefaultIs[] = "default scope is ";
const char kNoDefault[] = "<null>";

}  // namespace

int configReadTransactions(const char* const* keys, int count) {
  const char* const* list = keys;
  int n = count;
  if (list == nullptr || n <= 0) {
    list = kDefaultConfigKeys;
    n = kDefaultConfigKeyCount;
  }
  int regions = 0;
  for (int i = 0; i < n; i++) {
    if (list[i] != nullptr && std::strcmp(list[i], kRegionsKey) == 0) regions = 1;
  }
  return n + kConfigReadExtraCount + regions;
}

void ConfigSnapshot::clear() {
  count = 0;
  for (int i = 0; i < kMaxConfigKeys; i++) values[i] = ConfigValue();
  hasAcl = false;
  acl[0] = 0;
  rawCount = 0;
  rawDropped = 0;
  for (int i = 0; i < kConfigRawSlots; i++) raw[i][0] = 0;
  firmware[0] = 0;
  board[0] = 0;
  hasAclRestored = false;
  aclRestored = false;
}

bool ConfigSnapshot::put(const char* key, const char* value) {
  if (key == nullptr || value == nullptr) return false;
  for (int i = 0; i < count; i++) {
    if (std::strcmp(values[i].key, key) == 0) {
      std::snprintf(values[i].value, sizeof values[i].value, "%s", value);
      return true;
    }
  }
  if (count >= kMaxConfigKeys) return false;
  std::snprintf(values[count].key, sizeof values[count].key, "%s", key);
  std::snprintf(values[count].value, sizeof values[count].value, "%s", value);
  count++;
  return true;
}

bool ConfigSnapshot::addRaw(const char* line) {
  if (line == nullptr) return false;
  if (rawCount < kMaxConfigRawLines) {
    std::snprintf(raw[rawCount], sizeof raw[rawCount], "%s", line);
    rawCount++;
    return true;
  }
  // Full. The line itself is gone - there is nowhere to put 200 more characters - but the fact
  // of it is not: the slot after the twelve counts what the list is missing, so the server
  // reads a short list as a short list rather than as everything the repeater said.
  rawDropped++;
  std::snprintf(raw[kMaxConfigRawLines], sizeof raw[0], "raw: %d line(s) the repeater said did not fit this snapshot",
                rawDropped);
  rawCount = kConfigRawSlots;
  return false;
}

namespace {

// One attempt at the result, with the first `values` values and the first `rawLines` raw lines.
// When either is short of what the snapshot holds, one more raw line says so. 0 when it did not
// fit, as the public writer's contract is.
size_t writeResultAttempt(const ConfigSnapshot& s, int values, int rawLines, char* out, size_t cap) {
  JsonWriter w(out, cap);
  w.beginObject();
  w.key("values");
  w.beginObject();
  for (int i = 0; i < values; i++) {
    w.key(s.values[i].key);
    w.string(s.values[i].value);
  }
  if (s.hasAcl) {
    w.key(kAclKey);
    w.string(s.acl);
  }
  w.endObject();
  w.key("raw");
  w.beginArray();
  for (int i = 0; i < rawLines; i++) {
    w.comma();
    w.string(s.raw[i]);
  }
  if (values < s.count || rawLines < s.rawCount) {
    char cut[kMaxConfigRawLen + 1];
    std::snprintf(cut, sizeof cut, "result: %d value(s) and %d raw line(s) left out, it did not fit %lu bytes",
                  s.count - values, s.rawCount - rawLines, static_cast<unsigned long>(cap));
    w.comma();
    w.string(cut);
  }
  w.endArray();
  if (s.firmware[0] != 0) {
    w.key("firmware");
    w.string(s.firmware);
  }
  if (s.board[0] != 0) {
    w.key("board");
    w.string(s.board);
  }
  if (s.hasAclRestored) {
    w.key("aclRestored");
    w.boolean(s.aclRestored);
  }
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

}  // namespace

size_t writeConfigReadResult(const ConfigSnapshot& s, char* out, size_t cap) {
  // The whole thing first: every snapshot a real repeater makes fits, and that attempt is the
  // one the contract fixture is compared byte for byte against. Then less of it - the raw lines,
  // which are what the repeater could not parse, and only then the values, which are what it
  // could - until it fits. The loops are bounded by the snapshot's own counts and the last of
  // them writes no values and no raw lines at all, which is inside kMinConfigReadResultCap.
  for (int lines = s.rawCount; lines >= 0; lines--) {
    const size_t n = writeResultAttempt(s, s.count, lines, out, cap);
    if (n > 0) return n;
  }
  for (int values = s.count - 1; values >= 0; values--) {
    const size_t n = writeResultAttempt(s, values, 0, out, cap);
    if (n > 0) return n;
  }
  return 0;
}

const char* cliReplyValue(const char* reply) {
  if (reply == nullptr) return nullptr;
  while (isSpace(*reply)) reply++;
  if (reply[0] == '>' && reply[1] == ' ') return reply + 2;
  return nullptr;
}

char* trimInPlace(char* text) {
  if (text == nullptr) return text;
  char* start = text;
  while (isSpace(*start)) start++;
  size_t n = std::strlen(start);
  while (n > 0 && isSpace(start[n - 1])) start[--n] = 0;
  if (start != text) std::memmove(text, start, n + 1);
  return text;
}

bool regionListingIsTruncated(const char* listing) {
  if (listing == nullptr) return false;
  const size_t n = std::strlen(listing);
  return n >= kRegionReplyLimit - 1 || n == 0 || listing[n - 1] != '\n';
}

bool regionDefaultOf(const char* reply, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = 0;
  if (reply == nullptr) return false;
  while (isSpace(*reply)) reply++;
  const size_t prefix = sizeof kDefaultIs - 1;
  if (std::strncmp(reply, kDefaultIs, prefix) != 0) return false;
  const char* name = reply + prefix;
  while (isSpace(*name)) name++;
  size_t n = std::strlen(name);
  while (n > 0 && isSpace(name[n - 1])) n--;
  // "default scope is " is a prefix of "default scope is now ", which the *setting* command
  // answers: reading one as the other would make the default scope "now dk".
  if (n == 0 || std::strncmp(name, "now ", 4) == 0) return false;
  if (n + 1 > cap) n = cap - 1;
  std::memcpy(out, name, n);
  out[n] = 0;
  return true;
}

bool regionValue(const char* listing, const char* defaultReply, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = 0;
  if (listing == nullptr || regionListingIsTruncated(listing)) return false;
  size_t n = std::strlen(listing);
  while (n > 0 && (listing[n - 1] == '\n' || listing[n - 1] == '\r')) n--;
  if (n == 0) return false;
  char name[kMaxConfigKeyLen + 1];
  const char* scope = regionDefaultOf(defaultReply, name, sizeof name) ? name : kNoDefault;
  const int written = std::snprintf(out, cap, "%.*s\ndefault %s", static_cast<int>(n), listing, scope);
  return written > 0 && static_cast<size_t>(written) < cap;
}

size_t accessListTokens(const AccessListEntry* entries, int count, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  size_t n = 0;
  out[0] = 0;
  for (int i = 0; i < count; i++) {
    const int written = std::snprintf(out + n, cap - n, "%s%s:%u", i > 0 ? " " : "", entries[i].prefix,
                                      static_cast<unsigned>(entries[i].role()));
    if (written < 0 || static_cast<size_t>(written) >= cap - n) {
      out[n] = 0;
      break;
    }
    n += static_cast<size_t>(written);
  }
  return n;
}

}  // namespace mm
