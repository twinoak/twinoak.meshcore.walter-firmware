// The repeater's own request handler behind the local door (spec/LINK-PROTOCOL.md §5.9, link 1.2,
// task p9-06): what `sim::SimRak::local_door` is given on a bench.
//
// It is **not** a second implementation of MeshCore's handler. Three of the four answers do not
// depend on the request's `data` at all, and those are the *fixtures'* own bytes
// (spec/fixtures/link/link-v1.2.json, the same file the repeater firmware's codec is tested
// against), so what a test reads back is what the specification says the node answers. The fourth
// - the neighbour table, §5.9's `0x06`, whose page is chosen by `data` - is paged here, because a
// Walter that asks for one page and stops would pass a fixture and fail on a radio with eleven
// neighbours. The layout it writes is `spec/tools/linkref.py`'s `neighbours_answer()`: the table's
// count (i16), the page's count (i16), then per entry the key prefix, the seconds since it was
// heard (u32) and its SNR in quarter-decibels (i8).
//
// Where the specification says the handler **answers nothing** - a neighbours request of a version
// it does not know, an access-list request whose reserved bytes are not zero - this returns false,
// which the RAK turns into §5.9's ERR_RANGE and the local door into silence.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "core/link/codec/wire.h"
#include "fixtures/link_fixture.h"

namespace sim {

// The `data` of a fixture case of link-v1.2.json, as bytes: the answer the node packs.
inline std::vector<std::uint8_t> localAnswerFixture(const char* case_name) {
  const jsonfix::Value* found = fixtures::linkCaseIn(fixtures::linkFixture12(), case_name);
  REQUIRE_MESSAGE(found != nullptr, "the 1.2 fixtures have no case called " << case_name);
  return fixtures::fromHex((*found)["fields"]["data"].string());
}

struct FixtureLocalDoor {
  // One row of the neighbour table this door pages. The prefix is hex, as the fixtures write it.
  struct Neighbour {
    std::string prefix_hex;
    std::uint32_t heard_s_ago = 0;
    std::int8_t snr_q4 = 0;
  };

  // The three answers that do not depend on `data`, from the fixtures.
  std::vector<std::uint8_t> status = localAnswerFixture("local request: the status, as the node packs it");
  std::vector<std::uint8_t> owner_info = localAnswerFixture("local request: version, name and owner info");
  std::vector<std::uint8_t> access_list = localAnswerFixture("local request: one admin on the access list");
  // The table, newest first - which is the order the request's `order` byte 0 asks for.
  std::vector<Neighbour> neighbours;
  // §5.9: an access-list request whose reserved bytes are not zero is one the handler gives no
  // answer to. False is a handler that answers none of them at all, which is what a repeater
  // below admin does over the mesh - the case the door must read as silence.
  bool answers_access_list = true;

  // What was asked of it.
  std::vector<std::uint8_t> asked;    // every req_type, in order
  std::vector<int> page_offsets;      // the `offset` of every neighbours request

  // `count` rows of made-up neighbours. More than `kNeighboursPerPage` (nine) is what makes a
  // reader ask a second page, with an offset.
  void fillNeighbours(int count) {
    neighbours.clear();
    for (int i = 0; i < count; ++i) {
      Neighbour one;
      char head[3] = {};
      std::snprintf(head, sizeof head, "%02x", static_cast<unsigned>(i + 1) & 0xFFu);
      one.prefix_hex = std::string(head) + "112233445566778899aabbcc";
      one.heard_s_ago = static_cast<std::uint32_t>(60 * (i + 1));
      one.snr_q4 = static_cast<std::int8_t>(20 - i);
      neighbours.push_back(one);
    }
  }

  bool answer(std::uint8_t req_type, const std::uint8_t* data, std::size_t len,
              std::vector<std::uint8_t>* out) {
    asked.push_back(req_type);
    switch (req_type) {
      case core::link::local_req::kStatus:
        *out = status;
        return true;
      case core::link::local_req::kOwnerInfo:
        *out = owner_info;
        return true;
      case core::link::local_req::kAccessList:
        // §5.9: `data` is the request's two reserved bytes, which must be zero.
        if (!answers_access_list || len < 2 || data[0] != 0 || data[1] != 0) return false;
        *out = access_list;
        return true;
      case core::link::local_req::kNeighbours:
        return pageOfNeighbours(data, len, out);
      default:
        return false;
    }
  }

  // A std::function for `SimRak::local_door`, bound to this object.
  std::function<bool(std::uint8_t, const std::uint8_t*, std::size_t, std::vector<std::uint8_t>*)>
  handler() {
    return [this](std::uint8_t req_type, const std::uint8_t* data, std::size_t len,
                  std::vector<std::uint8_t>* out) { return answer(req_type, data, len, out); };
  }

 private:
  static void putU16(std::vector<std::uint8_t>& into, std::uint16_t value) {
    into.push_back(static_cast<std::uint8_t>(value & 0xFF));
    into.push_back(static_cast<std::uint8_t>(value >> 8));
  }

  bool pageOfNeighbours(const std::uint8_t* data, std::size_t len,
                        std::vector<std::uint8_t>* out) {
    // §5.9: "chosen and ordered by `data` as the request's version 0 lays it out: version, count,
    // offset (u16), order, key-prefix length. The four random bytes MeshCore's clients add after
    // them change nothing here." A version this handler does not know is one it answers nothing
    // to, which is ERR_RANGE on the link.
    if (len < 6 || data[0] != 0) return false;
    const int want = data[1];
    const int offset = data[2] | (data[3] << 8);
    const std::size_t prefix_len = data[5];
    page_offsets.push_back(offset);
    const int total = static_cast<int>(neighbours.size());
    std::vector<std::uint8_t> entries;
    int returned = 0;
    for (int i = 0; i < want && offset + i < total; ++i) {
      const Neighbour& one = neighbours[static_cast<std::size_t>(offset + i)];
      const std::vector<std::uint8_t> key = fixtures::fromHex(one.prefix_hex);
      REQUIRE(key.size() >= prefix_len);
      entries.insert(entries.end(), key.begin(), key.begin() + static_cast<long>(prefix_len));
      for (int b = 0; b < 4; ++b) {
        entries.push_back(static_cast<std::uint8_t>((one.heard_s_ago >> (8 * b)) & 0xFF));
      }
      entries.push_back(static_cast<std::uint8_t>(one.snr_q4));
      ++returned;
    }
    out->clear();
    putU16(*out, static_cast<std::uint16_t>(total));
    putU16(*out, static_cast<std::uint16_t>(returned));
    out->insert(out->end(), entries.begin(), entries.end());
    return true;
  }
};

}  // namespace sim
