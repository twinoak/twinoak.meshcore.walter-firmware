#include <agent/LoginSequence.h>

#include <cstdio>
#include <cstring>

#include <agent/ReplyCodec.h>

namespace mm {

const char kProbeAnsweredRefused[] = "refused";

namespace {

void note(char* out, size_t cap, const char* text) {
  if (out == nullptr || cap == 0) return;
  std::snprintf(out, cap, "%s", text);
}

void noteCut(char* out, size_t cap, const char* which) {
  if (out == nullptr || cap == 0) return;
  std::snprintf(out, cap, "the %s password is longer than %u bytes; the first %u are sent, as a companion sends them",
                which, static_cast<unsigned>(kMaxPasswordLen), static_cast<unsigned>(kMaxPasswordLen));
}

}  // namespace

const char* loginMaterialFor(const ClaimedJob& job, char* out, size_t cap) {
  if (out != nullptr && cap > 0) out[0] = '\0';
  switch (job.credentialClass) {
    case CredentialClass::Admin:
      if (!job.credential.hasAdminPassword) {
        note(out, cap, "the claim carried no admin material; nothing to log in with");
        return nullptr;
      }
      if (job.credential.adminPasswordCut) noteCut(out, cap, "admin");
      return job.credential.adminPassword;
    case CredentialClass::Guest:
      if (!job.credential.hasGuestPassword) {
        note(out, cap,
             "no guest password in the claim: the blank login, which a repeater with no guest password answers at "
             "guest");
        return "";
      }
      if (job.credential.guestPasswordCut) noteCut(out, cap, "guest");
      return job.credential.guestPassword;
    default:
      // Empty material at class none or acl is not missing material either: it is the ACL login,
      // which offers the repeater our own key and nothing else.
      return "";
  }
}

bool probesBetweenTries(const char* credentialCandidate, const char* material) {
  return credentialCandidate != nullptr && std::strcmp(credentialCandidate, "new") == 0 && material != nullptr &&
         material[0] != '\0';
}

LoginVerdict loginSequence(LoginExchange& exchange, bool probes) {
  LoginVerdict verdict;
  bool reachable = false;
  for (int attempt = 1; attempt <= 2; attempt++) {
    switch (exchange.login(attempt)) {
      case LoginTry::Ok:
        verdict.outcome = Outcome::Ok;
        return verdict;
      case LoginTry::Refused:
        verdict.outcome = Outcome::Unauthorized;
        return verdict;
      case LoginTry::Failed:
        verdict.outcome = Outcome::Failed;
        return verdict;
      case LoginTry::Timeout:
        verdict.outcome = Outcome::Timeout;
        return verdict;
      case LoginTry::NoReply:
        break;  // once more
    }
    if (probes && attempt == 1) {
      // The blank login, between the first silence and the second try: answered at any level,
      // the repeater is listening, and the second try is judged in that light.
      uint8_t role = 0;
      switch (exchange.probe(role)) {
        case ProbeAnswer::Answered:
          reachable = true;
          verdict.probeAnsweredRole = static_cast<uint8_t>(role & kPermRoleMask);
          break;
        case ProbeAnswer::Refused:
          reachable = true;
          verdict.probeRefused = true;
          break;
        case ProbeAnswer::NoReply:
          break;
      }
    }
  }
  if (reachable) {
    verdict.silentAfterReachable = true;
    verdict.outcome = Outcome::Unauthorized;
    char line[240];
    std::snprintf(line, sizeof line,
                  "the repeater answered a blank login at %s and then said nothing to the new candidate: on stock "
                  "firmware that silence is a refusal of the password (task p4.7-95); the server will try the old "
                  "candidate next",
                  probeAnsweredAs(verdict));
    exchange.log(line);
    return verdict;
  }
  // Two tries and then `unreachable`, which is what the .NET agent reports for the same: on
  // stock firmware a refused key is silence, and silence is all a board can see.
  verdict.outcome = Outcome::Unreachable;
  return verdict;
}

const char* probeAnsweredAs(const LoginVerdict& verdict) {
  return verdict.probeRefused ? kProbeAnsweredRefused : permissionName(verdict.probeAnsweredRole);
}

}  // namespace mm
