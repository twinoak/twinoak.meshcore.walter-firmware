#include <agent/ConfigApply.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <agent/CliReplies.h>
#include <agent/JsonWriter.h>

namespace mm {

const char kRegionLoadTerminator[] = " ";
const char kApplyDefaultExpect[] = "OK";
const char kApplyReplySeparator[] = " | ";

namespace {

bool hasLineBreak(const char* text) {
  for (; *text != '\0'; text++) {
    if (*text == '\r' || *text == '\n') return true;
  }
  return false;
}

const char* trimmed(char* text) {
  return trimInPlace(text);
}

}  // namespace

// ------------------------------------------------------------------ the plan

void ConfigApplyPlan::clear() {
  commandCount = 0;
  for (int i = 0; i < kMaxApplyCommands; i++) commands[i] = ApplyCommand();
  lineCount = 0;
  for (int i = 0; i < kMaxApplyLines; i++) lines[i] = ApplyLine();
  poolLen = 0;
  pool[0] = '\0';
  planHash[0] = '\0';
  refused = false;
  refusedWhy[0] = '\0';
}

void ConfigApplyPlan::refuse(const char* why) {
  if (refused) return;  // the first reason is the one that counts
  refused = true;
  std::snprintf(refusedWhy, sizeof refusedWhy, "%s", why != nullptr ? why : "the plan cannot be run");
}

bool ConfigApplyPlan::addCommand(const char* key, const char* command) {
  if (key == nullptr || key[0] == '\0' || command == nullptr || command[0] == '\0') {
    refuse("config.apply carries a command with no key or no line: nothing is sent");
    return false;
  }
  if (std::strlen(key) > kMaxConfigKeyLen || std::strlen(command) > kMaxApplyCommandLen || hasLineBreak(command)) {
    char why[kMaxApplyRefusalLen + 1];
    std::snprintf(why, sizeof why, "config.apply: the command for '%.32s' is longer than this firmware holds, or has a line break in it: nothing is sent", key);
    refuse(why);
    return false;
  }
  if (commandCount >= kMaxApplyCommands) {
    char why[kMaxApplyRefusalLen + 1];
    std::snprintf(why, sizeof why, "config.apply carries more than %d commands, which this firmware cannot hold: nothing is sent", kMaxApplyCommands);
    refuse(why);
    return false;
  }
  ApplyCommand& c = commands[commandCount];
  c = ApplyCommand();
  std::snprintf(c.key, sizeof c.key, "%s", key);
  std::snprintf(c.command, sizeof c.command, "%s", command);
  c.firstLine = lineCount;
  c.lineCount = 0;
  commandCount++;
  return true;
}

bool ConfigApplyPlan::addLine(const char* line, const char* expect, bool escape) {
  if (commandCount == 0) {
    refuse("config.apply carries a line before any command");
    return false;
  }
  ApplyCommand& c = commands[commandCount - 1];
  if (line == nullptr || line[0] == '\0' || hasLineBreak(line)) {
    char why[kMaxApplyRefusalLen + 1];
    std::snprintf(why, sizeof why, "config.apply: '%.32s' carries an empty line, or one with a line break in it: nothing is sent", c.key);
    refuse(why);
    return false;
  }
  const size_t lineLen = std::strlen(line);
  const size_t expectLen = expect != nullptr ? std::strlen(expect) : 0;
  if (lineLen > kMaxApplyLineLen || expectLen > kMaxApplyExpectLen) {
    char why[kMaxApplyRefusalLen + 1];
    std::snprintf(why, sizeof why, "config.apply: a line of '%.32s' is longer than this firmware holds: nothing is sent", c.key);
    refuse(why);
    return false;
  }
  if (lineCount >= kMaxApplyLines || poolLen + lineLen + 1 + (expect != nullptr ? expectLen + 1 : 0) > sizeof pool) {
    char why[kMaxApplyRefusalLen + 1];
    std::snprintf(why, sizeof why, "config.apply: '%.32s' has more lines than this firmware holds (%d, in %u bytes): nothing is sent", c.key,
                  kMaxApplyLines, static_cast<unsigned>(sizeof pool));
    refuse(why);
    return false;
  }
  ApplyLine& l = lines[lineCount];
  l = ApplyLine();
  l.lineAt = static_cast<uint16_t>(poolLen);
  std::memcpy(pool + poolLen, line, lineLen + 1);
  poolLen += lineLen + 1;
  if (expect != nullptr) {
    l.hasExpect = true;
    l.expectAt = static_cast<uint16_t>(poolLen);
    std::memcpy(pool + poolLen, expect, expectLen + 1);
    poolLen += expectLen + 1;
  }
  l.escape = escape;
  if (c.lineCount == 0) c.firstLine = lineCount;
  c.lineCount++;
  lineCount++;
  return true;
}

// ------------------------------------------------------------------ the outcome

const char* commandOutcomeName(CommandOutcome o) {
  switch (o) {
    case CommandOutcome::Ok: return "ok";
    case CommandOutcome::Error: return "error";
    case CommandOutcome::NoReply: return "no-reply";
    case CommandOutcome::NotSent:
    default: return "not-sent";
  }
}

void ApplyCommandResult::setReply(const char* text) {
  if (text == nullptr) {
    hasReply = false;
    reply[0] = '\0';
    return;
  }
  hasReply = true;
  std::snprintf(reply, sizeof reply, "%s", text);
}

void ConfigApplyOutcome::clear() {
  commandCount = 0;
  for (int i = 0; i < kMaxApplyCommands; i++) commands[i] = ApplyCommandResult();
  reReadStopped = false;
}

int ConfigApplyOutcome::sent() const {
  int n = 0;
  for (int i = 0; i < commandCount; i++) {
    if (commands[i].wasSent()) n++;
  }
  return n;
}

int ConfigApplyOutcome::ok() const {
  int n = 0;
  for (int i = 0; i < commandCount; i++) {
    if (commands[i].isOk()) n++;
  }
  return n;
}

bool ConfigApplyOutcome::allOk() const {
  if (commandCount == 0) return false;
  for (int i = 0; i < commandCount; i++) {
    if (!commands[i].isOk()) return false;
  }
  return true;
}

// ------------------------------------------------------------------ the engine

namespace {

class Lines {
 public:
  Lines(const ConfigApplyPlan& plan, const ApplyCommand& c) : plan_(plan), c_(c) {}
  int count() const { return c_.lineCount > 0 ? c_.lineCount : 1; }
  const char* text(int i) const { return c_.lineCount > 0 ? plan_.lineText(c_.firstLine + i) : c_.command; }
  // nullptr: the firmware answers nothing.
  const char* expect(int i) const { return c_.lineCount > 0 ? plan_.lineExpect(c_.firstLine + i) : kApplyDefaultExpect; }
  bool escape(int i) const { return c_.lineCount > 0 && plan_.lines[c_.firstLine + i].escape; }

 private:
  const ConfigApplyPlan& plan_;
  const ApplyCommand& c_;
};

void say(ApplyExchange& x, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void say(ApplyExchange& x, const char* fmt, ...) {
  char line[240];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  x.log(line);
}

void appendReply(char* joined, size_t cap, const char* reply, bool& first) {
  const size_t have = std::strlen(joined);
  if (!first) std::snprintf(joined + have, cap - have, "%s%s", kApplyReplySeparator, reply);
  else std::snprintf(joined + have, cap - have, "%s", reply);
  first = false;
}

}  // namespace

ApplyRun runConfigApply(const ConfigApplyPlan& plan, ApplyExchange& x, ConfigApplyOutcome& out, ConfigSnapshot& reRead) {
  out.clear();
  reRead.clear();
  out.commandCount = plan.commandCount;
  bool stopped = false;
  bool timedOut = false;
  char reply[kMaxCliReplyLen + 1];
  char joined[kMaxApplyReplyLen + 1];
  char what[kMaxApplyLineLen + kMaxConfigKeyLen + 48];

  for (int i = 0; i < plan.commandCount; i++) {
    const ApplyCommand& entry = plan.commands[i];
    ApplyCommandResult& r = out.commands[i];
    if (stopped) {
      r.outcome = CommandOutcome::NotSent;
      r.setReply(nullptr);
      continue;
    }

    const Lines lines(plan, entry);
    CommandOutcome outcome = CommandOutcome::Ok;
    joined[0] = '\0';
    bool firstReply = true;
    int answered = 0;
    const char* last = nullptr;  // the last reply, or the reason a line did not go
    char lastText[kMaxCliReplyLen + 1] = {0};

    for (int n = 0; n < lines.count(); n++) {
      const char* line = lines.text(n);
      const char* expect = lines.expect(n);
      if (outcome != CommandOutcome::Ok) {
        // The sequence has failed. Nothing more of it is sent - except a line that gets the
        // repeater out of a mode an earlier one put it into, which is sent exactly once and
        // whose answer changes nothing about the outcome.
        if (!lines.escape(n)) continue;
        std::snprintf(what, sizeof what, "leave the mode '%s' left open", entry.key);
        ApplySend s;
        s.what = what;
        s.line = line;
        s.timestamp = x.nextTimestamp();
        s.awaitReply = expect != nullptr;
        s.retry = false;
        s.escape = true;
        const ApplyExchange::Sent esc = x.send(s, reply, sizeof reply);
        say(x, "%s: the sequence failed part-way; sent the line that ends it so the repeater takes commands again: %s", entry.key,
            esc == ApplyExchange::Sent::Answered ? "sent" : esc == ApplyExchange::Sent::NoReply ? "no reply"
                                                   : esc == ApplyExchange::Sent::NotSent   ? "not sent"
                                                                                           : "the lease ran out");
        break;
      }

      ApplySend s;
      s.line = line;
      s.retry = false;
      // The loader's blank line is the escape line on the way through as well: it goes out
      // whatever the lease says, because the repeater is in load mode until it arrives.
      s.escape = lines.escape(n);
      if (expect == nullptr) {
        // The firmware answers this line with nothing at all, so there is nothing to wait for.
        std::snprintf(what, sizeof what, "'%s' (no reply expected)", line);
        s.what = what;
        s.timestamp = x.nextTimestamp();
        s.awaitReply = false;
        const ApplyExchange::Sent pushed = x.send(s, reply, sizeof reply);
        if (pushed != ApplyExchange::Sent::Answered) {
          outcome = CommandOutcome::NotSent;
          if (pushed == ApplyExchange::Sent::LeaseOut) {
            timedOut = true;
            std::snprintf(lastText, sizeof lastText, "the lease ran out before '%s' was sent", line);
          } else {
            std::snprintf(lastText, sizeof lastText, "the packet for '%s' could not be built", line);
          }
          last = lastText;
          say(x, "%s: this exit node did not send '%s': %s", entry.key, line, lastText);
        }
        continue;
      }

      std::snprintf(what, sizeof what, "'%s'", line);
      s.what = what;
      s.timestamp = x.nextTimestamp();
      s.awaitReply = true;
      const ApplyExchange::Sent sent = x.send(s, reply, sizeof reply);
      switch (sent) {
        case ApplyExchange::Sent::Answered:
          trimmed(reply);
          std::snprintf(lastText, sizeof lastText, "%s", reply);
          last = lastText;
          appendReply(joined, sizeof joined, reply, firstReply);
          answered++;
          if (!cliReplyIsConfirmed(reply, expect)) {
            outcome = CommandOutcome::Error;
            say(x, "%s: the repeater answered '%s' with '%s', and this line wanted '%s'", entry.key, line, reply, expect);
          }
          break;
        case ApplyExchange::Sent::NoReply:
          // The command went out and nothing came back. It may have applied; the server
          // records the expectation and the next config read settles it (§5a). Never sent again.
          outcome = CommandOutcome::NoReply;
          last = nullptr;
          say(x, "%s: no reply to '%s' - the value may or may not have applied; not sent again", entry.key, line);
          break;
        case ApplyExchange::Sent::LeaseOut:
          outcome = CommandOutcome::NotSent;
          timedOut = true;
          std::snprintf(lastText, sizeof lastText, "the lease ran out before '%s' was sent", line);
          last = lastText;
          say(x, "%s: %s", entry.key, lastText);
          break;
        case ApplyExchange::Sent::NotSent:
        default:
          // The packet could not be built: the line never left this exit node.
          outcome = CommandOutcome::NotSent;
          std::snprintf(lastText, sizeof lastText, "the packet for '%s' could not be built", line);
          last = lastText;
          say(x, "%s: this exit node did not send '%s': %s", entry.key, line, lastText);
          break;
      }
    }

    // For an entry of several lines the record is every reply it got, in order: the loader's
    // `OK - loaded N regions` is the one confirmation the whole load has, and a row that kept
    // only the last one would have thrown it away (task p4-57).
    r.outcome = outcome;
    if (lines.count() > 1 && answered > 0) {
      r.setReply(joined);
    } else {
      r.setReply(last);
    }
    stopped = outcome != CommandOutcome::Ok;
  }

  // The re-read (§5 step 5): every key that was sent, whatever the repeater said to it - a
  // refused value is worth reading back too - and `ver` last, so the snapshot the server folds
  // this into carries the firmware. Sent keys only: a key that was never sent was never touched.
  // It stops at the first key the repeater does not answer at all, because every get after it
  // would fail the same way and cost two transactions each doing so.
  int touched = 0;
  int read = 0;
  char command[kMaxConfigKeyLen + 8];
  char rawLine[kMaxConfigRawLen + 1];
  for (int i = 0; i < plan.commandCount && !out.reReadStopped; i++) {
    if (!out.commands[i].wasSent()) continue;
    const char* key = plan.commands[i].key;
    bool seen = false;
    for (int j = 0; j < i; j++) {
      if (out.commands[j].wasSent() && std::strcmp(plan.commands[j].key, key) == 0) seen = true;
    }
    if (seen) continue;  // Distinct: a key sent twice is read back once
    touched++;

    if (std::strcmp(key, kRegionsKey) == 0) {
      if (!x.readRegions(reRead)) {
        say(x, "the re-read stopped at the regions: the keys after it are not read back, and the server confirms nothing it did not read");
        out.reReadStopped = true;
        break;
      }
      if (reRead.count > 0 && std::strcmp(reRead.values[reRead.count - 1].key, kRegionsKey) == 0) read++;
      continue;
    }

    std::snprintf(command, sizeof command, "get %s", key);
    std::snprintf(what, sizeof what, "re-read '%s'", command);
    ApplySend s;
    s.what = what;
    s.line = command;
    s.timestamp = x.nextTimestamp();
    s.awaitReply = true;
    s.retry = true;
    s.escape = false;
    const ApplyExchange::Sent sent = x.send(s, reply, sizeof reply);
    if (sent != ApplyExchange::Sent::Answered) {
      if (sent == ApplyExchange::Sent::LeaseOut) timedOut = true;
      say(x, "the re-read stopped at '%s': %s; the keys after it are not read back, and the server confirms nothing it did not read",
          command, sent == ApplyExchange::Sent::NoReply ? "no reply" : sent == ApplyExchange::Sent::LeaseOut ? "the lease ran out" : "not sent");
      out.reReadStopped = true;
      break;
    }
    trimmed(reply);
    if (const char* value = cliReplyValue(reply)) {
      reRead.put(key, value);
      read++;
    } else {
      // A raw line is the command, a colon and up to 160 characters, as the config read keeps one.
      std::snprintf(rawLine, sizeof rawLine, "%.36s: %.160s", command, reply);
      reRead.addRaw(rawLine);
    }
  }

  if (!out.reReadStopped) {
    ApplySend s;
    s.what = "re-read 'ver'";
    s.line = "ver";
    s.timestamp = x.nextTimestamp();
    s.awaitReply = true;
    s.retry = true;
    s.escape = false;
    const ApplyExchange::Sent sent = x.send(s, reply, sizeof reply);
    if (sent != ApplyExchange::Sent::Answered) {
      if (sent == ApplyExchange::Sent::LeaseOut) timedOut = true;
      say(x, "the re-read stopped at 'ver': %s; the server confirms nothing it did not read",
          sent == ApplyExchange::Sent::NoReply ? "no reply" : sent == ApplyExchange::Sent::LeaseOut ? "the lease ran out" : "not sent");
      out.reReadStopped = true;
    } else {
      trimmed(reply);
      std::snprintf(reRead.firmware, sizeof reRead.firmware, "%.*s", static_cast<int>(kMaxConfigTextLen), reply);
    }
  }

  say(x, "applied %d of %d command(s) as the repeater answered them; re-read %d of %d touched key(s)%s", out.ok(), out.commandCount, read,
      touched, out.reReadStopped ? " before it stopped" : "");

  // `ok` when every command was answered as its lines expect (the re-read is the server's
  // evidence, not this job's outcome); `failed` when any was refused, unanswered or never sent;
  // `timeout` when the lease ran out under it. A failed apply is never retried: the job has one
  // attempt, and a partial apply re-sent would be sets made twice.
  if (timedOut) return ApplyRun::Timeout;
  return out.allOk() ? ApplyRun::Ok : ApplyRun::Failed;
}

// ------------------------------------------------------------------ the result

size_t writeConfigApplyResult(const ConfigApplyPlan& plan, const ConfigApplyOutcome& out, const ConfigSnapshot& reRead,
                              char* buf, size_t cap) {
  JsonWriter w(buf, cap);
  w.beginObject();
  w.key("commands");
  w.beginArray();
  for (int i = 0; i < out.commandCount && i < plan.commandCount; i++) {
    const ApplyCommandResult& r = out.commands[i];
    w.comma();
    w.beginObject();
    w.key("key");
    w.string(plan.commands[i].key);
    w.key("command");
    w.string(plan.commands[i].command);
    w.key("outcome");
    w.string(commandOutcomeName(r.outcome));
    if (r.hasReply) {
      w.key("reply");
      w.string(r.reply);
    }
    w.key("isOk");
    w.boolean(r.isOk());
    w.key("wasSent");
    w.boolean(r.wasSent());
    w.endObject();
  }
  w.endArray();
  w.key("reRead");
  w.beginObject();
  for (int i = 0; i < reRead.count; i++) {
    w.key(reRead.values[i].key);
    w.string(reRead.values[i].value);
  }
  w.endObject();
  w.key("reReadRaw");
  w.beginArray();
  for (int i = 0; i < reRead.rawCount; i++) {
    w.comma();
    w.string(reRead.raw[i]);
  }
  w.endArray();
  if (reRead.firmware[0] != '\0') {
    w.key("firmware");
    w.string(reRead.firmware);
  }
  w.key("sent");
  w.integer(out.sent());
  w.key("ok");
  w.integer(out.ok());
  w.key("allOk");
  w.boolean(out.allOk());
  w.endObject();
  return w.overflowed() ? 0 : w.length();
}

}  // namespace mm
