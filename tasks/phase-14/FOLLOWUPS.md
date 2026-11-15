# Phase 14 — follow-ups

Items for a later task of this phase are tagged with it. Only the reviewer ticks an item.

- [ ] **[later] The library counts a flood under the scope it *asked* for, not the one it got.**
  `FloodPolicy::sent(flooded, mode)` keeps the mode the rule chose and `answered()` files the
  transaction under it (`components/agent-core/src/FloodScope.cpp`), so a flood asked `Scoped`
  that §6.2's `scoped` says went out **unscoped** - a `region default` cleared under this end's
  feet, a `mode` 2 that did not take - is counted as `answeredScoped`. This end notices (the
  name is read again and `LinkMeshCounters::scope_surprises` counts it) and cannot correct the
  library's figure: `agent-core` is a hash-checked copy of upstream and is not changed here.
  The cure is in the exit-node firmware: `sent()` would take what the mesh says the send came
  to. Until then `scope_surprises` is how often the three counts could be wrong, and on a
  healthy unit it is 0.
- [ ] **[later] No way to turn the unscoped try off on a unit.** The library's `flood.unscoped`
  (*retry* or *never*) has a console key on the companion exit nodes and none here: the default,
  *retry*, holds. The day `flood.max.unscoped` is nought on the Danish mesh that try is airtime
  for nothing, and the heartbeat's `answeredUnscoped` is how the day is seen coming.
- [ ] **[later] The radio's half is the repeater firmware's pile 3.13.** Until a radio of link 1.3
  is on the unit this board says nothing about scope and its heartbeat has no `flood` object.
  That is correct, and worth knowing before somebody looks for the line on meshmaster.dk.
- [ ] **[later] The specification's source is not committed.** `spec/MANIFEST.txt` says *at commit
  204210b plus uncommitted changes*: links 1.1, 1.2 and 1.3 are in the working tree of
  `twinoak.meshcore.standard-repeater-design-v2/software/spec` and in no commit of it. Thomas's
  to commit there; the copy here is byte for byte what that tree holds.
- [ ] **[later] Phase 13's follow-ups are still open**, the first of them above all: a wake by the
  radio's line inside a wait sends nothing.
  The first is task 06 of this pile, written on 15 November.
- [ ] **[later] The radio says `ok` to `walter.wake 1` whether or not it raised an attention.**
  Refused inside the hourly limit, or with one already pending, the answer is the same `ok`
  (`Settings.cpp`, repeater firmware). `sensor get walter.wake` is where the truth is read, and
  meshmaster.dk's wake job does not read it. The repeater firmware's and meshmaster.dk's to
  settle, not this board's.
- [ ] **[later] After a power cut the wait is owed whole.** The saved state is in flash and
  survives; the radio restarted with the board, so nothing measures the time away. The epoch
  beside the saved state, once the board has a clock again, would.
