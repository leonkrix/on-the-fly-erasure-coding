# On-the-fly erasure coding

An implementation of **Tetrys**, an on-the-fly network erasure code for delay- and loss-sensitive
data, as a C++ client and server over UDP, together with the library, the simulator and the
experiment scripts that were used to evaluate it on a testbed.

It is the code of my Bachelor's thesis *On-The-Fly Network Erasure Coding Protocol for Delay and
Loss-Sensitive Data* (Technical University of Munich, Chair of Network Architectures and Services,
2023 to 2024).

## Background

Real-time applications cannot wait for retransmissions, and classic block erasure codes add a delay
that depends on the block size and the round-trip time. Tetrys codes **on the fly**: the sender keeps
a sliding window of source packets and sends a coded packet (a random linear combination over a
finite field) after every few source packets. The receiver acknowledges what it has decoded, which
lets the sender shrink the window, and lost source packets are recovered from the coded packets
without a retransmission. The existing work on Tetrys was based on simulation; this repository is a
real implementation that was measured in a real network.

The thesis found that the implementation reaches full reliability in almost all scenarios without
retransmissions, with a delay below one round-trip time, and that the encoding time on the sender
grows quickly with the size of the sliding window, which limits where it can be used.

## What is in this repository

| Folder | What it is |
| --- | --- |
| [`client/`](client) | The Tetrys client and server over UDP: `udpSender` and `udpReceiver`. This is what was measured. |
| [`lib/`](lib) | The coding functionality as a small, documented library (encoder, decoder, acknowledgements) with a Doxygen documentation. |
| [`simulator/`](simulator) | A simulator of the protocol (sliding window, coded packets, loss of source, coded and acknowledgement packets), for understanding and debugging the logic. |
| [`experiments/`](experiments) | The scripts that run the measurements on the testbed (uniform loss, burst loss, delay) and set the nodes up. |
| [`wireshark/`](wireshark) | A Wireshark dissector for the packets of the protocol (`tetrys.lua`). |

The measurement results are not part of the repository.

## Dependencies

All three programs use finite field arithmetic and random linear network coding from three libraries
of the *moep* family:

- **libmoepgf**, finite field arithmetic, public: <https://github.com/moepinet/libmoepgf> (LGPL-2.1).
- **libmoep-2.0-nc** and **moep80211ncm-0.0** (which contains **libmoeprlnc**), random linear
  network coding. They were published on the download server of the chair, which is not online any
  more. **They are available on request at the Chair of Network Architectures and Services at TUM.**

The build script expects the three libraries in a folder `dependencies/` next to `client/`, one folder
each (`libmoepgf`, `libmoep-2.0-nc`, `moep80211ncm-0.0`). Besides them the client needs `spdlog`,
CMake and a C++17 compiler (`client/baseinstall.sh` installs the packages on Debian).

## Build and run

Developed and measured on Debian 11 (Bullseye); other systems were not tried.

The client is built with `client/baseinstall.sh` and run as described in
[`client/README.md`](client/README.md). The simulator is described in
[`simulator/README.md`](simulator/README.md), the library in [`lib/README.md`](lib/README.md).

## Experiments

The measurements ran on a testbed of three nodes controlled with the *pos* tool of the chair: a sender
and a receiver connected through a router node that adds loss and delay with `tc netem`.

```
sender (10.0.0.1)  <-->  router (netem: loss, burst loss, delay)  <-->  receiver (10.0.0.4)
```

- `experiments/setup/setup.sh` reboots the nodes with a Debian image, copies the repository and the
  dependencies, builds the client and configures the network (`experiments/setup/<node>/config.sh`).
  The node names are those of the testbed it was written for.
- `experiments/uniform_loss/run.py <code rate> <max loss in %> <runs per loss> <window size>` measures
  with a constant loss rate that grows in steps of 0.1 %; `run_all.py` repeats it for several code
  rates and window sizes.
- `experiments/burst_loss/run.py` and `experiments/delay/run.py` measure with bursts of losses (a
  two-state loss model) and with an added delay.

The scripts write the measurements of the sender and the receiver as CSV files to `~/results/`.

## Status

A research prototype made for a thesis: it has no automated test suite, and the code of the three
programs (`client`, `simulator`, `lib`) is not unified.

## References

- J. Detchart, E. Lochin, J. Lacan and V. Roca: *Tetrys, an On-the-Fly Network Coding Protocol*,
  Internet-Draft draft-irtf-nwcrg-tetrys-03, IETF, 2022.
- P. U. Tournoux, E. Lochin, J. Lacan, A. Bouabdallah and V. Roca: *On-the-fly erasure coding for
  real-time video applications*, IEEE Transactions on Multimedia 13(4), 2011.

## License

Copyright (c) 2023-2024 Leon Krix. All rights reserved, see [`LICENSE`](LICENSE). The libraries listed
under "Dependencies" keep their own licenses.
