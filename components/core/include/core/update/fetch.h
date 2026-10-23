// The fetch of a release (task p3-02): the three steps that bring a signed release from a base
// URL into the image store, over the client, the download and phase 2's verdict -
//
//   1. GET `<base>/manifest.json` and `<base>/manifest.json.sig`, small and bounded (the same
//      8 KB and 128 bytes the USB transfer allows, core/image/receive.h);
//   2. checks 1, 2, 3 and the manifest's half of 6 (ImageVerdict::judgeManifest) - the signature
//      by a key this build trusts, the manifest, the image for this slot - **before a single byte
//      of the image is asked for**;
//   3. the one file into the candidate (or scanner) slot, in ranges, resumable
//      (core/net/download.h); then checks 4, 5 and the rest of 6 over the bytes as they lie in
//      the Walter's flash, and the record written with the verdict's token.
//
// What this class does **not** decide: where releases live, or that there is a new one. The base
// URL is the caller's - the console's `fetch <base-url>` today, the wake cycle's backend seam
// later (spec/SYSTEM.md §8). Nothing here names a server.
//
// A refusal at check 4, 5 or 6 leaves the slot empty and the bytes gone: a file whose hash is not
// the manifest's would otherwise be "resumed" into the next fetch and fail check 4 for ever. A
// download that stops on its budget leaves the bytes and the store's progress record, so the
// next wake continues where this one ended.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/board/slot.h"
#include "core/image/receive.h"
#include "core/image/store.h"
#include "core/image/verdict.h"
#include "core/net/client.h"
#include "core/net/download.h"
#include "core/net/url.h"
#include "interfaces/clock.h"
#include "interfaces/text_out.h"

namespace core {
namespace update {

// The two small requests: a manifest and its signature are a few KB; a minute each is a link
// that is not working. TO BE SET ON THE BENCH.
constexpr std::uint32_t kManifestRequestMs = 60000;

enum class FetchVerdict : std::uint8_t {
  kStored = 0,       // all six checks passed; the record is written
  kRefused = 1,      // one of the six checks refused it: `check` and `why` say which
  kDenied = 2,       // 401 or 403 from the server
  kLater = 3,        // 429 or 5xx: `retry_after_s`
  kBudget = 4,       // the download stopped on its budget; the bytes stay, it resumes next wake
  kFailed = 5,       // a request or a range failed; the bytes stay for a resume where there were any
  kBadUrl = 6,       // not an https base URL, or too long once the file name is appended
  kNoSlot = 7,       // this adapter has no radio in that slot, or the golden image was asked for
  kStoreRefused = 8, // the store would not open the write (the bad list, a transfer open ...)
  kWrongFile = 9,    // the server's file is not the size the manifest promised
};

const char* describe(FetchVerdict verdict);

struct FetchResult {
  FetchVerdict verdict = FetchVerdict::kFailed;
  const char* why = "";
  image::Check check = image::Check::kNone;  // kRefused: which check
  std::uint16_t status = 0;                  // the last HTTP status seen
  std::uint64_t bytes = 0;                   // image bytes downloaded in this call
  std::uint32_t held = 0;                    // image bytes in flash afterwards (0 when stored or refused)
  std::uint32_t expected = 0;                // the file's size, once the manifest was accepted
  std::uint32_t retry_after_s = 0;
  std::uint32_t elapsed_ms = 0;
  char file_name[image::kMaxFileName] = {};
  char version[image::kVersionSize] = {};
  // What the agent's offer reports (task p4-05): the file's hash as the manifest promised it
  // (set once the manifest was accepted), how many bytes were already in flash when the download
  // began (a resumed download), and - once stored - the manifest's image id and the descriptor
  // of the file as it lies in flash.
  std::uint8_t sha256[image::kSha256Size] = {};
  std::uint32_t resumed_from = 0;
  char image_id[image::kMaxIdSize] = {};
  image::Fields descriptor;
};

class Fetch {
 public:
  // `out` may be nullptr; with one, each step prints a line as it passes (the console's `fetch`).
  Fetch(net::Client& client, net::Download& download, image::Store& store,
        image::ImageVerdict& verdict, interfaces::Clock& clock, interfaces::TextOut* out);

  // `base_url` is where `manifest.json`, `manifest.json.sig` and the files beside them are; with
  // or without a trailing slash. `which` is the candidate or the scanner image - never golden,
  // which is reached by a promotion (core/image/store.h). `authorization` goes with every request
  // (nullptr for none), and a request that carries one is *sensitive*: the transport wipes the
  // head it wrote once it is on the wire (core/net/client.h, task p4-03's rule).
  //
  // Two things the agent's `twinoak.fw.offer` needed of this (task p4-05): `file_dir` is a
  // directory under the base the files are in - meshmaster.dk serves a release's files at
  // `{base}/files/{name}` and its manifest at `{base}/manifest.json` (its
  // `docs/dev/unit-protocol.md` §4) - nullptr being "beside the manifest", as the console's
  // `fetch` and a release on GitHub have them; and `listener` hears every range as it ends, which
  // is where a progress report and a lease extension go between ranges (the download's own
  // listener, core/net/download.h), beside the console's line for it.
  FetchResult run(const char* base_url, image::Which which, board::Slot slot,
                  const net::Budget& budget, const char* authorization = nullptr,
                  const char* file_dir = nullptr, net::RangeListener* listener = nullptr);

  // The same three steps into any transfer target (task p4-05): the Walter's own other
  // application slot, which `core/update/self_update.h` is as a target, for a
  // `twinoak.fw.offer` whose product is the Walter's. The target's own preconditions - a radio
  // update in progress, the running image unconfirmed - are the caller's to ask first
  // (`SelfUpdate::canReceive`); the six checks are the same as for a radio image.
  FetchResult runInto(const char* base_url, image::TransferTarget& target, const net::Budget& budget,
                      const char* authorization = nullptr, const char* file_dir = nullptr,
                      net::RangeListener* listener = nullptr);

 private:
  bool buildUrl(const char* base_url, const char* name);
  FetchResult fetchInto(const char* base_url, image::TransferTarget& target, image::Store* store,
                        image::Which which, board::Slot slot, const net::Budget& budget,
                        const char* authorization, const char* file_dir, net::RangeListener* listener);
  void print(const char* text);
  void printf(const char* format, ...) __attribute__((format(printf, 2, 3)));

  net::Client& client_;
  net::Download& download_;
  image::Store& store_;
  image::ImageVerdict& verdict_;
  interfaces::Clock& clock_;
  interfaces::TextOut* out_;
  image::StoreTarget target_;
  // Static storage: the manifest, its signature, the URL being fetched.
  std::uint8_t manifest_[image::transfer::kMaxManifestBytes];
  char signature_[image::transfer::kMaxSignatureBytes];
  char url_[net::kMaxUrlLength + 1];
};

}  // namespace update
}  // namespace core
