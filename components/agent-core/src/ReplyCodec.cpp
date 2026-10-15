#include <agent/ReplyCodec.h>

#include <cstdio>
#include <cstring>

namespace mm {

namespace {

// Everything a repeater sends is little-endian: it is a memcpy of a struct on an ESP32 or an
// nRF52, and these read it a byte at a time so that the host tests do not depend on the host's
// own endianness or alignment.
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
int16_t i16(const uint8_t* p) { return static_cast<int16_t>(u16(p)); }
uint32_t u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

const char kHex[] = "0123456789abcdef";

// One Cayenne LPP type: the name, the payload size, the scale and whether it is signed.
//
// Which types are signed is the **writer's** answer and not the reader's (task p3.5-42):
// CayenneLPP::getTypeSigned of the ElectronicCats library a repeater packs its reply with names
// nine of them, and MeshCore's own LPPReader disagrees about `voltage`, which it reads unsigned.
// The writer wins; the two readings differ only above 327.67 V.
struct LppType {
  uint8_t code;
  const char* name;
  uint8_t size;
  double scale;
  bool isSigned;
  const char* unit;
};

const LppType kTypes[] = {
    {0, "digitalInput", 1, 1, false, nullptr},
    {1, "digitalOutput", 1, 1, false, nullptr},
    {2, "analogInput", 2, 0.01, true, nullptr},
    {3, "analogOutput", 2, 0.01, true, nullptr},
    {100, "genericSensor", 4, 1, false, nullptr},
    {101, "luminosity", 2, 1, false, "lux"},
    {102, "presence", 1, 1, false, nullptr},
    {103, "temperature", 2, 0.1, true, "C"},
    {104, "humidity", 1, 0.5, false, "%"},
    {115, "pressure", 2, 0.1, false, "hPa"},
    {116, "voltage", 2, 0.01, true, "V"},
    {117, "current", 2, 0.001, true, "A"},
    {118, "frequency", 4, 1, false, "Hz"},
    {120, "percentage", 1, 1, false, "%"},
    {121, "altitude", 2, 1, true, "m"},
    {125, "concentration", 2, 1, false, "ppm"},
    {128, "power", 2, 1, false, "W"},
    {130, "distance", 4, 0.001, false, "m"},
    {131, "energy", 4, 0.001, false, "kWh"},
    {132, "direction", 2, 1, false, "deg"},
    {133, "unixTime", 4, 1, false, "s"},
    {142, "switch", 1, 1, false, nullptr},
};

// The multi-value types, one reading per component.
struct LppVector {
  uint8_t code;
  const char* name;
  const char* parts[3];
  uint8_t partSize;
  double scale;
  bool isSigned;
  const char* unit;
};

const LppVector kVectors[] = {
    {113, "accelerometer", {"x", "y", "z"}, 2, 0.001, true, "G"},
    {134, "gyrometer", {"x", "y", "z"}, 2, 0.01, true, "deg/s"},
    {135, "colour", {"r", "g", "b"}, 1, 1, false, nullptr},
};

constexpr uint8_t kLppGps = 136;
constexpr size_t kCipherBlockSize = 16;

const LppType* scalarType(uint8_t code) {
  for (const LppType& t : kTypes) {
    if (t.code == code) return &t;
  }
  return nullptr;
}

const LppVector* vectorType(uint8_t code) {
  for (const LppVector& v : kVectors) {
    if (v.code == code) return &v;
  }
  return nullptr;
}

double readBigEndian(const uint8_t* bytes, size_t size, bool isSigned) {
  long long value = 0;
  for (size_t i = 0; i < size; i++) value = (value << 8) | bytes[i];
  if (isSigned) {
    const size_t bits = size * 8;
    if ((value & (1LL << (bits - 1))) != 0) value -= 1LL << bits;
  }
  return static_cast<double>(value);
}

// The tail a telemetry reply arrives with and nothing in the frame accounts for: fewer than a
// cipher block of bytes, all zero. Anything else from the zero channel byte on is a reply this
// table does not understand, and is reported rather than swallowed.
bool isPadding(const uint8_t* tail, size_t len) {
  if (len >= kCipherBlockSize) return false;
  for (size_t i = 0; i < len; i++) {
    if (tail[i] != 0) return false;
  }
  return true;
}

void addReading(LppReading& r, int channel, uint8_t code, const char* name, const char* suffix, double value,
                const char* unit) {
  r.channel = channel;
  r.typeCode = code;
  if (suffix == nullptr) {
    std::snprintf(r.type, sizeof r.type, "%s", name);
  } else {
    std::snprintf(r.type, sizeof r.type, "%s.%s", name, suffix);
  }
  r.value = value;
  r.unit = unit;
}

}  // namespace

void toHexLower(const uint8_t* bytes, size_t len, char* out) {
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = kHex[bytes[i] >> 4];
    out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

const char* permissionName(uint8_t permissions) {
  switch (permissions & kPermRoleMask) {
    case kPermAdmin: return "admin";
    case kPermReadWrite: return "read-write";
    case kPermReadOnly: return "read-only";
    default: return "guest";
  }
}

const char* credentialClassName(CredentialClass c) {
  switch (c) {
    case CredentialClass::Guest: return "guest";
    case CredentialClass::Acl: return "acl";
    case CredentialClass::Admin: return "admin";
    default: return "none";
  }
}

bool parseCredentialClass(const char* text, CredentialClass& out) {
  if (text == nullptr) return false;
  if (std::strcmp(text, "none") == 0) { out = CredentialClass::None; return true; }
  if (std::strcmp(text, "guest") == 0) { out = CredentialClass::Guest; return true; }
  if (std::strcmp(text, "acl") == 0) { out = CredentialClass::Acl; return true; }
  if (std::strcmp(text, "admin") == 0) { out = CredentialClass::Admin; return true; }
  return false;
}

uint8_t roleNeededFor(CredentialClass c) {
  switch (c) {
    case CredentialClass::Admin: return kPermAdmin;
    case CredentialClass::Acl: return kPermReadOnly;
    default: return kPermGuest;
  }
}

uint8_t LoginReply::role() const {
  if (hasAclPermissions) return aclPermissions & kPermRoleMask;
  return isAdminFlag != 0 ? kPermAdmin : kPermGuest;
}

bool parseLoginReply(const uint8_t* data, size_t len, LoginReply& out) {
  if (data == nullptr || len < 2) return false;
  LoginReply r;
  // A legacy repeater answers the two letters "OK" where the new one answers a status byte
  // (MyMesh::onContactResponse, companion-v1.17.1: the same two branches).
  if (data[0] == 'O' && data[1] == 'K') {
    r.legacy = true;
    out = r;
    return true;
  }
  if (data[0] != 0) return false;  // not RESP_SERVER_LOGIN_OK
  if (len < 4) return false;
  r.isAdminFlag = data[2];
  r.hasAclPermissions = true;
  r.aclPermissions = data[3];
  if (len >= 9) {
    r.hasFirmwareLevel = true;
    r.firmwareLevel = data[8];
  }
  out = r;
  return true;
}

bool parseRepeaterStats(const uint8_t* stats, size_t len, StatusReading& out) {
  if (stats == nullptr || len < kRepeaterStatsMinLen) return false;
  StatusReading s;
  size_t i = 0;
  s.batteryMv = u16(&stats[i]); i += 2;
  s.txQueueLen = u16(&stats[i]); i += 2;
  s.noiseFloorDbm = i16(&stats[i]); i += 2;
  s.lastRssi = i16(&stats[i]); i += 2;
  s.packetsRecv = u32(&stats[i]); i += 4;
  s.packetsSent = u32(&stats[i]); i += 4;
  s.airtimeS = u32(&stats[i]); i += 4;
  s.uptimeS = u32(&stats[i]); i += 4;
  s.sentFlood = u32(&stats[i]); i += 4;
  s.sentDirect = u32(&stats[i]); i += 4;
  s.recvFlood = u32(&stats[i]); i += 4;
  s.recvDirect = u32(&stats[i]); i += 4;
  s.errEvents = u16(&stats[i]); i += 2;
  s.lastSnr = i16(&stats[i]) / 4.0; i += 2;
  s.directDups = u16(&stats[i]); i += 2;
  s.floodDups = u16(&stats[i]); i += 2;
  // The struct grew at the end, so a shorter one parses field for field and the last two are
  // unknown rather than zero - a zero would be reported as a real reading of no errors.
  if (len >= 52) {
    s.hasRxAirtime = true;
    s.rxAirtimeS = u32(&stats[i]);
    i += 4;
  }
  if (len >= 56) {
    s.hasRecvErrors = true;
    s.recvErrors = u32(&stats[i]);
  }
  out = s;
  return true;
}

bool parseOwnerInfoFirmware(const uint8_t* data, size_t len, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  if (data == nullptr || len == 0) return false;
  size_t n = 0;
  while (n < len && data[n] != '\n' && data[n] != '\0') n++;
  if (n == 0) return false;
  if (n > cap - 1) n = cap - 1;
  std::memcpy(out, data, n);
  out[n] = '\0';
  return true;
}

bool parseNeighboursPage(const uint8_t* data, size_t len, int prefixLen, NeighboursPage& out) {
  if (data == nullptr || len < 4 || prefixLen <= 0 || prefixLen > kNeighbourPrefixLen) return false;
  NeighboursPage page;
  page.total = i16(&data[0]);
  const int returned = i16(&data[2]);
  size_t i = 4;
  const size_t stride = static_cast<size_t>(prefixLen) + 5;
  for (int n = 0; n < returned && page.count < kNeighboursPerPage && i + stride <= len; n++) {
    Neighbour& e = page.entries[page.count];
    toHexLower(&data[i], static_cast<size_t>(prefixLen), e.prefix);
    i += static_cast<size_t>(prefixLen);
    e.lastHeardS = static_cast<int32_t>(u32(&data[i]));
    i += 4;
    e.snr = static_cast<int8_t>(data[i]) / 4.0;
    i += 1;
    page.count++;
  }
  if (page.total < page.count) page.total = page.count;
  out = page;
  return true;
}

int decodeLpp(const uint8_t* lpp, size_t len, LppReading* out, int max, size_t& undecodedFrom) {
  undecodedFrom = len;
  if (lpp == nullptr || out == nullptr || max <= 0) return 0;
  int count = 0;
  size_t i = 0;
  while (i < len) {
    if (lpp[i] == 0) {
      undecodedFrom = isPadding(&lpp[i], len - i) ? len : i;
      return count;
    }
    if (i + 2 > len) break;
    const int channel = lpp[i];
    const uint8_t code = lpp[i + 1];
    const size_t start = i;
    i += 2;

    if (const LppType* t = scalarType(code)) {
      if (i + t->size > len || count >= max) {
        undecodedFrom = start;
        return count;
      }
      addReading(out[count++], channel, code, t->name, nullptr, readBigEndian(&lpp[i], t->size, t->isSigned) * t->scale,
                 t->unit);
      i += t->size;
    } else if (const LppVector* v = vectorType(code)) {
      const size_t size = static_cast<size_t>(v->partSize) * 3;
      if (i + size > len || count + 3 > max) {
        undecodedFrom = start;
        return count;
      }
      for (int p = 0; p < 3; p++) {
        addReading(out[count++], channel, code, v->name, v->parts[p],
                   readBigEndian(&lpp[i + static_cast<size_t>(p) * v->partSize], v->partSize, v->isSigned) * v->scale,
                   v->unit);
      }
      i += size;
    } else if (code == kLppGps) {
      // 3-byte lat and lon at 0.0001 degrees, 3-byte altitude at 0.01 m, all signed.
      if (i + 9 > len || count + 3 > max) {
        undecodedFrom = start;
        return count;
      }
      addReading(out[count++], channel, code, "gps", "lat", readBigEndian(&lpp[i], 3, true) * 0.0001, "deg");
      addReading(out[count++], channel, code, "gps", "lon", readBigEndian(&lpp[i + 3], 3, true) * 0.0001, "deg");
      addReading(out[count++], channel, code, "gps", "alt", readBigEndian(&lpp[i + 6], 3, true) * 0.01, "m");
      i += 9;
    } else {
      // A length nobody knows cannot be skipped over honestly.
      undecodedFrom = start;
      return count;
    }
  }
  undecodedFrom = i;
  return count;
}

uint8_t traceFlagsFor(int hashSize) {
  switch (hashSize) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    default: return 3;
  }
}

bool parseTrace(uint8_t flags, const uint8_t* hashes, size_t hashBytes, const uint8_t* snrsQ4, size_t snrCount,
                TraceReading& out) {
  if (hashes == nullptr) return false;
  TraceReading t;
  t.hashSize = traceHashSizeFor(flags);
  const size_t hops = hashBytes / static_cast<size_t>(t.hashSize);
  for (size_t h = 0; h < hops && t.count < kMaxTraceHops; h++) {
    TraceHop& hop = t.hops[t.count];
    toHexLower(&hashes[h * static_cast<size_t>(t.hashSize)], static_cast<size_t>(t.hashSize), hop.prefix);
    if (snrsQ4 != nullptr && h < snrCount) {
      hop.hasSnr = true;
      hop.snr = static_cast<int8_t>(snrsQ4[h]) / 4.0;
    }
    t.count++;
  }
  out = t;
  return true;
}

bool parseAccessList(const uint8_t* data, size_t len, AccessListEntry* out, int max, int& count) {
  count = 0;
  if (data == nullptr && len > 0) return false;
  size_t offset = 0;
  while (offset + static_cast<size_t>(kAccessListEntrySize) <= len && data[offset + kAccessListPrefixBytes] != 0) {
    if (count < max) {
      AccessListEntry& e = out[count];
      e = AccessListEntry();
      toHexLower(&data[offset], static_cast<size_t>(kAccessListPrefixBytes), e.prefix);
      e.permissions = data[offset + kAccessListPrefixBytes];
    }
    count++;
    offset += static_cast<size_t>(kAccessListEntrySize);
  }
  if (count > max) count = max;
  // What follows the last entry is the cipher block's padding and must be zero: anything else
  // means the bytes are not the layout this reader knows, and a guessed ACL is what would revoke
  // an association. Refuse instead.
  for (size_t i = offset; i < len; i++) {
    if (data[i] != 0) {
      count = 0;
      return false;
    }
  }
  return true;
}

bool parseCliReply(const uint8_t* data, size_t len, char* text, size_t cap) {
  if (text == nullptr || cap == 0) return false;
  text[0] = 0;
  if (data == nullptr || len <= 5) return false;
  const uint8_t type = static_cast<uint8_t>(data[4] >> 2);
  if (type != kTxtTypeCliData && type != kTxtTypePlain) return false;
  size_t n = 0;
  const size_t avail = len - 5;
  while (n < avail && n < kMaxCliReplyLen && n + 1 < cap && data[5 + n] != 0) {
    text[n] = static_cast<char>(data[5 + n]);
    n++;
  }
  text[n] = 0;
  return true;
}

}  // namespace mm
