// The memory access port against the simulated nRF52840's AHB-AP (task p1-04): IDR checked once
// and CSW written, single words, blocks across one and several 1 KB boundaries with TAR rewritten
// at each, what forgetting to rewrite it does (the simulator wraps as the strict reading of ADIv5
// C2.2.2 has it), the same host against the Cortex-M4's 4 KB wrap, errors carrying their address,
// and a protected chip's port refusing memory.
#include <doctest.h>

#include <vector>

#include "sim/nrf52_bench.h"

using core::board::Outcome;
using core::swd::ApIdrFields;
using core::swd::MemAp;
using core::swd::MemResult;
using core::swd::Status;
namespace map = core::swd::mem_ap;

namespace {

struct ApBench : sim::Nrf52Bench {
  MemAp ap{dp, 0};
  void connectAndPowerUp() {
    REQUIRE(acquire() == Outcome::kOk);
    REQUIRE(dp.connect().ok());
    REQUIRE(dp.powerUp().ok());
  }
};

}  // namespace

TEST_CASE("decodeApIdr: the Cortex-M4's AHB-AP and Nordic's CTRL-AP") {
  ApIdrFields f = core::swd::decodeApIdr(0x24770011);
  CHECK(f.revision == 2);
  CHECK(f.designer == core::swd::kDesignerArm);
  CHECK(f.ap_class == map::kIdrClassMemAp);
  CHECK(f.variant == 1);
  CHECK(f.type == 1);
  CHECK(f.reserved_clear);
  f = core::swd::decodeApIdr(0x02880000);
  CHECK(f.revision == 0);
  CHECK(f.designer == core::nrf52::ctrl_ap::kDesignerNordic);  // continuation 2, identity 0x44
  CHECK(f.ap_class == map::kIdrClassNone);
  CHECK(f.reserved_clear);
  CHECK(!core::swd::decodeApIdr(0x24770111).reserved_clear);  // bit 8 set: "unidentifiable"
}

TEST_CASE("open: IDR read once and checked, CSW written for word accesses with single increment") {
  ApBench b;
  b.connectAndPowerUp();
  const MemResult r = b.ap.open();
  CHECK(r.ok());
  CHECK(r.value == 0x24770011);
  CHECK(b.ap.opened());
  CHECK(b.ap.idr() == map::kCortexM4AhbApIdr);
  CHECK(b.ap.counters().idr_reads == 1);
  CHECK(b.ap.counters().csw_writes == 1);
  CHECK((b.chip.csw() & map::kCswSizeMask) == map::kCswSizeWord);
  CHECK((b.chip.csw() & map::kCswAddrIncMask) == map::kCswAddrIncSingle);
  CHECK((b.chip.csw() & map::kCswMasterTypeDebug) != 0);
  CHECK(b.chip.csw() == map::kCswValue);
  CHECK(map::kCswValue == 0x23000012);
  // Read back, DeviceEn (read-only, bit 6) is set on an open chip: the 0x23000052 a debugger shows.
  const core::swd::Result csw = b.dp.readAp(0, map::kCsw);
  CHECK(csw.ok());
  CHECK(csw.value == 0x23000052);
  // Before open, nothing goes on the wire.
  MemAp other(b.dp, 0);
  const std::uint32_t transfers = b.dp.counters().transfers;
  CHECK(other.read32(0x20000000).status == Status::kNoMemAp);
  CHECK(other.write32(0x20000000, 1).status == Status::kNoMemAp);
  CHECK(b.dp.counters().transfers == transfers);
  CHECK(b.target.errors().empty());
  CHECK(b.chip.errors().empty());
  b.release();
}

TEST_CASE("open refuses what is not a memory access port: IDR zero, another class, reserved bits set") {
  ApBench b;
  b.chip.ahb_ap_idr = 0;
  b.connectAndPowerUp();
  CHECK(b.ap.open().status == Status::kNoMemAp);
  CHECK(!b.ap.opened());
  b.chip.ahb_ap_idr = 0x02880000;  // a CTRL-AP's: class 0
  CHECK(b.ap.open().status == Status::kNoMemAp);
  b.chip.ahb_ap_idr = 0x24770111;  // bit 8
  CHECK(b.ap.open().status == Status::kNoMemAp);
  b.chip.ahb_ap_idr = 0x24770011;
  CHECK(b.ap.open().ok());
  CHECK(b.ap.counters().errors == 3);
  b.release();
}

TEST_CASE("read32 and write32: a word in RAM, the TAR cache, an unaligned address refused") {
  ApBench b;
  b.connectAndPowerUp();
  REQUIRE(b.ap.open().ok());
  MemResult r = b.ap.write32(0x20000010, 0xCAFEBABE);
  CHECK(r.ok());
  CHECK(r.address == 0x20000010);
  r = b.ap.read32(0x20000010);
  CHECK(r.ok());
  CHECK(r.value == 0xCAFEBABE);
  CHECK(r.address == 0x20000010);
  // The write left TAR at +4 (auto-increment); the read of 0x20000010 had to write it again -
  // two TAR writes so far. A read of the next word needs none.
  CHECK(b.ap.counters().tar_writes == 2);
  r = b.ap.read32(0x20000014);
  CHECK(r.ok());
  CHECK(r.value == 0);
  CHECK(b.ap.counters().tar_writes == 2);
  const std::uint32_t transfers = b.dp.counters().transfers;
  CHECK(b.ap.read32(0x20000011).status == Status::kBadAddress);
  CHECK(b.ap.write32(0x20000002, 0).status == Status::kBadAddress);
  CHECK(b.dp.counters().transfers == transfers);
  CHECK(b.target.errors().empty());
  CHECK(b.chip.errors().empty());
  b.release();
}

TEST_CASE("a block across one 1 KB boundary and across several: TAR rewritten at each, the data lands") {
  ApBench b;
  b.connectAndPowerUp();
  REQUIRE(b.ap.open().ok());
  // 600 words from 0x20000100: 2400 bytes, crossing 0x20000400 and 0x20000800, ending at 0x20000A60.
  std::vector<std::uint32_t> data(600);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = 0x1000000u + static_cast<std::uint32_t>(i);
  MemResult r = b.ap.writeBlock(0x20000100, data.data(), data.size());
  CHECK(r.ok());
  CHECK(b.ap.counters().tar_writes == 3);  // 0x20000100, 0x20000400, 0x20000800
  CHECK(b.ap.counters().drw_writes == 600);
  std::vector<std::uint32_t> back(600, 0);
  r = b.ap.readBlock(0x20000100, back.data(), back.size());
  CHECK(r.ok());
  CHECK(b.ap.counters().tar_writes == 6);
  CHECK(b.ap.counters().drw_reads == 600);
  CHECK(back == data);
  // Across one boundary only: 4 words at 0x200007F8 -> TAR at 0x200007F8 and 0x20000800.
  const std::uint32_t four[4] = {1, 2, 3, 4};
  const std::uint32_t tars = b.ap.counters().tar_writes;
  CHECK(b.ap.writeBlock(0x200007F8, four, 4).ok());
  CHECK(b.ap.counters().tar_writes == tars + 2);
  CHECK(b.ap.read32(0x20000800).value == 3);
  CHECK(b.ap.read32(0x20000804).value == 4);
  // The words before and after the block are untouched.
  CHECK(b.ap.read32(0x200000FC).value == 0);
  CHECK(b.ap.read32(0x20000A60).value == 0);
  CHECK(b.target.errors().empty());
  CHECK(b.chip.errors().empty());
  // Nothing landed outside RAM: the first flash page is still blank.
  CHECK(b.chip.flashWord(0x100) == 0xFFFFFFFF);
  b.release();
}

TEST_CASE("forgetting to rewrite TAR at the boundary writes to the wrong place - which the simulator shows") {
  ApBench b;
  b.connectAndPowerUp();
  REQUIRE(b.ap.open().ok());
  // Through the raw debug port, as a careless host would: TAR once, then four DRW writes
  // straight across 0x20000400.
  REQUIRE(b.dp.writeAp(0, map::kTar, 0x200003F8).ok());
  REQUIRE(b.dp.writeAp(0, map::kDrw, 0xAAAA0001).ok());
  REQUIRE(b.dp.writeAp(0, map::kDrw, 0xAAAA0002).ok());
  REQUIRE(b.dp.writeAp(0, map::kDrw, 0xAAAA0003).ok());
  REQUIRE(b.dp.writeAp(0, map::kDrw, 0xAAAA0004).ok());
  CHECK(b.chip.counters().tar_wraps == 1);
  // The third and fourth words went to the start of the 1 KB block, not past its end.
  CHECK(b.ap.read32(0x200003F8).value == 0xAAAA0001);
  CHECK(b.ap.read32(0x200003FC).value == 0xAAAA0002);
  CHECK(b.ap.read32(0x20000400).value == 0);
  CHECK(b.ap.read32(0x20000404).value == 0);
  CHECK(b.ap.read32(0x20000000).value == 0xAAAA0003);
  CHECK(b.ap.read32(0x20000004).value == 0xAAAA0004);
  b.release();
}

TEST_CASE("the same host against an AHB-AP that wraps at 4 KB, as the Cortex-M4's does (DDI 0439B Table 8-6)") {
  ApBench b;
  b.chip.tar_wrap_bytes = 4096;
  b.connectAndPowerUp();
  REQUIRE(b.ap.open().ok());
  std::vector<std::uint32_t> data(600);  // 2400 bytes from 0x20000100 across 0x20000400 and 0x20000800, inside one 4 KB block
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = 0x2000000u + static_cast<std::uint32_t>(i);
  CHECK(b.ap.writeBlock(0x20000100, data.data(), data.size()).ok());
  // Rewritten at every 1 KB all the same - three times - and this AP never had to wrap: on the
  // real chip the rewrites at 1 KB are redundant and harmless, and the one at 4 KB is needed.
  CHECK(b.ap.counters().tar_writes == 3);
  CHECK(b.chip.counters().tar_wraps == 0);
  std::vector<std::uint32_t> back(600, 0);
  CHECK(b.ap.readBlock(0x20000100, back.data(), back.size()).ok());
  CHECK(back == data);
  // And across the 4 KB boundary the same host is right because it rewrote there too.
  const std::uint32_t four[4] = {5, 6, 7, 8};
  CHECK(b.ap.writeBlock(0x20000FF8, four, 4).ok());
  CHECK(b.ap.read32(0x20001000).value == 7);
  CHECK(b.ap.read32(0x20001004).value == 8);
  CHECK(b.ap.read32(0x20000000).value == 0);
  b.release();
}

TEST_CASE("an error carries the address that failed, and drops the TAR cache") {
  ApBench b;
  b.connectAndPowerUp();
  REQUIRE(b.ap.open().ok());
  // 0x50000000 is nothing on this chip: the AP transaction ends in an error, STICKYERR is set
  // and the RDBUFF read gets FAULT.
  MemResult r = b.ap.read32(0x50000000);
  CHECK(r.status == Status::kFault);
  CHECK(r.address == 0x50000000);
  CHECK((r.ctrl_stat & core::swd::ctrl_stat::kStickyErr) != 0);
  CHECK(b.ap.counters().errors == 1);
  CHECK(b.chip.counters().bus_errors == 1);
  // The debug port cleared the flag; the next access works and writes TAR again.
  const std::uint32_t tars = b.ap.counters().tar_writes;
  r = b.ap.read32(0x20000000);
  CHECK(r.ok());
  CHECK(b.ap.counters().tar_writes == tars + 1);
  // A write to nothing: the FAULT reaches the next transfer, which is the next word of the block.
  const std::uint32_t two[2] = {1, 2};
  r = b.ap.writeBlock(0x50000000, two, 2);
  CHECK(r.status == Status::kFault);
  CHECK(r.address == 0x50000004);
  // In a block the failing word is named: words 0..9 at 0x20000000 then nothing at 0x50000000 is
  // not one block, so: the RAM's end. 0x20040000 is past it.
  std::vector<std::uint32_t> out(4);
  r = b.ap.readBlock(0x2003FFF8, out.data(), 4);
  CHECK(r.status == Status::kFault);
  CHECK(r.address == 0x20040000);
  b.chip.clearErrors();
  b.release();
}

TEST_CASE("with access-port protection on, the debug port answers and the AHB-AP refuses memory") {
  ApBench b;
  b.chip.approtect = true;
  b.connectAndPowerUp();
  // The IDR is still readable; CSW says DeviceEn 0; the chip's protection shows in the CTRL-AP.
  const MemResult r = b.ap.open();
  CHECK(r.ok());  // open() checks the IDR and writes CSW; it is memory that is refused
  const core::swd::Result csw = b.dp.readAp(0, map::kCsw);
  CHECK(csw.ok());
  CHECK((csw.value & map::kCswDeviceEn) == 0);
  CHECK(b.ap.read32(0x10000100).status == Status::kFault);
  CHECK(b.ap.write32(0x20000000, 1).status == Status::kFault);
  CHECK(b.chip.counters().protected_accesses >= 2);
  const core::swd::Result status = b.dp.readAp(core::nrf52::ctrl_ap::kApsel, core::nrf52::ctrl_ap::kApprotectStatus);
  CHECK(status.ok());
  CHECK((status.value & core::nrf52::ctrl_ap::kApprotectStatusOpen) == 0);
  b.release();
}

TEST_CASE("a lost line in the middle of a block: the lines-lost status, the address, nothing driven after") {
  ApBench b;
  b.connectAndPowerUp();
  REQUIRE(b.ap.open().ok());
  std::vector<std::uint32_t> data(64, 0x5A5A5A5A);
  // A kill takes the lines in the middle: the pin driver refuses from the 20th DRW write on.
  unsigned writes = 0;
  b.pins.fail_when = [&](const fakes::Op& op) {
    if (op.gpio == core::board::kSwdIo.gpio && op.kind == fakes::OpKind::kMode &&
        op.mode == interfaces::PinMode::kOutput) {
      writes++;
    }
    return writes > 60;
  };
  const MemResult r = b.ap.writeBlock(0x20000000, data.data(), data.size());
  CHECK(r.status == Status::kLinesLost);
  CHECK(r.address >= 0x20000000);
  CHECK(r.address < 0x20000100);
  b.pins.fail_when = nullptr;
  b.release();
}
