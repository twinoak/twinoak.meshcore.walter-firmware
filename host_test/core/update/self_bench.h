// The Walter's own update on a bench (task p2-04): one *world* that outlives a Walter - the two
// application slots with the bootloader's rules (fakes/fake_own_update.h), the image store's and
// the journal's flash, retained storage, the scripted radio, one clock - and a *Walter* built
// over it that can be destroyed and built again, which is what a restart is. The bootloader's
// rules run at `reboot()`: a slot that was pending-verify is aborted, the slot with the newest
// valid entry is selected, a new one becomes pending. So "a trial is one boot" is what the fake
// enforces, as the real bootloader does, and the machine under test only has to notice.
//
// A new image reaches the other slot the way it does on a unit: the framed transfer of
// core/image/receive.h, frames built with the link's own codec, through all six checks.
#pragma once

#include <doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/board/radio_pins.h"
#include "core/board/slot.h"
#include "core/console/console.h"
#include "core/health/coredump.h"
#include "core/health/health.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/link/client/client.h"
#include "core/link/client/port.h"
#include "core/link/codec/frame.h"
#include "core/link/codec/packet.h"
#include "core/update/journal.h"
#include "core/update/self_update.h"
#include "core/update/update_bench.h"
#include "core/image/store_bench.h"
#include "fakes/capture.h"
#include "fakes/fake_own_update.h"
#include "fakes/fake_pins.h"
#include "fakes/fake_retained.h"
#include "sim/scripted_radio.h"
#include "sim/sim_flash.h"

namespace bench {

namespace upd = core::update;
using upd::Condition;
using upd::SelfBoot;
using upd::SelfPayload;
using upd::SelfReason;
using upd::SelfState;
using upd::SelfStep;

// The uplink a test sets: reached, failed, or no server configured.
class FakeUplink final : public upd::UplinkCheck {
 public:
  upd::UplinkVerdict verdict = upd::UplinkVerdict::kReached;
  int checks = 0;
  upd::UplinkVerdict check() override {
    ++checks;
    return verdict;
  }
  const char* serverHost() const override { return host_; }
  std::uint16_t serverPort() const override { return port_; }
  // `server.url` at the console (task p3-03).
  void setServer(const char* host, std::uint16_t port) override {
    host_ = host != nullptr ? host : "";
    port_ = port;
  }

 private:
  const char* host_ = "server.example";
  std::uint16_t port_ = 443;
};

struct SelfWorld {
  StoreBench store;  // the image store over a simulated flash, its clock, its wall clock, the test key
  fakes::FakeClock& clock = store.clock;
  sim::SimFlash journal_flash{upd::kJournalPartitionBytes};
  // The `coredump` partition (task p11-03): in the world and not in the Walter, because a dump
  // outlives the boot that wrote it - that is the whole point of it - and because the boot that
  // reads it is the one after the trial.
  sim::SimFlash coredump_flash{0x10000};
  fakes::FakeOwnUpdate own{64 * 1024};
  fakes::FakeRetained retained;
  sim::ScriptedRadio radio{clock};

  // What the Walter was asked to do that a board would do by itself.
  bool restart_asked = false;

  SelfWorld() {
    // The radio answers HELLO as the fixtures do; `radio.deaf = true` is a radio that is gone.
    radio.fw_version = "0.3.0+golden0";
    radio.board = "rak4630-single";
  }
};

// One life of the Walter over the world.
struct SelfWalter {
  SelfWorld& w;
  fakes::FakePins pins{w.clock};
  core::board::RadioPins board{pins, w.clock, w.retained};
  core::board::SingleTarget single;
  core::board::SlotLock slots{single};
  core::link::client::LinkPort port{board, w.radio};
  core::link::client::Client client{port, w.clock, w.store.wall, slots};
  upd::Journal journal{w.journal_flash};
  FakeBattery battery;
  FakeUplink uplink;
  fakes::CaptureOut out;
  fakes::FakeRestart restart;
  core::health::Health health{w.retained, w.clock};
  core::health::CoreDump dumps{w.coredump_flash};
  core::image::Receiver receiver{w.store.store, w.store.verdict, out, w.clock};
  upd::SelfUpdate self{w.own,   w.store.store, journal, client, uplink,
                       w.clock, restart,       battery, health, &dumps};
  // The console over this Walter. The eighteen nullptrs are every seam this bench has no use for
  // (the supervisor, the uplink, the settings, the duty cycle, the agent's four, ...); the last
  // argument is the `coredump` partition (task p11-03), so that `coredump` and `self status`'s
  // evidence lines are tested here, where the trial they are about happens.
  core::console::Console console{out, board, health, w.clock, restart, client, slots,
                                 &w.store.store, &receiver, nullptr, &journal, &self,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                 &dumps};
  core::link::FrameReceiver rx;

  explicit SelfWalter(SelfWorld& world, core::health::ResetReason why = core::health::ResetReason::kSoftware)
      : w(world) {
    w.restart_asked = false;
    w.journal_flash.powerOn();
    w.coredump_flash.powerOn();
    w.store.flash.powerOn();
    w.own.powerOn();
    REQUIRE(w.store.store.begin() == StoreError::kNone);
    pins.record_log = false;
    health.begin(why, core::health::WakeCause::kNotFromSleep);
    w.clock.advance(core::board::kKillTiming.settle_ms);
    board.safe();
    REQUIRE(journal.begin() == upd::JournalError::kNone);
    port.open();
  }
  ~SelfWalter() { CHECK(pins.violations.empty()); }

  // What main does at boot: the machine reads the journal and the slots, then the loop turns.
  SelfBoot boot() {
    const SelfBoot b = self.begin();
    self.mainLoopReached();
    return b;
  }

  // Steps the trial until it ends (confirmed or rolled back), or `max_ms` of simulated time pass.
  SelfStep runTrial(std::uint64_t max_ms = upd::kTrialWindowMs + 60000) {
    const std::uint64_t until = w.clock.nowMs() + max_ms;
    SelfStep s;
    while (self.onTrial() && w.clock.nowMs() < until) {
      s = self.step();
      if (s.changed) break;
      w.clock.delayMs(100);
    }
    return s;
  }

  // Whether this life asked for a restart: the switch's, or a rejection's (the fake's rule).
  bool restartAsked() const { return restart.asked > 0 || w.own.restart_asked; }

  std::string runLine(const char* line) {
    out.clear();
    console.runLine(line);
    return out.text;
  }

  // -- the laptop at the other end of the port: one frame in, the answer out ----------------
  struct Reply {
    bool got = false;
    std::uint8_t status = 0xFF;
    std::uint32_t committed = 0;
    std::string text;
  };

  Reply send(std::uint8_t type, std::uint8_t seq, const std::uint8_t* payload, std::size_t size) {
    out.clear();
    std::uint8_t frame[core::link::kMaxWrite];
    const std::size_t wrote = core::link::buildFrame(type, seq, core::link::bytes(payload, size), frame, sizeof frame);
    REQUIRE(wrote > 0);
    receiver.feed(frame, wrote);
    Reply reply;
    for (char c : out.text) {
      if (rx.push(static_cast<std::uint8_t>(c)) != core::link::FrameReceiver::Fed::kPacket) continue;
      const core::link::Packet& packet = rx.packet();
      REQUIRE(packet.payload.size >= 6);
      reply.got = true;
      reply.status = packet.payload.data[0];
      reply.committed = static_cast<std::uint32_t>(packet.payload.data[2]) |
                        (static_cast<std::uint32_t>(packet.payload.data[3]) << 8) |
                        (static_cast<std::uint32_t>(packet.payload.data[4]) << 16) |
                        (static_cast<std::uint32_t>(packet.payload.data[5]) << 24);
      reply.text.assign(reinterpret_cast<const char*>(packet.payload.data) + 6, packet.payload.size - 6);
    }
    return reply;
  }

  // One file, in chunks of 240, the file frame first; the reply to the last frame sent is
  // returned, and a refusal stops the sending there.
  Reply sendFile(std::uint8_t kind, const std::uint8_t* data, std::size_t size) {
    namespace transfer = core::image::transfer;
    const std::uint8_t header[5] = {kind, static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8),
                                    static_cast<std::uint8_t>(size >> 16), static_cast<std::uint8_t>(size >> 24)};
    Reply reply = send(transfer::kFile, 0, header, sizeof header);
    if (!reply.got || reply.status != transfer::kOk) return reply;
    std::uint8_t seq = 0;
    for (std::size_t at = 0; at < size;) {
      const std::size_t n = size - at < 240 ? size - at : 240;
      reply = send(transfer::kData, seq, data + at, n);
      if (!reply.got || reply.status != transfer::kOk) return reply;
      at += n;
      ++seq;
    }
    return send(transfer::kEnd, seq, nullptr, 0);
  }

  // The whole push of a release of this firmware into the other slot, as walterctl does it:
  // `self receive`, then the manifest, the signature, the image, the commit. Returns the last
  // reply's text; `stored` says whether the Walter kept it.
  std::string pushSelf(const Candidate& image, bool* stored) {
    namespace transfer = core::image::transfer;
    *stored = false;
    const std::string began = runLine("self receive");
    if (!receiver.active()) return began;
    Reply reply = sendFile(transfer::kKindManifest, image.manifest.data(), image.manifest.size());
    if (!receiver.active()) return reply.text;
    reply = sendFile(transfer::kKindSignature, reinterpret_cast<const std::uint8_t*>(image.signature.data()),
                     image.signature.size());
    if (!receiver.active()) return reply.text;
    reply = sendFile(transfer::kKindImage, image.file.data(), image.file.size());
    if (!receiver.active()) return reply.text;
    reply = send(transfer::kCommit, 0, nullptr, 0);
    *stored = reply.got && reply.status == transfer::kDone;
    return reply.text;
  }
};

// A unit, with the Walter alive over the world; reboot() is the bootloader's rules and a new life.
struct SelfUnit {
  SelfWorld world;
  std::unique_ptr<SelfWalter> walter;

  SelfUnit() : walter(std::make_unique<SelfWalter>(world, core::health::ResetReason::kPowerOn)) {}

  // `why` is the reset the next boot reports (task p11-03): the default is the software reset a
  // switch or a rejection asks for, and a trial that was killed by a panic boots with
  // ResetReason::kPanic, which is what 0.1.3's rollback found.
  void reboot(core::health::ResetReason why = core::health::ResetReason::kSoftware) {
    walter.reset();
    world.own.reboot();
    walter = std::make_unique<SelfWalter>(world, why);
  }

  // Receives an image into the other slot and switches to it: the two console commands, as a
  // person would type them. Returns whether the switch asked for the restart.
  bool receiveAndSwitch(const Candidate& image, bool radio_down = false) {
    bool stored = false;
    const std::string text = walter->pushSelf(image, &stored);
    REQUIRE_MESSAGE(stored, text);
    walter->runLine(radio_down ? "self switch radio-down" : "self switch");
    return walter->restartAsked();
  }

  // The last self-update entry of the journal, decoded.
  SelfPayload lastEntry() const {
    upd::Entry e;
    SelfPayload p;
    REQUIRE(walter->journal.lastOf(upd::kind::kSelfUpdate, &e));
    REQUIRE(upd::decode(e, &p));
    return p;
  }

  // Every self-update entry, oldest first, as (state, reason).
  std::vector<std::pair<SelfState, SelfReason>> story() const {
    std::vector<std::pair<SelfState, SelfReason>> out;
    upd::Cursor c;
    upd::Entry e;
    bool more = walter->journal.first(&c, &e);
    while (more) {
      SelfPayload p;
      if (upd::decode(e, &p)) out.insert(out.begin(), {p.state, p.reason});
      more = walter->journal.previous(&c, &e);
    }
    return out;
  }
};

}  // namespace bench
