#include <agent/CliReplies.h>

#include <cstddef>
#include <cstring>

namespace mm {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

char fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// The prefixes MeshMaster.Contracts' CliReplies.HardErrorPrefixes holds, in its order.
const char* const kHardErrorPrefixes[] = {"error", "err", "(err", "??:", "unknown config", "unknown command"};

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

}  // namespace mm
