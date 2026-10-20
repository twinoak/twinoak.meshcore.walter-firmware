// The grant (task p5-02; spec/SYSTEM.md S-5a): what a person typed after `rak provision`, in
// S-5a's grammar and nothing else - failure cases first. The door's half, the grant against the
// chip, is test_provision_door.cpp.
#include <doctest.h>

#include <string>

#include "core/nrf52/provision_grant.h"

using core::nrf52::GrantKind;
using core::nrf52::grantFromTypedCommand;
using core::nrf52::ProvisionGrant;

TEST_CASE("not a grant: empty, a word, 15 or 17 hex digits, a digit that is not hex, LOCKED in capitals") {
  const char* const bad[] = {
      "",
      "confirm",
      "0BADF00DDEADBEE",      // 15
      "0BADF00DDEADBEEF0",    // 17
      "0BADF00DDEADBEEG",     // G
      "0BADF00D DEADBEEF",    // a space
      "LOCKED",               // S-5a writes the word in lower case; the DEVICEID is the case-free part
      "Locked",
      "locked:",
      "locked:0123456789ABCDE",    // 15
      "locked:0123456789ABCDEF0",  // 17
      "locked:0123456789ABCDEG",
      "locked 0123456789ABCDEF",
      "unlocked",
  };
  for (const char* text : bad) {
    const ProvisionGrant g = grantFromTypedCommand(text);
    CHECK(g.kind() == GrantKind::kNone);
    CHECK(!g.valid());
    CHECK(!g.spent());
    if (g.valid()) MESSAGE("accepted, and should not have been: " << std::string(text));
  }
  CHECK(grantFromTypedCommand(nullptr).kind() == GrantKind::kNone);
}

TEST_CASE("a DEVICEID: 16 hex digits, the first eight DEVICEID[1], read without regard to case") {
  const ProvisionGrant upper = grantFromTypedCommand("0BADF00DDEADBEEF");
  CHECK(upper.kind() == GrantKind::kDeviceId);
  CHECK(upper.valid());
  CHECK(upper.deviceId(1) == 0x0BADF00D);
  CHECK(upper.deviceId(0) == 0xDEADBEEF);
  const ProvisionGrant lower = grantFromTypedCommand("0badf00ddeadbeef");
  CHECK(lower.kind() == GrantKind::kDeviceId);
  CHECK(lower.deviceId(1) == 0x0BADF00D);
  CHECK(lower.deviceId(0) == 0xDEADBEEF);
  const ProvisionGrant mixed = grantFromTypedCommand("0BadF00dDeadBeef");
  CHECK(mixed.deviceId(1) == 0x0BADF00D);
  CHECK(mixed.deviceId(0) == 0xDEADBEEF);
  const ProvisionGrant zeros = grantFromTypedCommand("0000000000000000");
  CHECK(zeros.kind() == GrantKind::kDeviceId);
  CHECK(zeros.deviceId(0) == 0);
  CHECK(zeros.deviceId(1) == 0);
  CHECK(std::string(core::nrf52::describe(GrantKind::kDeviceId)) == "a DEVICEID");
}

TEST_CASE("locked, and locked:<key16> with the key kept upper-cased") {
  const ProvisionGrant locked = grantFromTypedCommand("locked");
  CHECK(locked.kind() == GrantKind::kLocked);
  CHECK(locked.valid());
  CHECK(std::string(locked.key16()).empty());
  const ProvisionGrant keyed = grantFromTypedCommand("locked:0123456789abcdef");
  CHECK(keyed.kind() == GrantKind::kLockedWithKey);
  CHECK(std::string(keyed.key16()) == "0123456789ABCDEF");
  const ProvisionGrant keyed_upper = grantFromTypedCommand("locked:FEDCBA9876543210");
  CHECK(std::string(keyed_upper.key16()) == "FEDCBA9876543210");
  // A locked grant carries no DEVICEID.
  CHECK(keyed.deviceId(0) == 0);
  CHECK(keyed.deviceId(1) == 0);
}

TEST_CASE("a grant moved from is spent; the one it moved to is not") {
  ProvisionGrant g = grantFromTypedCommand("0BADF00DDEADBEEF");
  CHECK(!g.spent());
  ProvisionGrant taken(std::move(g));
  CHECK(g.spent());  // NOLINT(bugprone-use-after-move): the point of the test
  CHECK(!taken.spent());
  CHECK(taken.kind() == GrantKind::kDeviceId);
  CHECK(taken.deviceId(1) == 0x0BADF00D);
  ProvisionGrant again = grantFromTypedCommand("locked");
  again = std::move(taken);
  CHECK(taken.spent());  // NOLINT(bugprone-use-after-move)
  CHECK(!again.spent());
  CHECK(again.kind() == GrantKind::kDeviceId);
}
