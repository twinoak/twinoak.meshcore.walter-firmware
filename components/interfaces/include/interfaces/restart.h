// A restart of this board, asked for by the console's `reboot`. The platform's is esp_restart();
// the host tests' records that it was asked and returns, so a test can go on.
#pragma once

namespace interfaces {

class Restart {
 public:
  virtual ~Restart() = default;
  // Restarts the board. On a real board this does not return; a fake may.
  virtual void restart() = 0;
};

}  // namespace interfaces
