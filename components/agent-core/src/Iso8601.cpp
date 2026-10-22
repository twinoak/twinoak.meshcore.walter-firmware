#include <agent/Iso8601.h>

#include <cstdio>

namespace mm {

namespace {

bool readDigits(const char*& p, int count, int& out) {
  out = 0;
  for (int i = 0; i < count; i++) {
    if (p[i] < '0' || p[i] > '9') return false;
    out = out * 10 + (p[i] - '0');
  }
  p += count;
  return true;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil).
int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yy + (m <= 2));
}

int daysInMonth(int y, int m) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return leap ? 29 : 28;
  }
  return days[m - 1];
}

}  // namespace

bool parseUtc(const char* text, uint32_t& epoch) {
  if (text == nullptr) return false;
  const char* p = text;
  int y, mo, d, h, mi, s;
  if (!readDigits(p, 4, y) || *p++ != '-') return false;
  if (!readDigits(p, 2, mo) || *p++ != '-') return false;
  if (!readDigits(p, 2, d) || *p++ != 'T') return false;
  if (!readDigits(p, 2, h) || *p++ != ':') return false;
  if (!readDigits(p, 2, mi) || *p++ != ':') return false;
  if (!readDigits(p, 2, s)) return false;
  if (*p == '.') {
    p++;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
      p++;
      digits++;
    }
    if (digits == 0) return false;
  }
  if (*p == 'Z') {
    p++;
  } else if (p[0] == '+' && p[1] == '0' && p[2] == '0' && p[3] == ':' && p[4] == '0' && p[5] == '0') {
    p += 6;
  } else {
    return false;
  }
  if (*p != '\0') return false;

  if (y < 1970 || y > 2105 || mo < 1 || mo > 12 || d < 1 || d > daysInMonth(y, mo)) return false;
  if (h > 23 || mi > 59 || s > 60) return false;

  const int64_t seconds = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
  if (seconds < 0 || seconds > 0xFFFFFFFFLL) return false;
  epoch = static_cast<uint32_t>(seconds);
  return true;
}

void formatUtc(uint32_t epoch, char* out, size_t cap) {
  int y, m, d;
  civilFromDays(static_cast<int64_t>(epoch / 86400), y, m, d);
  const uint32_t secs = epoch % 86400;
  std::snprintf(out, cap, "%04d-%02d-%02dT%02u:%02u:%02uZ", y, m, d, static_cast<unsigned>(secs / 3600),
                static_cast<unsigned>((secs / 60) % 60), static_cast<unsigned>(secs % 60));
}

void formatClock(uint32_t epoch, char* out, size_t cap) {
  const uint32_t secs = epoch % 86400;
  std::snprintf(out, cap, "%02u:%02u:%02u", static_cast<unsigned>(secs / 3600), static_cast<unsigned>((secs / 60) % 60),
                static_cast<unsigned>(secs % 60));
}

}  // namespace mm
