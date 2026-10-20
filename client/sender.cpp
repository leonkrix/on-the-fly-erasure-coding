#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <list>
#include <cstring>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <numeric>
#include <filesystem>
#include <random>
#include <unistd.h>
#include <thread>

extern "C" {
#include <moeprlnc/rlnc.h>
#include <moepgf/moepgf.h>
}
using namespace std;

struct sockaddr_in senderAddr, receiverAddr, senderAddrState, receiverAddrState;
int socketFd, socketState, socketConnectedState;

/// struct to hold information of a source packet
/// \param symbolID
/// \param generation
/// \param payloadSize
/// \param payload[2048]
struct SourcePacket {
    uint16_t symbolID{};
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

//sender vars
uint8_t coded_id = 0;
uint16_t current_last_id = 0;
list<SourcePacket> sliding_window;
uint64_t generation = 0;
uint64_t lastAckedGen = 0;
int offset = 0;
uint8_t currentWindowCycle = 0;

//parameter
/// max size of the sliding window
uint16_t sliding_window_size = 256;
/// code rate k, after each k source packets, 1 coded packet is created and sent
int code_rate = 4;
/// amount of total packets to simulate
int amount_packets = 100000;
/// debug option to print debug information
bool debug = false;


// network limiter
/// packets per second, no limit if 0
int packetRatePerSecond = 0;
auto start = chrono::high_resolution_clock::now();
double sleepTime = 0;

//measurement vars
vector<chrono::nanoseconds> encodeTimes;
vector<chrono::nanoseconds> timeBetweenSource;
auto startBetweenSource = chrono::high_resolution_clock::now();
auto endBetweenSource = chrono::high_resolution_clock::now();
auto startSendSource = chrono::high_resolution_clock::now();
auto endSendSource = chrono::high_resolution_clock::now();
vector<int> encodeBlockSize;
int totalSource = 0;
int completeLostWindows = 0;

/// Sends source packet to receiver\n
/// tetrys header type = 0
/// \param sourcePacket
void send_source_to_receiver(SourcePacket *sourcePacket) {
    if ((generation > 0) || (sourcePacket->symbolID > 0 && generation == 1)) {
        endBetweenSource = chrono::high_resolution_clock::now();

        auto elapsedBetweenSourceTime = chrono::duration_cast<chrono::nanoseconds>(
                endBetweenSource - startBetweenSource);
        timeBetweenSource.push_back(elapsedBetweenSourceTime);
    }
    startBetweenSource = chrono::high_resolution_clock::now();

    size_t size = sourcePacket->payloadSize + 14;
    uint8_t buffer[size];
    uint8_t bufferGen[8];
    //0 = source packet
    buffer[0] = 0;
    buffer[1] = (sourcePacket->symbolID >> 8) & 0xFF;
    buffer[2] = (sourcePacket->symbolID >> 0) & 0xFF;
    buffer[3] = sourcePacket->windowCycle;
    buffer[12] = (sourcePacket->payloadSize >> 8) & 0xFF;
    buffer[13] = (sourcePacket->payloadSize >> 0) & 0xFF;

    for (int i = 0; i < 8; ++i) {
        bufferGen[i] = (sourcePacket->generation >> (7 - i) * 8) & 0xFF;
    }
    copy(bufferGen, bufferGen + 8, buffer + 4);
    copy(sourcePacket->payload, sourcePacket->payload + size - 14, buffer + 14);

    socklen_t length = sizeof(receiverAddr);
    sendto(socketFd, buffer, size, 0, (const struct sockaddr *) &receiverAddr, length);
    totalSource++;

    //limit send rate
    if (packetRatePerSecond > 0) {
        if (generation == 1) {
            start = chrono::high_resolution_clock::now();
            sleepTime = (double) 1.0 / (double) packetRatePerSecond;
        }
        double runTime = (double) std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - start).count() * 1e-9;
        double currentRate = totalSource / runTime;

        if (currentRate > packetRatePerSecond) {
            sleepTime *= 1.02;
        } else if (currentRate < packetRatePerSecond) {
            sleepTime *= 0.98;
        }
        double sleep = sleepTime * 1e9;
        std::this_thread::sleep_for(std::chrono::nanoseconds((long) sleep));
    }
}

/// Creates a random source symbol and fill sourcePacket \n
/// symbol range: ASCII 33 (!) - 126(~)
/// \return filled sourcePacket
void create_source_packet() {
    SourcePacket sourcePacket{};
    for (int i = 0; i < 32; i++) {
        sourcePacket.payload[i] = 33 + ::rand() % 93;
    }
    sourcePacket.symbolID = current_last_id;
    sourcePacket.generation = generation - 1;
    sourcePacket.windowCycle = currentWindowCycle;
    current_last_id++;

    if (sourcePacket.symbolID < sliding_window_size) {
        //adds sourcePacket to the end of the sliding window
        sliding_window.push_back(sourcePacket);
    } else {
        //Throw away a complete total window if max size is reached
        if (currentWindowCycle == 0) {
            currentWindowCycle = 1;
        } else {
            currentWindowCycle = 0;
        }
        sliding_window.clear();
        sourcePacket.windowCycle = currentWindowCycle;
        sourcePacket.symbolID = 0;
        current_last_id = 1;
        coded_id = 0;
        offset = 0;
        sliding_window.push_back(sourcePacket);
        completeLostWindows++;
    }
}

/// Sends coded packet to receiver\n
/// tetrys header type = 1
/// \param codedPacket
void send_coded_to_receiver(CodedPacket *codedPacket) {
    size_t size = codedPacket->payloadSize + 22;
    uint8_t buffer[size];
    uint8_t bufferSmallestGenUsed[8];
    uint8_t bufferGen[8];
    //1 = coded packet
    buffer[0] = 1;
    buffer[1] = (codedPacket->symbolID >> 8) & 0xFF;
    buffer[2] = (codedPacket->symbolID >> 0) & 0xFF;
    buffer[3] = codedPacket->windowCycle;
    buffer[20] = (codedPacket->payloadSize >> 8) & 0xFF;
    buffer[21] = (codedPacket->payloadSize >> 0) & 0xFF;

    for (int i = 0; i < 8; ++i) {
        bufferSmallestGenUsed[i] = (codedPacket->smallestGenUsed >> (7 - i) * 8) & 0xFF;
    }
    copy(bufferSmallestGenUsed, bufferSmallestGenUsed + 8, buffer + 4);
    for (int i = 0; i < 8; ++i) {
        bufferGen[i] = (codedPacket->generation >> (7 - i) * 8) & 0xFF;
    }
    copy(bufferGen, bufferGen + 8, buffer + 12);
    copy(codedPacket->payload, codedPacket->payload + size - 22, buffer + 22);

    socklen_t length = sizeof(receiverAddr);
    sendto(socketFd, buffer, size, 0, (const struct sockaddr *) &receiverAddr, length);
}

/// Creates a new block, filled with all source symbols currently part of the sliding window \n
/// encodes the source symbols with unit vector (e.g. 0010), spacer and size \n
/// encodes one coded symbol per block, covering all source symbols part of the block
void encode_and_sent() {
    auto encodeTime = chrono::high_resolution_clock::now();

    //create block with size = current symbols in the sliding window
    int blockSize = (int) sliding_window.size();

    //init encoding block
    rlnc_block_t block;
    if ((block = rlnc_block_init(blockSize, 32, MOEPGF_MAX_ALIGNMENT, MOEPGF256)) == nullptr) {
        cout << "rlnc_block_init() failed\n";
        exit(-1);
    }

    //fill block with all currently active symbols
    auto it = sliding_window.begin();
    for (int i = 0; i < blockSize; ++i) {
        rlnc_block_add(block, i, it->payload, 32);
        advance(it, 1);
    }

    int limit = blockSize - code_rate;
    //encode block and create codedSourceSymbols
    for (int i = 0; i < blockSize; ++i) {
        SourcePacket codedSourcePacket{};
        codedSourcePacket.payloadSize = rlnc_block_encode(block, codedSourcePacket.payload,
                                                          sizeof(codedSourcePacket.payload), RLNC_STRUCTURED);
        codedSourcePacket.symbolID = i;
        codedSourcePacket.windowCycle = currentWindowCycle;
        codedSourcePacket.generation = generation - 1;

        //only sent new symbols to receiver
        if (i >= limit) {
            startSendSource = chrono::high_resolution_clock::now();
            send_source_to_receiver(&codedSourcePacket);
            endSendSource = chrono::high_resolution_clock::now();
            encodeTime += (endSendSource - startSendSource);
        }
    }

    //encode block and create codedSymbol
    CodedPacket codedPacket{};
    /*
    //encode one more time for linear independence
    unsigned int seed = generation - lastAckedGen - 1;
    size_t codedSize = rlnc_block_encode(block, codedPacket.payload, sizeof(codedPacket.payload), RLNC_STRUCTURED,
                                         seed);
*/
    for (int i = 0; i < generation - lastAckedGen - 1 + offset; ++i) {
        rlnc_block_encode(block, codedPacket.payload, sizeof(codedPacket.payload), RLNC_STRUCTURED);
    }
    size_t codedSize = rlnc_block_encode(block, codedPacket.payload, sizeof(codedPacket.payload), RLNC_STRUCTURED);

    /* //useless
    bool noZero = false;
    while (!noZero) {
        for (int i = 0; i < codedSize - 34; ++i) {
            if (codedPacket.payload[i] == 0) {
                rlnc_block_encode(block, codedPacket.payload, sizeof(codedPacket.payload), RLNC_STRUCTURED);
                offset++;
                break;
            }
            if (i == codedSize - 34 - 1) {
                noZero = true;
            }
        }
    }
    */

    //fill codedPacket with codedSymbol and increase coded id
    codedPacket.symbolID = coded_id;
    codedPacket.windowCycle = currentWindowCycle;
    codedPacket.smallestGenUsed = sliding_window.front().generation;
    codedPacket.generation = generation - 1;
    codedPacket.payloadSize = codedSize;
    coded_id++;

    encodeBlockSize.push_back(blockSize);
    encodeTimes.push_back(chrono::high_resolution_clock::now() - encodeTime);
    //sent coded to receiver
    send_coded_to_receiver(&codedPacket);

    rlnc_block_free(block);
}

/// checks if new ack from receiver arrived and updates sliding window accordingly
void handleAck() {
    uint8_t buffer[64]{};
    socklen_t length = sizeof(receiverAddr);

    ssize_t n = recvfrom(socketFd, buffer, 64, MSG_DONTWAIT, (struct sockaddr *) &receiverAddr, &length);

    if (n == 0) {
        return;
    }

    /*
    //check for END ACK
    if (buffer[0] == 11 && n == 8) {
        cout << "END ACK received, leaving...\n";
        transmissionEnded = true;
        return;
    }
    */

    //ack packet
    if (buffer[0] == 2 && n == 9) {
        uint8_t bufferGen[8];
        copy(buffer + 1, buffer + 9, bufferGen);

        ::uint64_t ackGeneration =
                ((std::uint64_t) bufferGen[7] << 0) | ((std::uint64_t) bufferGen[6] << 8) |
                ((std::uint64_t) bufferGen[5] << 16) | ((std::uint64_t) bufferGen[4] << 24) |
                ((std::uint64_t) bufferGen[3] << 32) | ((std::uint64_t) bufferGen[2] << 40) |
                ((std::uint64_t) bufferGen[1] << 48) | ((std::uint64_t) bufferGen[0] << 56);

        //update vars
        lastAckedGen = ackGeneration;

        if (debug) {
            cout << "Ack received: in latest decoded gen: " << ackGeneration << "\n\n";
        }

        bool found = false;

        //return if acked gen not part of sliding window
        for (SourcePacket s: sliding_window) {
            if (s.generation == ackGeneration) {
                found = true;
                break;
            }
        }
        if (!found) {
            return;
        }

        //remove acked gens from the sliding window
        auto it1 = sliding_window.begin();
        auto it2 = sliding_window.begin();

        for (int i = 0; i < sliding_window.size(); ++i) {
            if (it2->generation <= ackGeneration) {
                advance(it2, 1);
            } else {
                break;
            }
        }
        sliding_window.erase(it1, it2);
        it1 = sliding_window.begin();

        for (int i = 0; i < sliding_window.size(); ++i) {
            SourcePacket sourcePacket = it1.operator*();
            sourcePacket.symbolID = i;

            it1 = sliding_window.erase(it1);
            sliding_window.insert(it1, sourcePacket);
        }

        //update vars
        it1 = sliding_window.begin();
        advance(it1, sliding_window.size() - 1);
        current_last_id = it1->symbolID + 1;
        coded_id = 0;
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
                "-port port\n"
                "-c code_rate\n"
                "-p amount_packets\n"
                "-ws sliding_window_size\n";
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (0 == strcmp("--help", argv[i])) {
            cout << "Usage (addr must be set, other parameters are optional):\n"
                    "--help\n"
                    "--debug\n"
                    "-s sender_addr\n"
                    "-r receiver_addr\n"
                    "-port port\n"
                    "-c code_rate\n"
                    "-p amount_packets\n"
                    "-ws sliding_window_size\n";
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
        if (0 == strcmp("-c", argv[i])) {
            code_rate = stoi(argv[i + 1]);
        }
        if (0 == strcmp("-p", argv[i])) {
            amount_packets = stoi(argv[i + 1]);
        }
        if (0 == strcmp("-ws", argv[i])) {
            sliding_window_size = stoi(argv[i + 1]);
        }
        if (0 == strcmp("--debug", argv[i])) {
            debug = true;
        }
        if (0 == strcmp("-para", argv[i])) {
            parameterToLog = stof(argv[i + 1]);
        }
    }

    if (code_rate == 0) {
        cout << "Code rate must be > 0";
        return 1;
    }

    ::srand(time(nullptr));

    //set sliding window max size dynamically to always fit complete gens, depending on code rate
    int diff = sliding_window_size % code_rate;
    if (diff != 0) {
        sliding_window_size = sliding_window_size - diff + code_rate;
    }

    if (debug) {
        cout << "Code rate set to: " << code_rate << "\n";
        cout << "Sliding window size set to: " << sliding_window_size << "\n";
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
    setsockopt(socketState, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(socketConnectedState, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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

    receiverAddrState.sin_family = AF_INET;
    receiverAddrState.sin_addr.s_addr = inet_addr(receiver_addr);
    receiverAddrState.sin_port = htons(1338);

    //bind socket to address and port
    if (bind(socketFd, (const struct sockaddr *) &senderAddr, sizeof(senderAddr)) < 0) {
        perror("bind failed");
        exit(-2);
    }
    if (bind(socketState, (const struct sockaddr *) &senderAddrState, sizeof(senderAddrState)) < 0) {
        perror("bind failed");
        exit(-2);
    }
    if (listen(socketState, 1) < 0) {
        perror("listen failed");
        exit(EXIT_FAILURE);
    }
    if ((socketConnectedState = accept(socketState, nullptr, nullptr)) < 0) {
        perror("accept failed");
        exit(EXIT_FAILURE);
    }

    //wait for receiver
    cout << "Sender started, waiting for Receiver to ready up\n";
    bool receiverRdy = false;
    while (!receiverRdy) {
        char buffer[1024];
        ssize_t n = read(socketConnectedState, buffer, 1024);
        if (n == 4 && buffer[0] == 12) {
            receiverRdy = true;
        }
    }

    //Send code rate/sliding window size information
    ::uint8_t bufferRdy[7];
    bufferRdy[0] = 12;
    char bufferCharRdy[4] = "RDY";
    copy(bufferCharRdy, bufferCharRdy + 3, bufferRdy + 1);
    bufferRdy[4] = code_rate;
    bufferRdy[5] = (sliding_window_size >> 8) & 0xFF;
    bufferRdy[6] = (sliding_window_size >> 0) & 0xFF;
    send(socketConnectedState, bufferRdy, 7, 0);

    char bufferA[64];
    while (true) {
        ssize_t n = read(socketConnectedState, bufferA, 64);
        if (n == 4 && bufferA[0] == 13) {
            break;
        }
    }
    cout << "Receiver ready, starting to send data\n";
    auto beginComplete = chrono::high_resolution_clock::now();

    for (int i = 1; i < amount_packets + 1; ++i) {
        if (i % code_rate - 1 == 0) {
            if (debug) {
                cout << generation + 1 << ". Generation:\n";
            }
            generation++;
        }
        handleAck();

        create_source_packet();

        if (i % code_rate == 0) {
            //print the sliding window
            if (debug) {
                cout << "Senders' sliding window (showing Source IDs) = { ";
                for (SourcePacket n: sliding_window) {
                    cout << (int) n.symbolID << ", ";
                }
                cout << "};" << " Size: " << sliding_window.size() << "\n";

                //print sliding window info
                cout << "Sliding window index range: [" << 0 << ":" << current_last_id - 1 << "]\n\n";
                cout << "Sliding window gen range: [" << sliding_window.front().generation << ":"
                     << sliding_window.back().generation << "]\n\n";
            }
            //encode generation and send source packets + coded packet
            encode_and_sent();
        }
    }

    auto endComplete = chrono::high_resolution_clock::now();

    cout << "transmission finished, sending END packet...\n";
    ::uint8_t buffer[4];
    buffer[0] = 10;
    char bufferChar[13] = "END";
    copy(bufferChar, bufferChar + 4, buffer + 1);

    for (int i = 0; i < 10; ++i) {
        sendto(socketFd, buffer, 4, 0, (const struct sockaddr *) &receiverAddr, sizeof(receiverAddr));
    }
    send(socketConnectedState, buffer, 4, 0);
    for (int i = 0; i < 100; ++i) {
        handleAck();
    }

    close(socketConnectedState);
    shutdown(socketState, SHUT_RDWR);

    //Measurement calculation
    auto elapsedComplete = chrono::duration_cast<chrono::nanoseconds>(endComplete - beginComplete);
    float packetRate = (float) (generation * code_rate) / (float) ((double) elapsedComplete.count() * 1e-9);
    spdlog::info("Complete packet handling time of {} encoded packets: {:.{}f} seconds -> {} p/s",
                 generation * code_rate,
                 (float) elapsedComplete.count() * 1e-9, 9,
                 packetRate, 9);

    chrono::nanoseconds averageEncodeTime =
            accumulate(encodeTimes.begin(), encodeTimes.end(), (chrono::nanoseconds) 0) / encodeTimes.size();
    chrono::nanoseconds averageBetweenSourceTime =
            accumulate(timeBetweenSource.begin(), timeBetweenSource.end(), (chrono::nanoseconds) 0) /
            timeBetweenSource.size();
    float averageEncodeBlockSize =
            accumulate(encodeBlockSize.begin(), encodeBlockSize.end(), (float) 0) / (float) encodeBlockSize.size();
    spdlog::info(
            "Avg encode time: {} nanoseconds with an avg block size: {} and {} total encodes, {} complete windows lost, "
            "{} ns between send source packets",
            averageEncodeTime.count(), averageEncodeBlockSize, encodeTimes.size(), completeLostWindows,
            averageBetweenSourceTime.count());
    float packetLossSource = (((float) amount_packets / (float) totalSource) - 1) * 100;

    //logger init
    bool newFile = false;
    if (!filesystem::exists("logs/measurementsSender_" + to_string(code_rate) + ".csv")) {
        newFile = true;
    }

    spdlog::set_pattern("%v");
    auto loggerM = spdlog::basic_logger_mt("measurements", "logs/measurementsSender_" + to_string(code_rate) + ".csv");
    loggerM->flush_on(spdlog::level::info);
    string filename =
            (string) "logs/resultsSender_" + to_string(code_rate) + "_" + to_string(packetLossSource) + ".csv";
    auto loggerB = spdlog::basic_logger_mt("blockSize", filename);
    loggerB->flush_on(spdlog::level::info);

    if (newFile) {
        loggerM->info("totalPacket,totalTime,throughput,avgEncodeTime,avgBlockSize,lostWindows,slidingWindowMax,"
                      "timeBetweenSource,parameterToLog");
    }

    loggerM->info("{},{},{},{},{},{},{},{},{}", generation * code_rate, (float) elapsedComplete.count() * 1e-9,
                  packetRate, averageEncodeTime.count(), averageEncodeBlockSize, completeLostWindows,
                  sliding_window_size, averageBetweenSourceTime.count(), parameterToLog);

    loggerB->info("gen,encodeTime,blockSize,timeBetweenSource,parameterToLog");
    ::uint64_t genIter = 0;
    auto it = encodeTimes.begin();
    auto it2 = timeBetweenSource.begin();
    for (int blockSize: encodeBlockSize) {
        loggerB->info("{},{},{},{},{}", genIter, it.operator*().count(), blockSize, it2.operator*().count(),
                      parameterToLog);
        genIter++;
        advance(it, 1);
        advance(it2, code_rate);
    }

    return 0;
}
