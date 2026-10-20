#include <iostream>
#include <list>
#include <bitset>
#include <cstring>

extern "C" {
#include <moeprlnc/rlnc.h>
#include <moepgf/moepgf.h>
}
using namespace std;

struct SourcePacket {
    int symbolID;
    size_t payloadSize;
    uint8_t payload[1028];
};

struct EncodingVector {
    //size_t ev_len;
    //bitset<4> ccgi;
    //bitset<2> I;
    //bitset<1> C;
    //bitset<1> V;
    int NB_IDS{};
    //int NB_COEFS;
    int FIRST_SOURCE_ID{};
    //int b_id;
    list<int> id_bit_vector;
    //list<int> coef_bit_vector;
};

struct CodedPacket {
    int symbolID{};
    EncodingVector ev;
    size_t payloadSize{};
    uint8_t payload[1028]{};
};

class Sender {
public:
    list<SourcePacket> sliding_window;
};

class Receiver {
public:
    list<SourcePacket> source_buffer;
    list<CodedPacket> coded_buffer;
    list<SourcePacket> decoded_buffer;
};

//sender vars
Sender sender;
int coded_id = 0;
int current_first_id = 0;
int current_last_id = 0;
int lastAckedFromReceiver = 0;

//receiver vars
Receiver receiver;
int lostGenerations = 0;
int totalGenerations = 0;
int amountLost = 0;
int amountCodedLost = 0;
int newFirstSourceID = 0;
int amountPrevLost = 0;
int amountPrevCodedLost = 0;
int sourceBufferSize = 0;
int latestDecodedSymbol = 0;
bool successfullyDecoded = false;
int generationsBuffered = 0;

//parameter
int sliding_window_size = 128;
int source_loss_percentage = 15;
int coded_loss_percentage = 20;
int ack_loss_percentage = 50;
int code_rate = 4;
int amount_packets = 800;

int generation = 1;

//TODO general: wraparound/ sequence number range

//Create a random source symbol and fill sourcePacket
//ASCII 33 (!) - 126(~)
SourcePacket create_source_packet() {
    SourcePacket sourcePacket{};
    for (unsigned char &i: sourcePacket.payload) {
        uint8_t randomData = 33 + ::rand() % 93;
        i = randomData;
    }
    sourcePacket.symbolID = current_last_id;
    current_last_id++;

    return sourcePacket;
}

void check_for_new_ack() {
    if (lastAckedFromReceiver > 0) {
        bool found = false;
        for (SourcePacket s: sender.sliding_window) {
            if (s.symbolID == lastAckedFromReceiver) {
                found = true;
                break;
            }
        }
        if (!found) {
            return;
        }

        auto it1 = sender.sliding_window.begin();
        auto it2 = sender.sliding_window.begin();

        for (int i = 0; i < sender.sliding_window.size(); ++i) {
            if (it2->symbolID <= lastAckedFromReceiver) {
                advance(it2, 1);
            } else {
                break;
            }
        }
        sender.sliding_window.erase(it1, it2);
        it1 = sender.sliding_window.begin();
        current_first_id = it1->symbolID;

        if (sender.sliding_window.empty()) {
            current_first_id = 0;
            current_last_id = 0;
            coded_id = 0;
        }
        lastAckedFromReceiver = 0;
    }
}

//TODO if sliding window with new packet > sliding window size -> replace
//Fill source buffer (sliding window) at sender with source packets
void fill_source_buffer(SourcePacket sourcePacket) {
    //fill/replace source buffer with new source symbols
    sender.sliding_window.push_back(sourcePacket);
}

//Send source packets to receiver, packetloss changeable
void send_source_to_receiver(SourcePacket sourcePacket) {
    int random = 1 + ::rand() % 100;
    if (source_loss_percentage < random) {
        receiver.source_buffer.push_back(sourcePacket);
    } else {
        cout << "Lost Packet ID: " << sourcePacket.symbolID << " with payload: ";
        for (int i = current_last_id - current_first_id + 2; i < sourcePacket.payloadSize; ++i) {
            cout << sourcePacket.payload[i];
        }
        cout << "\n";
    }
}

//Send coded packets to the receiver, packetloss changeable
void send_coded_to_receiver(CodedPacket codedPacket) {
    int random = 1 + ::rand() % 100;
    if (coded_loss_percentage < random) {
        receiver.coded_buffer.push_back(codedPacket);
    } else {
        cout << "Lost Coded Packet with ID: " << codedPacket.symbolID << "\n\n";
    }
}

//TODO add max amount of symbols part of the sliding window/coded packet
//Create and return a coded packet from all source symbols currently active
void encode_and_send() {
    //create block with size = current symbols in sliding window
    int blockSize = sender.sliding_window.size();

    //init encoding block
    rlnc_block_t block = rlnc_block_init(blockSize, 32, MOEPGF_MAX_ALIGNMENT, MOEPGF256);

    //pointer to current first id in the sliding window
    auto it = sender.sliding_window.begin();

    //fill block with all currently active symbols
    for (int i = 0; i < blockSize; ++i) {
        rlnc_block_add(block, i, it->payload, 32);
        advance(it, 1);
    }
    int id = current_first_id;

    int limit = blockSize - code_rate;
    //encode block and create codedSourceSymbols
    for (int i = 0; i < blockSize; ++i) {
        SourcePacket codedSourcePacket{};
        codedSourcePacket.payloadSize = rlnc_block_encode(block, codedSourcePacket.payload,
                                                          sizeof(codedSourcePacket.payload), RLNC_STRUCTURED);
        codedSourcePacket.symbolID = id;
        id++;
        if (i >= limit) {
            send_source_to_receiver(codedSourcePacket);
        }
    }

    //encode block and create codedSymbol
    CodedPacket codedPacket;
    //encode one more time for linear independence
    rlnc_block_encode(block, codedPacket.payload, sizeof(codedPacket.payload), RLNC_STRUCTURED);
    if (current_first_id > 0) {
        rlnc_block_encode(block, codedPacket.payload, sizeof(codedPacket.payload), RLNC_STRUCTURED);
    }
    size_t codedSize = rlnc_block_encode(block, codedPacket.payload, sizeof(codedPacket.payload), RLNC_STRUCTURED);

    //fill codedPacket with codedSymbol and increase coded id
    codedPacket.symbolID = coded_id;
    codedPacket.payloadSize = codedSize;
    coded_id++;

    //Set FIRST_SOURCE_ID, NB_IDS (number of source symbols used)
    //Fill id_bit_vector with source symbols
    codedPacket.ev.FIRST_SOURCE_ID = current_first_id;
    codedPacket.ev.NB_IDS = blockSize;

    for (int i = current_first_id; i < current_last_id; ++i) {
        codedPacket.ev.id_bit_vector.push_back(i);
    }

    send_coded_to_receiver(codedPacket);
}

//determine lost source packets from all packets that should be received
int determineLostPackets() {
    int allPackets = (lostGenerations + 1) * code_rate;
    int lostPackets;

    if (sourceBufferSize == 0) {
        lostPackets = allPackets - receiver.source_buffer.size();
    } else {
        lostPackets = allPackets - receiver.source_buffer.size() + sourceBufferSize - (lostGenerations * code_rate);
    }
    sourceBufferSize = receiver.source_buffer.size();

    amountCodedLost = (allPackets / code_rate) - receiver.coded_buffer.size();
    if (allPackets > code_rate && amountCodedLost != 0) {
        amountCodedLost = (allPackets / code_rate) - receiver.coded_buffer.size() - amountPrevCodedLost;
    }

    return lostPackets;
}

//ack the latest decoded symbol (restored or received)
void ackSymbols(int latestDecodedID) {
    if (latestDecodedID == 0) {
        return;
    }

    int random = 1 + ::rand() % 99;
    if (ack_loss_percentage >= random) {
        cout << "\nAcknowledgement lost (last successfully received symbol ID: " << latestDecodedID << ")\n";
        return;
    }

    //current_first_id = latestDecodedID + 1;
    lastAckedFromReceiver = latestDecodedID;
}

//Flush all not needed Source Packets at receivers' buffer for decoding,
//once a new coded packet arrives where newFirstSourceID > stored SourceSymbolID
//and stored symbols not needed for decoding anymore
void flushSourcePackets(int FirstSourceID) {
    //get range of source symbols to delete
    auto it = receiver.source_buffer.begin();
    auto it2 = receiver.source_buffer.begin();
    int flushedSymbols = 0;
    bool flushed = false;

    if (sourceBufferSize == receiver.source_buffer.size()) {
        return;
    }

    if ((FirstSourceID == 0 && successfullyDecoded) || (FirstSourceID == it->symbolID && successfullyDecoded)) {
        advance(it, receiver.source_buffer.size() - 1);
        if (it->symbolID > latestDecodedSymbol) {
            return;
        }
    }

    it = receiver.source_buffer.begin();
    if (receiver.source_buffer.size() > code_rate) {
        advance(it, receiver.source_buffer.size() - code_rate);
        for (int i = 0; i < code_rate; ++i) {
            if (it->symbolID < code_rate) {
                FirstSourceID = 0;
                newFirstSourceID = 0;
                break;
            }
            advance(it, 1);
        }
    }

    it = receiver.source_buffer.begin();
    if ((FirstSourceID == 0 && successfullyDecoded) || (receiver.source_buffer.size() == latestDecodedSymbol + 1)) {
        advance(it2, receiver.source_buffer.size() - 1);
        if (!successfullyDecoded) {
            return;
        }
        it2 = receiver.source_buffer.begin();
        for (int i = 0; i < receiver.source_buffer.size(); ++i) {
            if (it2->symbolID == FirstSourceID && i > 0) {
                break;
            }
            if (it2->symbolID == latestDecodedSymbol) {
                advance(it2, 1);
                if (sourceBufferSize > 0) {
                    sourceBufferSize--;
                }
                flushedSymbols++;
                break;
            } else {
                advance(it2, 1);
                if (sourceBufferSize > 0) {
                    sourceBufferSize--;
                }
                flushedSymbols++;
            }
        }
        if (successfullyDecoded) {
            latestDecodedSymbol = 0;
            flushed = true;
        }
    }

    if (!successfullyDecoded) {
        for (CodedPacket c: receiver.coded_buffer) {
            if (c.ev.id_bit_vector.begin().operator*() < FirstSourceID) {
                FirstSourceID = c.ev.id_bit_vector.begin().operator*();
            }
        }
    }

    //Iterate through buffer until all not needed source symbols included
    if (FirstSourceID > 0 && !flushed) {
        for (int i = 0; i < receiver.source_buffer.size(); i++) {
            if (it2->symbolID < FirstSourceID) {
                advance(it2, 1);
                if (sourceBufferSize > 0) {
                    sourceBufferSize--;
                }
                flushedSymbols++;
            } else {
                break;
            }
        }
    }

    //Erase symbols in range from receivers' buffer, reset helper vars
    receiver.source_buffer.erase(it, it2);
    amountPrevCodedLost = 0;
    successfullyDecoded = false;
    generationsBuffered = generationsBuffered - (flushedSymbols / code_rate);

}

//Delete Coded Packet in receivers' buffer if used/ not needed
void deleteCodedPacket() {
    receiver.coded_buffer.clear();
    amountPrevCodedLost++;
}

//Decode packets from coded symbol stored at receiver if loss happened
void decode_lost_packet() {
    totalGenerations++;
    int acks = 0;

    //get new FirstSourceID used for decoding from newest coded symbol
    //if no coded symbol stored, = 0 or last stored value
    auto itC = receiver.coded_buffer.begin();
    if (!receiver.coded_buffer.empty()) {
        size_t size = receiver.coded_buffer.size();
        advance(itC, size - 1);
        //set newFirstSourceID from newest coded packet
        newFirstSourceID = itC->ev.FIRST_SOURCE_ID;
    }

    //check if Source Symbols currently stored can be removed
    flushSourcePackets(newFirstSourceID);

    //blockSize = all packets currently stored at the receiver
    int blockSize = (1 + generationsBuffered) * code_rate;

    //determine lost packets/generations
    int lostInThisGeneration = determineLostPackets();
    amountPrevLost = amountPrevLost + lostInThisGeneration;
    if (lostInThisGeneration > 0) {
        amountLost = amountLost + lostInThisGeneration;
        lostGenerations++;
    }

    //if no source loss, source symbols can be decoded without coded packet
    if (amountLost == 0) {
        //add padding if needed
        if (receiver.source_buffer.size() > code_rate) {
            auto itSEnd = receiver.source_buffer.begin();

            int highestSize = ((1 + generationsBuffered) * code_rate) + 34;

            //source symbol padding
            itSEnd = receiver.source_buffer.begin();

            for (int i = 0; i < receiver.source_buffer.size(); ++i) {
                int padding[highestSize - 34];
                for (int j = 0; j < highestSize - 34; ++j) {
                    padding[j] = 0;
                }

                padding[i] = 1;
                int payload[34];
                SourcePacket source = itSEnd.operator*();
                int paddingIndex = 0;
                //determine payload index start
                for (int j = 0; j < source.payloadSize; ++j) {
                    if (source.payload[j] == 32) {
                        paddingIndex = j;
                        break;
                    }
                }

                for (int j = 0; j < 34; ++j) {
                    payload[j] = source.payload[paddingIndex + j];
                }

                //change payload size
                source.payloadSize = highestSize;

                //insert new padding
                int sourcePayloadIndex = 0;
                for (int j = 0; j < highestSize - 34; ++j) {
                    source.payload[sourcePayloadIndex] = padding[j];
                    sourcePayloadIndex++;
                }

                //insert payload
                for (int j = 0; j < 34; ++j) {
                    source.payload[sourcePayloadIndex] = payload[j];
                    sourcePayloadIndex++;
                }

                //replace coded source symbol in buffer with padded symbol
                itSEnd = receiver.source_buffer.erase(itSEnd);
                receiver.source_buffer.insert(itSEnd, source);
            }
        }

        //determine index (start of range of not already decoded symbols)
        int index = 0;
        if (receiver.source_buffer.size() > code_rate) {
            index = receiver.source_buffer.size() - code_rate;
            if (receiver.coded_buffer.empty()) {
                blockSize = receiver.source_buffer.size();
            }
        }

        //init decode block
        rlnc_block_t block = rlnc_block_init(blockSize, 256, MOEPGF_MAX_ALIGNMENT, MOEPGF256);

        //pointer to last element in source buffer
        auto itS = receiver.source_buffer.begin();
        advance(itS, receiver.source_buffer.size() - 1);

        //Decode source symbols
        for (int i = 0; i < blockSize; i++) {
            rlnc_block_decode(block, itS->payload, itS->payloadSize);
            advance(itS, -1);
        }

        //get decoded symbols
        SourcePacket source{};
        for (int i = index; i < blockSize; i++) {

            source.payloadSize = 32;

            rlnc_block_get(block, i, source.payload, 32);

            cout << "Purged Symbol with ID ";
            source.symbolID = newFirstSourceID + i;
            cout << source.symbolID << ": ";
            for (int j = 0; j < 32; j++) {
                cout << source.payload[j];
            }
            cout << "\n";
            acks++;

            //Decoded packet cannot be stored in source buffer, future use may need the decoded versions
            //store in decoded buffer
            receiver.decoded_buffer.push_back(source);
        }

        //set helper vars and delete coded packet
        latestDecodedSymbol = source.symbolID;
        deleteCodedPacket();
        successfullyDecoded = true;
        generationsBuffered++;
        return;
    }

    //Check if decoding possible when source symbol loss, return if not possible
    if (amountLost > receiver.coded_buffer.size()) {
        if (lostInThisGeneration == 0) {
            lostGenerations++;
        }
        cout << "Decoding not possible, too many lost packets, Packets lost: ";
        cout << amountLost;
        cout << " Coded Symbols available: ";
        cout << receiver.coded_buffer.size();
        cout << "\n";

        successfullyDecoded = false;
        generationsBuffered++;
        return;
    }

    //init decode block
    rlnc_block_t block = rlnc_block_init(blockSize, 64, MOEPGF_MAX_ALIGNMENT, MOEPGF256);

    //helper var
    int idDiff = 0;

    //padding for source and coded symbols depending on amount stored generations
    if (amountLost > 1 || receiver.source_buffer.size() > code_rate) {
        auto itCEnd = receiver.coded_buffer.begin();
        auto itSEnd = receiver.source_buffer.begin();
        advance(itCEnd, receiver.coded_buffer.size() - 1);
        advance(itSEnd, receiver.source_buffer.size() - 1);

        int highestSize = ((1 + generationsBuffered) * code_rate) + 34;

        //source symbol padding
        itSEnd = receiver.source_buffer.begin();
        int missingIndices[amountLost];
        int k = 0;

        //determine the smallest ID used for decoding
        int ID = itSEnd->symbolID;
        for (CodedPacket c: receiver.coded_buffer) {
            if (c.ev.id_bit_vector.begin().operator*() < ID) {
                ID = c.ev.id_bit_vector.begin().operator*();
            }
        }

        //determine missing packets and its indices
        for (int i = 0; i < receiver.source_buffer.size() + amountLost; ++i) {
            if (ID == itSEnd->symbolID) {
                ID++;
                advance(itSEnd, 1);
            } else {
                missingIndices[k] = i;
                k++;
                ID++;
            }
            if (i == receiver.source_buffer.size() + amountLost - 1 && k <= amountLost) {
                missingIndices[k] = i;
            }
        }

        //source symbol padding
        itSEnd = receiver.source_buffer.begin();
        k = 0;
        for (int i = 0; i < receiver.source_buffer.size() + amountLost; ++i) {
            int padding[highestSize - 34];
            for (int j = 0; j < highestSize - 34; ++j) {
                padding[j] = 0;
            }
            if (i != missingIndices[k] || k == amountLost) {
                padding[i] = 1;
                int payload[34];
                SourcePacket source = itSEnd.operator*();
                int paddingIndex = 0;
                //determine payload index start
                for (int j = 0; j < source.payloadSize; ++j) {
                    if (source.payload[j] == 32) {
                        paddingIndex = j;
                        break;
                    }
                }

                for (int j = 0; j < 34; ++j) {
                    payload[j] = source.payload[paddingIndex + j];
                }

                //change payload size
                source.payloadSize = highestSize;

                //insert new padding
                int sourcePayloadIndex = 0;
                for (int j = 0; j < highestSize - 34; ++j) {
                    source.payload[sourcePayloadIndex] = padding[j];
                    sourcePayloadIndex++;
                }

                //insert payload
                for (int j = 0; j < 34; ++j) {
                    source.payload[sourcePayloadIndex] = payload[j];
                    sourcePayloadIndex++;
                }

                //replace coded source symbol in buffer with padded symbol
                itSEnd = receiver.source_buffer.erase(itSEnd);
                receiver.source_buffer.insert(itSEnd, source);
            } else {
                k++;
            }
        }

        //coded symbol padding
        //special coded stores indices of coded packets which need a padding on the beginning
        int specialCoded[receiver.coded_buffer.size()];
        itCEnd = receiver.coded_buffer.begin();
        int smallestFirstID = itCEnd->ev.FIRST_SOURCE_ID;
        int biggestFirstID = itCEnd->ev.FIRST_SOURCE_ID;
        k = 0;

        //determine smallest and biggest FirstSourceID
        for (CodedPacket c: receiver.coded_buffer) {
            if (c.ev.FIRST_SOURCE_ID < smallestFirstID) {
                smallestFirstID = c.ev.FIRST_SOURCE_ID;
            }
            if (c.ev.FIRST_SOURCE_ID > biggestFirstID) {
                biggestFirstID = c.ev.FIRST_SOURCE_ID;
            }
        }

        idDiff = biggestFirstID - smallestFirstID;

        //determine indices of special coded symbols
        for (int i = 0; i < receiver.coded_buffer.size(); ++i) {
            if (itCEnd->ev.FIRST_SOURCE_ID > smallestFirstID) {
                specialCoded[k] = i;
                k++;
            }
            advance(itCEnd, 1);
        }

        itCEnd = receiver.coded_buffer.begin();
        //padding for coded symbols, needing 0's in the beginning
        if (k > 0) {
            int m = 0;

            for (int i = 0; i < receiver.coded_buffer.size(); ++i) {
                if (i == specialCoded[m] && m <= k) {
                    int diff = itCEnd->ev.FIRST_SOURCE_ID - smallestFirstID;
                    m++;
                    CodedPacket coded = itCEnd.operator*();

                    //add padding of amount = diff
                    coded.payloadSize += diff;

                    //shift bytes right from paddingIndex
                    for (int j = coded.payloadSize; j >= diff; j--) {
                        coded.payload[j] = coded.payload[j - diff];
                    }

                    //add padding
                    for (int j = 0; j < diff; ++j) {
                        coded.payload[j] = 0;
                    }

                    //replace coded symbol in buffer with padded symbol
                    itCEnd = receiver.coded_buffer.erase(itCEnd);
                    receiver.coded_buffer.insert(itCEnd, coded);
                } else {
                    advance(itCEnd, 1);
                }
            }
        }

        itCEnd = receiver.coded_buffer.begin();
        //padding for coded symbols, 0's on the right
        for (int l = 0; l < receiver.coded_buffer.size(); l++) {
            if (itCEnd->payloadSize < highestSize) {
                CodedPacket coded = itCEnd.operator*();
                int paddingIndex = coded.payloadSize - 34;

                //size diff
                int diff = highestSize - coded.payloadSize;

                //add padding of amount = diff
                for (int i = 0; i < diff; ++i) {
                    coded.payloadSize++;

                    //shift bytes right from paddingIndex
                    for (int j = coded.payloadSize; j > paddingIndex; j--) {
                        coded.payload[j] = coded.payload[j - 1];
                    }

                    //add padding
                    coded.payload[paddingIndex] = 0;
                }

                //replace coded symbol in buffer with padded symbol
                itCEnd = receiver.coded_buffer.erase(itCEnd);
                receiver.coded_buffer.insert(itCEnd, coded);
            } else {
                advance(itCEnd, 1);
            }
        }
    }

    //pointer to first element in source buffer
    auto itS = receiver.source_buffer.begin();

    //Decode source symbols
    for (int i = 0; i < receiver.source_buffer.size(); i++) {
        rlnc_block_decode(block, itS->payload, itS->payloadSize);
        advance(itS, 1);
    }

    //print block
    //print_block(block);

    //print symbol info
    /*
    for (SourcePacket s: receiver.source_buffer) {
        for (int i = 0; i < s.payloadSize; ++i) {
            int c = s.payload[i];
            cout << c << " ";
        }
        cout << "with size: " << s.payloadSize << "\n";
    }
    for (CodedPacket coded: receiver.coded_buffer) {
        for (int i = 0; i < coded.payloadSize; ++i) {
            int c = coded.payload[i];
            cout << c << " ";
        }
        cout << "with size: " << coded.payloadSize << "\n";
    }
    */

    //TODO already determined earlier in the code -> replace
    //determine missing ids
    ::uint8_t missing[32];
    int missingIndices[amountLost];
    int k = 0;

    cout << "\nLost packet amount at receiver: " << amountLost << ", missing ID(s):";
    for (int i = 0; i < blockSize; i++) {
        int size = rlnc_block_get(block, i, missing, 32);
        if (size == 0) {
            missingIndices[k] = i + newFirstSourceID - idDiff;
            cout << " " << i + newFirstSourceID - idDiff;
            k++;
        }
    }
    cout << "\n\n";

    itC = receiver.coded_buffer.begin();
    //copy codedSymbol from buffer and decode block with coded Symbol
    for (int i = 0; i < amountLost; ++i) {
        rlnc_block_decode(block, itC->payload, itC->payloadSize);
        advance(itC, 1);
    }

    //print decoded block
    //print_block(block);

    //get decoded symbol from block, fill newly decoded source packet and insert in senders' decoded buffer
    SourcePacket decodedPacket{};

    for (int i = 0; i < blockSize; i++) {
        rlnc_block_get(block, i, decodedPacket.payload, 32);

        decodedPacket.symbolID = newFirstSourceID + i - idDiff;
        decodedPacket.payloadSize = 32;

        if (decodedPacket.symbolID > latestDecodedSymbol || (decodedPacket.symbolID == 0 && latestDecodedSymbol == 0)) {
            cout << "Decoded Packet ID: ";
            cout << decodedPacket.symbolID;
            cout << " with Symbol: ";
            for (int j = 0; j < 32; j++) {
                cout << decodedPacket.payload[j];
            }
            cout << "\n";

            //add decoded symbol to decoded buffer
            receiver.decoded_buffer.push_back(decodedPacket);

            //add unit vector only to lost source symbol and add it to source buffer for future decoding
            for (int j = 0; j < amountLost; ++j) {
                if (decodedPacket.symbolID == missingIndices[j]) {
                    int paddingIndex = missingIndices[j] - newFirstSourceID + idDiff;
                    //add padding
                    for (int l = 0; l <= paddingIndex; ++l) {
                        decodedPacket.payloadSize++;
                        //shift bytes right from paddingIndex
                        for (int m = decodedPacket.payloadSize; m > l; m--) {
                            decodedPacket.payload[m] = decodedPacket.payload[m - 1];
                        }
                        //add padding
                        if (l == paddingIndex) {
                            decodedPacket.payload[l] = 1;
                            break;
                        } else {
                            decodedPacket.payload[l] = 0;
                        }
                    }
                    decodedPacket.payloadSize++;
                    for (int m = decodedPacket.payloadSize; m > paddingIndex; m--) {
                        decodedPacket.payload[m] = decodedPacket.payload[m - 1];
                    }
                    paddingIndex++;
                    decodedPacket.payload[paddingIndex] = 32;

                    decodedPacket.payloadSize++;
                    for (int m = decodedPacket.payloadSize; m > paddingIndex; m--) {
                        decodedPacket.payload[m] = decodedPacket.payload[m - 1];
                    }
                    paddingIndex++;
                    decodedPacket.payload[paddingIndex] = 0;

                    //insert coded source symbol in source buffer with padded unit vector
                    auto iter = receiver.source_buffer.begin();
                    advance(iter, missingIndices[j] - newFirstSourceID + idDiff);
                    receiver.source_buffer.insert(iter, decodedPacket);
                    break;
                }
            }
        }
    }

    //delete the coded packet(s), set help parameters
    latestDecodedSymbol = decodedPacket.symbolID;
    deleteCodedPacket();
    lostGenerations = 0;
    amountLost = 0;
    amountPrevLost = 0;
    sourceBufferSize = receiver.source_buffer.size();
    successfullyDecoded = true;
    generationsBuffered++;
}

//TODO change amount_packets to amount_generations
int main(int argc, char *argv[]) {
    //set parameter
    if (argc < 5) {
        cout << "Usage: -c code_rate -p amount_packets -spl source_packet_loss -cpl coded_packet_loss -al ack_loss\n";
        //return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp("-c", argv[i])) {
            code_rate = stoi(argv[i + 1]);
        }
        if (0 == strcmp("-p", argv[i])) {
            amount_packets = stoi(argv[i + 1]);
        }
        if (0 == strcmp("-spl", argv[i])) {
            source_loss_percentage = stoi(argv[i + 1]);
        }
        if (0 == strcmp("-cpl", argv[i])) {
            coded_loss_percentage = stoi(argv[i + 1]);
        }
        if (0 == strcmp("-al", argv[i])) {
            ack_loss_percentage = stoi(argv[i + 1]);
        }
    }

    cout << "Starting simulator with a code rate of: " << code_rate << ", sending " << amount_packets;
    cout << " packets with a source loss of: " << source_loss_percentage << "%, a coded loss of: "
         << coded_loss_percentage << "% and an ack loss of: " << ack_loss_percentage << "%:\n\n";

    //seed for random function
    ::srand(time(nullptr));

    //run simulator
    for (int i = 1; i < amount_packets + 1; ++i) {
        if (i % code_rate - 1 == 0) {
            cout << generation << ". Generation:\n";
            generation++;
        }

        SourcePacket sourcePacket = create_source_packet();
        fill_source_buffer(sourcePacket);

        if (i % code_rate == 0) {
            //print sliding window
            std::cout << "Senders' sliding window (showing Source IDs) = { ";
            for (SourcePacket n: sender.sliding_window) {
                cout << n.symbolID << ", ";
            }
            std::cout << "};";
            cout << " Size: ";
            cout << sender.sliding_window.size();
            cout << "\n";

            //print sliding window info
            cout << "Sliding window index range: [";
            cout << current_first_id;
            cout << ":";
            cout << current_last_id - 1;
            cout << "]\n\n";

            //encode source symbols at sender and send to receiver
            encode_and_send();

            //decode packets at receiver if possible (+ send ack)
            decode_lost_packet();

            //TODO ability to change frequency of acks
            //ack successfully received source symbols (only full generations)
            ackSymbols(latestDecodedSymbol);

            //check for new ack at sender
            check_for_new_ack();

            //print receivers source buffer info
            cout << "\nReceivers' source buffer (showing source IDs) = { ";
            for (SourcePacket n: receiver.source_buffer) {
                cout << n.symbolID << ", ";
            }
            cout << "};" << " Size: " << receiver.source_buffer.size() << "\n";

            //print receivers coded buffer info
            cout << "Receivers' coded buffer (showing coded IDs) = { ";
            for (CodedPacket n: receiver.coded_buffer) {
                cout << n.symbolID << ", ";
            }
            cout << "};" << " Size: " << receiver.coded_buffer.size() << "\n";

            //print receivers decoded buffer info
            cout << "Amount of decoded symbols at Receiver = " << receiver.decoded_buffer.size() << "\n\n";
        }
    }
    return 0;
}