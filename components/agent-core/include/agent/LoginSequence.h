// The two tries of a login, and the blank login between them that tells a wrong password from a
// repeater off the air (EXIT-NODES §7, CONFIGURATION §7 step 4; meshmaster.dk's task p4.7-95,
// whose finding for this repository is task p1.3-13).
//
// `JobRunner::ensureSession` is the .NET agent's `EnsureSessionAsync`: the material of the job's
// class once, and once more when the first try got no reply. Refused is `unauthorized`, silence
// twice is `unreachable` - and on stock firmware a password the repeater does not know is silence
// (`handleLoginReq`, examples/simple_repeater/MyMesh.cpp:89-141 at d9296435, is `return 0` to a
// non-blank password that is neither of its two), so a wrong password and a dead repeater look the
// same from here. That matters in exactly one place: a job handed the **new** candidate of an
// `unverified` credential rotation, where the server's fallback to the old candidate is keyed to
// a refusal and a silence would leave the row unverified for ever.
//
// The .NET agent's answer (task p4.7-95) is a **blank** login between the two tries: this exit
// node's own key and no password, the `acl` login every status read makes. `handleLoginReq`'s
// first statement answers it at the ACL entry's level for any key the ACL holds, and its second
// answers *guest* (and writes a guest entry) on a repeater with no guest password set. Answered at
// any level - a refusal is an answer too - the repeater is on the air, and a second silence to
// the candidate right after it is the password's silence: `unauthorized`, with `LoginSilentResult`
// as the data so the server can tell it from a refusal the repeater spoke and from a silence that
// taught nothing. Silent too, nothing is known: `unreachable`, as before. The **second** try is
// the one judged, not the first - a reply the mesh ate is how the row became unverified in the
// first place, and a second try the repeater answers is a plain login that settles the row.
//
// One transaction more, on that path only. The probe stores no session: the next job is a
// candidate job and logs in afresh anyway.
//
// The policy is here, on a host, so that every path can be walked against a scripted exchange
// (test/test_rotation_probe); the board supplies the logins (JobRunner.cpp's LoginOnTheAir),
// the way ConfigApply.h's engine and ApplyOnTheAir divide an apply.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/JobEnvelopes.h>

namespace mm {

// The login material of a job's class, and the one line the job's log gets about it
// (EXIT-NODES §7; `JobContext.LoginMaterial` on the .NET agent, Handlers.cs):
//
//   - the **password** the claim carried for the class, with a line when the claim carried more
//     than the companion sends (`kMaxPasswordLen`) and never the value;
//   - `""` for a login that carries **no password at all**, which is not missing material: the
//     `acl` login at class `acl` and `none` - this exit node's own key and nothing else - and
//     since the guest door (task p1.4-14; meshmaster.dk's p4.7-94, reading p4.5-81) the **blank
//     login** at class `guest` when the claim carried no guest password. For a pair that door
//     made with no password the claim carries no credential at all (`AgentProtocol`: the job
//     goes out with none and the agent logs in blank), and the frame this sends is the frame the
//     `acl` login sends: `handleLoginReq` compares the password with `_prefs.guest_password`,
//     which `NodePrefs()` leaves empty, so a repeater whose owner never set one answers it *at
//     guest* and the job runs, and one that *has* a guest password answers it with silence,
//     which the two tries below report as `unreachable` - by design on both sides
//     (EXIT-NODES §4.1 step 2);
//   - `nullptr` at class **`admin`** with no admin password, which every caller turns into
//     `unauthorized` at no transaction, because there is no admin login without one.
//
// `note` takes the line and is left empty when there is nothing to say; it never carries a
// password. Here rather than in `JobRunner` so that every branch is walked on a host.
constexpr size_t kLoginNoteCap = 160;
const char* loginMaterialFor(const ClaimedJob& job, char* note, size_t noteCap);

// What one login came to - the runner's own reading of a reply. `Refused` is a reply that was
// not a login-ok, and a login answered below the class it was for (task p3.5-41); `NoReply` is
// what stock firmware answers a key and password it does not accept; `Failed` is a packet that
// could not be built; `Timeout` is the lease running out before the send.
enum class LoginTry : uint8_t { Ok, Refused, NoReply, Failed, Timeout };

// What the blank login came to. `Refused` is the legacy PUSH_CODE_LOGIN_FAIL frame, which a stock
// repeater never sends and which is an answer all the same; `NoReply` is also what the gate's
// refusal (the lease ran out) comes to, because either way nothing was learned.
enum class ProbeAnswer : uint8_t { NoReply, Answered, Refused };

// What the board supplies: a login with the job's material, a blank login, and the job's log.
class LoginExchange {
 public:
  virtual ~LoginExchange() = default;
  // One login with the job's material, `attempt` 1 or 2: counted, logged, and the session
  // bookkeeping a reply implies.
  virtual LoginTry login(int attempt) = 0;
  // One blank login: counted, logged, and **no session stored** whatever came back. `role` is
  // the role byte the reply carried when it was `Answered` (kPermGuest .. kPermAdmin).
  virtual ProbeAnswer probe(uint8_t& role) = 0;
  virtual void log(const char* line) = 0;
};

// What the sequence came to.
struct LoginVerdict {
  Outcome outcome = Outcome::Unreachable;
  // The candidate's second try was silent right after a blank login the repeater answered: the
  // outcome is `unauthorized` and the result's data is LoginSilentResult.
  bool silentAfterReachable = false;
  // How the probe was answered, when it was: the legacy refusal, or a login at `probeAnsweredRole`.
  bool probeRefused = false;
  uint8_t probeAnsweredRole = 0;
};

// Whether a job probes between its tries: the claim's `credential.candidate` is `new` and the
// material is a password - the .NET agent's `Candidate == "new" && material.Length > 0`. An `old`
// candidate, no candidate, or a blank material (an acl-class job, whose login *is* the probe)
// never probes.
bool probesBetweenTries(const char* credentialCandidate, const char* material);

// The sequence: `login(1)`; on silence and `probes`, `probe()`; on silence `login(2)`; and the
// verdict - `ok`, `unauthorized`, `failed` or `timeout` the moment a try comes to one of them,
// else `unauthorized` with `silentAfterReachable` after an answered probe and `unreachable`
// after a silent one or none.
LoginVerdict loginSequence(LoginExchange& exchange, bool probes);

// The tier the probe was answered at, in the server's words: `guest`, `read-only`, `read-write`,
// `admin` (permissionName of the role), or `refused` for the legacy frame.
const char* probeAnsweredAs(const LoginVerdict& verdict);
extern const char kProbeAnsweredRefused[];

}  // namespace mm
