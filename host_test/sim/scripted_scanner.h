// A scripted NoiseScope on the other end of the UART (task p16-03, moved here by task p16-04).
//
// The scanner's half is the repeater repository's own `docs/noisescope.md` §5: **every command is
// answered with an `ack` line**, a finite activity ends with a `done`, and a measurement line is
// one NDJSON object. The knobs here are what a session's five step endings need - no `ack`, an
// `ack` that is not ok, lines without a `done`, more lines than the room holds - and nothing else.
//
// It is a hook on `sim::ScriptedRadio` and nothing more, so it serves both benches a session is
// driven on: the update bench's (host_test/core/update/session_bench.h, where a session is a
// typed command) and the wake bench's (host_test/agent/, where it is a claimed job).
#pragma once

#include <doctest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sim/scripted_radio.h"

namespace bench {

// The first word of a command line, which is what an `ack` is matched on.
inline std::string firstWordOf(const std::string& line) {
  const std::size_t space = line.find(' ');
  return space == std::string::npos ? line : line.substr(0, space);
}

// NoiseScope's own lines, as §5 writes them.
inline std::string scannerAck(const std::string& command, bool ok, const std::string& message) {
  return "{\"t\":\"ack\",\"cmd\":\"" + command + "\",\"ok\":" + (ok ? "true" : "false") +
         ",\"msg\":\"" + message + "\"}";
}

inline std::string scannerDone(const std::string& command) {
  return "{\"t\":\"done\",\"cmd\":\"" + command + "\",\"seq\":18,\"ts\":1754820100,\"ms\":112000}";
}

// A measurement line of `size` bytes, shaped like a real one: the command's own `t`, a sequence
// number, and an array padded to the length wanted.
inline std::string scannerLine(const std::string& what, unsigned seq, std::size_t size) {
  const std::string head = "{\"t\":\"" + what + "\",\"seq\":" + std::to_string(seq) +
                           ",\"ts\":1754820100,\"f0\":863.000,\"f1\":870.000,\"n\":281,\"avg\":[";
  const std::string tail = "]}";
  REQUIRE(size > head.size() + tail.size());
  return head + std::string(size - head.size() - tail.size(), '7') + tail;
}

inline const char* kScannerInfo =
    "{\"t\":\"info\",\"fw\":\"noisescope\",\"ver\":\"0.1.0\",\"role\":\"scanner\","
    "\"board\":\"rak4630-single\",\"devid\":\"deadbeef01020304\",\"rtc\":1,\"auto_min\":10}";

// What this scanner answers one measuring command with.
struct Answer {
  bool ack = true;         // an `ack` at all. false is the `silent` ending
  bool ok = true;          // ... and it says ok. false is the `refused` ending
  std::size_t lines = 1;   // measurement lines after the `ack`
  std::size_t line_bytes = 300;
  bool done = true;        // a `done` line after them. false leaves the step to its clock
};

// What the scripted scanner answers each command with. One hook on the radio, installed for the
// whole life of a world: the greeting's three words get an `ack` (and `info` its own line), the
// six that measure get an `ack` and then lines, and `stop` gets an `ack`. `answer` is what every
// measuring command gets; `answers["hist"]` is what one of them gets instead.
struct ScriptedScanner {
  Answer answer;
  std::map<std::string, Answer> answers;
  std::uint32_t first_line_ms = 50;
  std::uint32_t between_ms = 50;
  std::vector<std::string> said;  // every command this scanner was given, in order
  // Something for a test to do the moment a command arrives - the server going away in the middle
  // of the second step, say. Called before the `ack`.
  std::function<void(const std::string&)> on_command;

  const Answer& answerFor(const std::string& word) const {
    const std::map<std::string, Answer>::const_iterator at = answers.find(word);
    return at == answers.end() ? answer : at->second;
  }

  void install(sim::ScriptedRadio& radio) {
    radio.on_text_command = [this, &radio](const std::string& line) {
      said.push_back(line);
      if (on_command) on_command(line);
      const std::string word = firstWordOf(line);
      const bool measures = word == "stat" || word == "sweep" || word == "scope" ||
                            word == "hist" || word == "dwell" || word == "cad";
      if (!measures) {
        // `time`, `auto`, `info`, `stop`: an `ack`, and for `info` the line §5 promises.
        radio.sendLine(scannerAck(word, true, "ok"));
        if (word == "info") radio.sendLine(kScannerInfo, 10);
        return true;
      }
      const Answer& a = answerFor(word);
      if (!a.ack) return true;  // nothing at all: the step ends `silent`
      radio.sendLine(scannerAck(word, a.ok, a.ok ? "started" : "bad arguments"));
      if (!a.ok) return true;
      std::uint32_t at = first_line_ms;
      for (std::size_t i = 0; i < a.lines; ++i) {
        radio.sendLine(scannerLine(word, static_cast<unsigned>(17 + i), a.line_bytes), at);
        at += between_ms;
      }
      if (a.done) radio.sendLine(scannerDone(word), at);
      return true;
    };
  }

  // What the scanner was told, after the greeting's three words.
  std::vector<std::string> commandsAfterGreeting() const {
    std::vector<std::string> rest;
    for (const std::string& line : said) {
      const std::string word = firstWordOf(line);
      if (word == "time" || word == "auto" || word == "info") continue;
      rest.push_back(line);
    }
    return rest;
  }

  bool wasSaid(const std::string& line) const {
    for (const std::string& seen : said) {
      if (seen == line) return true;
    }
    return false;
  }
};

}  // namespace bench
