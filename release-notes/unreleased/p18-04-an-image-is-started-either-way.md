A unit whose reset line does not reach its radio can now flash it all the same: the chip is asked
whether it has a reset pin at all, and an image a pulse did not start is started over the debug
port instead. `rak swd id` and `rak watch` say what this unit's line is. Tested on the host against
a simulated radio with a dead line; no board with one has been flashed yet.
