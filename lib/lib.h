#ifndef LIB_LIB_H
#define LIB_LIB_H

#include <cstdint>

/// \mainpage Documentation for the "Tetrys-like" network coding library\n
/// Documentation of header and source files are provided

/// @struct encoder
/// @brief Actual encoder, private (structs vars should be not accessed)
struct encoder;
/// @struct decoder
/// @brief Actual decoder, private (structs vars should be not accessed)
struct decoder;

/// @struct encoder_stats
/// @brief Current statistics for the encoder, can be get with get_stats(encoder_t encoder)
struct encoder_stats {
    int slidingWindowSize{}; ///< Size of current sliding window, equals number of source packets in the sliding window
    int currentLastId{}; ///< ID of next source packet to be added to sliding window, current last id in the window = currentLastId - 1
    int codedId{}; ///< ID of the next coded packet to be generated, last generated coded packet id = codedId - 1
    uint64_t currentGen{}; ///< Current gen
    int freeInCurrentGen{}; ///< Number of symbols packets free in current gen
    int currentWindowCycle{}; ///< Current window cycle (Changes if sliding window max size is reached
};

/// @struct decoder_stats
/// @brief Current statistics for the decoder, can be get with get_stats(decoder_t decoder)
struct decoder_stats {
    int amountSource{}; ///< Current number of stored source packets
    int amountCoded{}; ///< Current number of stored coded packets
    int amountDecoded{}; ///< Current number of not yet get decoded symbols
    int missingPackets{}; ///< Number of missing source packets in all currently stored gens
    int generationsBuffered{}; ///< Number of all gens stored after the first gen in the buffer (all gens in the buffer = generationsBuffered + 1)
    uint64_t currentGen{}; ///< Newest gen in the buffer
    bool ableToDecode{}; ///< True if able to decode, false if too many packets missing
};

/// Typedef struct for the encoder, public
typedef struct encoder *encoder_t;
/// Typedef struct for the decoder, public
typedef struct decoder *decoder_t;

/// \brief Initialize the encoder with the set codeRate and max_sliding_window size
/// \param code_rate Code rate of the encoder
/// \param max_sliding_window Maximum packets simultaneously part of the sliding window
/// \return encoder_t, pointer to \ref encoder
encoder_t encoder_init(int code_rate, int max_sliding_window);

/// \brief Initialize the decoder with the set codeRate and max_sliding_window size
/// \param code_rate Code rate of the encoder
/// \param max_sliding_window Maximum packets simultaneously part of the sliding window
/// \return decoder_t, pointer to \ref decoder
decoder_t decoder_init(int code_rate, int max_sliding_window);

/// \brief Frees the encoder and its vars after use
/// \param encoder Encoder to free
void encoder_free(encoder_t encoder);

/// \brief Frees the decoder and its vars after use
/// \param decoder Decoder to free
void decoder_free(decoder_t decoder);

/// \brief Adds one source symbol buffer of size 32 to the sliding window
/// \details Creates the ready to send source packet, stored in dst, dst size must be enough to store all bytes\n
/// Source Packet in dst contains all necessary headers
/// \param encoder Encoder to add symbols
/// \param src Source Symbols to add to the sliding window (32B)
/// \param dst Destination buffer to store the ready to send source packet, must be big enough
/// \return Size of bytes written in dst (Source Packet size), 0 if not added due to complete generation -> call encode() first)
std::size_t encoder_add(encoder_t encoder, uint8_t src[32], uint8_t *dst);

/// \brief Encodes current symbols of the sliding window
/// \details Creates a coded symbol, stored in dst, dst size must be enough to store all bytes\n
/// Coded Packet in dst contains all necessary headers
/// \param encoder Encoder to encode
/// \param dst Destination buffer to store the encoded coded symbol
/// \return Size of bytes written in dst (Coded Packet size)
std::size_t encode(encoder_t encoder, uint8_t *dst);

/// \brief Handles acknowledgement packet from decoder
/// \details Must provide the returned buffer from generate_ack(decoder_t decoder, uint8_t *dst), size = 9\n
/// updates encoder.lastAckedGen
/// \param encoder Encoder to handle the ack
/// \param src Ack buffer
void encoder_handle_ack(encoder_t encoder, uint8_t *src);

/// \brief Adds Source or Coded Packets to the Decoder
/// \details Must be provided with source buffer, either from encode(encoder_t encoder, uint8_t *dst)
/// or encoder_add(encoder_t encoder, uint8_t src[32], uint8_t *dst)\n
/// -> Header needed
/// \param decoder Decoder to add the packet
/// \param src Source buffer containing the packet
void decoder_add(decoder_t decoder, uint8_t *src);

/// \brief Decodes all packets if possible
/// \param decoder Decoder to decode
/// \return 0 if decode not possible, number of decoded elements if possible
int decode(decoder_t decoder);

/// \brief Gets the oldest decoded Source Symbol, will be written to dst, size must be appropriate
/// \param decoder Decoder to get the symbol from
/// \param dst Destination buffer to write the symbol to
/// \return 0 if no decoded packet available, else returns payload size (32B)
std::size_t get_decoded(decoder_t decoder, uint8_t *dst);

/// \brief Generates an acknowledgement packet, which should be used after at the encoder's handle_ack() function
/// \details Packet comes with ready to send header, always ack-ing the latestDecodedGen\n
/// Ack is written to dst buffer, size must be appropriate (9B)
/// \param decoder Decoder to generate the ack
/// \param dst Destination buffer to write the ack to
/// \return 9, size of ack packet
std::size_t generate_ack(decoder_t decoder, uint8_t *dst);

/// \brief Get current statistics of a encoder
/// \param decoder Encoder to get stats from
/// \return encoder_stats struct
encoder_stats get_stats(encoder_t encoder);

/// \brief Get current statistics of a decoder
/// \param decoder Decoder to get stats from
/// \return decoder_stats struct
decoder_stats get_stats(decoder_t decoder);

/// \brief Prints the current encoder_stats, temporary
/// \param decoder Encoder to get stats from
void print_encoder_stats(encoder_t encoder);

/// \brief Prints the current decoder_stats, temporary
/// \param decoder Decoder to get stats from
void print_decoder_stats(decoder_t decoder);

/// \brief Prints some information of the current sliding window, temporary
/// \param encoder Sliding window of encoder
void print_sliding_window(encoder_t encoder);

#endif //LIB_LIB_H
