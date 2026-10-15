// A claimed job as the server writes one (task p1.5-17).
//
// `tests/fixtures/contracts/ClaimedJob.json` is the shape, member for member, and this builder
// writes it: a test names the id, the type, the class, the lease, the payload and the credential,
// and `FakeUplink::queue` hands the result to `mm::parseClaimResponse` - the firmware's own
// parser - so nothing here is a second reading of the envelope.
//
// The one thing a test must change from the fixture is the repeater's **public key**: the
// fixture's is `a1b2`, four hex characters, which is all the server's own round-trip test needed
// and which `AgentMesh::beginJob` (and `FakeMesh`'s reading of it) refuses, because a key that is
// not 64 hex characters is not an identity to address. So the keys below are whole ones.

#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include <agent/Iso8601.h>

namespace fake {

// The repeater this mesh has, the exit node's own key, and a third key for an `acl.set` on
// somebody else's entry. Whole keys, lowercase, as a claim carries them.
inline const std::string& repeaterKey() {
  static const std::string key = "7f8e9dacbbcad9e8" + std::string(48, '3');
  return key;
}
inline const std::string& selfKey() {
  static const std::string key = "c3d4e5f607182900" + std::string(48, '7');
  return key;
}
inline const std::string& strangerKey() {
  static const std::string key = "beef0011223344aa" + std::string(48, '9');
  return key;
}

inline std::string iso(uint32_t epoch) {
  char out[24];
  mm::formatUtc(epoch, out, sizeof out);
  return out;
}

class Job {
 public:
  Job(long long id, std::string type) : id_(id), type_(std::move(type)) {}

  Job& forRepeater(long long id, std::string label, std::string key) {
    repeaterId_ = id;
    label_ = std::move(label);
    key_ = std::move(key);
    return *this;
  }
  Job& atClass(std::string credentialClass) {
    class_ = std::move(credentialClass);
    return *this;
  }
  Job& leasedUntil(uint32_t epoch) {
    lease_ = epoch;
    return *this;
  }
  Job& withTimeouts(int perHopMs, int loginMs) {
    perHopMs_ = perHopMs;
    loginMs_ = loginMs;
    return *this;
  }
  Job& withPayload(std::string json) {
    payload_ = std::move(json);
    return *this;
  }
  Job& withAdminPassword(std::string password) {
    adminPassword_ = std::move(password);
    hasAdmin_ = true;
    return *this;
  }
  Job& withGuestPassword(std::string password) {
    guestPassword_ = std::move(password);
    hasGuest_ = true;
    return *this;
  }
  Job& withNewPassword(std::string password) {
    newPassword_ = std::move(password);
    hasNew_ = true;
    return *this;
  }
  Job& withAclLevel(int level) {
    aclLevel_ = level;
    hasAcl_ = true;
    return *this;
  }
  Job& withCandidate(std::string which) {
    candidate_ = std::move(which);
    return *this;
  }
  // The path reachability knows: the hop hashes as hex, in order.
  Job& alongPath(std::vector<std::string> hops) {
    hops_ = std::move(hops);
    hasPath_ = true;
    return *this;
  }
  // A direct neighbour: a known path of zero hops, which is not the same as no path at all.
  Job& asDirectNeighbour() {
    hops_.clear();
    hasPath_ = true;
    return *this;
  }
  Job& withNoPath() {
    hops_.clear();
    hasPath_ = false;
    return *this;
  }
  Job& groupedWith(long long other) {
    group_.push_back(other);
    return *this;
  }
  Job& idempotency(std::string key) {
    idempotency_ = std::move(key);
    return *this;
  }

  std::string json() const {
    std::string out = "{";
    out += "\"id\":" + std::to_string(id_);
    out += ",\"type\":\"" + type_ + "\"";
    out += ",\"repeater\":{\"id\":" + std::to_string(repeaterId_) + ",\"label\":\"" + label_ +
           "\",\"publicKey\":\"" + key_ + "\"}";
    out += ",\"credentialClass\":\"" + class_ + "\"";
    out += ",\"leaseUntil\":\"" + iso(lease_) + "\"";
    out += ",\"timeouts\":{\"perHopMs\":" + std::to_string(perHopMs_) + ",\"loginMs\":" +
           std::to_string(loginMs_) + "}";
    out += ",\"payload\":" + payload_;
    const bool credential = hasAdmin_ || hasGuest_ || hasNew_ || hasAcl_ || !candidate_.empty();
    if (credential) {
      out += ",\"credential\":{";
      bool first = true;
      if (hasGuest_) {
        out += std::string(first ? "" : ",") + "\"guestPassword\":\"" + guestPassword_ + "\"";
        first = false;
      }
      if (hasAdmin_) {
        out += std::string(first ? "" : ",") + "\"adminPassword\":\"" + adminPassword_ + "\"";
        first = false;
      }
      if (hasAcl_) {
        out += std::string(first ? "" : ",") + "\"acl\":" + std::to_string(aclLevel_);
        first = false;
      }
      if (hasNew_) {
        out += std::string(first ? "" : ",") + "\"newPassword\":\"" + newPassword_ + "\"";
        first = false;
      }
      if (!candidate_.empty()) {
        out += std::string(first ? "" : ",") + "\"candidate\":\"" + candidate_ + "\"";
      }
      out += "}";
    }
    if (hasPath_) {
      out += ",\"path\":{\"hops\":[";
      for (size_t i = 0; i < hops_.size(); i++) {
        if (i > 0) out += ",";
        out += "\"" + hops_[i] + "\"";
      }
      out += "],\"source\":\"advert\",\"learnedAt\":\"" + iso(lease_ - 120) + "\"}";
    }
    out += ",\"idempotencyKey\":\"" + idempotency_ + "\"";
    if (!group_.empty()) {
      out += ",\"groupWith\":[";
      for (size_t i = 0; i < group_.size(); i++) {
        if (i > 0) out += ",";
        out += std::to_string(group_[i]);
      }
      out += "]";
    }
    out += "}";
    return out;
  }

  operator std::string() const { return json(); }

 private:
  long long id_;
  std::string type_;
  long long repeaterId_ = 17;
  std::string label_ = "Grenaa Chimney #1";
  std::string key_ = repeaterKey();
  std::string class_ = "none";
  uint32_t lease_ = 0;
  int perHopMs_ = 4000;
  int loginMs_ = 8000;
  std::string payload_ = "{}";
  std::string adminPassword_;
  std::string guestPassword_;
  std::string newPassword_;
  std::string candidate_;
  int aclLevel_ = 0;
  bool hasAdmin_ = false;
  bool hasGuest_ = false;
  bool hasNew_ = false;
  bool hasAcl_ = false;
  bool hasPath_ = true;
  std::vector<std::string> hops_ = {"a1", "7f"};
  std::vector<long long> group_;
  std::string idempotency_ = "schedule:17:job:2026-09-18T10:00Z";
};

}  // namespace fake
