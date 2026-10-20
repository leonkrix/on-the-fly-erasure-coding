# Usage

## Build and run
To build, run
```
chmod +x client/baseinstall.sh
bash client/baseinstall.sh
```
at the receiver and at the sender (as root, from the directory that holds `client` and `dependencies`). It installs the packages, builds the dependencies (see the main README, "Dependencies") and the client. Developed on Debian Bullseye. The log files are written to `logs/`, which the script creates.

The sender can be run executing
```
client/udpSender --help --debug -s sender_addr -r receiver_addr -port port -c code_rate -p amount_packets -ws window_size
```
with:
- --help
- --debug (slower, prints debug information)
- -s sender_addr (needed, e.g. 10.0.0.1)
- -r receiver_addr (needed, e.g. 10.0.0.2)
- -port port (optional, default: 1337)
- -c code_rate (optional, default: 4)
- -p amount_packets (optional, default: 100000)
- -ws window_size (optional, default: 256, will be automatically adapted to fit complete gens)
- -para value (optional): a number that is written to the measurement log, for example the loss rate of a run (the experiment scripts use it)

The receiver can be run executing
```
client/udpReceiver --help --debug -s sender_addr -r receiver_addr -port port
```
with:
- --help
- --debug (slower, prints debug information)
- -s sender_addr (needed, e.g. 10.0.0.1)
- -r receiver_addr (needed, e.g. 10.0.0.2)
- -port port (optional, default: 1337)

The order of the flags can be ignored.

> Note: Start the sender first, then the receiver, the sender will wait until the receiver is started and ready.
