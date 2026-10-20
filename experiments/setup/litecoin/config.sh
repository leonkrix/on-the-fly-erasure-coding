ip addr add 10.0.0.1 dev eno8
ip l set eno8 up
ip r add 10.0.0.0/24 dev eno8
