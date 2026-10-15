#include <agent/Sessions.h>

#include <cstdio>
#include <cstring>

namespace mm {

namespace {

bool expired(uint32_t nowMs, uint32_t loggedInAtMs) {
  return static_cast<int32_t>(nowMs - loggedInAtMs) >= static_cast<int32_t>(SessionStore::kLifetimeMs);
}

}  // namespace

int SessionStore::indexOf(const char* key) const {
  if (key == nullptr) return -1;
  for (size_t i = 0; i < kMaxSessions; i++) {
    if (used_[i] && std::strcmp(sessions_[i].key, key) == 0) return static_cast<int>(i);
  }
  return -1;
}

const RepeaterSession* SessionStore::get(const char* key, CredentialClass wanted, uint32_t nowMs) {
  const int i = indexOf(key);
  if (i < 0) return nullptr;
  RepeaterSession& s = sessions_[i];
  if (expired(nowMs, s.loggedInAtMs)) {
    used_[i] = false;
    return nullptr;
  }
  if (!s.satisfies(wanted)) return nullptr;
  if (s.credentialClass != wanted && !s.isAdmin()) return nullptr;
  return &s;
}

const RepeaterSession* SessionStore::put(const char* key, CredentialClass credentialClass, uint8_t permissions,
                                         uint32_t nowMs) {
  if (key == nullptr || key[0] == '\0') return nullptr;
  int i = indexOf(key);
  if (i < 0) {
    for (size_t k = 0; k < kMaxSessions; k++) {
      if (!used_[k] || expired(nowMs, sessions_[k].loggedInAtMs)) {
        i = static_cast<int>(k);
        break;
      }
    }
  }
  if (i < 0) i = 0;  // every slot is live: the oldest thing to do is take the first
  RepeaterSession& s = sessions_[i];
  std::snprintf(s.key, sizeof s.key, "%s", key);
  // A session is never held above its level: when what was granted falls short of the class the
  // login was for, it is kept at guest, which any accepted login satisfies.
  s.permissions = permissions;
  s.credentialClass = (permissions & kPermRoleMask) >= roleNeededFor(credentialClass) ? credentialClass
                                                                                      : CredentialClass::Guest;
  s.loggedInAtMs = nowMs;
  used_[i] = true;
  return &s;
}

void SessionStore::invalidate(const char* key) {
  const int i = indexOf(key);
  if (i >= 0) used_[i] = false;
}

void SessionStore::clear() {
  for (size_t i = 0; i < kMaxSessions; i++) used_[i] = false;
}

const RepeaterSession* SessionStore::slot(size_t i, uint32_t nowMs) const {
  if (i >= kMaxSessions || !used_[i] || expired(nowMs, sessions_[i].loggedInAtMs)) return nullptr;
  return &sessions_[i];
}

size_t SessionStore::live(uint32_t nowMs) const {
  size_t n = 0;
  for (size_t i = 0; i < kMaxSessions; i++) {
    if (used_[i] && !expired(nowMs, sessions_[i].loggedInAtMs)) n++;
  }
  return n;
}

}  // namespace mm
