// `rak provision` (task p5-03): the sequence of spec/SYSTEM.md S-5a. provision.h says what each
// step is; this file is the order, the refusals and the bookkeeping. Every register this touches
// is the door's or the nRF52 layer's, cited there; nothing here names one.
#include "core/provision/provision.h"

#include <cstdio>

#include "core/flasher/board_profile.h"
#include "core/swd/idle_pause.h"

namespace core {
namespace provision {

const char* describe(Step step) {
  switch (step) {
    case Step::kNone: return "not begun";
    case Step::kCheck: return "check";
    case Step::kAttach: return "attach";
    case Step::kVariant: return "variant";
    case Step::kErase: return "erase";
    case Step::kWrite: return "write";
    case Step::kVerify: return "verify";
    case Step::kReset: return "reset";
    case Step::kDone: return "done";
  }
  return "?";
}

const char* describe(Status status) {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kNoStore: return "this build has no image store";
    case Status::kNoProfile: return "no board profile for this slot";
    case Status::kNoBase: return "no base in the store for this slot ('walterctl.py push --as base')";
    case Status::kBaseRefused: return "the stored base fails check 5p";
    case Status::kBaseUnreadable: return "the stored base's bytes are not its record's SHA-256: push it again";
    case Status::kUpdateRunning: return "the radio's update has a run in hand: it holds the radio";
    case Status::kSupervisorBusy: return "the supervisor is climbing its ladder: it holds the radio";
    case Status::kSlotBusy: return "another slot's operation holds the selection";
    case Status::kBatteryLow: return "the battery is below the update's threshold";
    case Status::kBatteryFalling: return "the battery is falling faster than the update allows";
    case Status::kKillAsserted: return "the kill line is driven: the radio's rail is off";
    case Status::kPins: return "the SWD lines could not be acquired";
    case Status::kSwd: return "the debug port failed";
    case Status::kNotNrf52840: return "not an nRF52840 with 4096-byte pages";
    case Status::kGrantMalformed: return "not a radio: type the DEVICEID (16 hex digits, as 'rak swd id' prints it), 'locked', or 'locked:<key16>'";
    case Status::kGrantMismatch: return "the word typed does not fit the port: a DEVICEID names an open radio, 'locked' a closed one";
    case Status::kWrongDevice: return "the DEVICEID typed is not this radio's";
    case Status::kApplicationAnswers: return "an application answers on the link: name it, 'locked:<key16>' with the first 16 hex digits of its public key";
    case Status::kWrongKey: return "the key typed is not the first 16 hex digits of the public key IDENTITY returns";
    case Status::kNoApplication: return "nothing answers HELLO: a radio with no application is named 'locked', without a key";
    case Status::kDoor: return "the door refused to open";
    case Status::kVariantUnknown: return "INFO.VARIANT is not in the table: refused, nothing erased";
    case Status::kVariantUnknownErased: return "INFO.VARIANT is not in the table: stopped with the radio erased - it has lost nothing it had not lost already; extend core/provision/variant.h and run the same command again";
    case Status::kEraseFailed: return "the chip erase failed";
    case Status::kPortStillClosed: return "the access port is still closed after the erase and one soft reset";
    case Status::kNotBlank: return "not blank after the erase";
    case Status::kHaltFailed: return "the core would not halt";
    case Status::kBaseWords: return "the stored base could not be walked: it is not the file that was judged";
    case Status::kWriteFailed: return "a write through the door failed";
    case Status::kVerifyFailed: return "a read-back failed";
    case Status::kVerifyMismatch: return "a word read back differs from the base";
    case Status::kResetFailed: return "the pin reset was refused";
    case Status::kLookFailed: return "no debug port after the reset (the base was verified before it)";
  }
  return "?";
}

// The wire, the debug port, the nRF52 session and the door: made on run()'s stack, torn down
// on every exit path by the destructor (close the session, release the lines).
struct Provision::Session {
  Session(Provision& owner, nrf52::ProvisionGrant&& grant)
      : p(owner), wire(owner.pins_.swd(), owner.clock_), dp(wire), nrf(dp, owner.clock_),
        door(nrf, dp, owner.clock_, static_cast<nrf52::ProvisionGrant&&>(grant)) {}
  ~Session() { release(); }
  void release() {
    if (nrf.isOpen()) (void)nrf.close();
    if (held) {
      p.pins_.swdRelease();
      held = false;
    }
  }
  Provision& p;
  swd::Wire wire;
  swd::Dp dp;
  nrf52::Nrf52 nrf;
  nrf52::ProvisionDoor door;
  bool held = false;
};

Provision::Provision(board::RadioPins& pins, board::SlotLock& slots, interfaces::Clock& clock,
                     link::client::Client& radio, image::Store* store,
                     update::BatterySource* battery, update::RadioUpdate* update,
                     supervisor::Supervisor* supervisor)
    : pins_(pins), slots_(slots), clock_(clock), radio_(radio), store_(store), battery_(battery),
      update_(update), supervisor_(supervisor) {}

namespace {

std::uint32_t low32(std::uint64_t v) { return static_cast<std::uint32_t>(v); }

void copyText(char* out, std::size_t size, const char* text) {
  std::size_t i = 0;
  for (; text[i] != 0 && i + 1 < size; ++i) out[i] = text[i];
  out[i] = 0;
}

char hexDigit(unsigned v) { return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10)); }

bool sameText(const char* a, const char* b) {
  std::size_t i = 0;
  for (; a[i] != 0 && b[i] != 0; ++i) {
    if (a[i] != b[i]) return false;
  }
  return a[i] == 0 && b[i] == 0;
}

}  // namespace

// ---------------------------------------------------------------------------------- the steps

void Provision::step(Step s, Result* r, StepListener* listener) {
  const std::uint64_t now = clock_.nowMs();
  r->step_ms[static_cast<std::size_t>(s)] = low32(now - step_began_ms_);
  step_began_ms_ = now;
  if (listener != nullptr) listener->onStep(s, *r);
}

// A step that failed: the status and where, and its time. The listener is not told - a refusal
// is the caller's to print once, after run() returns, in one `error:` line.
bool Provision::fail(Result* r, Status status, Step s, StepListener* listener) {
  (void)listener;
  r->status = status;
  r->ended = s;
  step(s, r, nullptr);
  return false;
}

// -- step 1 --------------------------------------------------------------------------------------

// The base, checked again (the store is flash): 5p's seven rules against this build's region
// for the board, then the bytes against the record's hash. The reader's runs are what step 5
// walks, so the file written is the file judged.
bool Provision::checkStore(board::Slot slot, Result* r) {
  const std::uint64_t t0 = clock_.nowMs();
  if (store_ == nullptr) {
    r->status = Status::kNoStore;
    return false;
  }
  const flasher::BoardProfile* profile = flasher::profileForSlot(slot);
  if (profile == nullptr) {
    r->status = Status::kNoProfile;
    return false;
  }
  image::Record record;
  if (store_->read(image::Which::kBase, slot, &record) != image::StoreError::kNone || !record.present) {
    r->status = Status::kNoBase;
    return false;
  }
  r->base_present = true;
  r->base = record.base;
  for (std::size_t i = 0; i < image::kSha256Size; ++i) r->base_sha256[i] = record.sha256[i];
  if (store_->open(image::Which::kBase, slot, &file_) != image::StoreError::kNone) {
    r->status = Status::kNoBase;
    return false;
  }
  judged_ = reader_.read(file_, profile->app.begin, profile->app.end);
  if (!judged_.ok()) {
    copyText(r->why, sizeof r->why, judged_.why);
    r->status = Status::kBaseRefused;
    return false;
  }
  r->base = judged_.summary;
  if (store_->verify(image::Which::kBase, slot) != image::StoreError::kNone) {
    r->status = Status::kBaseUnreadable;
    return false;
  }
  r->base_check_ms = low32(clock_.nowMs() - t0);
  return true;
}

// The radio update's rule (core/update/radio_update.cpp, check): low or falling is a refusal;
// no reading at all is accepted, as --first-install accepts it - the gauge is on the radio's
// I2C and a factory RUI3 radio answers no POWER_GET.
bool Provision::checkBattery(Result* r) {
  update::BatteryReading reading;
  if (battery_ == nullptr || !battery_->read(&reading)) return true;
  r->battery_known = true;
  r->battery_mv = reading.millivolts;
  if (reading.millivolts < update::kBatteryMinMv) {
    r->status = Status::kBatteryLow;
    return false;
  }
  if (reading.trend_known && reading.mv_per_hour <= update::kBatteryFallingMvPerHour) {
    r->status = Status::kBatteryFalling;
    return false;
  }
  return true;
}

bool Provision::precheck(board::Slot slot, Result* r) {
  if (!checkStore(slot, r)) return false;
  if (pins_.killDriven()) {
    r->status = Status::kKillAsserted;
    return false;
  }
  if (update_ != nullptr && update_->active()) {
    r->status = Status::kUpdateRunning;
    return false;
  }
  if (supervisor_ != nullptr && supervisor_->climbing()) {
    r->status = Status::kSupervisorBusy;
    return false;
  }
  if (slots_.held() && slots_.slot() != slot) {
    r->status = Status::kSlotBusy;
    return false;
  }
  // The link, once: HELLO (and IDENTITY when it answers) - the session the battery is read
  // over, and the answer step 2 names a closed radio by.
  askLink(r);
  return checkBattery(r);
}

// -- step 2 --------------------------------------------------------------------------------------

// S-5a: a closed port is named `locked` when the link answers no HELLO, and `locked:<key16>`
// when an application answers - <key16> the first 16 hex digits of the public key IDENTITY
// returns, upper-cased here so that it compares with the grant's. Asked in step 1, before the
// battery (which is read over the same session) and before the debug port is touched.
bool Provision::askLink(Result* r) {
  r->link_asked = true;
  const link::client::Answer<link::HelloResponse> hello = radio_.hello();
  if (!hello.ok()) return true;
  const link::client::Answer<link::IdentityResponse> id = radio_.identity();
  if (!id.ok() || id.message.pubkey.data == nullptr || id.message.pubkey.size < nrf52::kKey16Digits / 2) {
    return true;  // an application that cannot say who it is has no name to lose
  }
  r->application_answered = true;
  for (std::size_t i = 0; i < nrf52::kKey16Digits / 2; ++i) {
    const std::uint8_t b = id.message.pubkey.data[i];
    r->key16[2 * i] = hexDigit(b >> 4);
    r->key16[2 * i + 1] = hexDigit(b & 0x0F);
  }
  r->key16[nrf52::kKey16Digits] = 0;
  return true;
}

// The line to type, from what was found: the DEVICEID as `rak swd id` prints it, or the
// `locked` form the link decides.
void Provision::suggest(Result* r) {
  if (!r->attached) {
    r->next[0] = 0;
    return;
  }
  if (r->port_open_at_attach) {
    if (!r->identified) {
      r->next[0] = 0;
      return;
    }
    std::snprintf(r->next, sizeof r->next, "rak provision %08lx%08lx confirm",
                  static_cast<unsigned long>(r->identity.device_id[1]),
                  static_cast<unsigned long>(r->identity.device_id[0]));
    return;
  }
  if (r->application_answered) {
    std::snprintf(r->next, sizeof r->next, "rak provision locked:%s confirm", r->key16);
  } else {
    copyText(r->next, sizeof r->next, "rak provision locked confirm");
  }
}

bool Provision::attach(Session& s, board::Slot slot, Result* r, bool with_grant) {
  const board::Outcome acquired = pins_.swdAcquire();
  if (acquired != board::Outcome::kOk) {
    r->pins = acquired;
    r->status = Status::kPins;
    return false;
  }
  s.held = true;
  const nrf52::Result opened = s.nrf.open(slot);
  if (s.dp.connected()) r->dpidr = s.dp.dpidr();
  r->ctrl_ap_idr = s.nrf.ctrlApIdr();
  if (!opened.ok() && opened.status != nrf52::Status::kProtected) {
    r->nrf = opened;
    r->status = opened.status == nrf52::Status::kNotNrf52840 ? Status::kNotNrf52840 : Status::kSwd;
    return false;
  }
  r->attached = true;
  r->port_open_at_attach = s.nrf.protection() == nrf52::Protection::kOpen;
  if (r->port_open_at_attach) {
    r->identified = s.nrf.identified();
    r->identity = s.nrf.identity();
    if (!r->identified) {
      r->status = Status::kNotNrf52840;
      return false;
    }
    r->variant = lookupVariant(r->identity.variant_raw, table_, table_size_);
  }
  suggest(r);
  if (!with_grant) return true;
  r->grant = s.door.grantKind();
  if (!nameMatches(r)) return false;
  // The door checks the port half again, against the chip, and consumes the grant.
  const nrf52::DoorResult d = s.door.open();
  r->door = d;
  if (!d.ok()) {
    r->status = d.status == nrf52::DoorStatus::kWrongDevice     ? Status::kWrongDevice
                : d.status == nrf52::DoorStatus::kGrantMismatch ? Status::kGrantMismatch
                : d.status == nrf52::DoorStatus::kGrantMalformed ? Status::kGrantMalformed
                                                                  : Status::kDoor;
    return false;
  }
  return true;
}

// The typed radio against what was found - before the door, so that the refusal names the
// reason in the sequence's own words; the door says it again in its own.
bool Provision::nameMatches(Result* r) {
  using nrf52::GrantKind;
  if (r->grant == GrantKind::kNone) {
    r->status = Status::kGrantMalformed;
    return false;
  }
  if (r->port_open_at_attach) {
    if (r->grant != GrantKind::kDeviceId) {
      r->status = Status::kGrantMismatch;
      return false;
    }
    return true;  // the DEVICEID itself is the door's to compare (kWrongDevice comes back as kDoor)
  }
  if (r->grant == GrantKind::kDeviceId) {
    r->status = Status::kGrantMismatch;
    return false;
  }
  if (r->application_answered) {
    if (r->grant != GrantKind::kLockedWithKey) {
      r->status = Status::kApplicationAnswers;
      return false;
    }
    return true;  // the key is compared below, once the grant's text is at hand
  }
  if (r->grant == GrantKind::kLockedWithKey) {
    r->status = Status::kNoApplication;
    return false;
  }
  return true;
}

// -- step 3 --------------------------------------------------------------------------------------

bool Provision::identifyAndLookUp(Session& s, Result* r) {
  r->identified = s.nrf.identified();
  r->identity = s.nrf.identity();
  if (!r->identified) {
    r->status = Status::kNotNrf52840;
    return false;
  }
  r->variant = lookupVariant(r->identity.variant_raw, table_, table_size_);
  if (r->variant.kind == VariantKind::kUnknown) {
    r->status = r->erased ? Status::kVariantUnknownErased : Status::kVariantUnknown;
    return false;
  }
  r->kind = r->variant.kind == VariantKind::kImproved ? nrf52::ApprotectKind::kImproved
                                                      : nrf52::ApprotectKind::kLegacy;
  return true;
}

// -- step 4 --------------------------------------------------------------------------------------

bool Provision::erase(Session& s, Result* r) {
  nrf52::DoorResult d = s.door.eraseAll();
  r->erased = true;  // ERASEALL was written: whatever came back, the radio starts from erased
  r->door = d;
  if (!d.ok()) {
    r->status = Status::kEraseFailed;
    return false;
  }
  r->port_open_after_erase = d.port_open;
  if (!d.port_open) {
    // S-5a: one CTRL-AP soft reset, and attach again; still closed -> stop.
    d = s.door.softReset();
    r->door = d;
    r->soft_reset_done = true;
    if (!d.ok()) {
      r->status = Status::kEraseFailed;
      return false;
    }
    if (!d.port_open) {
      r->status = Status::kPortStillClosed;
      return false;
    }
  }
  return true;
}

// -- step 5 --------------------------------------------------------------------------------------

// One run of contiguous words through the door: flash below the application region or from the
// bootloader up (the door refuses anything else), UICR for the words above 0x10001000.
bool Provision::flushRun(Session& s, Result* r, std::uint64_t at, std::size_t n) {
  if (n == 0) return true;
  // A tick of real blocking before each run, between transfers and never inside one (task
  // p8-01, core/swd/idle_pause.h). A run of kRunWords is a tenth of a second at 125 kHz and a
  // base is a hundred and eighty of them, so without this the whole write - and then the whole
  // verify after it - is one unbroken stretch of busy waiting, which the task watchdog ends.
  swd::letIdleTaskRun(clock_);
  const std::uint32_t addr = static_cast<std::uint32_t>(at);
  const bool uicr = at >= image::kUicrFirst;
  const nrf52::DoorResult d = uicr ? s.door.writeUicr(addr, run_, n) : s.door.writeFlash(addr, run_, n);
  r->door = d;
  if (!d.ok()) {
    r->status = Status::kWriteFailed;
    return false;
  }
  if (uicr) {
    r->uicr_words += static_cast<std::uint32_t>(n);
  } else {
    r->flash_words += static_cast<std::uint32_t>(n);
    r->flash_runs++;
  }
  return true;
}

bool Provision::writeBase(Session& s, Result* r) {
  words_.begin(file_, judged_.runs);
  std::uint64_t run_at = 0;
  std::size_t n = 0;
  std::uint64_t address = 0;
  std::uint32_t word = 0;
  while (words_.take(&address, &word)) {
    const bool contiguous = n > 0 && address == run_at + 4u * n;
    const bool same_kind = n == 0 || ((run_at >= image::kUicrFirst) == (address >= image::kUicrFirst));
    if (!contiguous || !same_kind || n == kRunWords) {
      if (!flushRun(s, r, run_at, n)) return false;
      run_at = address;
      n = 0;
    }
    run_[n++] = word;
  }
  if (words_.failed()) {
    r->status = Status::kBaseWords;
    return false;
  }
  if (!flushRun(s, r, run_at, n)) return false;
  // Last of all, UICR.APPROTECT with the table's kind: 0x5A on a revision 3 part, nothing on a
  // legacy one (the door never takes a value).
  const nrf52::DoorResult d = s.door.writeApprotect(r->kind);
  r->door = d;
  if (!d.ok()) {
    r->status = Status::kWriteFailed;
    return false;
  }
  r->approtect_written = r->kind == nrf52::ApprotectKind::kImproved ? kApprotectHwDisabledWord : kErasedWord;
  return true;
}

// -- step 6 --------------------------------------------------------------------------------------

bool Provision::compareRun(Session& s, Result* r, std::uint64_t at, std::size_t n) {
  if (n == 0) return true;
  swd::letIdleTaskRun(clock_);  // task p8-01: the verify is as long as the write
  const std::uint32_t addr = static_cast<std::uint32_t>(at);
  const nrf52::Result read = s.nrf.read(addr, back_, n);
  if (!read.ok()) {
    r->nrf = read;
    r->status = Status::kVerifyFailed;
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (back_[i] != run_[i]) {
      r->mismatch_address = addr + 4u * static_cast<std::uint32_t>(i);
      r->mismatch_expected = run_[i];
      r->mismatch_actual = back_[i];
      r->status = Status::kVerifyMismatch;
      return false;
    }
  }
  r->verified_words += static_cast<std::uint32_t>(n);
  return true;
}

bool Provision::verifyBase(Session& s, Result* r) {
  words_.begin(file_, judged_.runs);
  std::uint64_t run_at = 0;
  std::size_t n = 0;
  std::uint64_t address = 0;
  std::uint32_t word = 0;
  while (words_.take(&address, &word)) {
    const bool contiguous = n > 0 && address == run_at + 4u * n;
    if (!contiguous || n == kRunWords) {
      if (!compareRun(s, r, run_at, n)) return false;
      run_at = address;
      n = 0;
    }
    run_[n++] = word;
  }
  if (words_.failed()) {
    r->status = Status::kBaseWords;
    return false;
  }
  if (!compareRun(s, r, run_at, n)) return false;
  // UICR.APPROTECT, read through the door's blank check (the only reader of that register
  // outside the door is the door), against the table's value.
  nrf52::BlankCheck bc;
  const nrf52::DoorResult d = s.door.blankCheck(&bc);
  r->door = d;
  if (!d.ok()) {
    r->status = Status::kVerifyFailed;
    return false;
  }
  r->approtect_read = bc.approtect;
  if (bc.approtect != r->approtect_written) {
    r->mismatch_address = 0;
    r->mismatch_expected = r->approtect_written;
    r->mismatch_actual = bc.approtect;
    r->status = Status::kVerifyMismatch;
    return false;
  }
  return true;
}

// -- step 7 --------------------------------------------------------------------------------------

bool Provision::resetAndLook(Session& s, board::Slot slot, Result* r) {
  // The halt released and the session closed, so that the radio runs its bootloader after the
  // reset rather than sitting halted; a core that will not answer the release is reset anyway.
  (void)s.nrf.run();
  s.release();
  // ARCHITECTURE §2 rule 2: the pulse waits out the spacing rather than violating it.
  r->reset_wait_ms = pins_.msUntilResetAllowed();
  const board::Outcome pulsed = pins_.resetPulse();
  if (pulsed != board::Outcome::kOk) {
    r->pins = pulsed;
    r->status = Status::kResetFailed;
    return false;
  }
  radio_.radioWasReset();  // the link's session, the boot_id, whatever it was running: gone
  clock_.delayMs(kBootloaderSettleMs);
  // And look: the port open or closed after the bootloader has run (spec/SYSTEM.md §10.2).
  const board::Outcome acquired = pins_.swdAcquire();
  if (acquired != board::Outcome::kOk) {
    r->pins = acquired;
    r->status = Status::kLookFailed;
    return false;
  }
  s.held = true;
  const nrf52::Result opened = s.nrf.open(slot);
  if (!opened.ok() && opened.status != nrf52::Status::kProtected) {
    r->nrf = opened;
    r->status = Status::kLookFailed;
    return false;
  }
  r->port_open_after_reset = opened.ok() && s.nrf.protection() == nrf52::Protection::kOpen;
  if (r->port_open_after_reset) {
    nrf52::BlankCheck bc;
    const nrf52::DoorResult d = s.door.blankCheck(&bc);
    if (!d.ok()) {
      r->door = d;
      r->status = Status::kLookFailed;  // the port answered and then did not: said, like no port
      return false;
    }
    r->nrffw0_after = bc.nrffw0;
  }
  s.release();
  return true;
}

// ------------------------------------------------------------------------------------- check

Result Provision::check(board::Slot slot) {
  Result r;
  const std::uint64_t t0 = clock_.nowMs();
  step_began_ms_ = t0;
  r.ended = Step::kCheck;
  const board::SlotHold hold(slots_, slot);
  if (!hold.ok()) {
    r.status = Status::kSlotBusy;
    r.check_status = Status::kSlotBusy;
    return r;
  }
  // Every finding of step 1 is reported, not only the first - `check` is how a person learns
  // what to push and what to type - and the radio is looked at whatever the store said.
  // check_status keeps the first refusal step 1 would make; status is the attach's.
  if (!checkStore(slot, &r)) r.check_status = r.status;
  r.status = Status::kOk;
  askLink(&r);
  if (!checkBattery(&r) && r.check_status == Status::kOk) r.check_status = r.status;
  r.status = Status::kOk;
  if (pins_.killDriven() && r.check_status == Status::kOk) r.check_status = Status::kKillAsserted;
  if (update_ != nullptr && update_->active() && r.check_status == Status::kOk) {
    r.check_status = Status::kUpdateRunning;
  }
  if (supervisor_ != nullptr && supervisor_->climbing() && r.check_status == Status::kOk) {
    r.check_status = Status::kSupervisorBusy;
  }
  step(Step::kCheck, &r, nullptr);
  nrf52::ProvisionGrant none = nrf52::grantFromTypedCommand("");
  Session s(*this, static_cast<nrf52::ProvisionGrant&&>(none));
  r.ended = Step::kAttach;
  if (attach(s, slot, &r, false) && r.port_open_at_attach) {
    if (r.variant.kind == VariantKind::kUnknown) r.status = Status::kVariantUnknown;
  }
  step(Step::kAttach, &r, nullptr);
  r.total_ms = low32(clock_.nowMs() - t0);
  return r;
}

// --------------------------------------------------------------------------------------- run

Result Provision::run(board::Slot slot, nrf52::ProvisionGrant&& grant, StepListener* listener) {
  Result r;
  const std::uint64_t t0 = clock_.nowMs();
  step_began_ms_ = t0;
  // The slot, taken and held for the whole command (task p1-06).
  const board::SlotHold hold(slots_, slot);
  if (!hold.ok()) {
    fail(&r, Status::kSlotBusy, Step::kCheck, listener);
    r.total_ms = low32(clock_.nowMs() - t0);
    return r;
  }
  // The grant's key, kept before the grant moves into the door and is spent there: the sequence
  // compares it against what IDENTITY said (the door only checks the port half).
  char typed_key[nrf52::kKey16Digits + 1] = {};
  copyText(typed_key, sizeof typed_key,
           grant.kind() == nrf52::GrantKind::kLockedWithKey ? grant.key16() : "");
  Session s(*this, static_cast<nrf52::ProvisionGrant&&>(grant));
  for (;;) {
    // 1. check
    if (!precheck(slot, &r)) {
      fail(&r, r.status, Step::kCheck, listener);
      break;
    }
    step(Step::kCheck, &r, listener);
    // 2. attach, and the name against the radio
    if (!attach(s, slot, &r, true)) {
      fail(&r, r.status, Step::kAttach, listener);
      break;
    }
    if (!r.port_open_at_attach && r.application_answered && !sameText(typed_key, r.key16)) {
      fail(&r, Status::kWrongKey, Step::kAttach, listener);
      break;
    }
    step(Step::kAttach, &r, listener);
    // 3. the variant, on an open radio: before anything is erased
    if (r.port_open_at_attach) {
      if (!identifyAndLookUp(s, &r)) {
        fail(&r, r.status, Step::kVariant, listener);
        break;
      }
      step(Step::kVariant, &r, listener);
    }
    // From here the radio is going to be erased: the supervisor is told first, so that a
    // Walter which restarts in the middle leaves the silent radio alone.
    if (supervisor_ != nullptr) supervisor_->markAwaitingFirstInstall();
    // 4. erase, the one soft reset if needed, identify, blank check, halt
    if (!erase(s, &r)) {
      fail(&r, r.status, Step::kErase, listener);
      break;
    }
    if (!r.port_open_at_attach) {
      // 3, on a closed radio: the identity is readable only now
      if (!identifyAndLookUp(s, &r)) {
        fail(&r, r.status, Step::kVariant, listener);
        break;
      }
      step(Step::kVariant, &r, listener);
    } else {
      r.identified = s.nrf.identified();
      if (!r.identified) {
        fail(&r, Status::kNotNrf52840, Step::kErase, listener);
        break;
      }
    }
    nrf52::BlankCheck bc;
    r.door = s.door.blankCheck(&bc);
    if (!r.door.ok()) {
      fail(&r, Status::kEraseFailed, Step::kErase, listener);
      break;
    }
    r.blank = bc.blank;
    if (!bc.blank) {
      fail(&r, Status::kNotBlank, Step::kErase, listener);
      break;
    }
    r.nrf = s.nrf.halt();
    if (!r.nrf.ok()) {
      fail(&r, Status::kHaltFailed, Step::kErase, listener);
      break;
    }
    step(Step::kErase, &r, listener);
    // 5. write
    if (!writeBase(s, &r)) {
      fail(&r, r.status, Step::kWrite, listener);
      break;
    }
    step(Step::kWrite, &r, listener);
    // 6. verify
    if (!verifyBase(s, &r)) {
      fail(&r, r.status, Step::kVerify, listener);
      break;
    }
    step(Step::kVerify, &r, listener);
    // 7. reset and look
    if (!resetAndLook(s, slot, &r)) {
      fail(&r, r.status, Step::kReset, listener);
      break;
    }
    step(Step::kReset, &r, listener);
    r.ended = Step::kDone;
    break;
  }
  if (r.ok()) suggest(&r);
  r.total_ms = low32(clock_.nowMs() - t0);
  return r;
}

}  // namespace provision
}  // namespace core
