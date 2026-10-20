ip addr add 10.0.0.2 dev eno8
ip l set eno8 up
ip r add 10.0.0.1 dev eno8
ip addr add 10.0.0.3 dev eno7
ip l set eno7 up
ip r add 10.0.0.4 dev eno7 
sysctl -w net.ipv4.ip_forward=1
ip l add bridgeL type bridge
ip l set bridgeL up
ip l set eno7 master bridgeL
ip l set eno8 master bridgeL

tc qdisc add dev eno7 root netem loss 0%
tc qdisc add dev eno8 root netem loss 0%
