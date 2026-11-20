// The two tries of a login, and the blank login between them that tells a wrong password from a
// repeater off the air (EXIT-NODES §7, CONFIGURATION §7 step 4; meshmaster.dk's task p4.7-95,
// whose finding for this repository is task p1.3-13).
//
// `JobRunner::ensureSession` is the .NET agent's `EnsureSessionAsync`: the material of the job's
// class once, and once more when the first try got no reply. Refused is `unauthorized`, silence
// twice is `unreachable` - and on stock firmware a password the repeater does not know is silence
// (`handleLoginReq`, examples/simple_repeater/MyMesh.cpp:89-141 at 81dbae59, is `return 0` to a
// non-blank password that is neither of its two), so a wrong password and a dead repeater look the
// same from here. That matters in exactly one place: a job handed the **new** candidate of an
// `unverified` credential rotation, where the server's fallback to the old candidate is keyed to
// a refusal and a silence would leave the row unverified for ever.
//
// The .NET agent's answer (task p4.7-95) is a **blank** login between the two tries: this exit
// node's own key and no password, the `acl` login every status read makes. `handleLoginReq`'s
// first statement answers it at the ACL entry's level for any key the ACL holds, and its second
// answers *guest* (and writes a guest entry) on a repeater with no guest password set. Answered at
// any level the repeater is on the air, and a second silence to
// the candidate right after it is the password's silence: `unauthorized`, with `LoginSilentResult`
// as the data so the server can tell it from a refusal the repeater spoke and from a silence that
// taught nothing. Silent too, nothing is known: `unreachable`, as before. The **second** try is
// the one judged, not the first - a reply the mesh ate is how the row became unverified in the
// first place, and a second try the repeater answers is a plain login that settles the row.
//
// One transaction more, on that path only. The probe stores no session: the next job is a
// candidate job and logs in afresh anyway.
//
// **What ends a login's wait** (task p1.10-47; meshmaster.dk's task p4.32-186 is the server's
// half). A login reply carries the repeater's own clock and no tag of ours, so a login is awaited
// with tag 0 - and until this task whatever the peer sent next was taken as its answer, which on
// a job whose request had just timed out was that request's **late** reply. The runner therefore
// drops a response that is not a login reply and waits on
// (`JobRunner::awaitLoginReply`): what reaches `login()` and `probe()` below is a login reply or a
// silence, and never a third thing. So nothing here says *refused* of a login the repeater did not
// answer - `Refused` is a login answered **below the class it was for** and nothing else, and
// nothing of MeshCore at the pin sends a refusal to a login at all (`handleLoginReq` answers
// RESP_SERVER_LOGIN_OK or `return 0`).
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
// **`answeredLocally` is asked first, and answers every class with `""`** (task p1.10-41;
// `IMesh::answersLocally`, task p1.10-40). A job the mesh answers without the air is answered by
// the board at the other end of the seam: the repeater-v2 unit's Walter serves a job for its
// *own* radio over the UART, and its local door (`LinkMesh::localLogin`,
// `components/agent/link_mesh.cpp` in twinoak.meshcore.walter-firmware) answers the login itself,
// **at admin, whatever material the login carried** - it does not look at it. So there is nothing
// to log in with and nothing missing: the material is the empty string at `admin` as much as at
// `acl`, and a password the claim did carry is **not sent**, because the door would ignore it.
// Deciding this before the class is what the question's place in this function is for: until
// 7 November 2026 `admin` answered `nullptr` here and `ensureSession` reported `unauthorized`
// without sending anything, so every CLI job of a unit's own repeater (`cli`, `reboot`,
// `config.apply`, `clock.sync`, `advert.request`, `acl.set`, `password.set` - all `admin`-class)
// needed a stored password that nothing reads, and a radio whose password nobody knows could not
// be configured through its own unit at all (job 22016, the scheduled `config.read` of a
// re-registered repeater, *no admin password stored*). meshmaster.dk hands nothing at any class
// for such a pair from the Walter that has this (`AgentProtocol.DecideAsync`, its task p4.27-175).
// A job that is **not** local is unchanged, down to the note: `admin` with no password still
// refuses, at no transaction and with nothing sent.
//
// `note` takes the line and is left empty when there is nothing to say; it never carries a
// password. Here rather than in `JobRunner` so that every branch is walked on a host.
constexpr size_t kLoginNoteCap = 160;
const char* loginMaterialFor(const ClaimedJob& job, bool answeredLocally, char* note, size_t noteCap);

// What one login came to - the runner's own reading of a reply. `Refused` is a login the repeater
// **answered below the class it was for** (task p3.5-41) and, since task p1.10-47, nothing else:
// a response that is not a login reply is dropped and the wait goes on, so it never arrives here.
// `NoReply` is what stock firmware answers a key and password it does not accept; `Failed` is a
// packet that could not be built; `Timeout` is the lease running out before the send.
enum class LoginTry : uint8_t { Ok, Refused, NoReply, Failed, Timeout };

// What the blank login came to: a login reply, at the level it granted, or silence. There is no
// third value (task p1.10-47) - nothing of MeshCore at the pin refuses a login, and a response
// that is not a login reply never ends the probe's wait. `NoReply` is also what the gate's
// refusal (the lease ran out) comes to, because either way nothing was learned.
enum class ProbeAnswer : uint8_t { NoReply, Answered };

// What the board supplies: a login with the job's material, a blank login, and the job's log.
class LoginExchange {
 public:
  virtual ~LoginExchange() = default;
  // One login with the job's material, `attempt` 1 or 2: counted, logged, and the session
  // bookkeeping a reply implies.
  virtual LoginTry login(int attempt) = 0;
  // One blank login: counted, logged, and **no session stored** whatever came back. `role` is
  // the role byte the login reply carried when it was `Answered` (kPermGuest .. kPermAdmin).
  virtual ProbeAnswer probe(uint8_t& role) = 0;
  virtual void log(const char* line) = 0;
};

// What the sequence came to.
struct LoginVerdict {
  Outcome outcome = Outcome::Unreachable;
  // The candidate's second try was silent right after a blank login the repeater answered: the
  // outcome is `unauthorized` and the result's data is LoginSilentResult.
  bool silentAfterReachable = false;
  // The level the probe's login reply granted, when it was answered at all.
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

// The tier the probe was answered at, in the server's words: `guest`, `read-only`, `read-write`
// or `admin` - `permissionName` of the role its login reply granted. The fifth value this had
// until task p1.10-47, `refused`, is gone: the probe cannot be refused, so the word was never a
// reading of anything this firmware saw. `LoginSilentResult.probeAnsweredAs` still carries it in
// the protocol, for an agent whose companion emits PUSH_CODE_LOGIN_FAIL.
const char* probeAnsweredAs(const LoginVerdict& verdict);

}  // namespace mm
