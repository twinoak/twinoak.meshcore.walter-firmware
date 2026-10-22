#include <agent/Credentials.h>

#include <cstring>

namespace mm {

namespace {

const char kHex[] = "0123456789abcdef";

// At most kMaxPasswordLen bytes of `text` into `out`; true when there was more.
bool take(char* out, const char* text) {
  size_t n = 0;
  if (text != nullptr) {
    while (text[n] != '\0' && n < kMaxPasswordLen) {
      out[n] = text[n];
      n++;
    }
  }
  out[n] = '\0';
  return text != nullptr && text[n] != '\0';
}

}  // namespace

void wipeBytes(void* p, size_t len) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  for (size_t i = 0; i < len; i++) v[i] = 0;
}

void JobCredential::setAdminPassword(const char* text) {
  wipeBytes(adminPassword, sizeof adminPassword);
  adminPasswordCut = take(adminPassword, text);
  hasAdminPassword = adminPassword[0] != '\0';
}

void JobCredential::setGuestPassword(const char* text) {
  wipeBytes(guestPassword, sizeof guestPassword);
  guestPasswordCut = take(guestPassword, text);
  hasGuestPassword = guestPassword[0] != '\0';
}

void JobCredential::setNewPassword(const char* text) {
  wipeBytes(newPassword, sizeof newPassword);
  newPasswordCut = take(newPassword, text);
  hasNewPassword = newPassword[0] != '\0';
}

void JobCredential::wipe() {
  wipeBytes(adminPassword, sizeof adminPassword);
  wipeBytes(guestPassword, sizeof guestPassword);
  wipeBytes(newPassword, sizeof newPassword);
  hasAdminPassword = false;
  hasGuestPassword = false;
  hasNewPassword = false;
  adminPasswordCut = false;
  guestPasswordCut = false;
  newPasswordCut = false;
}

bool JobCredential::isWiped() const {
  if (hasAdminPassword || hasGuestPassword || hasNewPassword || adminPasswordCut || guestPasswordCut || newPasswordCut) {
    return false;
  }
  for (size_t i = 0; i < sizeof adminPassword; i++) {
    if (adminPassword[i] != 0) return false;
  }
  for (size_t i = 0; i < sizeof guestPassword; i++) {
    if (guestPassword[i] != 0) return false;
  }
  for (size_t i = 0; i < sizeof newPassword; i++) {
    if (newPassword[i] != 0) return false;
  }
  return true;
}

size_t redactLoginPayloadHex(const uint8_t* payload, size_t len, char* out, size_t cap) {
  return redactPayloadHex(payload, len, kLoginPayloadClearBytes, out, cap);
}

size_t redactPayloadHex(const uint8_t* payload, size_t len, size_t clearBytes, char* out, size_t cap) {
  size_t n = 0;
  if (out == nullptr || cap == 0) return 0;
  for (size_t i = 0; i < len && payload != nullptr; i++) {
    if (i < clearBytes) {
      if (n + 3 > cap) break;
      out[n++] = kHex[payload[i] >> 4];
      out[n++] = kHex[payload[i] & 0x0F];
    } else {
      if (n + 2 > cap) break;
      out[n++] = '*';
    }
  }
  out[n] = '\0';
  return n;
}

}  // namespace mm
