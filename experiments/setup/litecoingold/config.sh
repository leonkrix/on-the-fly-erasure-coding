ip addr add 10.0.0.4 dev eno7
ip l set eno7 up
ip r add 10.0.0.0/24 dev eno7
