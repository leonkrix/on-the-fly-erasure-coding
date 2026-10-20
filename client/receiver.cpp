#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <list>
#include <cstring>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <numeric>
#include <filesystem>
#include <random>
#include <unistd.h>

extern "C" {
#include <moeprlnc/rlnc.h>
#include <moepgf/moepgf.h>
}
using namespace std;

struct SourcePacket {
    uint16_t symbolID;
    uint64_t generation{};
    uint8_t windowCycle{};
    size_t payloadSize{};
    uint8_t payload[2048]{};
};

struct CodedPacket {
    uint16_t symbolID{};
    uint64_t smallestGenUsed{};
    uint64_t generation{};
    uint8_t windowCycle{};
    size_t payloadSize{};
    uint8_t payload[2048]{};
};

//Receiver vars
list<SourcePacket> source_buffer{};
list<CodedPacket> coded_buffer{};
list<SourcePacket> decoded_buffer{};
uint64_t smallestGenNeeded = 0;
uint64_t latestDecodedGen = 0;
int generationsBuffered = 0;
uint64_t currentGen = 0;
bool retry = false;
uint8_t currentWindowCycle = 0;
uint16_t sliding_window_size = 256;

//parameter
int code_rate = 4;
bool debug = false;

//measurement vars
vector<chrono::nanoseconds> decodeTimes;
vector<chrono::nanoseconds> waitingTimes;
vector<chrono::nanoseconds> recurrenceTimes;
auto beginRecurrenceTime = chrono::high_resolution_clock::now();
auto endRecurrenceTime = chrono::high_resolution_clock::now();
bool lost = false;
vector<int> decodeBlockSize;
vector<int> decodeSourceBufferSize;
vector<::uint64_t> decodeGen;
int totalSource = 0;
int totalCoded = 0;
int completeLostWindows = 0;
int receivedAndDecoded = 0;

struct sockaddr_in senderAddr, receiverAddr, senderAddrState, receiverAddrState;
int socketFd, socketState;

/// sends an acknowledgment with the latest successfully decoded gen at the receiver to the server \n
/// Tetrys header type is 2
void sendAck() {
    uint8_t buffer[9];
    uint8_t bufferGen[8];
    for (int i = 0; i < 8; ++i) {
        bufferGen[i] = (latestDecodedGen >> (7 - i) * 8) & 0xFF;
    }

    //Tetrys type 2 = window update packet
    buffer[0] = 2;

    //copy gen
    copy(bufferGen, bufferGen + 8, buffer + 1);

    socklen_t length = sizeof(senderAddr);
    sendto(socketFd, buffer, 9, 0, (const struct sockaddr *) &senderAddr, length);

    if (debug) {
        cout << "Ack send in range of: " << (int) source_buffer.front().symbolID << ":"
             << (int) source_buffer.back().symbolID << " with latest decoded gen: " << latestDecodedGen << "\n\n";
    }
}

/// removes all not for decoding needed source packets from the receiver's source_buffer,
/// depending on the smallest gen needed by any coded packet in the coded_buffer
/// \param smallestGen
void flushSourcePackets(uint64_t smallestGen) {
    //init range of source symbols to delete
    auto it = source_buffer.begin();
    auto it2 = source_buffer.begin();
    int flushedSymbols = 0;

    //depending on ack rate
    if (smallestGen > (latestDecodedGen + 1)) {
        smallestGen = latestDecodedGen;
    }

    //Iterate through source_buffer until all not needed source symbols included
    for (int i = 0; i < source_buffer.size(); i++) {
        if (it2->generation < smallestGen) {
            advance(it2, 1);
            flushedSymbols++;
        } else {
            break;
        }
    }

    //Erase symbols in range from receivers' buffer
    //set helper vars
    source_buffer.erase(it, it2);
    generationsBuffered = generationsBuffered - (flushedSymbols / code_rate);
}

/// Decode source/lost packets (with coded symbol(s)) after the rank enables it
void decode_packet() {
    // determine lost source packets from all packets that should be received until this point
    int allPackets = (generationsBuffered + 1) * code_rate;
    int amountLost = allPackets - (int) source_buffer.size();

    //blockSize are all stored source symbols at the receiver + number of missing symbols combined
    int blockSize = (int) source_buffer.size() + amountLost;

    //if no source loss, source symbols can be decoded without a coded packet
    if (amountLost == 0) {
        auto itS = source_buffer.begin();
        advance(itS, source_buffer.size() - code_rate);
        //change padding
        for (int i = 0; i < code_rate; ++i) {
            int padding[code_rate];
            for (int j = 0; j < code_rate; ++j) {
                padding[j] = 0;
            }

            padding[i] = 1;
            uint8_t payload[34];
            SourcePacket source = itS.operator*();
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
            source.payloadSize = code_rate + 34;

            //insert new padding
            int sourcePayloadIndex = 0;
            for (int j = 0; j < code_rate; ++j) {
                source.payload[sourcePayloadIndex] = padding[j];
                sourcePayloadIndex++;
            }

            //insert payload
            for (uint8_t j: payload) {
                source.payload[sourcePayloadIndex] = j;
                sourcePayloadIndex++;
            }

            //replace coded source symbol in buffer with padded symbol
            itS = source_buffer.erase(itS);
            source_buffer.insert(itS, source);
        }

        //init decode block
        rlnc_block_t block = rlnc_block_init(code_rate, 32, MOEPGF_MAX_ALIGNMENT, MOEPGF256);

        //Decode source symbols
        itS = source_buffer.begin();
        advance(itS, source_buffer.size() - code_rate);
        for (int i = 0; i < code_rate; i++) {
            rlnc_block_decode(block, itS->payload, itS->payloadSize);
            advance(itS, 1);
        }

        itS = source_buffer.begin();
        advance(itS, source_buffer.size() - code_rate);
        //get decoded symbols
        SourcePacket source{};
        for (int i = 0; i < code_rate; i++) {
            source.payloadSize = 32;
            if (0 == rlnc_block_get(block, i, source.payload, 32)) {
                exit(-3);
            }
            source.symbolID = itS->symbolID;
            advance(itS, 1);
            source.generation = currentGen;

            if (debug) {
                cout << "Purged Symbol with ID ";
                cout << (int) source.symbolID << ": ";
                for (int j = 0; j < 32; j++) {
                    cout << source.payload[j];
                }
                cout << " in generation: " << source.generation << "\n";
            }

            //Decoded packet cannot be stored in source buffer, future use may need the decoded versions
            //store in decoded buffer
            decoded_buffer.push_back(source);
        }

        //set helper var, delete coded packet(s), free decode block
        latestDecodedGen = source.generation;
        coded_buffer.clear();
        rlnc_block_free(block);

        //set measurement vars
        decodeBlockSize.push_back(code_rate);
        decodeSourceBufferSize.push_back((int) source_buffer.size());
        decodeGen.push_back(latestDecodedGen);
        return;
    }

    //init decode block
    rlnc_block_t block = rlnc_block_init(blockSize, 32, MOEPGF_MAX_ALIGNMENT, MOEPGF256);

    //padding for source and coded symbols needed, depending on amount stored generations
    if (amountLost > 1 || source_buffer.size() > code_rate) {
        auto itCEnd = coded_buffer.begin();
        auto itSEnd = source_buffer.begin();

        int highestSize = ((1 + generationsBuffered) * code_rate) + 34;

        //source symbol padding
        uint64_t startGen = source_buffer.front().generation;
        if (smallestGenNeeded < startGen) {
            startGen = smallestGenNeeded;
        }

        for (int i = 0; i < source_buffer.size(); ++i) {
            uint8_t padding[highestSize - 34];
            for (int j = 0; j < highestSize - 34; ++j) {
                padding[j] = 0;
            }

            int diffGen = (int) (itSEnd->generation - startGen);
            int indexInGen = (itSEnd->symbolID % code_rate) + (diffGen * code_rate);
            padding[indexInGen] = 1;
            uint8_t payload[34];
            SourcePacket source = itSEnd.operator*();
            //determine payload index start
            int paddingIndex = (int) source.payloadSize - 34;

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
            for (uint8_t j: payload) {
                source.payload[sourcePayloadIndex] = j;
                sourcePayloadIndex++;
            }

            //replace coded source symbol in buffer with padded symbol
            itSEnd = source_buffer.erase(itSEnd);
            source_buffer.insert(itSEnd, source);

        }

        //coded symbol padding
        //special coded stores indices of coded packets which need a padding on the beginning
        int specialCoded[coded_buffer.size()];
        itCEnd = coded_buffer.begin();
        uint64_t smallestGen = itCEnd->smallestGenUsed;
        int k = 0;

        //determine smallest gen
        for (const CodedPacket &c: coded_buffer) {
            if (c.smallestGenUsed < smallestGen) {
                smallestGen = c.smallestGenUsed;
            }
        }

        //determine indices of special coded symbols
        for (int i = 0; i < coded_buffer.size(); ++i) {
            if (itCEnd->smallestGenUsed > smallestGen) {
                specialCoded[k] = i;
                k++;
            }
            advance(itCEnd, 1);
        }

        itCEnd = coded_buffer.begin();
        //padding for coded symbols, needing 0's in the beginning
        if (k > 0) {
            int m = 0;
            for (int i = 0; i < coded_buffer.size(); ++i) {
                if (i == specialCoded[m] && m <= k) {
                    int diff = (int) (itCEnd->smallestGenUsed - smallestGen) * code_rate;
                    m++;
                    CodedPacket coded = itCEnd.operator*();

                    if (retry) {
                        int oldDiff = 0;
                        for (int j = 0; j < coded.payloadSize; ++j) {
                            if (coded.payload[j] == 0) {
                                oldDiff++;
                            } else {
                                break;
                            }
                        }
                        if (oldDiff % code_rate != 0) {
                            oldDiff = oldDiff - (oldDiff % code_rate);
                        }
                        diff = diff - oldDiff;
                    }

                    if (diff != 0) {
                        //add padding of amount = diff
                        coded.payloadSize += diff;

                        //shift bytes right from paddingIndex
                        for (int j = (int) coded.payloadSize; j >= diff; j--) {
                            coded.payload[j] = coded.payload[j - diff];
                        }

                        //add padding
                        for (int j = 0; j < diff; ++j) {
                            coded.payload[j] = 0;
                        }

                        //replace coded symbol in buffer with padded symbol
                        itCEnd = coded_buffer.erase(itCEnd);
                        coded_buffer.insert(itCEnd, coded);
                    } else {
                        advance(itCEnd, 1);
                    }
                } else {
                    advance(itCEnd, 1);
                }
            }
        }

        itCEnd = coded_buffer.begin();
        //padding for coded symbols, 0's on the right
        for (int l = 0; l < coded_buffer.size(); l++) {
            if (itCEnd->payloadSize < highestSize) {
                CodedPacket coded = itCEnd.operator*();
                int paddingIndex = (int) coded.payloadSize - 34;

                //size diff
                int diff = highestSize - (int) coded.payloadSize;

                //add padding of amount = diff
                for (int i = 0; i < diff; ++i) {
                    coded.payloadSize++;

                    //shift bytes right from paddingIndex
                    for (int j = (int) coded.payloadSize; j > paddingIndex; j--) {
                        coded.payload[j] = coded.payload[j - 1];
                    }

                    //add padding
                    coded.payload[paddingIndex] = 0;
                }

                //replace coded symbol in buffer with padded symbol
                itCEnd = coded_buffer.erase(itCEnd);
                coded_buffer.insert(itCEnd, coded);
            } else {
                advance(itCEnd, 1);
            }
        }
    }

    //Decode source symbols
    auto itS = source_buffer.begin();
    for (int i = 0; i < source_buffer.size(); i++) {
        rlnc_block_decode(block, itS->payload, itS->payloadSize);
        advance(itS, 1);
    }

    //print symbol info
    /*
    for (SourcePacket s: source_buffer) {
        for (int i = 0; i < s.payloadSize; ++i) {
            int c = s.payload[i];
            cout << c << " ";
        }
        cout << "with size: " << s.payloadSize << "\n";
    }

    for (const CodedPacket &coded: coded_buffer) {
        for (int i = 0; i < coded.payloadSize; ++i) {
            int c = coded.payload[i];
            cout << c << " ";
        }
        cout << "with size: " << coded.payloadSize << "\n";
        cout << "gen range: " << coded.smallestGenUsed << ":" << coded.generation << "\n";
    }*/

    //determine missing ids, by getting all symbols from the block, if size == 0 -> lost
    int missingInd[amountLost];
    int k = 0;
    if (debug) {
        cout << "\nLost packet amount at receiver: " << amountLost << ", missing (index):";
    }
    for (int i = 0; i < blockSize; ++i) {
        uint8_t tmp[32];
        ssize_t size = rlnc_block_get(block, i, tmp, 32);
        if (size == 0) {
            if (debug) {
                cout << i << " ";
            }
            missingInd[k] = i;
            k++;
        }
    }
    if (debug) {
        cout << "\n\n";
    }

    auto itC = coded_buffer.begin();
    //decode block with coded Symbol(s)
    for (int i = 0; i < amountLost; ++i) {
        rlnc_block_decode(block, itC->payload, itC->payloadSize);
        if (debug) {
            cout << "Decode rank: " << rlnc_block_rank_decode(block) << "\n";
        }
        advance(itC, 1);
    }

    // if block not decoded from coded symbol, remove the last coded symbol and return
    if (rlnc_block_rank_decode(block) != (source_buffer.size() + amountLost)) {
        if (debug) {
            ::perror("rank not complete");
            for (const CodedPacket &c: coded_buffer) {
                cout << (int) c.symbolID << " ";
            }
            cout << "\n";
            for (const CodedPacket &coded: coded_buffer) {
                for (int i = 0; i < coded.payloadSize; ++i) {
                    int c = coded.payload[i];
                    cout << c << " ";
                }
                cout << "with size: " << coded.payloadSize << "\n";
            }
        }
        //::exit(-5);
        coded_buffer.pop_back();
        retry = true;
        return;
    }

    //get decoded symbol from block, fill the newly decoded source packet and insert missing in senders' decoded buffer
    SourcePacket decodedPacket{};
    uint64_t gen = smallestGenNeeded;
    int counter = 0;

    for (int i = 0; i < blockSize; i++) {
        ssize_t size = rlnc_block_get(block, i, decodedPacket.payload, 32);

        //debug, if still size == 0, error
        if (size == 0) {
            if (debug) {
                for (const CodedPacket &coded: coded_buffer) {
                    for (int j = 0; j < coded.payloadSize; ++j) {
                        int c = coded.payload[j];
                        cout << c << " ";
                    }
                    cout << "with size: " << coded.payloadSize << "\n";
                    cout << "gen range: " << coded.smallestGenUsed << ":" << coded.generation << "\n";
                }
                ::exit(-5);
            }
        }

        if (i < sliding_window_size) {
            decodedPacket.symbolID = i;
        } else {
            decodedPacket.symbolID = sliding_window_size - code_rate + (i % code_rate);
        }

        decodedPacket.payloadSize = 32;

        if ((i % code_rate == 0 && i > 0)) {
            gen++;
        }
        decodedPacket.generation = gen;

        if (gen > latestDecodedGen || (gen == 0 && latestDecodedGen == 0)) {
            if (debug) {
                cout << "Decoded Packet ID: ";
                cout << (int) decodedPacket.symbolID;
                cout << " with Symbol: ";
                for (int j = 0; j < 32; j++) {
                    cout << decodedPacket.payload[j];
                }
                cout << " in generation: " << decodedPacket.generation << "\n";
            }

            //add decoded symbol to decoded buffer
            decoded_buffer.push_back(decodedPacket);

            //add unit vector only to lost source symbol and add it to source buffer for future decoding
            for (int j = 0; j < amountLost; ++j) {
                if (i == missingInd[j]) {
                    int paddingIndex = missingInd[counter];
                    //add padding
                    for (int l = 0; l <= paddingIndex; ++l) {
                        decodedPacket.payloadSize++;
                        //shift bytes right from paddingIndex
                        for (int m = (int) decodedPacket.payloadSize; m > l; m--) {
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
                    for (int m = (int) decodedPacket.payloadSize; m > paddingIndex; m--) {
                        decodedPacket.payload[m] = decodedPacket.payload[m - 1];
                    }
                    paddingIndex++;
                    decodedPacket.payload[paddingIndex] = 32;

                    decodedPacket.payloadSize++;
                    for (int m = (int) decodedPacket.payloadSize; m > paddingIndex; m--) {
                        decodedPacket.payload[m] = decodedPacket.payload[m - 1];
                    }
                    paddingIndex++;
                    decodedPacket.payload[paddingIndex] = 0;

                    //insert coded source symbol in source buffer with padded unit vector
                    auto iter = source_buffer.begin();
                    advance(iter, missingInd[counter]);
                    source_buffer.insert(iter, decodedPacket);
                    counter++;

                    receivedAndDecoded++;
                }
            }
        }
    }

    //delete the coded packet(s), set help parameters, free block
    latestDecodedGen = decodedPacket.generation;
    coded_buffer.clear();
    retry = false;
    rlnc_block_free(block);

    //set measurement vars
    if (lost) {
        lost = false;
        endRecurrenceTime = chrono::high_resolution_clock::now();
        auto elapsedRecurrenceTime = chrono::duration_cast<chrono::nanoseconds>(
                endRecurrenceTime - beginRecurrenceTime);
        recurrenceTimes.push_back(elapsedRecurrenceTime);
    }

    decodeBlockSize.push_back(blockSize);
    decodeSourceBufferSize.push_back((int) source_buffer.size());
    decodeGen.push_back(latestDecodedGen);
}

/// handle all incoming packets at the receiver, split into status, source and coded packets
/// sets source and coded packet structs from buffer
void handlePackets() {
    long n;
    ::uint8_t buffer[2048];
    socklen_t length = sizeof(senderAddr);
    bool justDecoded = false;
    auto beginWaitingTime = chrono::high_resolution_clock::now();

    while (true) {
        start:

        //receive packets
        n = recvfrom(socketFd, buffer, 2048, 0, (struct sockaddr *) &senderAddr, &length);

        /*
        //rdy packet
        if (buffer[0] == 12 && n == 5) {
            code_rate = buffer[4];
            if (debug) {
                cout << "code rate set to: " << code_rate << "\n";
            }
        }
        */

        //end packet
        if (buffer[0] == 10 && n == 4) {
            break;
        }

        //source packet
        if (buffer[0] == 0) {
            if (justDecoded) {
                justDecoded = false;
                beginWaitingTime = chrono::high_resolution_clock::now();
            }
            SourcePacket sourcePacket{};
            sourcePacket.symbolID = ((std::uint16_t) buffer[2] << 0) | ((std::uint16_t) buffer[1] << 8);
            sourcePacket.windowCycle = buffer[3];
            sourcePacket.payloadSize = ((std::uint16_t) buffer[13] << 0) | ((std::uint16_t) buffer[12] << 8);

            uint8_t bufferGen[8];
            copy(buffer + 4, buffer + 12, bufferGen);

            sourcePacket.generation =
                    ((std::uint64_t) bufferGen[7] << 0) | ((std::uint64_t) bufferGen[6] << 8) |
                    ((std::uint64_t) bufferGen[5] << 16) | ((std::uint64_t) bufferGen[4] << 24) |
                    ((std::uint64_t) bufferGen[3] << 32) | ((std::uint64_t) bufferGen[2] << 40) |
                    ((std::uint64_t) bufferGen[1] << 48) | ((std::uint64_t) bufferGen[0] << 56);

            copy(buffer + 14, buffer + sourcePacket.payloadSize + 14, sourcePacket.payload);

            if (sourcePacket.generation > currentGen) {
                generationsBuffered = generationsBuffered + (int) (sourcePacket.generation - currentGen);
                currentGen = sourcePacket.generation;

                if (sourcePacket.symbolID % code_rate != 0 && !lost) {
                    lost = true;
                    beginRecurrenceTime = chrono::high_resolution_clock::now();
                }
            } else {
                if (!lost &&
                    (sourcePacket.symbolID != (source_buffer.back().symbolID + 1) && sourcePacket.symbolID != 0)) {
                    lost = true;
                    beginRecurrenceTime = chrono::high_resolution_clock::now();
                }
            }

            /*
            //print info
            int gen = sourcePacket.generation;
            cout << "Source Packet with id: " << sourcePacket.symbolID << ", current gen: " << gen
                 << ": ";
            for (int i = 0; i < sourcePacket.payloadSize; ++i) {
                int c = sourcePacket.payload[i];
                cout << c << " ";
            }
            cout << " payload with size: " << sourcePacket.payloadSize << "\n";
            */

            //Check for complete Sliding Window removed
            if (sourcePacket.windowCycle == currentWindowCycle) {
                source_buffer.push_back(sourcePacket);
            } else {
                generationsBuffered = 0;
                source_buffer.clear();
                coded_buffer.clear();
                currentWindowCycle = sourcePacket.windowCycle;
                source_buffer.push_back(sourcePacket);
                completeLostWindows++;
                if (debug) {
                    cout << "Complete sliding window lost\n";
                }
            }
            totalSource++;
            receivedAndDecoded++;
        }

            //coded packet
        else if (buffer[0] == 1) {
            CodedPacket codedPacket;
            codedPacket.symbolID = ((std::uint16_t) buffer[2] << 0) | ((std::uint16_t) buffer[1] << 8);
            codedPacket.windowCycle = buffer[3];
            codedPacket.payloadSize = ((std::uint16_t) buffer[21] << 0) | ((std::uint16_t) buffer[20] << 8);

            uint8_t bufferSmallestGenUsed[8];
            copy(buffer + 4, buffer + 12, bufferSmallestGenUsed);
            uint8_t bufferGen[8];
            copy(buffer + 12, buffer + 20, bufferGen);

            codedPacket.smallestGenUsed =
                    ((std::uint64_t) bufferSmallestGenUsed[7] << 0) |
                    ((std::uint64_t) bufferSmallestGenUsed[6] << 8) |
                    ((std::uint64_t) bufferSmallestGenUsed[5] << 16) |
                    ((std::uint64_t) bufferSmallestGenUsed[4] << 24) |
                    ((std::uint64_t) bufferSmallestGenUsed[3] << 32) |
                    ((std::uint64_t) bufferSmallestGenUsed[2] << 40) |
                    ((std::uint64_t) bufferSmallestGenUsed[1] << 48) |
                    ((std::uint64_t) bufferSmallestGenUsed[0] << 56);

            codedPacket.generation =
                    ((std::uint64_t) bufferGen[7] << 0) | ((std::uint64_t) bufferGen[6] << 8) |
                    ((std::uint64_t) bufferGen[5] << 16) | ((std::uint64_t) bufferGen[4] << 24) |
                    ((std::uint64_t) bufferGen[3] << 32) | ((std::uint64_t) bufferGen[2] << 40) |
                    ((std::uint64_t) bufferGen[1] << 48) | ((std::uint64_t) bufferGen[0] << 56);

            copy(buffer + 22, buffer + codedPacket.payloadSize + 22, codedPacket.payload);

            if (codedPacket.generation > currentGen) {
                generationsBuffered = generationsBuffered + (int) (codedPacket.generation - currentGen);
                currentGen = codedPacket.generation;
            }

            totalCoded++;

            if (justDecoded && codedPacket.generation <= latestDecodedGen) {
                justDecoded = false;
                beginWaitingTime = chrono::high_resolution_clock::now();
                flushSourcePackets(codedPacket.smallestGenUsed);
                goto start;
            }

            /*
            //print info
            int smallest = codedPacket.smallestGenUsed;
            int current = codedPacket.generation;
            cout << "Coded Packet with id: " << codedPacket.symbolID << ", gen range: " << smallest << ":" << current
                 << ": ";
            for (int i = 0; i < codedPacket.payloadSize; ++i) {
                int c = codedPacket.payload[i];
                cout << c << " ";
            }
            cout << " payload with size: " << codedPacket.payloadSize << "\n";
            */

            //Check for complete Sliding Window removed
            if (codedPacket.windowCycle == currentWindowCycle) {
                coded_buffer.push_back(codedPacket);
            } else {
                generationsBuffered = 0;
                source_buffer.clear();
                coded_buffer.clear();
                currentWindowCycle = codedPacket.windowCycle;
                coded_buffer.push_back(codedPacket);
                completeLostWindows++;
                if (debug) {
                    cout << "Complete sliding window lost\n";
                }
            }
        }

        //set current smallestGenNeeded for decoding if coded packet available
        if (!coded_buffer.empty()) {
            smallestGenNeeded = coded_buffer.front().smallestGenUsed;
        }
        //flush source packets
        flushSourcePackets(smallestGenNeeded);

        //decode packets once possible (lost packets <= coded packets)
        if ((source_buffer.size() + coded_buffer.size()) >= ((generationsBuffered + 1) * code_rate)) {
            if (debug) {
                cout << "Generation(s) complete, going to decode...\n";
            }

            auto endWaitingTime = chrono::high_resolution_clock::now();
            auto elapsedWaitingTime = chrono::duration_cast<chrono::nanoseconds>(endWaitingTime - beginWaitingTime);
            waitingTimes.push_back(elapsedWaitingTime);

            auto beginDecodeTime = chrono::high_resolution_clock::now();

            //decode packets
            decode_packet();
            justDecoded = true;

            auto endDecodeTime = chrono::high_resolution_clock::now();
            auto elapsedDecodeTime = chrono::duration_cast<chrono::nanoseconds>(endDecodeTime - beginDecodeTime);
            decodeTimes.push_back(elapsedDecodeTime);

            //print receivers source buffer info
            if (debug) {
                cout << "\nReceivers' source buffer (showing source IDs) = { ";
                for (const SourcePacket &sourcePacket: source_buffer) {
                    cout << (int) sourcePacket.symbolID << ", ";
                }
                cout << "};" << " Size: " << source_buffer.size() << "\n";

                //print receivers coded buffer info
                cout << "Receivers' coded buffer (showing coded IDs) = { ";
                for (const CodedPacket &codedPacket: coded_buffer) {
                    cout << (int) codedPacket.symbolID << ", ";
                }
                cout << "};" << " Size: " << coded_buffer.size() << "\n";

                //print receivers decoded buffer info
                cout << "Amount of decoded symbols at Receiver = " << decoded_buffer.size() << "\n\n";
            }

            //send acknowledgement
            sendAck();
        }
    }
}

int main(int argc, char *argv[]) {
    //set parameter
    char *sender_addr;
    char *receiver_addr;
    int port = 1337;

    float parameterToLog = 0;

    if (argc < 5) {
        cout << "Usage (addr must be set, other parameters are optional):\n"
                "--help\n"
                "--debug\n"
                "-s sender_addr\n"
                "-r receiver_addr\n"
                "-port port\n";
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp("--help", argv[i])) {
            cout << "Usage (addr must be set, other parameters are optional):\n"
                    "--help\n"
                    "--debug\n"
                    "-s sender_addr\n"
                    "-r receiver_addr\n"
                    "-port port\n";
            return 1;
        }
        if (0 == strcmp("-s", argv[i])) {
            sender_addr = argv[i + 1];
        }
        if (0 == strcmp("-r", argv[i])) {
            receiver_addr = argv[i + 1];
        }
        if (0 == strcmp("-port", argv[i])) {
            port = stoi(argv[i + 1]);
        }
        if (0 == strcmp("--debug", argv[i])) {
            debug = true;
        }
        if (0 == strcmp("-para", argv[i])) {
            parameterToLog = stof(argv[i + 1]);
        }
    }

    //create sockets
    if ((socketFd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket failed");
        exit(-1);
    }
    if ((socketState = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(-1);
    }

    const int opt = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    //set server and receiver address
    memset(&senderAddr, 0, sizeof(senderAddr));
    memset(&receiverAddr, 0, sizeof(receiverAddr));

    senderAddr.sin_family = AF_INET;
    senderAddr.sin_addr.s_addr = inet_addr(sender_addr);
    senderAddr.sin_port = htons(port);

    receiverAddr.sin_family = AF_INET;
    receiverAddr.sin_addr.s_addr = inet_addr(receiver_addr);
    receiverAddr.sin_port = htons(port);

    senderAddrState.sin_family = AF_INET;
    senderAddrState.sin_addr.s_addr = inet_addr(sender_addr);
    senderAddrState.sin_port = htons(1338);

    //bind socket to address and port
    if (bind(socketFd, (const struct sockaddr *) &receiverAddr, sizeof(receiverAddr)) < 0) {
        perror("bind failed");
        exit(-2);
    }

    int optBuf = 40000000;
    setsockopt(socketFd, SOL_SOCKET, SO_RCVBUF, &optBuf, sizeof(optBuf));

    //send ready status to sender
    if ((connect(socketState, (struct sockaddr *) &senderAddrState, sizeof(senderAddrState))) < 0) {
        printf("\nConnection Failed \n");
        return -1;
    }

    cout << "Receiver ready, sending RDY packet...\n";
    ::uint8_t buffer[4];
    buffer[0] = 12;
    char bufferChar[4] = "RDY";
    copy(bufferChar, bufferChar + 3, buffer + 1);
    send(socketState, buffer, 4, 0);

    uint8_t bufferCode[64];
    while (true) {
        ssize_t n = read(socketState, bufferCode, 64);
        if (n == 7 && bufferCode[0] == 12) {
            //rdy packet
            code_rate = bufferCode[4];
            sliding_window_size = ((std::uint16_t) bufferCode[6] << 0) | ((std::uint16_t) bufferCode[5] << 8);
            if (debug) {
                cout << "code rate set to: " << code_rate << "\n";
                cout << "sliding window size set to: " << sliding_window_size << "\n";
            }
            break;
        }
    }
    buffer[0] = 13;
    char bufferCharA[4] = "ACK";
    copy(bufferCharA, bufferCharA + 3, buffer + 1);
    send(socketState, buffer, 4, 0);

    cout << "RDY packet send to Sender, waiting for data...\n";
    auto beginComplete = chrono::high_resolution_clock::now();

    handlePackets();

    char bufferE[64];
    while (true) {
        ssize_t n = read(socketState, bufferE, 64);
        if (bufferE[0] == 10 && n == 4) {
            cout << "end transmission received, sending ack, leaving...\n";
            ::uint8_t bufferEnd[8];
            bufferEnd[0] = 11;
            char bufferCharAE[8] = "ACK END";

            copy(bufferCharAE, bufferCharAE + 7, bufferEnd + 1);
            sendto(socketFd, bufferEnd, 8, 0, (const struct sockaddr *) &senderAddr, sizeof(senderAddr));
            break;
        }
    }

    close(socketState);

    //Measurement calculation
    auto endComplete = chrono::high_resolution_clock::now();
    auto elapsedComplete = chrono::duration_cast<chrono::nanoseconds>(endComplete - beginComplete);
    float packetRate = (float) ((currentGen + 1) * code_rate) / (float) ((double) elapsedComplete.count() * 1e-9);

    spdlog::info("Complete packet handling time of {} decoded packets: {:.{}f} seconds -> {} p/s",
                 (currentGen + 1) * code_rate,
                 (float) elapsedComplete.count() * 1e-9, 9,
                 packetRate, 9);

    chrono::nanoseconds averageWaitingTime =
            accumulate(waitingTimes.begin(), waitingTimes.end(), (chrono::nanoseconds) 0) / waitingTimes.size();
    spdlog::info("Avg waiting time before decoding: {} nanoseconds", averageWaitingTime.count());

    chrono::nanoseconds averageRecurrenceTime = (chrono::nanoseconds) 0;
    if (!recurrenceTimes.empty()) {
        averageRecurrenceTime =
                accumulate(recurrenceTimes.begin(), recurrenceTimes.end(), (chrono::nanoseconds) 0) /
                recurrenceTimes.size();
    }
    spdlog::info("Avg recurrence time: {} nanoseconds, with {} entries", averageRecurrenceTime.count(),
                 recurrenceTimes.size());

    chrono::nanoseconds averageDecodeTime =
            accumulate(decodeTimes.begin(), decodeTimes.end(), (chrono::nanoseconds) 0) / decodeTimes.size();
    float averageDecodeBlockSize =
            accumulate(decodeBlockSize.begin(), decodeBlockSize.end(), (float) 0) / (float) decodeBlockSize.size();
    spdlog::info("Avg decode time: {} nanoseconds with an avg block size: {} and {} total decodes",
                 averageDecodeTime.count(), averageDecodeBlockSize, decodeTimes.size());

    spdlog::info("Avg decoding delay: {} nanoseconds", averageWaitingTime.count() + averageDecodeTime.count());

    //allSource = all source packets without packet loss
    float allSource = ((float) currentGen + 1) * (float) code_rate;
    float lossRateSource = ((allSource - (float) totalSource) / allSource) * 100;

    //allCoded = all coded packets without packet loss
    float allCoded = (float) currentGen + 1;
    float lossRateCoded = ((allCoded - (float) totalCoded) / allCoded) * 100;

    spdlog::info("Total received source packets: {} -> packet loss of source packets: {}%, "
                 "total received coded packets: {} -> packet loss of coded packets: {}%",
                 totalSource, lossRateSource,
                 totalCoded, lossRateCoded;
    spdlog::info("Complete Sliding Windows skipped: {}, successfully received: {}/{} packets", completeLostWindows,
                 decoded_buffer.size(), allSource;

    //logger init
    bool newFile = false;
    if (!filesystem::exists("logs/measurementsReceiver_" + to_string(code_rate) + ".csv")) {
        newFile = true;
    }

    spdlog::set_pattern("%v");
    auto loggerM = spdlog::basic_logger_mt("measurements",
                                           "logs/measurementsReceiver_" + to_string(code_rate) + ".csv");
    loggerM->flush_on(spdlog::level::info);
    string filename = (string) "logs/resultsReceiver_" + to_string(code_rate) + "_" + to_string(
            lossRateSource) + ".csv";
    auto loggerB = spdlog::basic_logger_mt("blockSize", filename);
    loggerB->flush_on(spdlog::level::info);

    if (newFile) {
        loggerM->info(
                "totalPackets,totalTime,throughput,avgWaitingTime,avgDecodingTime,avgBlockSize,totalDecodes,"
                "avgDecodingDelay,totalSource,sourceLoss,totalCoded,codedLoss,completeWindowsSkipped,totalReceived,"
                "slidingWindowMax,parameterToLog,totalReceivedAndDecoded,avgRecurrenceTime");
    }

    loggerM->info("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}", (currentGen + 1) * code_rate,
                  (float) elapsedComplete.count() * 1e-9, packetRate, averageWaitingTime.count(),
                  averageDecodeTime.count(),
                  averageDecodeBlockSize, decodeTimes.size(), averageWaitingTime.count() + averageDecodeTime.count(),
                  totalSource, lossRateSource,
                  totalCoded, lossRateCoded, completeLostWindows,
                  decoded_buffer.size(), sliding_window_size, parameterToLog, receivedAndDecoded,
                  averageRecurrenceTime.count());

    loggerB->info("gen,decodeTime,waitingTime,blockSize,sourceBufferSize,parameterToLog");
    auto it = decodeGen.begin();
    auto itD = decodeTimes.begin();
    auto itW = waitingTimes.begin();
    auto itS = decodeSourceBufferSize.begin();
    for (int blockSize: decodeBlockSize) {
        loggerB->info("{},{},{},{},{},{}", it.operator*(), itD.operator*().count(), itW.operator*().count(),
                      blockSize, itS.operator*(), parameterToLog);
        advance(it, 1);
        advance(itD, 1);
        advance(itW, 1);
        advance(itS, 1);
    }

    return 0;
}
