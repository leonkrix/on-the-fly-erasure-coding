# Simulator Usage

## Build and run
Install the dependencies (libmoepgf and libmoeprlnc, see the main README, "Dependencies"), then build with CMake:
```
cmake -S . -B build
cmake --build build
```
The binary is `build/simulator`.

The simulator can be run executing
```
build/simulator -c code_rate -p amount_packets -spl source_packet_loss -cpl coded_packet_loss -al ack_loss
```
with:\
-c = amount of source packets send before a coded packet is send (default: 4)\
-p = amount of all packets the simulator is going to create (default: 800)\
-spl = percentage of source packets to be lost (default: 15)\
-cpl = percentage of coded packets to be lost (default: 20)\
-al = percentage of acknowledgement packets to be lost (default: 50)\
All parameters are optional, and the default values are used if not set.


## About
### What does it do?
The simulator will create for each source packet a random source symbol and an ID in the range of the size of the sliding window. One source symbol currently consists of 32 random 8 bit unsigned char (uint8_t) in the range of [33,126] (makes it easier to debug with ASCII symbols). These symbols are added to the sliding window at the sender and send to the receiver (percentage of packet loss of a source packet can be set).  
The sliding window is implemented as a list, filled to a fixed size (e.g. 128). Once this list is full, a wraparound takes place, and new symbols will be added to the beginning of that list, overwriting old symbols.  
After each k source packets added to the sliding window (k = code_rate), a coded symbol is created and after filling the coded packet with that coded symbol and the range of the used source symbols, its send to the receiver as well (coded packet loss percentage can be set).  
The coded symbol is build using RLNC blocks from the libmoep/ncm. Such a block is initiated with the size of the number of source symbols currently in the sliding window. These source symbols are then added to the block, "simple decoded" with its unit vector within the block, and one coded symbol is built with. Both encoding steps are done with the `rlnc_block_encode` function and the `RLNC_STRUCUTRED` flag.  
The receiver will store all received source/coded packets in a separate buffer.
If a packet loss happens, the receiver will determine the number of lost packets and will initiate again a rlnc block with the size of all packets stored in the source buffer + the number of lost packets (This should equal a multiple amount of the code rate). If `amount of lost source packets <= amount of received coded packets`, it enables the receiver to recover all lost source packets to that point. If the number of lost source packets is higher, the receiver has to wait for other coded packets until he is able to do so. Both the source and coded symbols are decoded with the `rlnc_block_decode` function. The receiver should be able to recover the matrix, and the lost symbols can be retrieved from the block. These symbols are added to the source buffer, and all coded packets are removed from the coded buffer as they are no longer needed for future decoding. If there is no loss, all symbols of that generation are simply decoded and "purged" (only if the previous decoding process of a lost source symbol is finished.  
Before each decoding try, the receiver will try to remove all not needed source packets from its buffer, comparing all source IDs in the buffer to the latest received coded packet (its `FIRST_SOURCE_ID` parameter). All source packets with IDs smaller than this new first source id (which is basically telling the receiver that the sender received the ack and removed these source symbols/packets from its sliding window) are removed, except if the current decoding process is not yet finished.  
Speaking of acks, after each decoding or if no packet loss happened, the receiver will ack the complete generation of received/recovered symbols.  

The simulator will print out for each generation (code_rate): whether a loss happened (packet ID and lost symbol), the recovered symbol if possible, the sender's sliding window list with its range and both receiver's source and coded buffer, as well as the amount of all successfully decoded symbols to that point.
