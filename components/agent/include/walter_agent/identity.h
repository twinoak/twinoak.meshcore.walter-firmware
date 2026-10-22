// Who this exit node is, as the heartbeat says it (task p4-04): `mm::HeartbeatIdentity`
// (components/agent-core, agent/AgentLoop.h) filled from the radio over the box link and from
// this firmware's own tables - **and from nothing a person types**. The report of this task has
// the table of every field and its source.
//
//   companionPublicKey   §5.6 `IDENTITY`. This exit node *is* its repeater (spec/SYSTEM.md S-1),
//                        so the repeater's own MeshCore key is the key the server knows it by.
//                        The library takes it through `AgentLoop::setIdentity`, not through the
//                        identity struct, so `publicKeyHex()` is what the owner hands it.
//   companionFirmware    §5.1 `HELLO`'s `fw_version` - the **repeater firmware's** release
//                        version, `0.1.0`, and not the MeshCore tag that `upstream` carries.
//                        meshmaster.dk's firmware page looks this up against the *Repeater*
//                        product's releases (`FirmwarePageRules.ReleaseOf`, whose `SameVersion`
//                        forgives a leading `v`), and its register keeps the first word of it as
//                        the firmware token (`Register/FirmwareVersion.Parse`, which accepts any
//                        string). The Heltec sends its MeshCore tag here because on that board
//                        the companion firmware *is* MeshCore; on a unit of this design the
//                        repeater firmware is twinoak's and has a release line of its own.
//   agentVersion         this firmware's version (`core::version()`), which the same page looks
//                        up against the *Walter* product.
//   board                `walter`. transport: `native` - meshmaster.dk's word for an exit node
//                        that *is* its repeater (EXIT-NODES §2, `AgentTransports`).
//   radio                §5.7 `CLI get radio`, **one line and one request**: the repeater's
//                        console answers `> <freq>,<bw>,<sf>,<cr>` (MeshCore's
//                        `CommonCLI::handleGetCmd` at d9296435, the `radio` branch), which is the
//                        only line that carries all four - `get freq` answers the frequency
//                        alone and there is no `get bw`, `get sf` or `get cr` at all. The
//                        specification's own fixture is that exchange (`spec/fixtures/link/`
//                        `link-v1.json`, *cli request* / *cli response*).
//   jobTypes             **not here.** The library builds the list itself at every heartbeat,
//                        from `mm::IMesh::supports()` over its thirteen core types and then the
//                        extension's (agent/AgentLoop.cpp's `buildHeartbeat`). `LinkMesh`
//                        answers `supports()` from the session's `MESH_CLIENT` capability, so a
//                        radio with nothing to carry advertises nothing - which is exactly what a
//                        unit registered before its repeater has to say.
//   budget               the server's, kept by the library.
//
// **A unit may have no radio identity at all**, and still checks in: since 20 October a unit is
// registered at meshmaster.dk before its repeater is, and its RAK may hold no application yet.
// Then `known()` is false, `publicKeyHex()` is null, and the heartbeat carries no
// `companionPublicKey`, no `companionFirmware`, no `radio` block and an empty `jobTypes` - a unit
// without a radio can run no job, and an empty list is how it says so. The server keeps the key
// it had (`Envelopes.cs`: every one of those fields is optional) and the unit's page shows it
// alive and *radio not identified*. The first heartbeat after the radio has an identity carries
// the key.
//
// Logic, like core/: no ESP-IDF include, no heap, every byte of state a member of this object.
#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/AgentLoop.h>
#include <agent/Airtime.h>
#include <agent/ConfigApply.h>
#include <agent/ConfigRead.h>
#include <agent/Envelopes.h>
#include <agent/IJobExtension.h>
#include <agent/IMesh.h>
#include <agent/JobEnvelopes.h>

#include "core/link/client/client.h"
#include "core/power/interval_policy.h"

namespace agent {

namespace lc = core::link::client;

// meshmaster.dk's two words for what this unit is. Neither is validated by the library - the
// vocabulary is the server's (agent/AgentLoop.h, `HeartbeatIdentity::transport`).
constexpr const char kBoardName[] = "walter";
constexpr const char kTransport[] = "native";

// The line that carries the radio block, and the `>` the repeater's console prefixes a `get`
// reply with (MeshCore's CommonCLI at d9296435).
constexpr const char kRadioQuery[] = "get radio";

// -- what one wake promises the server ---------------------------------------------------------
//
// `mm::WakeIntent`: how long until this agent will be back, and what this wake has the energy
// for (EXIT-NODES §6, *An agent that paces itself*). The interval is the duty cycle's -
// core/power/interval_policy.h decided it from the battery - and the transaction cap is the
// table below it.
namespace intent {

// The most transactions one claimed job can cost, out of the library's own bounds: a
// `config.read` of the most keys a claim may name (`kMaxConfigKeys`), the two extras it always
// asks for and the `regions` listing, plus the login every credential class but `none` needs and
// the ACL restore an admin session on a repeater with a grant ends with (`transactionsOf` in
// agent/AgentLoop.cpp, `transactionsFor` in agent/Airtime.cpp). Not a number of ours and not a
// bench number: it is the library's arithmetic, and a release that reprices a job moves it.
constexpr int kLargestJob = mm::kMaxConfigKeys + mm::kConfigReadExtraCount + 1 +
                            mm::kTransactionsForLogin + mm::kTransactionsForAclRestore;
// A `config.apply` of the most commands one may carry is the other candidate for "the dearest
// job there is"; it is cheaper, and this is what holds that true at the next bump.
static_assert(2 * mm::kMaxApplyCommands + 1 + mm::kTransactionsForLogin +
                      mm::kTransactionsForAclRestore <= kLargestJob,
              "the dearest job is a config.read of a full key list");

// -- the cap, band by band ---------------------------------------------------------------------
//
// **Every number below is a PLACEHOLDER and is TO BE SET ON THE BENCH** against what a wake with
// an attach actually costs the pack (docs/bench.md §17). What is not a placeholder is their
// shape: each is written in the library's own arithmetic above, so a release that reprices a job
// moves them with it.

// `plenty`: both jobs a claim may hand over (`kClaimMax`), each the dearest there is. A pack
// above three quarters under a sunny panel can pay for a wake that does everything.
constexpr int kPlenty = mm::kClaimMax * kLargestJob;
// `normal`: half of it - one worst-case job, or several ordinary ones.
constexpr int kNormal = kPlenty / 2;
// `saving`: one job's worth, and the job meant is the real one - the daily `config.read` of the
// server's own default key list (`kDefaultConfigKeyCount` keys, the two extras, `regions`, the
// login and the restore), which is what `configReadTransactions(nullptr, 0)` prices and what
// `docs/library.md` calls eleven minutes at two a minute.
constexpr int kSaving = mm::kDefaultConfigKeyCount + mm::kConfigReadExtraCount + 1 +
                        mm::kTransactionsForLogin + mm::kTransactionsForAclRestore;
// `critical`: none. The modem is not brought up at all in this band
// (core/power/interval_policy.h's `Allowance::modem`), so the intent is moot - and 0 is the
// library's own "an agent that proposes nothing", which leaves the field out of the heartbeat
// altogether rather than promising a wake that will not happen.
constexpr int kCritical = 0;
// `no-reading`: the medium one, as `kUnknownMinutes` is `kNormalMinutes` - a Walter whose radio
// is silent is exactly the one that has to reach its uplink and say so.
constexpr int kUnknown = kNormal;

static_assert(kPlenty > kNormal && kNormal >= kSaving && kSaving > kCritical,
              "a worse band never promises more");
static_assert(kSaving >= mm::kTransactionsPerJobAtLeast,
              "a band that uses the modem has to be able to hold one job");

int forBand(core::power::Band band);

}  // namespace intent

// The intent for a wake: the band's cap, and the interval the policy decided, in seconds.
mm::WakeIntent intentFor(const core::power::Decision& decision);
mm::WakeIntent intentFor(core::power::Band band, std::uint32_t interval_seconds);

// -- the radio block ---------------------------------------------------------------------------

struct RadioBlock {
  bool known = false;
  mm::RadioParameters parameters;
};

// `> 869.618,62.5,8,8` into the four numbers. False for anything else, and `out` untouched: a
// reply this end cannot read is *no radio block*, which the heartbeat leaves out, and never four
// numbers guessed at. `text` need not be NUL-terminated; `size` bounds it.
bool parseRadioLine(const char* text, std::size_t size, mm::RadioParameters* out);

// -- what the heartbeat needs that this object does not own ------------------------------------
//
// The library's `doHeartbeat()` fills `errors` from six counters and reads the queue depth and
// the budget off the runner and the server (agent/AgentLoop.cpp). None of those is the
// identity's, so a heartbeat built **outside** the loop - which is what the console's
// `agent heartbeat` is - takes them here. Task p4-05, which owns the loop, fills them from it;
// this task fills what it can (the mesh's `overheard`, the ring's drops, the platform's reboots)
// and leaves the loop's own three at 0, which is what they are before the first wake.
struct HeartbeatExtras {
  std::uint32_t at = 0;  // UNIX seconds; 0 while this unit has no time (the library sends 0 too)
  mm::AgentBudget budget{mm::kDefaultMaxTransactionsPerMinute, mm::kDefaultMinGapSeconds};
  int queue_depth = 0;
  long long overheard = 0;
  long long wifi_reconnects = 0;
  long long http_failures = 0;
  long long observations_dropped = 0;
  long long results_dropped = 0;
  long long reboots = 0;
};

struct IdentityCounters {
  std::uint32_t refreshes = 0;       // refresh() calls that asked the radio something
  std::uint32_t identity_reads = 0;  // §5.6 requests that answered
  std::uint32_t radio_reads = 0;     // §5.7 `get radio` requests that answered
  std::uint32_t boot_changes = 0;    // a new boot_id: everything read again
  std::uint32_t unreadable = 0;      // a `get radio` reply these four numbers are not in
  std::uint32_t no_identity = 0;     // IDENTITY answered with a key of all zeros, or did not
};

class Identity {
 public:
  // `version` is this firmware's own (`core::version()`); a static string, kept by pointer.
  Identity(lc::Client& radio, const char* version);

  // Read what has to be read, and no more. Nothing is asked of a radio with no session - opening
  // one is `HELLO`, and that belongs to whoever runs the wake (the supervisor's check, step 2) -
  // so this is free when there is nothing to do: the key and the version are asked for once a
  // boot and again when `boot_id` changes, and the radio block again after that or after a
  // `set` line crossed the local door. True when this unit has a radio identity afterwards.
  bool refresh();

  // The repeater's settings may have changed: the block is read again at the next refresh().
  // Called when a `set ...` line went through the local door for the unit's own repeater (a
  // `config.apply` job, or `rak cli`), and by a restart.
  void radioSettingsStale() { radio_.known = false; }

  // Everything forgotten, as a boot leaves it: the bench, and a test.
  void forget();

  // ------------------------------------------------------------------ what the owner hands over

  // True when the radio has an identity: the key is known and is not all zeros.
  bool known() const { return key_known_; }
  // 64 lowercase hex characters, or **nullptr** while the radio has made no identity - which is
  // exactly what `AgentLoop::setIdentity` takes for "it has none".
  const char* publicKeyHex() const { return key_known_ ? key_hex_ : nullptr; }
  // The same 32 bytes, for what takes a key and not a string: `LinkMesh::setOwnKey` (the local
  // door) and the observation drain (an advert about this unit's own repeater). nullptr while
  // the radio has made no identity.
  const std::uint8_t* publicKey() const { return key_known_ ? key_ : nullptr; }
  // The struct the library reads at every heartbeat. Kept alive by this object.
  const mm::HeartbeatIdentity& heartbeatIdentity() const { return identity_; }

  // ------------------------------------------------------------------ what the console reads

  const RadioBlock& radio() const { return radio_; }
  const char* companionFirmware() const { return firmware_; }
  const char* board() const { return board_; }       // the radio's own board, from HELLO
  const char* upstream() const { return upstream_; } // what the repeater firmware is built on
  const char* name() const { return name_; }         // §5.6's advert name
  std::uint32_t bootId() const { return boot_id_; }
  // Whether a HELLO has been answered in this boot: what the console says instead of a key when
  // nothing has been asked of the radio yet.
  bool sessionUp() const { return radio_client_.session().up; }
  const IdentityCounters& counters() const { return counters_; }

  // ------------------------------------------------------------------ the heartbeat, as it would be sent

  // The `mm::Heartbeat` the library would build from this identity, field for field as
  // `AgentLoop::buildHeartbeat` and `doHeartbeat` build it (agent/AgentLoop.cpp). `mesh` answers
  // `supports()` for the job type list and `extension` adds its types after the thirteen, as the
  // library does; either may be null, and then the list is empty. The `const char*` fields point
  // into this object and into `errors`, which the caller owns.
  void fillHeartbeat(mm::Heartbeat* out, const mm::IMesh* mesh, const mm::IJobExtension* extension,
                     const HeartbeatExtras& extras, const mm::WakeIntent& intent,
                     mm::ErrorCounter* errors, int error_capacity);

  // The JSON body of `POST /agent/v1/heartbeat` for that heartbeat, into `out`: what the console
  // prints and what a test holds against meshmaster.dk's contract fixture. 0 when it did not fit.
  std::size_t writeHeartbeat(const mm::IMesh* mesh, const mm::IJobExtension* extension,
                             const HeartbeatExtras& extras, const mm::WakeIntent& intent,
                             char* out, std::size_t cap);

  // How many error counters `fillHeartbeat` writes: the library's six.
  static constexpr int kErrorCount = 6;

 private:
  bool readKey();
  bool readRadio();

  lc::Client& radio_client_;

  bool key_known_ = false;
  std::uint8_t key_[core::link::kPubkeySize] = {};
  char key_hex_[2 * core::link::kPubkeySize + 1] = {};
  char name_[lc::kVersionMax] = {};
  char firmware_[lc::kVersionMax] = {};
  char board_[lc::kVersionMax] = {};
  char upstream_[lc::kVersionMax] = {};
  std::uint32_t boot_id_ = 0;
  bool boot_seen_ = false;  // a session has been read: `boot_id_` means something

  RadioBlock radio_;
  mm::HeartbeatIdentity identity_;
  IdentityCounters counters_;
  // The job type list, as the library keeps its own: the thirteen and the extension's.
  const char* job_types_[mm::kJobTypeCount + mm::kMaxExtensionTypes] = {};
};

}  // namespace agent
