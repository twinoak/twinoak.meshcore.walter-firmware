# What the bench brought back on 9 November (0.1.3's trial)

The first update of the Walter's own image over LTE, LTE Exit Node 1 (exit node 5 on meshmaster.dk).

- **The offer**, once meshmaster.dk served the manifest (its PRs #190 and #192): 0.1.3+766e061
  stored in slot 1, 784 560 bytes in 12 ranges over LTE.
- **The apply**: job 23943, 15:02:32 UTC. The unit's journal and result:
  `switching` → `rolled-back: the image never reached its main loop (the bootloader gave up on it)`,
  outcome `rolledBack`, 0.1.2 → 0.1.2.
- **`self status`** afterwards:
  ```
  running: slot 0 at 0x010000 (3072 KB): valid (confirmed), version 0.1.2+5aa5542
  other:   slot 1 at 0x310000 (3072 KB): aborted (was on trial at a reset; the bootloader gave up on it), version 0.1.3+766e061
  the running image is confirmed; the last self-update was rolled back
  the last self-update's story (3 entries):
     1. seq 355: received 0.1.3 into slot 1 (784560 bytes, sha256 d8707684e5d017e7...)
     2. seq 363: switching to 0.1.3, from 0.1.2 (the radio answered)
     3. seq 364: rolled back: the image never reached its main loop (the bootloader gave up on it)
  ```
  *aborted*, not *invalid*: the bootloader accepted the image and started it.
- **`reboots`**:
  ```
  boots since the record began: 342
    1. wake from deep sleep (timer) - this boot; the run before it lasted 0 s
    2. wake from deep sleep (timer); the run before it lasted 0 s
    3. panic; the run before it lasted 12 s
    4. wake from deep sleep (timer); the run before it lasted 0 s
    5. wake from deep sleep (timer)
  ```
  The 12 s is 0.1.2's wake that ran the apply; 0.1.3 left no boot of its own in the record.
- **The core dump**, read with `esptool read_flash 0xFF0000 0x10000`:
  [`coredump-0.1.3-ota-trial.bin`](coredump-0.1.3-ota-trial.bin). Its header says 3 204 bytes,
  version `0x00090102` (ESP32-S3, ELF, CRC-32), and the CRC holds. Two tasks, `ipc0` and `ipc1`:
  nothing of ours had run. Its `ESP_PANIC_DETAILS` note:
  ```
  assert failed: esp_startup_start_app app_startup.c:86 (res == pdTRUE)
  ```
  ESP-IDF's `xTaskCreatePinnedToCore` for the main task (`CONFIG_ESP_MAIN_TASK_STACK_SIZE` =
  16 384) failed: no free block of the heap could hold the main task's stack when the static
  initialisers had run. Its `ESP_CORE_DUMP_INFO` names the application ELF's SHA-256 as
  `5c1cb8f37…`: the release build's, not a local one.

What this does not say is how much was missing. That is 01's to measure.
