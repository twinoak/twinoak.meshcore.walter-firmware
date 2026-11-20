#include <agent/CliReplies.h>

#include <cstddef>
#include <cstring>

namespace mm {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

char fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// The prefixes MeshMaster.Contracts' CliReplies.HardErrorPrefixes holds, in its order.
const char* const kHardErrorPrefixes[] = {"error", "err", "(err", "??:", "unknown config", "unknown command"};

// The two commands whose reply `CommonCLI::handleRegionCmd` writes with
// `RegionMap::exportTo(reply, 160)`, which is the whole of the repeater's reply window: a prefix
// takes three bytes off the front of that window and a full map would then write past
// `onPeerDataRecv`'s `temp[166]`. `region` is the bare listing and `region def` is the one-line
// form of it; `region default`, `region home`, `region save` and the rest answer a sentence.
bool fillsTheReplyWindow(const char* command) {
  if (std::strcmp(command, "region") == 0) return true;
  return std::strncmp(command, "region def", 10) == 0 && (command[10] == '\0' || command[10] == ' ');
}

char hexDigit(uint8_t nibble) { return static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10)); }

}  // namespace

bool cliReplyIsConfirmed(const char* reply, const char* expect) {
  if (reply == nullptr) return false;
  if (expect == nullptr) expect = "OK";
  while (isSpace(*reply)) reply++;
  return std::strncmp(reply, expect, std::strlen(expect)) == 0;
}

bool cliReplyIsHardError(const char* reply) {
  if (reply == nullptr) return false;
  while (isSpace(*reply)) reply++;
  for (const char* prefix : kHardErrorPrefixes) {
    size_t i = 0;
    while (prefix[i] != '\0' && reply[i] != '\0' && fold(reply[i]) == prefix[i]) i++;
    if (prefix[i] == '\0') return true;
  }
  return false;
}

void cliPrefixFor(uint8_t n, char* out, size_t cap) {
  if (out == nullptr || cap <= kCliPrefixLen) {
    if (out != nullptr && cap > 0) out[0] = '\0';
    return;
  }
  out[0] = hexDigit(static_cast<uint8_t>(n >> 4));
  out[1] = hexDigit(static_cast<uint8_t>(n & 0x0F));
  out[2] = '|';
  out[3] = '\0';
}

bool cliLineTakesPrefix(const char* command) {
  if (command == nullptr) return false;
  // handleCommand's own condition, on the line as it would go out: `strlen(command) > 4` with
  // the prefix already on it, which is two characters of command or more.
  if (std::strlen(command) + kCliPrefixLen <= 4) return false;
  return !fillsTheReplyWindow(command);
}

const char* cliReplyBehindPrefix(const char* reply, const char* prefix) {
  if (reply == nullptr) return nullptr;
  if (prefix == nullptr || prefix[0] == '\0') return reply;
  if (std::strncmp(reply, prefix, kCliPrefixLen) != 0) return nullptr;
  return reply + kCliPrefixLen;
}

}  // namespace mm
