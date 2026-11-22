// Per-repeater login state with a lifetime (EXIT-NODES §5, §7).
//
// A session is reused within its lifetime, so the status and telemetry probes the server
// schedules in one slot and claims together cost **one** login; it is dropped when a request goes
// unanswered (the repeater may have rebooted and forgotten us) and when a job needs a higher
// class than the session has.
//
// **The cache never holds a session above its level** (task p3.5-41). A login is judged against
// the class it was for the moment the reply comes back: a session whose granted role falls short
// of the class that asked for it is stored at the class it can carry, and get() hands one out
// only when its role satisfies the class asked for, however the session was obtained. So a
// read-only session is reusable by a guest-class job and invisible to an admin-class one.
//
// Host-portable: millisecond counts in, four slots of static RAM, no allocation.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/IMesh.h>
#include <agent/JobEnvelopes.h>
#include <agent/ReplyCodec.h>

namespace mm {

struct RepeaterSession {
  char key[kPublicKeyHexLen + 1] = {0};
  CredentialClass credentialClass = CredentialClass::None;
  uint8_t permissions = 0;
  uint32_t loggedInAtMs = 0;

  // **The path the claim of the job that opened this session carried** (task p1.21-71), so that a
  // job which reuses the session can tell whether the way back the repeater holds is still the
  // way its sends are about to go. The repeater was told that way back before this session's own
  // login; a claim that now names another path means the repeater would answer along hops this
  // job is not using, so the way back is told again before the job's first send.
  //
  // `hasPath` false is a session opened by a flood - the server knew no path - and `pathLen` 0
  // with `hasPath` true is a direct pair, whose way back is the empty path. `pathLen` is the
  // encoded `path_len` byte MeshCore uses, `(hashSize - 1) << 6 | hopCount`, so a path of the
  // same hops at another width is a different path, which it is: the hops a packet is addressed
  // to are the bytes, not the nodes.
  bool hasPath = false;
  uint8_t pathLen = 0;
  uint8_t path[kMeshPathCap] = {};

  // Were this session's hops the same as these? The comparison the reuse above is decided by.
  bool samePathAs(const uint8_t* bytes, uint8_t len, bool has) const {
    if (has != hasPath) return false;
    if (!has) return true;
    if (len != pathLen) return false;
    const size_t width = static_cast<size_t>(len & 63) * static_cast<size_t>((len >> 6) + 1);
    if (width == 0) return true;
    if (bytes == nullptr) return false;
    for (size_t i = 0; i < width && i < sizeof path; i++) {
      if (path[i] != bytes[i]) return false;
    }
    return true;
  }

  uint8_t role() const { return permissions & kPermRoleMask; }
  bool isAdmin() const { return role() == kPermAdmin; }
  const char* tier() const { return permissionName(permissions); }
  bool satisfies(CredentialClass wanted) const { return role() >= roleNeededFor(wanted); }
};

class SessionStore {
 public:
  // Four repeaters at once: a claim takes two jobs, and a board paces one transaction at a time,
  // so this is already more than the loop can have in flight.
  static constexpr size_t kMaxSessions = 4;

  // The .NET agent's default (AgentOptions.SessionLifetimeSeconds): ten minutes, which is inside
  // the firmware's own transient login table and short enough that a repeater that rebooted is
  // found out by the next job rather than by the one after it.
  static constexpr uint32_t kLifetimeMs = 600000;

  // The live session for a repeater at this class, or nullptr: absent, expired, logged in at a
  // class that cannot carry this one, or granted less than this class needs. An admin session
  // serves every class; an acl or a guest one serves its own alone.
  const RepeaterSession* get(const char* key, CredentialClass wanted, uint32_t nowMs);

  // Record a login. When the granted role falls short of the class the login was for, the
  // session is stored at the class it can carry - guest, which any accepted login satisfies -
  // rather than at the class that asked.
  //
  // `pathBytes`/`pathLen`/`hasPath` are the path the job that made this login was claimed with
  // (task p1.21-71), kept so that a job reusing the session can see that the claim's path has
  // changed. They have defaults - *no path* - so a caller that does not care passes nothing and
  // every job of such a caller is then a job whose path has not changed.
  const RepeaterSession* put(const char* key, CredentialClass credentialClass, uint8_t permissions, uint32_t nowMs,
                             const uint8_t* pathBytes = nullptr, uint8_t pathLen = 0, bool hasPath = false);

  void invalidate(const char* key);
  void clear();

  size_t live(uint32_t nowMs) const;

  // The slots, for the console's `sessions`: the session in slot `i` when it is live, else
  // nullptr. Nothing in a session is a secret - a key, a class, a role byte and a time.
  const RepeaterSession* slot(size_t i, uint32_t nowMs) const;

 private:
  int indexOf(const char* key) const;

  RepeaterSession sessions_[kMaxSessions];
  bool used_[kMaxSessions] = {};
};

}  // namespace mm
