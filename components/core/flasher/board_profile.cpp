#include "core/flasher/board_profile.h"

namespace core {
namespace flasher {

namespace {

bool same(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  while (*a != 0 && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

const BoardProfile* const kProfiles[] = {&kRak4630Single};

}  // namespace

const BoardProfile* findProfile(const char* board_name) {
  for (const BoardProfile* p : kProfiles) {
    if (same(p->name, board_name)) return p;
  }
  return nullptr;
}

}  // namespace flasher
}  // namespace core
