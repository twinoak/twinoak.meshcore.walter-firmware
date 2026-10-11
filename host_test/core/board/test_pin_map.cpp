// The pin map against the document (task p0-03): docs/ARCHITECTURE.md §2's table is read here and
// held against core/board/pin_map.h, net by net - D-sub pin and GPIO - so that the document and
// the code cannot drift apart. Both come from INTERCONNECT.md at the frozen commit; the test
// does not read that (another repository), it holds our two copies to each other.
#include <doctest.h>

#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include "core/board/pin_map.h"

namespace {

struct Row {
  std::string net;
  int dsub;
  int gpio;
};

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> cells(const std::string& line) {
  std::vector<std::string> out;
  std::size_t start = 1;  // after the leading '|'
  for (std::size_t i = 1; i < line.size(); ++i) {
    if (line[i] == '|') {
      out.push_back(trim(line.substr(start, i - start)));
      start = i + 1;
    }
  }
  return out;
}

bool isNumber(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

// The rows of the first table under "## 2." whose second and third cells are numbers.
std::vector<Row> readArchitectureTable() {
  std::ifstream in(std::string(TWINOAK_REPO_ROOT) + "/docs/ARCHITECTURE.md");
  REQUIRE_MESSAGE(in.good(), "cannot read docs/ARCHITECTURE.md from " TWINOAK_REPO_ROOT);
  std::vector<Row> rows;
  std::string line;
  bool inSection = false;
  while (std::getline(in, line)) {
    if (line.rfind("## ", 0) == 0) {
      if (inSection) break;
      inSection = line.rfind("## 2.", 0) == 0;
      continue;
    }
    if (!inSection || line.empty() || line[0] != '|') continue;
    const std::vector<std::string> c = cells(line);
    if (c.size() < 3 || !isNumber(c[1]) || !isNumber(c[2])) continue;
    // The net is the text in the cell's first pair of backticks ("`WALTER_TX` → radio RX").
    std::string net = c[0];
    const std::size_t open = net.find('`');
    const std::size_t close = open == std::string::npos ? open : net.find('`', open + 1);
    if (close != std::string::npos) net = net.substr(open + 1, close - open - 1);
    rows.push_back(Row{net, std::stoi(c[1]), std::stoi(c[2])});
  }
  return rows;
}

}  // namespace

TEST_CASE("the pin map is the table of docs/ARCHITECTURE.md section 2, net for net") {
  const std::vector<Row> rows = readArchitectureTable();
  REQUIRE_MESSAGE(rows.size() == core::board::kRadioPinCount,
                  "the document's table has " << rows.size() << " pin rows, the code "
                                              << core::board::kRadioPinCount);
  for (const core::board::PinDef& pin : core::board::kRadioPins) {
    bool found = false;
    for (const Row& row : rows) {
      if (row.net != pin.net) continue;
      found = true;
      CHECK_MESSAGE(row.dsub == pin.dsub, pin.net << ": the document says D-sub " << row.dsub
                                                  << ", the code " << int(pin.dsub));
      CHECK_MESSAGE(row.gpio == pin.gpio, pin.net << ": the document says GPIO " << row.gpio
                                                  << ", the code " << int(pin.gpio));
    }
    CHECK_MESSAGE(found, pin.net << " is in the code and not in the document");
  }
  // And in the document's order, which is the D-sub order.
  for (std::size_t i = 0; i < rows.size(); ++i) {
    CHECK(rows[i].net == core::board::kRadioPins[i].net);
  }
}

TEST_CASE("the seven pins are distinct GPIOs and distinct D-sub pins, on the glitch list") {
  for (std::size_t i = 0; i < core::board::kRadioPinCount; ++i) {
    for (std::size_t j = i + 1; j < core::board::kRadioPinCount; ++j) {
      CHECK(core::board::kRadioPins[i].gpio != core::board::kRadioPins[j].gpio);
      CHECK(core::board::kRadioPins[i].dsub != core::board::kRadioPins[j].dsub);
    }
    // GPIO1-14 glitch low for ~60 us at power-up (BENCH-BREADBOARD.md §6 caveat 3): every one of
    // ours is on that list, which is why a Walter power cycle reboots the radio.
    CHECK(core::board::kRadioPins[i].gpio >= 1);
    CHECK(core::board::kRadioPins[i].gpio <= 14);
  }
  CHECK(core::board::kNReset.gpio == 1);
  CHECK(core::board::kPwrKill.gpio == 5);
}
