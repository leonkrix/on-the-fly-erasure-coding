#!/bin/bash
#
# Sets up the three nodes of the testbed (sender, router with netem, receiver): reboots them
# with a Debian Bullseye image, copies this repository and the dependencies, builds the client and
# configures the network. It is run on the controller of the testbed (the "pos" tool), from the
# directory that holds this repository and the folder "dependencies" (see the README).
# The names of the nodes (litecoin, litecoincash, litecoingold) are those of the testbed it was
# written for; use your own.

pos allocations allocate litecoin litecoincash litecoingold

pos nodes image litecoin debian-bullseye
pos nodes image litecoincash debian-bullseye
pos nodes image litecoingold debian-bullseye

IdLitecoin=$(pos nodes reset litecoin --non-blocking)
IdLitecoincash=$(pos nodes reset litecoincash --non-blocking)
IdLitecoingold=$(pos nodes reset litecoingold --non-blocking)

pos nodes list
echo "waiting for reset"

pos commands await "$IdLitecoin"
pos commands await "$IdLitecoincash"
pos commands await "$IdLitecoingold"

pos nodes bootstrap litecoin
pos nodes bootstrap litecoincash
pos nodes bootstrap litecoingold

pos nodes list
echo "all rebooted"

cd "$HOME"
set -x
set -e

pos nodes copy -r litecoin on-the-fly-erasure-coding/client
pos nodes copy -r litecoingold on-the-fly-erasure-coding/client
pos nodes copy -r litecoin dependencies
pos nodes copy -r litecoingold dependencies

pos commands launch litecoin chmod +x client/baseinstall.sh
pos commands launch litecoingold chmod +x client/baseinstall.sh

IdLitecoinBI=$(pos commands launch litecoin bash client/baseinstall.sh --non-blocking)
pos commands launch litecoingold bash client/baseinstall.sh

echo "waiting for base install"

pos commands await "$IdLitecoinBI"

echo "base install finished"

pos nodes copy -r litecoin on-the-fly-erasure-coding/experiments
pos nodes copy -r litecoincash on-the-fly-erasure-coding/experiments
pos nodes copy -r litecoingold on-the-fly-erasure-coding/experiments

pos commands launch litecoin bash experiments/setup/litecoin/config.sh
pos commands launch litecoincash bash experiments/setup/litecoincash/config.sh
pos commands launch litecoingold bash experiments/setup/litecoingold/config.sh

echo "setup complete"
