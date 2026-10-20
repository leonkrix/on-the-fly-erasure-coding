#include "lib.h"
#include <list>
#include <cstdlib>
#include <iostream>

extern "C" {
#include <moeprlnc/rlnc.h>
#include <moepgf/moepgf.h>
}

/// @struct SourcePacket
/// @brief Holds information of one unique SourcePacket
struct SourcePacket {
    uint16_t symbolID{}; ///< \brief symbol id of this SourcePacket \details set by \ref encoder.currentLastId
    uint64_t generation{}; ///< \brief generation this SourcePacket is part of \details set by \ref encoder.generation
    uint8_t windowCycle{}; ///< \brief window cycle value {0;1} of this SourcePacket \details set by \ref encoder.currentWindowCycle
    size_t payloadSize{}; ///< \brief size of SourcePacket.payload \details set after calling encoder_add()
    uint8_t payload[2048]{}; ///< \brief actual payload data of this SourcePacket \details set after calling encoder_add()
};

/// @struct CodedPacket
/// @brief Holds information of one unique CodedPacket
struct CodedPacket {
    uint16_t symbolID{}; ///< \brief symbol id of the CodedPacket \details set by \ref encoder.codedId
    uint64_t smallestGenUsed{}; ///< \brief smallest generation used in the linear combination
    ///< \details equals SourcePacket.generation of first element in the encoder.slidingWindow
    ///< at time of generating this CodedSymbol with encode()
    uint64_t generation{}; ///< \brief generation this CodedPacket is part of \details set by \ref encoder.generation
    uint8_t windowCycle{}; ///< \brief window cycle value {0;1} of this CodedPacket \details set by \ref encoder.currentWindowCycle
    size_t payloadSize{}; ///< \brief size of CodedPacket.payload \details set after calling encode()
    uint8_t payload[2048]{}; ///< \brief actual payload data of this CodedPacket \details set after calling encoder_add()
};

/// @struct encoder
/// @brief Holds information of the encoder
struct encoder {
    uint8_t codedId{}; ///< \brief ID of the next CodedPacket to be generated \details Increases with each encode() call,
    ///< gets set to 0 by a successfully encoder_handle_ack() call
    uint16_t currentLastId{}; ///< \brief ID of the next SourcePacket to be created \details Increases with each encoder_add() call,
    ///< is reset if the encoder.slidingWindowSize is reached and the encoder.slidingWindow is cleared
    std::list<SourcePacket> *slidingWindow{}; ///< \brief sliding window of the encoder \details SourcePacket are added
    ///< with encoder_add() and removed by the encoder_handle_ack() function
    uint64_t generation{}; ///< \brief current active generation \details increases by successfully calling encode()
    uint64_t lastAckedGen{}; ///< \brief last acknowledged generation from the receiver \details gets updated by encoder_handle_ack()
    int offset{}; ///< \brief ensures linear independent coded symbols
    uint8_t currentWindowCycle{}; ///< \brief current window cycle status {0;1} \details switches if the
    ///< encoder.slidingWindowSize is reached and the encoder.slidingWindow is cleared\n
    ///< used to ensure clearing of the source buffer at the receiver
    uint16_t slidingWindowSize{}; ///< \brief max size of the encoder.slidingWindow \brief if size is reached,
    ///< encoder.slidingWindow get cleared and IDs are reset
    int codeRate{}; ///< \brief code rate of the encoder \details gets set by encoder_init()
    int freeInCurrentGen{}; ///< \brief number of SourcePacket needed to complete the current encoder.generation
    ///< \details decreases by calling encoder_add() and is reset by once successfully called encode()
};

/// @struct decoder
/// @brief Holds information of the decoder
struct decoder {
    std::list<SourcePacket> *sourceBuffer{}; ///< \brief buffer to store SourcePacket \details SourcePacket are added
    ///< with decoder_add() and removed by the flush_source_packets() function
    std::list<CodedPacket> *codedBuffer{}; ///< \brief buffer to store CodedPacket \details CodedPacket are added
    ///< with decoder_add() and removed by the decode() function after a successfully decoding
    std::list<SourcePacket> *decodedBuffer{}; ///< \brief buffer to store decoded SourcePacket \details SourcePacket are added
    ///< with decode() after a successfully decoding and removed by the get_decoded() function
    uint64_t smallestGenNeeded{}; ///< \brief smallest generation currently needed for decoding
    ///< \details is updated by decoder_add() if decoder.codedBuffer is empty and a new CodedPacket is added
    uint64_t latestDecodedGen{}; ///< \brief latest generation decoded \details is updated after a successfully decoding
    ///< with encode()
    int generationsBuffered{}; ///< \brief generations currently buffered (not including the first gen)
    ///< \details after one generation is currently in the decoder.sourceBuffer this value is increased to keep track on
    ///< how many additional generations are stored, e.g. after the first gen was incomplete and not able to recover\n
    ///< is increased by decoder_add() and decreased by flush_source_packets()
    uint64_t currentGen{}; ///< \brief current latest generation received \details both SourcePacket.generation
    ///< and CodedPacket.generation update this value via decoder_add()
    bool retry{}; ///< \brief retry value if a decoding failed \details set by decode()
    uint8_t currentWindowCycle{}; ///< \brief window cycle value \details can be updated by either SourcePacket.windowCycle
    ///< or CodedPacket.windowCycle in decoder_add(), if changed decoder.sourceBuffer and decoder.codedBuffer are cleared
    uint16_t slidingWindowSize{}; ///< \brief max sliding window size \details no specific limit of stored SourcePacket
    ///< needed at the decoder, but limits the max SourcePacket.symbolID
    int codeRate{}; ///< \brief code rate of the decoder \details gets set by decoder_init()
};

encoder_t encoder_init(int code_rate, int max_sliding_window) {
    encoder_t encoder;
    if (nullptr == (encoder = (encoder_t) malloc(sizeof(struct encoder)))) {
        perror("malloc() failed");
        return nullptr;
    }

    //set sliding window max size dynamically to always fit complete gens, depending on code rate
    int diff = max_sliding_window % code_rate;
    if (diff != 0) {
        max_sliding_window = max_sliding_window - diff + code_rate;
    }

    encoder->codeRate = code_rate;
    encoder->slidingWindowSize = max_sliding_window;
    encoder->freeInCurrentGen = code_rate;
    encoder->codedId = 0;
    encoder->currentLastId = 0;
    encoder->generation = 0;
    encoder->lastAckedGen = 0;
    encoder->offset = 0;
    encoder->currentWindowCycle = 0;
    encoder->slidingWindow = new std::list<SourcePacket>;
    return encoder;
}

decoder_t decoder_init(int code_rate, int max_sliding_window) {
    decoder_t decoder;
    if (nullptr == (decoder = (decoder_t) malloc(sizeof(struct decoder)))) {
        perror("malloc() failed");
        return nullptr;
    }

    //set sliding window max size dynamically to always fit complete gens, depending on code rate
    int diff = max_sliding_window % code_rate;
    if (diff != 0) {
        max_sliding_window = max_sliding_window - diff + code_rate;
    }

    decoder->sourceBuffer = new std::list<SourcePacket>;
    decoder->codedBuffer = new std::list<CodedPacket>;
    decoder->decodedBuffer = new std::list<SourcePacket>;
    decoder->smallestGenNeeded = 0;
    decoder->latestDecodedGen = 0;
    decoder->generationsBuffered = 0;
    decoder->currentGen = 0;
    decoder->retry = false;
    decoder->currentWindowCycle = 0;
    decoder->slidingWindowSize = max_sliding_window;
    decoder->codeRate = code_rate;
    return decoder;
}

void encoder_free(encoder_t encoder) {
    delete encoder->slidingWindow;
    free(encoder);
}

void decoder_free(decoder_t decoder) {
    delete decoder->sourceBuffer;
    delete decoder->codedBuffer;
    delete decoder->decodedBuffer;
    free(decoder);
}

size_t encoder_add(encoder_t encoder, uint8_t src[32], uint8_t *dst) {
    //check if gen full -> encode
    if (encoder->freeInCurrentGen == 0) {
        return 0;
    }

    SourcePacket sourcePacket{};

    if (encoder->currentLastId < encoder->slidingWindowSize) {
        sourcePacket.symbolID = encoder->currentLastId;
        encoder->currentLastId++;
    } else {
        if (encoder->currentWindowCycle == 0) {
            encoder->currentWindowCycle = 1;
        } else {
            encoder->currentWindowCycle = 0;
        }
        encoder->slidingWindow->clear();

        sourcePacket.symbolID = 0;
        encoder->currentLastId = 1;
        encoder->codedId = 0;
        encoder->offset = 0;
    }

    sourcePacket.generation = encoder->generation;
    encoder->freeInCurrentGen--;
    sourcePacket.windowCycle = encoder->currentWindowCycle;
    size_t size = 32 + 2 + encoder->currentLastId + encoder->freeInCurrentGen;

    sourcePacket.payloadSize = size;
    std::copy(src, src + 32, sourcePacket.payload);
    encoder->slidingWindow->push_back(sourcePacket);

    uint8_t buf[size];
    for (int i = 0; i < encoder->currentLastId; ++i) {
        buf[i] = 0;
    }
    buf[encoder->currentLastId - 1] = 1;
    for (int i = encoder->currentLastId; i <= encoder->currentLastId + encoder->freeInCurrentGen; ++i) {
        buf[i] = 0;
    }
    buf[encoder->slidingWindow->size() + encoder->freeInCurrentGen] = 32;
    buf[encoder->slidingWindow->size() + encoder->freeInCurrentGen + 1] = 0;
    std::copy(sourcePacket.payload, sourcePacket.payload + 32,
              buf + (encoder->slidingWindow->size() + encoder->freeInCurrentGen + 2));
    //std::copy(buf, buf + size, dst);

    /*
     * BEGIN HEADER SECTION, REMOVE IF NOT NEEDED, UNCOMMENT COPY ABOVE
     */
    uint8_t header[14];
    size += 14;
    uint8_t bufferGen[8];

    header[0] = 0;
    header[1] = (sourcePacket.symbolID >> 8) & 0xFF;
    header[2] = (sourcePacket.symbolID >> 0) & 0xFF;
    header[3] = sourcePacket.windowCycle;
    header[12] = (sourcePacket.payloadSize >> 8) & 0xFF;
    header[13] = (sourcePacket.payloadSize >> 0) & 0xFF;

    for (int i = 0; i < 8; ++i) {
        bufferGen[i] = (sourcePacket.generation >> (7 - i) * 8) & 0xFF;
    }
    std::copy(bufferGen, bufferGen + 8, header + 4);
    std::copy(header, header + 14, dst);
    std::copy(buf, buf + size - 14, dst + 14);
    /*
     * END HEADER SECTION
     */

    return size;
}

std::size_t encode(encoder_t encoder, uint8_t *dst) {
    //create block with size = current symbols in the sliding window
    int blockSize = (int) encoder->slidingWindow->size();

    //init encoding block
    rlnc_block_t block;
    if ((block = rlnc_block_init(blockSize, 32, MOEPGF_MAX_ALIGNMENT, MOEPGF256)) == nullptr) {
        std::cout << "rlnc_block_init() failed\n";
        exit(-1);
    }

    //fill block with all currently active symbols
    auto it = encoder->slidingWindow->begin();
    for (int i = 0; i < blockSize; ++i) {
        rlnc_block_add(block, i, it->payload, 32);
        advance(it, 1);
    }

    //encode block, trivial encoding of source symbols not needed
    for (int i = 0; i < blockSize; ++i) {
        uint8_t trash[2048];
        rlnc_block_encode(block, trash, sizeof(trash), RLNC_STRUCTURED);
    }

    //encode block and create codedSymbol
    CodedPacket codedPacket;
    //encode one more time for linear independence
    for (int i = 0; i < encoder->generation - encoder->lastAckedGen + encoder->offset; ++i) {
        rlnc_block_encode(block, codedPacket.payload, sizeof(codedPacket.payload), RLNC_STRUCTURED);
    }
    size_t codedSize = rlnc_block_encode(block, codedPacket.payload, sizeof(codedPacket.payload), RLNC_STRUCTURED);

    bool noZero = false;
    while (!noZero) {
        for (int i = 0; i < codedSize - 34; ++i) {
            if (codedPacket.payload[i] == 0) {
                rlnc_block_encode(block, codedPacket.payload, sizeof(codedPacket.payload), RLNC_STRUCTURED);
                encoder->offset++;
                break;
            }
            if (i == codedSize - 34 - 1) {
                noZero = true;
            }
        }
    }

    //fill codedPacket with codedSymbol and increase coded id
    codedPacket.symbolID = encoder->codedId;
    codedPacket.windowCycle = encoder->currentWindowCycle;
    codedPacket.smallestGenUsed = encoder->slidingWindow->front().generation;
    codedPacket.generation = encoder->generation;
    codedPacket.payloadSize = codedSize;
    encoder->codedId++;

    rlnc_block_free(block);
    encoder->freeInCurrentGen = encoder->codeRate;
    encoder->generation++;

    //std::copy(codedPacket.payload, codedPacket.payload + codedSize, dst);

    /*
     * BEGIN HEADER SECTION, REMOVE IF NOT NEEDED, UNCOMMENT COPY ABOVE
     */
    codedSize += 22;
    uint8_t header[codedSize];
    uint8_t bufferSmallestGenUsed[8];
    uint8_t bufferGen[8];

    header[0] = 1;
    header[1] = (codedPacket.symbolID >> 8) & 0xFF;
    header[2] = (codedPacket.symbolID >> 0) & 0xFF;
    header[3] = codedPacket.windowCycle;
    header[20] = (codedPacket.payloadSize >> 8) & 0xFF;
    header[21] = (codedPacket.payloadSize >> 0) & 0xFF;

    for (int i = 0; i < 8; ++i) {
        bufferSmallestGenUsed[i] = (codedPacket.smallestGenUsed >> (7 - i) * 8) & 0xFF;
    }
    std::copy(bufferSmallestGenUsed, bufferSmallestGenUsed + 8, header + 4);
    for (int i = 0; i < 8; ++i) {
        bufferGen[i] = (codedPacket.generation >> (7 - i) * 8) & 0xFF;
    }

    std::copy(bufferGen, bufferGen + 8, header + 12);
    std::copy(header, header + 22, dst);
    std::copy(codedPacket.payload, codedPacket.payload + codedSize - 22, dst + 22);
    /*
     * END HEADER SECTION
     */

    return codedSize;
}

void encoder_handle_ack(encoder_t encoder, uint8_t *src) {
    //ack packet
    if (src[0] == 2) {
        uint8_t bufferGen[8];
        std::copy(src + 1, src + 9, bufferGen);

        ::uint64_t ackGeneration =
                ((std::uint64_t) bufferGen[7] << 0) | ((std::uint64_t) bufferGen[6] << 8) |
                ((std::uint64_t) bufferGen[5] << 16) | ((std::uint64_t) bufferGen[4] << 24) |
                ((std::uint64_t) bufferGen[3] << 32) | ((std::uint64_t) bufferGen[2] << 40) |
                ((std::uint64_t) bufferGen[1] << 48) | ((std::uint64_t) bufferGen[0] << 56);

        //update vars
        encoder->lastAckedGen = ackGeneration;

        bool found = false;
        //return if acked gen not part of sliding window
        for (SourcePacket s: *encoder->slidingWindow) {
            if (s.generation == ackGeneration) {
                found = true;
                break;
            }
        }
        if (!found) {
            return;
        }

        //remove acked gens from the sliding window
        auto it1 = encoder->slidingWindow->begin();
        auto it2 = encoder->slidingWindow->begin();

        for (int i = 0; i < encoder->slidingWindow->size(); ++i) {
            if (it2->generation <= ackGeneration) {
                advance(it2, 1);
            } else {
                break;
            }
        }
        encoder->slidingWindow->erase(it1, it2);
        it1 = encoder->slidingWindow->begin();

        for (int i = 0; i < encoder->slidingWindow->size(); ++i) {
            SourcePacket sourcePacket = it1.operator*();
            sourcePacket.symbolID = i;

            it1 = encoder->slidingWindow->erase(it1);
            encoder->slidingWindow->insert(it1, sourcePacket);
        }

        //update vars
        it1 = encoder->slidingWindow->begin();
        advance(it1, encoder->slidingWindow->size() - 1);
        if (encoder->slidingWindow->empty()) {
            encoder->currentLastId = 0;
        } else {
            encoder->currentLastId = it1->symbolID + 1;
        }
        encoder->codedId = 0;
    }
}

/// \brief helper function to determine number of lost SourcePacket
/// \param decoder decoder to determine amountLost
/// \return amountLost, number of lost SourcePacket
int amount_lost(decoder_t decoder) {
    int allPackets = (decoder->generationsBuffered + 1) * decoder->codeRate;
    return allPackets - (int) decoder->sourceBuffer->size();
}

/// \brief helper function to remove all not needed SourcePacket from decoder.sourceBuffer
/// \param decoder decoder to remove the SourcePacket from
/// \param smallestGen new smallestGen needed for future decode()
void flush_source_packets(decoder_t decoder, uint64_t smallestGen) {
    //init range of source symbols to delete
    auto it = decoder->sourceBuffer->begin();
    auto it2 = decoder->sourceBuffer->begin();
    int flushedSymbols = 0;

    //depending on ack rate
    if (smallestGen > (decoder->latestDecodedGen + 1)) {
        smallestGen = decoder->latestDecodedGen;
    }

    //Iterate through sourceBuffer until all not needed source symbols included
    for (int i = 0; i < decoder->sourceBuffer->size(); i++) {
        if (it2->generation < smallestGen) {
            advance(it2, 1);
            flushedSymbols++;
        } else {
            break;
        }
    }

    //Erase symbols in range from receivers' buffer
    //set helper vars
    decoder->sourceBuffer->erase(it, it2);
    decoder->generationsBuffered = decoder->generationsBuffered - (flushedSymbols / decoder->codeRate);
}

void decoder_add(decoder_t decoder, uint8_t *src) {
    if (src[0] == 0) {
        SourcePacket sourcePacket{};
        sourcePacket.symbolID = ((std::uint16_t) src[2] << 0) | ((std::uint16_t) src[1] << 8);
        sourcePacket.windowCycle = src[3];
        sourcePacket.payloadSize = ((std::uint16_t) src[13] << 0) | ((std::uint16_t) src[12] << 8);

        uint8_t bufferGen[8];
        std::copy(src + 4, src + 12, bufferGen);

        sourcePacket.generation =
                ((std::uint64_t) bufferGen[7] << 0) | ((std::uint64_t) bufferGen[6] << 8) |
                ((std::uint64_t) bufferGen[5] << 16) | ((std::uint64_t) bufferGen[4] << 24) |
                ((std::uint64_t) bufferGen[3] << 32) | ((std::uint64_t) bufferGen[2] << 40) |
                ((std::uint64_t) bufferGen[1] << 48) | ((std::uint64_t) bufferGen[0] << 56);

        std::copy(src + 14, src + sourcePacket.payloadSize + 14, sourcePacket.payload);

        if (sourcePacket.generation > decoder->currentGen) {
            decoder->generationsBuffered =
                    decoder->generationsBuffered + (int) (sourcePacket.generation - decoder->currentGen);
            decoder->currentGen = sourcePacket.generation;
        }

        //Check for complete Sliding Window removed
        if (sourcePacket.windowCycle == decoder->currentWindowCycle) {
            decoder->sourceBuffer->push_back(sourcePacket);
        } else {
            decoder->generationsBuffered = 0;
            decoder->sourceBuffer->clear();
            decoder->codedBuffer->clear();
            decoder->currentWindowCycle = sourcePacket.windowCycle;
            decoder->sourceBuffer->push_back(sourcePacket);
        }
    } else if (src[0] == 1) {
        CodedPacket codedPacket;
        codedPacket.symbolID = ((std::uint16_t) src[2] << 0) | ((std::uint16_t) src[1] << 8);
        codedPacket.windowCycle = src[3];
        codedPacket.payloadSize = ((std::uint16_t) src[21] << 0) | ((std::uint16_t) src[20] << 8);

        uint8_t bufferSmallestGenUsed[8];
        std::copy(src + 4, src + 12, bufferSmallestGenUsed);
        uint8_t bufferGen[8];
        std::copy(src + 12, src + 20, bufferGen);

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

        std::copy(src + 22, src + codedPacket.payloadSize + 22, codedPacket.payload);

        if (codedPacket.generation > decoder->currentGen) {
            decoder->generationsBuffered =
                    decoder->generationsBuffered + (int) (codedPacket.generation - decoder->currentGen);
            decoder->currentGen = codedPacket.generation;
        }

        //dont add coded if corresponding gen already decoded
        if (codedPacket.generation <= decoder->latestDecodedGen && decoder->latestDecodedGen > 0) {
            return;
        }

        //Check for complete Sliding Window removed
        if (codedPacket.windowCycle == decoder->currentWindowCycle) {
            decoder->codedBuffer->push_back(codedPacket);
        } else {
            decoder->generationsBuffered = 0;
            decoder->sourceBuffer->clear();
            decoder->codedBuffer->clear();
            decoder->currentWindowCycle = codedPacket.windowCycle;
            decoder->codedBuffer->push_back(codedPacket);
        }
    }

    //set current smallestGenNeeded for decoding if coded packet available
    if (!decoder->codedBuffer->empty()) {
        decoder->smallestGenNeeded = decoder->codedBuffer->front().smallestGenUsed;
    }
    //flush source packets
    flush_source_packets(decoder, decoder->smallestGenNeeded);
}

int decode(decoder_t decoder) {
    if ((decoder->sourceBuffer->size() + decoder->codedBuffer->size()) >=
        ((decoder->generationsBuffered + 1) * decoder->codeRate)) {
        int amountLost = amount_lost(decoder);

        //blockSize are all stored source symbols at the receiver + number of missing symbols combined
        int blockSize = (int) decoder->sourceBuffer->size() + amountLost;

        //if no source loss, source symbols can be "decoded" without a coded packet
        if (amountLost == 0) {
            auto itS = decoder->sourceBuffer->begin();
            int counter = 0;

            for (int i = 0; i < decoder->sourceBuffer->size(); ++i) {
                if (itS->generation > decoder->latestDecodedGen ||
                    (itS->generation == 0 && decoder->latestDecodedGen == 0)) {
                    break;
                } else {
                    std::advance(itS, 1);
                    counter++;
                }
            }

            blockSize = (int) decoder->sourceBuffer->size() - counter;

            for (int i = 0; i < blockSize; ++i) {
                SourcePacket sourcePacket = itS.operator*();
                int index = 0;

                for (int j = 0; j < sourcePacket.payloadSize; j++) {
                    if (sourcePacket.payload[j] == 32 && sourcePacket.payload[j + 1] == 0) {
                        index = j + 2;
                    }
                }

                sourcePacket.payloadSize -= index;
                for (int j = 0; j < sourcePacket.payloadSize; ++j) {
                    sourcePacket.payload[j] = sourcePacket.payload[j + index];
                }

                //store in decoded buffer
                decoder->decodedBuffer->push_back(sourcePacket);
                //set helper var
                decoder->latestDecodedGen = sourcePacket.generation;
                advance(itS, 1);
            }

            decoder->codedBuffer->clear();

            return blockSize;
        }

        //init decode block
        rlnc_block_t block = rlnc_block_init(blockSize, 32, MOEPGF_MAX_ALIGNMENT, MOEPGF256);

        //padding for source and coded symbols needed, depending on amount stored generations
        if (amountLost > 1 || decoder->sourceBuffer->size() > decoder->codeRate) {
            auto itCEnd = decoder->codedBuffer->begin();
            auto itSEnd = decoder->sourceBuffer->begin();

            int highestSize = ((1 + decoder->generationsBuffered) * decoder->codeRate) + 34;

            //source symbol padding
            uint64_t startGen = decoder->sourceBuffer->front().generation;
            if (decoder->smallestGenNeeded < startGen) {
                startGen = decoder->smallestGenNeeded;
            }

            for (int i = 0; i < decoder->sourceBuffer->size(); ++i) {
                uint8_t padding[highestSize - 34];
                for (int j = 0; j < highestSize - 34; ++j) {
                    padding[j] = 0;
                }

                int diffGen = (int) (itSEnd->generation - startGen);
                int indexInGen = (itSEnd->symbolID % decoder->codeRate) + (diffGen * decoder->codeRate);
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
                itSEnd = decoder->sourceBuffer->erase(itSEnd);
                decoder->sourceBuffer->insert(itSEnd, source);

            }

            //coded symbol padding
            //special coded stores indices of coded packets which need a padding on the beginning
            int specialCoded[decoder->codedBuffer->size()];
            itCEnd = decoder->codedBuffer->begin();
            uint64_t smallestGen = itCEnd->smallestGenUsed;
            int k = 0;

            //determine smallest gen
            for (const CodedPacket &c: *decoder->codedBuffer) {
                if (c.smallestGenUsed < smallestGen) {
                    smallestGen = c.smallestGenUsed;
                }
            }

            //determine indices of special coded symbols
            for (int i = 0; i < decoder->codedBuffer->size(); ++i) {
                if (itCEnd->smallestGenUsed > smallestGen) {
                    specialCoded[k] = i;
                    k++;
                }
                advance(itCEnd, 1);
            }

            itCEnd = decoder->codedBuffer->begin();
            //padding for coded symbols, needing 0's in the beginning
            if (k > 0) {
                int m = 0;
                for (int i = 0; i < decoder->codedBuffer->size(); ++i) {
                    if (i == specialCoded[m] && m <= k) {
                        int diff = (int) (itCEnd->smallestGenUsed - smallestGen) * decoder->codeRate;
                        m++;
                        CodedPacket coded = itCEnd.operator*();

                        if (decoder->retry) {
                            int oldDiff = 0;
                            for (int j = 0; j < coded.payloadSize; ++j) {
                                if (coded.payload[j] == 0) {
                                    oldDiff++;
                                } else {
                                    break;
                                }
                            }
                            if (oldDiff % decoder->codeRate != 0) {
                                oldDiff = oldDiff - (oldDiff % decoder->codeRate);
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
                            itCEnd = decoder->codedBuffer->erase(itCEnd);
                            decoder->codedBuffer->insert(itCEnd, coded);
                        } else {
                            advance(itCEnd, 1);
                        }
                    } else {
                        advance(itCEnd, 1);
                    }
                }
            }

            itCEnd = decoder->codedBuffer->begin();
            //padding for coded symbols, 0's on the right
            for (int l = 0; l < decoder->codedBuffer->size(); l++) {
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
                    itCEnd = decoder->codedBuffer->erase(itCEnd);
                    decoder->codedBuffer->insert(itCEnd, coded);
                } else {
                    advance(itCEnd, 1);
                }
            }
        }

        //Decode source symbols
        auto itS = decoder->sourceBuffer->begin();
        for (int i = 0; i < decoder->sourceBuffer->size(); i++) {
            rlnc_block_decode(block, itS->payload, itS->payloadSize);
            advance(itS, 1);
        }

        //determine missing ids, by getting all symbols from the block, if size == 0 -> lost
        int missingInd[amountLost];
        int k = 0;
        for (int i = 0; i < blockSize; ++i) {
            uint8_t tmp[32];
            ssize_t size = rlnc_block_get(block, i, tmp, 32);
            if (size == 0) {
                missingInd[k] = i;
                k++;
            }
        }

        auto itC = decoder->codedBuffer->begin();
        //decode block with coded Symbol(s)
        for (int i = 0; i < amountLost; ++i) {
            rlnc_block_decode(block, itC->payload, itC->payloadSize);
            advance(itC, 1);
        }

        // if block not decoded from coded symbol, remove the last coded symbol and return
        if (rlnc_block_rank_decode(block) != (decoder->sourceBuffer->size() + amountLost)) {
            perror("rank not complete");
            decoder->codedBuffer->pop_back();
            decoder->retry = true;
            return 0;
        }

        //get decoded symbol from block, fill a newly decoded source packet and insert missing in senders' decoded buffer
        SourcePacket decodedPacket{};
        uint64_t gen = decoder->smallestGenNeeded;
        int counter = 0;
        int newlyDecoded = 0;

        for (int i = 0; i < blockSize; i++) {
            ssize_t size = rlnc_block_get(block, i, decodedPacket.payload, 32);

            if (i < decoder->slidingWindowSize) {
                decodedPacket.symbolID = i;
            } else {
                decodedPacket.symbolID = decoder->slidingWindowSize - decoder->codeRate + (i % decoder->codeRate);
            }

            decodedPacket.payloadSize = 32;
            if ((i % decoder->codeRate == 0 && i > 0)) {
                gen++;
            }
            decodedPacket.generation = gen;

            if (gen > decoder->latestDecodedGen || (gen == 0 && decoder->latestDecodedGen == 0)) {
                //add decoded symbol to decodedBuffer
                decoder->decodedBuffer->push_back(decodedPacket);
                newlyDecoded++;

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
                        auto iter = decoder->sourceBuffer->begin();
                        advance(iter, missingInd[counter]);
                        decoder->sourceBuffer->insert(iter, decodedPacket);
                        counter++;
                    }
                }
            }
        }

        //delete the coded packet(s), set help parameters, free block
        decoder->latestDecodedGen = decodedPacket.generation;
        decoder->codedBuffer->clear();
        decoder->retry = false;
        rlnc_block_free(block);
        return newlyDecoded;
    } else {
        return 0;
    }
}

std::size_t get_decoded(decoder_t decoder, uint8_t *dst) {
    if (!decoder->decodedBuffer->empty()) {
        std::copy(decoder->decodedBuffer->front().payload,
                  decoder->decodedBuffer->front().payload + decoder->decodedBuffer->front().payloadSize, dst);
        size_t size = decoder->decodedBuffer->front().payloadSize;
        decoder->decodedBuffer->pop_front();

        return size;
    } else {
        return 0;
    }
}

std::size_t generate_ack(decoder_t decoder, uint8_t *dst) {
    uint8_t buffer[9];
    uint8_t bufferGen[8];
    for (int i = 0; i < 8; ++i) {
        bufferGen[i] = (decoder->latestDecodedGen >> (7 - i) * 8) & 0xFF;
    }

    //Tetrys type 2 = window update packet
    buffer[0] = 2;

    //copy gen
    std::copy(bufferGen, bufferGen + 8, buffer + 1);
    std::copy(buffer, buffer + 9, dst);

    return 9;
}

encoder_stats get_stats(encoder_t encoder) {
    encoder_stats encoderStats;
    encoderStats.slidingWindowSize = (int) encoder->slidingWindow->size();
    encoderStats.currentLastId = encoder->currentLastId;
    encoderStats.codedId = encoder->codedId;
    encoderStats.currentGen = encoder->generation;
    encoderStats.freeInCurrentGen = encoder->freeInCurrentGen;
    encoderStats.currentWindowCycle = encoder->currentWindowCycle;

    return encoderStats;
}

decoder_stats get_stats(decoder_t decoder) {
    decoder_stats decoderStats;
    decoderStats.currentGen = decoder->currentGen;
    decoderStats.generationsBuffered = decoder->generationsBuffered;
    decoderStats.amountSource = (int) decoder->sourceBuffer->size();
    decoderStats.amountCoded = (int) decoder->codedBuffer->size();
    decoderStats.amountDecoded = (int) decoder->decodedBuffer->size();
    decoderStats.missingPackets = amount_lost(decoder);
    decoderStats.ableToDecode = ((decoder->sourceBuffer->size() + decoder->codedBuffer->size()) >=
                                 ((decoder->generationsBuffered + 1) * decoder->codeRate));
    return decoderStats;
}

void print_encoder_stats(encoder_t encoder) {
    encoder_stats encoderStats = get_stats(encoder);

    std::cout << "Current sliding window size: " << encoderStats.slidingWindowSize << ", current last id: "
              << encoderStats.currentLastId << ", current coded id: " << encoderStats.codedId << ", current gen: "
              << encoderStats.currentGen << ", free in current gen: " << encoderStats.freeInCurrentGen
              << ", current window cycle: " << encoderStats.currentWindowCycle << "\n";
}

void print_decoder_stats(decoder_t decoder) {
    decoder_stats decoderStats = get_stats(decoder);

    std::cout << "Current gen: " << decoderStats.currentGen << ", gens buffered: " << decoderStats.generationsBuffered
              << ", source stored: " << decoderStats.amountSource << ", coded stored: " << decoderStats.amountCoded
              << ", decoded stored: " << decoderStats.amountDecoded << ", lost packets: " << decoderStats.missingPackets
              << ", currently able to decode: " << std::boolalpha << decoderStats.ableToDecode << "\n";
}

void print_sliding_window(encoder_t encoder) {
    std::cout << "Sliding window ({ID,GEN}):\n";
    auto it = encoder->slidingWindow->begin();
    for (int i = 0; i < encoder->slidingWindow->size(); ++i) {
        std::cout << "{" << it->symbolID << "," << (int) it->generation << "} ";
        std::advance(it, 1);
    }
    std::cout << "\n";
}