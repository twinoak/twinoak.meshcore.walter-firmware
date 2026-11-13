# What meshmaster.dk says of the update to v0.1.4

Read by the reviewer on 12 November 2026 from meshmaster.dk: the page of **LTE Exit Node 1** (exit
node 5, the uplink of *LTE Repeater Node 1*, repeater 41) and the pages of the five jobs. Times are
UTC, 10 November 2026. Nobody was at the unit and no console was watched, so everything here is the
server's side.

## The four offers

| Job | Made | Ended | What the job's page says |
|---|---|---|---|
| 24781 | 11:05:43 | 11:35:50 | *expired*: no exit node took this job before it expired. Attempts 0 of 1 |
| 24812 | 11:58:26 | 12:28:50 | the same |
| 24837 | 12:30:01 | 13:00:50 | the same |
| 24932 | 14:42:11 | 14:52:27 | *succeeded*: stored as the candidate, `manager.walter.lte-m`, 781 kB |

An offer lives 30 minutes on the server. The first three were never handed to the unit: it did not
claim anything inside any of the three windows. Why it was not checking in then is not on these
pages.

## The offer that was taken (24932)

The result, as posted:

```json
{
  "bytes": 780528,
  "sha256": "5420b4d9e72524c0b2405f5f9c3d467e6a4536e4f08aba236325515002527271",
  "stored": true,
  "imageId": "manager.walter.lte-m",
  "resumed": false
}
```

The agent's log, whole:

```
job 24932 twinoak.fw.offer for repeater 41 "LTE Repeater Node 1" (none)
path from the server
this mesh answers for this repeater without the air: no airtime budget, no transaction
lease extended
stored manager.walter.lte-m 0.1.4+62538fa as the other slot's image, 780528 bytes
```

- One progress line is kept on the page: **687 kB of 781 kB at 14:51:31**. The attempt ended at
  14:52:26.
- The page does not say when the job was claimed, so the download's length is known only as
  **at most 10 min 15 s** (made 14:42:11, stored 14:52:26).
- The server's own count for the file, on the unit's page: *downloaded 877 kB of 781 kB of
  0.1.4+62538fa (more than the files are long: ranges were resumed or repeated) ·
  twinoak-walter-v0.1.4.bin · 14 requests*. Twelve ranges of 64 kB cover 780 528 bytes, so two
  requests and about 96 kB were asked for twice. Neither the result (`resumed: false`) nor the log
  mentions it.
- For comparison, v0.1.3 on 9 November: *785 kB of 785 kB · 12 requests*.

## The apply (24938)

Made 14:53:18, ended 15:20:17, lease 30 min, attempt 1 of 1. What the unit said:

> apply twinoak-walter-firmware release 6 to slot 0 — committed: the candidate proved itself and is
> the golden image now (0.1.2 → 0.1.4)

The journal in the result:

```
2026-11-10 15:20:13Z switching
2026-11-10 15:20:13Z on-trial
2026-11-10 15:20:13Z confirmed
```

All three lines carry the same second.

## Since

The heartbeat of 12 November: agent `0.1.4+62538fa`, check-in every 15 min (900 s) *by its own
choice (battery)*, companion connected, the radio's firmware `0.1.2+9e767dc`.

The update before it, for the other half of the story: v0.1.3 on 9 November, offer 23918 and apply
23943, *rolled back: the candidate failed its proof and the golden image is back (0.1.2 → 0.1.2)*
(`tasks/phase-11/evidence/`).
