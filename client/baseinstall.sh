#!/bin/bash
# Builds the client (udpSender and udpReceiver) on a node. Debian Bullseye, run as root from the
# directory that holds "client" and "dependencies". "dependencies" holds the three libraries,
# each in its own folder: libmoepgf, libmoep-2.0-nc and moep80211ncm-0.0 (see the README).
apt update -y
apt install -y cmake gcc clang meson ninja-build libnuma-dev git build-essential python3-pyelftools pkgconf libbsd-dev libnuma-dev libjansson-dev libfdt-dev libpcap-dev libelf-dev linux-headers-$(uname -r) python3-pip
apt install -y automake autoconf autoconf-archive autotools-dev libtool
apt install -y doxygen gdb
apt install -y libspdlog-dev
apt install -y libnl-3-dev libnl-genl-3-dev libnl-route-3-dev 
pip3 install conan

cd dependencies

# libmoepgf: git clone https://github.com/moepinet/libmoepgf dependencies/libmoepgf
cd libmoepgf
autoreconf -fi
./configure
make
make install
cd ..

# libmoep-2.0-nc: unpack the archive (from the chair) to dependencies/libmoep-2.0-nc
cd libmoep-2.0-nc
autoreconf -fi
./configure
make
make install
cd ..

# moep80211ncm-0.0: unpack the archive (from the chair) to dependencies/moep80211ncm-0.0
cd moep80211ncm-0.0
autoreconf -fi
./configure
make
make install

cd libmoeprlnc
autoreconf -fi
./configure
make
make install

cd ..
cd ..
cd ..

ldconfig

cd client
cmake ./
make

mkdir logs
