When the radio is busy writing its own flash, a flash of it no longer gives up after a few
milliseconds: the debug port now waits the operation out, up to half a second, so a session's
first try should go through instead of failing and being made again. The journal says how long it
waited, where, and how often. Tested on the host; the unit's next session is the first board to
meet it.
