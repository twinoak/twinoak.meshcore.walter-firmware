// The apply: the plan's commands in order, each sent **exactly once**, every reply kept
// verbatim, the sequence stopped at the first reply that is not the one the line expects and at
// the first silence, then the re-read of every touched key and `ver` (CONFIGURATION §5 step 5,
// §5a; tasks p4-53 and p4-57). The first code on this board that changes a repeater.
//
// This is the .NET agent's ConfigApplyHandler (src/MeshMaster.Agent/ConfigApplyHandler.cs in
// twinoak/meshmaster.dk) with the mesh taken out: the *policy* - what to send next, what a reply
// means, what is never sent again, which line goes out even after the sequence has failed - is
// here and runs on a host against a fake exchange, and the board's JobRunner supplies the one
// thing a host has not got, a CLI line on the air. The rails, each a test:
//
//   - a command is sent once, whatever comes back: a `set` sent twice is a change made twice,
//     and a reply the mesh ate is not evidence the command was not applied (§5a's lost reply,
//     recorded as `no-reply` for the next config read to settle);
//   - the sender timestamps of an apply and its re-read are **strictly increasing**:
//     MyMesh::onPeerDataRecv at d9296435 (examples/simple_repeater/MyMesh.cpp:709-711) treats a
//     timestamp equal to the last one as a *retry* and runs nothing, so a sequence sent faster
//     than one line a second - which a region load is, its lines expecting no reply - would be
//     one command and a run of silently discarded ones;
//   - an entry of several lines (`regions`, task p4-57) is one outcome, and its lines say what
//     the firmware answers them: nothing at all for `region load` and the region lines, which are
//     sent without waiting; `OK - loaded N regions` for the blank line that ends the load;
//     ` home is now …` and ` default scope is now …`, which do not start with `OK`;
//   - the blank line that ends a load - one space, because StrHelper::isBlank takes a run of
//     spaces and onPeerDataRecv reads a text as a CLI command only past its five-byte header -
//     is sent even after the sequence has failed: until it arrives, every CLI command that
//     repeater hears is read as a region (MyMesh::handleCommand, 1196-1230);
//   - the result is ConfigApplyResult as MeshMaster.Contracts writes it, byte for byte
//     (tests/fixtures/contracts/ConfigApplyResult.json).
//
// Host-portable: no Arduino, no allocation; the plan lives inside the claimed job in a fixed
// pool, the outcome in a fixed table, and the re-read in the ConfigSnapshot a config read fills.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/ConfigRead.h>
#include <agent/ReplyCodec.h>

namespace mm {

// The plan's bounds. A plan carries at most the thirteen remotely-settable keys of the table
// (CONFIGURATION §2), each one `set <key> <value>`; the regions entry is the loader sequence -
// `region load`, one line per region, the blank line, the home, the default, the save, and a
// flag line per region the loader would not have changed - which for a map that still fits the
// firmware's 160-character listing (about twenty regions, task p4-57) is under fifty lines.
constexpr int kMaxApplyCommands = 16;
constexpr size_t kMaxApplyCommandLen = 96;
constexpr int kMaxApplyLines = 64;
constexpr size_t kMaxApplyLineLen = 48;
constexpr size_t kMaxApplyExpectLen = 32;
constexpr size_t kApplyTextPoolCap = 2048;
constexpr size_t kMaxPlanHashLen = 64;
constexpr size_t kMaxApplyRefusalLen = 160;

// The line that ends a region load, and the reason it is one space (RegionSyntax.Terminator).
extern const char kRegionLoadTerminator[];

// What a line expects when the plan does not say: the firmware's own confirmation.
extern const char kApplyDefaultExpect[];

// How the replies of an entry of several lines are joined into the one it records.
extern const char kApplyReplySeparator[];

struct ApplyLine {
  uint16_t lineAt = 0;    // into the pool
  uint16_t expectAt = 0;  // into the pool, when hasExpect
  bool hasExpect = false; // false: the firmware answers nothing, and the line is not waited for
  bool escape = false;    // sent even after the sequence has failed (the loader's blank line)
};

struct ApplyCommand {
  char key[kMaxConfigKeyLen + 1] = {0};
  char command[kMaxApplyCommandLen + 1] = {0};
  int firstLine = 0;  // into `lines`; lineCount 0 is the ordinary case, one line expecting OK
  int lineCount = 0;
};

// The plan as the claim carries it (ConfigApplyPayload: Commands[{Key, Command, From, To,
// Lines[{Line, Expect, Escape}]}], PlanHash, Reason, DesiredVersion). `From`, `To`, the reason
// and the version are the server's and are not read. A plan this firmware cannot hold or run -
// too many commands or lines, a line too long, an empty key or line, a line break in a line -
// is `refused` with the reason, and the runner reports the job `failed` at no transaction.
struct ConfigApplyPlan {
  int commandCount = 0;
  ApplyCommand commands[kMaxApplyCommands];
  int lineCount = 0;
  ApplyLine lines[kMaxApplyLines];
  char pool[kApplyTextPoolCap] = {0};
  size_t poolLen = 0;
  char planHash[kMaxPlanHashLen + 1] = {0};

  bool refused = false;
  char refusedWhy[kMaxApplyRefusalLen + 1] = {0};

  void clear();
  // Append a command; false (and refused) when it does not fit or is malformed.
  bool addCommand(const char* key, const char* command);
  // Append a line to the last command added; `expect` nullptr is "the firmware answers nothing".
  bool addLine(const char* line, const char* expect, bool escape);
  void refuse(const char* why);

  const char* lineText(int index) const { return pool + lines[index].lineAt; }
  const char* lineExpect(int index) const { return lines[index].hasExpect ? pool + lines[index].expectAt : nullptr; }
};

// ConfigCommandOutcomes: the four words the agent has (the server adds `unconfirmed` of its own).
enum class CommandOutcome : uint8_t { Ok, Error, NoReply, NotSent };
const char* commandOutcomeName(CommandOutcome o);

// A reply is CommonCLI's 160-byte buffer; an entry of several lines records every reply it got,
// joined, and the count line `OK - loaded N regions` comes first so it is what survives a cut.
constexpr size_t kMaxApplyReplyLen = 240;

struct ApplyCommandResult {
  CommandOutcome outcome = CommandOutcome::NotSent;
  bool hasReply = false;
  char reply[kMaxApplyReplyLen + 1] = {0};

  bool isOk() const { return outcome == CommandOutcome::Ok; }
  // Whether the command left the exit node at all: ok, error or no-reply.
  bool wasSent() const { return outcome != CommandOutcome::NotSent; }
  void setReply(const char* text);
};

struct ConfigApplyOutcome {
  int commandCount = 0;
  ApplyCommandResult commands[kMaxApplyCommands];
  // The re-read stopped before it reached every touched key (or `ver`).
  bool reReadStopped = false;

  void clear();
  int sent() const;
  int ok() const;
  bool allOk() const;
};

// One CLI line on the air, as the engine asks for it.
struct ApplySend {
  const char* what = "";    // for the log: the line, or a name for it
  const char* line = "";
  uint32_t timestamp = 0;   // the sender timestamp from ApplyExchange::nextTimestamp(), or 0 when the exchange draws its own at the send
  bool awaitReply = true;   // false: sent and not waited for (a line the firmware answers with nothing)
  bool retry = false;       // the bounded retry a read gets - never for a write
  bool escape = false;      // the loader's blank line: goes out even past the lease
};

// What the board supplies: a line on the air, the timestamp series, the regions read, the log.
class ApplyExchange {
 public:
  enum class Sent : uint8_t {
    Answered,  // a reply is in the buffer (or, for a line not waited for, it went out)
    NoReply,   // sent, and nothing came back in the wait
    NotSent,   // the packet could not be built or sent: the line never left this exit node
    LeaseOut,  // the lease ran out before it could be sent
  };

  virtual ~ApplyExchange() = default;
  virtual Sent send(const ApplySend& s, char* reply, size_t cap) = 0;
  // The next sender timestamp: strictly above every one this board has sent to this repeater -
  // or 0, when the exchange draws it at the moment of the send. The board's does since task
  // p1.5-15 (IMesh::sendCli hands back the one it put in the packet, from the RTC's
  // getCurrentTimeUnique(), the one series every tag and every CLI line come from); a host fake
  // keeps its own series here, and the test holds that series strictly increasing.
  virtual uint32_t nextTimestamp() = 0;
  // The `regions` key read back (`region`, `region default`): true when the read stands, with
  // the value or a raw line in the snapshot; false when the repeater stopped answering.
  virtual bool readRegions(ConfigSnapshot& snapshot) = 0;
  virtual void log(const char* line) = 0;
};

enum class ApplyRun : uint8_t { Ok, Failed, Timeout };

// The whole apply: the commands, then the re-read into `reRead` (values, raw lines, firmware).
// Ok when every command was answered with what its lines expect; Failed when any was refused,
// unanswered or never sent; Timeout when the lease ran out part-way - and the outcome is filled
// in every case, because the result is the only record of what happened on the mesh.
ApplyRun runConfigApply(const ConfigApplyPlan& plan, ApplyExchange& exchange, ConfigApplyOutcome& out,
                        ConfigSnapshot& reRead);

// ConfigApplyResult as ContractsJson writes it: `commands` (key, command, outcome, reply when
// there is one, isOk, wasSent), `reRead`, `reReadRaw`, `firmware` when read, `sent`, `ok`,
// `allOk`. Returns the length, or 0 when it did not fit.
size_t writeConfigApplyResult(const ConfigApplyPlan& plan, const ConfigApplyOutcome& out, const ConfigSnapshot& reRead,
                              char* buf, size_t cap);

}  // namespace mm
