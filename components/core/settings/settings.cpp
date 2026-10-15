#include "core/settings/settings.h"

#include <cstdio>
#include <cstring>

namespace core {
namespace settings {

namespace {

void say(char* err, std::size_t cap, const char* text) {
  if (err == nullptr || cap == 0) return;
  std::snprintf(err, cap, "%s", text);
}

// The epoch as it is stored: decimal, so that a person reading the partition with `nvs_get_str`
// sees a number and not four bytes.
std::uint32_t epochFromText(const char* text) {
  std::uint32_t value = 0;
  std::size_t digits = 0;
  for (const char* p = text; *p != 0; ++p) {
    if (*p < '0' || *p > '9') return 0;
    if (++digits > 9) return 0;
    value = value * 10 + static_cast<std::uint32_t>(*p - '0');
  }
  return value;
}

}  // namespace

void Settings::begin() {
  rejected_ = 0;
  first_rejected_ = "";
  for (std::size_t i = 0; i < kKeyCount; ++i) load(static_cast<Key>(i));
  char epoch[16] = {};
  pin_epoch_ = store_.read(kPinEpochName, epoch, sizeof epoch) ? epochFromText(epoch) : 0;
  applyUrl();
  begun_ = true;
}

// One key out of the store and into its buffer. A value that is too long for the buffer, or that
// does not validate, is dropped: the key reads as unset and its fallback is what the unit uses.
void Settings::load(Key key) {
  const KeyDef& k = def(key);
  char* slot = values_[static_cast<std::size_t>(key)];
  slot[0] = 0;
  char raw[kMaxValueLen + 1] = {};
  if (!store_.read(k.nvs, raw, sizeof raw)) return;
  if (raw[0] == 0) return;
  if (!validate(k, raw, nullptr, 0)) {
    ++rejected_;
    if (first_rejected_[0] == 0) first_rejected_ = k.name;
    return;
  }
  std::memcpy(slot, raw, std::strlen(raw) + 1);
}

// `server.url` taken apart, at begin() and at every change. A URL the client's own parser
// refuses leaves no host, which is "no server is configured": the trial's third condition does
// not apply and no call is made. The parser's Url is 1.7 KB of host, port and path and must not
// be on the main task's 3584-byte stack, so it is static - one Settings, one call at a time.
void Settings::applyUrl() {
  server_host_[0] = 0;
  server_port_ = 443;
  const char* url = value(Key::kServerUrl);
  if (url[0] == 0) return;
  static net::Url parsed;
  if (net::parseUrl(url, &parsed) != net::UrlError::kNone) return;
  std::memcpy(server_host_, parsed.host, std::strlen(parsed.host) + 1);
  server_port_ = parsed.port;
}

bool Settings::bumpPinEpoch() {
  char text[16] = {};
  const std::uint32_t next = pin_epoch_ + 1;
  std::snprintf(text, sizeof text, "%lu", static_cast<unsigned long>(next));
  if (!store_.write(kPinEpochName, text)) return false;
  pin_epoch_ = next;
  return true;
}

const char* Settings::stored(Key key) const {
  const std::size_t i = static_cast<std::size_t>(key);
  return values_[i < kKeyCount ? i : 0];
}

std::size_t Settings::setCount() const {
  std::size_t n = 0;
  for (std::size_t i = 0; i < kKeyCount; ++i) {
    if (values_[i][0] != 0) ++n;
  }
  return n;
}

const char* Settings::value(Key key) const {
  const char* s = stored(key);
  if (s[0] != 0) return s;
  const KeyDef& k = def(key);
  return k.fallback != nullptr ? k.fallback : "";
}

std::int32_t Settings::number(Key key) const { return settings::number(value(key)); }

bool Settings::on(Key key) const { return std::strcmp(value(key), "on") == 0; }

Level Settings::level() const {
  Level out = Level::kInfo;
  if (levelFromName(value(Key::kLogLevel), &out)) return out;
  return Level::kInfo;
}

bool Settings::put(const KeyDef& key, const char* text, char* err, std::size_t cap) {
  if (text == nullptr) text = "";
  if (!validate(key, text, err, cap)) return false;
  if (!store_.ready()) {
    say(err, cap, "there is no settings store in this build: nothing can be kept (see the boot lines)");
    return false;
  }
  char* slot = values_[static_cast<std::size_t>(key.id)];
  const bool changed = std::strcmp(slot, text) != 0;
  if (!store_.write(key.nvs, text)) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "%s could not be stored", key.name);
    say(err, cap, msg);
    return false;
  }
  std::memcpy(slot, text, std::strlen(text) + 1);
  // A PIN that is a different PIN is a different PIN: the epoch moves, and core/uplink/policy.h
  // forgets a refusal recorded against the old one. Typing the same PIN again changes nothing,
  // so a refusal of *that* PIN is not forgotten by typing it a second time - which is the whole
  // point of the memory (three refusals block the card).
  if (key.id == Key::kLtePin && changed) {
    if (!bumpPinEpoch()) {
      say(err, cap, "the PIN was stored, but which PIN it is could not be: 'erase confirm' and set it again");
      return false;
    }
  }
  if (key.id == Key::kServerUrl) applyUrl();
  return true;
}

bool Settings::eraseAll(char* err, std::size_t cap) {
  bool ok = true;
  const char* failed = "";
  for (std::size_t i = 0; i < kKeyCount; ++i) {
    const KeyDef& k = kKeys[i];
    if (!store_.erase(k.nvs)) {
      ok = false;
      if (failed[0] == 0) failed = k.name;
    }
    values_[i][0] = 0;
  }
  // The PIN is gone, so the next PIN is a new one: the epoch moves here too, or a refusal
  // recorded against the PIN just forgotten could be applied to the one typed after it.
  if (!bumpPinEpoch()) {
    ok = false;
    if (failed[0] == 0) failed = def(Key::kLtePin).name;
  }
  applyUrl();
  rejected_ = 0;
  first_rejected_ = "";
  if (!ok) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "the store would not forget %s; the rest are forgotten", failed);
    say(err, cap, msg);
  }
  return ok;
}

}  // namespace settings
}  // namespace core
