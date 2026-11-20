// What the board sends the server, as structs that mirror MeshMaster.Contracts field for field.
//
//   AgentHeartbeat(string AgentVersion, string Transport, DateTime At, string? CompanionPublicKey,
//                  string? CompanionFirmware, RadioParameters? Radio, IReadOnlyList<string>? JobTypes,
//                  AgentBudget? Budget, bool CompanionConnected, int QueueDepth,
//                  IReadOnlyDictionary<string, long>? Errors, string? Board, int? NextCheckInS,
//                  int? MaxTransactionsThisWake, AgentObserver? Observer, AgentFlood? Flood)
//   AgentObserver(string? Iata, IReadOnlyList<AgentObserverSlot>? Slots)
//   AgentObserverSlot(int Slot, string? Preset, string? Host, string State, DateTime? Since,
//                     long Published, long Dropped, string? LastError)
//   AgentFlood(string? Scope, bool UnscopedRetry, long AnsweredScoped, long AnsweredUnscoped,
//              long Unanswered)
//   AgentUpdate(string Table, long? Id, string? Step)
//   UpdateResult(string Outcome, string FromVersion, string ToVersion, string? Refusal, int? Check,
//                IReadOnlyList<FirmwareJournalEntry> Journal)
//   FirmwareJournalEntry(DateTime At, string Step, string? Note)
//   RadioParameters(double? FreqMHz, double? BwKHz, int? Sf, int? Cr)
//   Observation(DateTime At, string Kind, string PublicKey, IReadOnlyList<string>? Path, int? Hops,
//               double? Snr, double? Rssi, ObservationFields? Fields)
//   ObservationFields(string? Name, double? Lat, double? Lon, string? Role, long? Flags)
//   ObservationBatch(string ClientId, IReadOnlyList<Observation> Items)
//
// (src/MeshMaster.Contracts/Envelopes.cs in twinoak/meshmaster.dk.) The bytes are what
// ContractsJson.cs makes of those records and nothing else: camelCase, the record's own field
// order, no spaces, no indentation, and a null field left out entirely - which is why every
// optional field here has a way of saying "absent" rather than "empty".
//
// Host-portable, like the reply parser beside it (ConfigReply.h): test/test_envelopes holds the
// heartbeat against tests/fixtures/contracts/AgentHeartbeat.json byte for byte, for the input
// that fixture was written from - and against AgentHeartbeatNoWake.json and
// AgentHeartbeatObserver.json for the two other shapes the same sample has.

#pragma once

#include <cstddef>
#include <cstdint>

#include <agent/ConfigReply.h>

namespace mm {

// RadioParameters: what the board's own SX1262 is programmed with. Doubles, because the record's
// fields are doubles and 869.618 carried through a float is 869.617981.
struct RadioParameters {
  double freqMHz = 0;
  double bwKHz = 0;
  int sf = 0;
  int cr = 0;
};

// One entry of the heartbeat's `errors`, which is an open key space on both ends. No member
// initialisers: the board is built as gnu++11, where a struct that has them is not an aggregate
// and `{"overheard", 41}` would not compile.
struct ErrorCounter {
  const char* name;
  long long value;
};

// One broker a consumer publishes to as an *observer*, as the heartbeat reports it
// (AgentObserverSlot; meshmaster.dk's D-46 and its task p4.17-140). This library knows nothing
// about what an observer is or does - no broker, no topic, no key - and nothing here is filled
// in by it: a consumer that has one gives the loop a provider (AgentLoop.h,
// `IHeartbeatObserver`) and the provider fills these. The field names and their order are the
// record's, because the bytes are this library's to write.
//
// The server bounds every string (AgentObserverRules: preset 32, host 64, state 32, lastError
// 120 characters, and at most six slots) and **a field over its bound is refused by name while
// the rest of the heartbeat is still taken** - a bad observer object must never take an exit
// node off the air. Nothing is bounded here: a provider that sends more than the server accepts
// loses the object and keeps its exit node.
struct HeartbeatObserverSlot {
  int slot = 0;                        // which slot, as its console numbers them: 1, 2
  const char* preset = nullptr;        // nullptr: the field is left out
  const char* host = nullptr;          // nullptr: the field is left out
  const char* state = "off";           // one word; the server keeps one it has not heard of
  uint32_t since = 0;                  // when the slot entered this state; 0: the field is left out
  long long published = 0;             // always written, as the record's non-nullable long is
  long long dropped = 0;
  const char* lastError = nullptr;     // nullptr: the field is left out
};

// The observer's state as the heartbeat reports it (AgentObserver): the region and one entry per
// slot. `hasSlots` false leaves `slots` out entirely, which is not the same as an empty array.
struct HeartbeatObserver {
  const char* iata = nullptr;  // nullptr: the field is left out
  bool hasSlots = false;
  const HeartbeatObserverSlot* slots = nullptr;
  int slotCount = 0;
};

// The flood scope as the heartbeat reports it (AgentFlood; meshmaster.dk's D-47 and its task
// p4.20-151). Like the observer above, this library knows nothing about what a scope *is*: it
// writes the bytes a consumer fills in. MeshMaster keeps the newest object on the exit node's row,
// shows one line of it and configures nothing - the scope is set where the exit node runs.
//
// `scope` nullptr is **the companion's own**: the exit node says nothing about scope and whatever
// its radio holds decides. The serializer omits a null member, so on the wire that is the absence
// of `scope` inside a `flood` object that is present, which is not the same as no `flood` at all.
struct HeartbeatFlood {
  const char* scope = nullptr;      // a region's name, "*", or nullptr: the member is left out
  bool unscopedRetry = false;       // the second try of a flooded transaction drops the scope
  long long answeredScoped = 0;     // always written, as the record's non-nullable longs are
  long long answeredUnscoped = 0;
  long long unanswered = 0;
};

// What this board can do about an update, as the heartbeat reports it (AgentUpdate; task
// p1.15-61 and meshmaster.dk's task p4.42-244). Like the observer and the flood scope above,
// **this library does not know what an update is** - not a slot, not a release, not a signature,
// not a trial - and it is not going to: a consumer that can be updated from the server gives the
// loop a provider (AgentLoop.h, `IUpdateProvider`) and the loop writes whatever it says.
//
// `table` is the one word that says what the board can do at all, and it is the consumer's
// vocabulary to keep: this firmware's is `one`, `two` and `other` (src/platform/PartitionTable.h).
// `id` and `step` are written **only while an ask is in hand** - `hasAsk` - and then they are the
// ask's own id and the newest step of its journal, which is all the server hears before the
// result.
struct HeartbeatUpdate {
  const char* table = "other";
  bool hasAsk = false;        // false: neither `id` nor `step` is written
  long long id = 0;
  const char* step = nullptr; // nullptr with hasAsk writes an empty string
};

struct Heartbeat {
  const char* agentVersion = "";
  const char* transport = "onboard";
  uint32_t at = 0;  // UNIX seconds; written as "YYYY-MM-DDTHH:MM:SSZ"

  const char* companionPublicKey = nullptr;  // nullptr: the field is left out
  const char* companionFirmware = nullptr;   // nullptr: the field is left out

  bool hasRadio = false;
  RadioParameters radio;

  // `jobTypes: []` until task 03 has a handler: an empty array is not the same as no field.
  bool hasJobTypes = true;
  const char* const* jobTypes = nullptr;
  int jobTypeCount = 0;

  bool hasBudget = true;
  AgentBudget budget;

  bool companionConnected = false;
  int queueDepth = 0;

  const ErrorCounter* errors = nullptr;  // nullptr or errorCount 0: the field is left out
  int errorCount = 0;

  const char* board = nullptr;  // nullptr: the field is left out

  // The agent's own promise, for an agent that paces itself (EXIT-NODES §6, *An agent that paces
  // itself*; meshmaster.dk's task p4.14-120). Both are optional and are written only when set:
  // **an agent that sends neither is an agent as before, byte for byte**, which is what the
  // Heltec is and what test/test_envelopes holds.
  //
  //   nextCheckInS             how long until this agent will be back. The server accepts it
  //                            between AgentLimits.MinCheckInS (60) and MaxCheckInS (86 400),
  //                            clamping rather than refusing, and the reply's `pollIntervalS`
  //                            is then what was accepted
  //   maxTransactionsThisWake  what this wake has the airtime and the energy for. The server
  //                            hands out jobs until the next one's price would take the wake
  //                            past it, and always at least one (task p4.14-121)
  bool hasNextCheckInS = false;
  int nextCheckInS = 0;
  bool hasMaxTransactionsThisWake = false;
  int maxTransactionsThisWake = 0;

  // The observer, last, as the record has it (meshmaster.dk's D-46): nullptr for every consumer
  // that is not one - which is every consumer until its owner turns one on - and then the
  // heartbeat is byte for byte what it was. The object and every string in it must outlive the
  // call that posts the heartbeat; nothing here is copied.
  const HeartbeatObserver* observer = nullptr;

  // The flood scope, after the observer as the record has it (D-47): nullptr for an exit node
  // that says nothing about scope, which is every one built before the field existed, and then
  // the heartbeat is byte for byte what it was. The object and its `scope` string must outlive
  // the call that posts the heartbeat; nothing here is copied.
  const HeartbeatFlood* flood = nullptr;

  // The update, **last**, as the record has it (task p1.15-61): nullptr for a consumer that
  // cannot be updated from the server - which is every one that gives the loop no provider, and
  // this firmware when it was built with no release key - and then the heartbeat is byte for byte
  // what it was. The object and its two strings must outlive the call that posts the heartbeat;
  // nothing here is copied.
  const HeartbeatUpdate* update = nullptr;
};

struct ObservationFields {
  const char* name = nullptr;  // nullptr: the field is left out
  bool hasPosition = false;
  int32_t latE6 = 0;  // degrees x 1e6, as an advert carries them
  int32_t lonE6 = 0;
  const char* role = nullptr;  // "companion" | "repeater" | "roomserver" | "sensor" | "unknown"
  bool hasFlags = false;
  long long flags = 0;
};

// One heard node. `path` is the hop hashes the advert arrived by, lowercase hex, in order;
// an empty path with hops 0 is a direct neighbour.
struct ObservationItem {
  uint32_t at = 0;
  const char* kind = "advert";
  const char* publicKey = "";

  bool hasPath = false;
  const char* const* path = nullptr;
  int pathCount = 0;

  bool hasHops = false;
  int hops = 0;

  bool hasSnr = false;
  double snr = 0;
  bool hasRssi = false;
  double rssi = 0;

  bool hasFields = false;
  ObservationFields fields;
};

// Where a result's journal entries come from (task p1.15-61). An interface and not an array,
// because the bound is forty entries of a step and a note and a consumer that keeps them as one
// text - which is what a board with 320 kB of RAM does - would otherwise have to build four
// kilobytes of structs to post three of them.
//
// **The two strings are borrowed**: `writeUpdateResult` writes each entry before it asks for the
// next, so an implementation may hand back pointers into one buffer of its own and overwrite them
// at the next call.
class UpdateJournal {
 public:
  virtual ~UpdateJournal() {}
  virtual int count() const = 0;
  // Entry `i`, 0 being the **oldest**: the moment (UNIX seconds), the step, and the note -
  // nullptr or empty leaves `note` out of the object. False past the end.
  virtual bool at(int i, uint32_t& when, const char*& step, const char*& note) const = 0;
};

// The one result of an update the server asked for (UpdateResult; task p1.15-61). What the words
// mean is meshmaster.dk's - `committed`, `rolledBack`, `refused`, `stuck`, and a refusal's
// `inProgress`, `table`, `current`, `fetch` or `check` - and this library writes them as they are
// given: the vocabulary lives at the two ends of the protocol and a copy of it here would be a
// third that could drift.
struct UpdateResult {
  const char* outcome = "";
  const char* fromVersion = "";
  const char* toVersion = "";
  const char* refusal = nullptr;  // nullptr: the member is left out
  bool hasCheck = false;          // false: the member is left out
  int check = 0;
  const UpdateJournal* journal = nullptr;  // nullptr: an empty array, which is not an absent one
};

// Write the JSON body of POST /agent/v1/heartbeat into `out` (cap bytes, NUL-terminated).
// Returns the length written, or 0 when it did not fit.
size_t writeHeartbeat(const Heartbeat& hb, char* out, size_t cap);

// Write the JSON body of POST /agent/v1/observations. `clientId` is the client-generated batch
// id the server de-duplicates on, so a batch that is retried must carry the one it was built
// with. Returns the length written, or 0 when it did not fit.
size_t writeObservationBatch(const char* clientId, const ObservationItem* items, int count, char* out, size_t cap);

// Write the JSON body of POST /agent/v1/updates/{id}/result into `out` (cap bytes,
// NUL-terminated). Returns the length written, or 0 when it did not fit - a caller with a journal
// too long for its buffer drops the oldest entries and asks again, which is what this firmware
// does (src/update/Ask.h).
size_t writeUpdateResult(const UpdateResult& result, char* out, size_t cap);

// The length of the longest prefix of `text` that is well-formed UTF-8 with no control
// characters, at most `max` bytes. A name in an advert is whatever a stranger put there, and a
// JSON string has to be UTF-8: this is what bounds one before it is copied.
size_t validUtf8PrefixLength(const char* text, size_t max);

}  // namespace mm
