A measurement session now ends *restored* instead of *stuck*: the radio is restarted by its reset
line after a flash, which stops the scanner's own watchdog from resetting the repeater a minute
into the health check that follows. Every flash of a radio ends that way now. Seen on simulators
only — the one session this has been through on a board is the one that went wrong.
