/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * LZ4 Compression Library Test
 * Tests for LZ4 library availability (RocksDB dependency)
 */

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef __has_include
#if __has_include(<lz4.h>)
#include <lz4.h>
#define HAS_LZ4 1
#else
#define HAS_LZ4 0
#endif
#else
#include <lz4.h>
#define HAS_LZ4 1
#endif

int test_lz4()
{
     try
     {
#if HAS_LZ4
          /* Test 1: Basic LZ4 compression */
          std::string input = "This is a test string for LZ4 compression. "
                              "It should compress well with repeated patterns. "
                              "This is a test string for LZ4 compression. "
                              "It should compress well with repeated patterns.";

          const int input_size = static_cast<int>(input.size());
          const int max_compressed_size = LZ4_compressBound(input_size);

          if (max_compressed_size <= 0)
          {
               return 1; /* Invalid compressed size bound */
          }

          std::vector<char> compressed(max_compressed_size);
          std::vector<char> decompressed(input_size);

          /* Compress the data */
          int compressed_size = LZ4_compress_default(
               input.data(),
               compressed.data(),
               input_size,
               max_compressed_size);

          if (compressed_size <= 0)
          {
               return 2; /* Compression failed */
          }

          /* Verify compression reduced size (for this repetitive data) */
          if (compressed_size >= input_size)
          {
               return 3; /* Compression should reduce size for this data */
          }

          /* Test 2: Decompress the data */
          int decompressed_size = LZ4_decompress_safe(
               compressed.data(),
               decompressed.data(),
               compressed_size,
               input_size);

          if (decompressed_size != input_size)
          {
               return 4; /* Decompression size mismatch */
          }

          /* Test 3: Verify decompressed data matches original */
          if (memcmp(decompressed.data(), input.data(), input_size) != 0)
          {
               return 5; /* Decompressed data doesn't match original */
          }

          /* Test 4: Test with binary data */
          std::vector<unsigned char> binary_input(256);
          for (size_t i = 0; i < 256; ++i)
          {
               binary_input[i] = static_cast<unsigned char>(i);
          }

          const int binary_input_size = static_cast<int>(binary_input.size());
          const int binary_max_compressed_size = LZ4_compressBound(binary_input_size);

          std::vector<char> binary_compressed(binary_max_compressed_size);
          std::vector<char> binary_decompressed(binary_input_size);

          int binary_compressed_size = LZ4_compress_default(
               reinterpret_cast<const char*>(binary_input.data()),
               binary_compressed.data(),
               binary_input_size,
               binary_max_compressed_size);

          if (binary_compressed_size <= 0)
          {
               return 6; /* Binary compression failed */
          }

          int binary_decompressed_size = LZ4_decompress_safe(
               binary_compressed.data(),
               binary_decompressed.data(),
               binary_compressed_size,
               binary_input_size);

          if (binary_decompressed_size != binary_input_size)
          {
               return 7; /* Binary decompression size mismatch */
          }

          if (memcmp(binary_decompressed.data(), binary_input.data(), binary_input_size) != 0)
          {
               return 8; /* Binary data mismatch */
          }

          /* Test 5: Test with small data */
          std::string small_input = "test";
          const int small_input_size = static_cast<int>(small_input.size());
          const int small_max_compressed_size = LZ4_compressBound(small_input_size);

          std::vector<char> small_compressed(small_max_compressed_size);
          std::vector<char> small_decompressed(small_input_size);

          int small_compressed_size = LZ4_compress_default(
               small_input.data(),
               small_compressed.data(),
               small_input_size,
               small_max_compressed_size);

          if (small_compressed_size <= 0)
          {
               return 9; /* Small data compression failed */
          }

          int small_decompressed_size = LZ4_decompress_safe(
               small_compressed.data(),
               small_decompressed.data(),
               small_compressed_size,
               small_input_size);

          if (small_decompressed_size != small_input_size ||
              memcmp(small_decompressed.data(), small_input.data(), small_input_size) != 0)
          {
               return 10; /* Small data mismatch */
          }

          return 0; /* Success */

#else
          /* LZ4 library not available */
          return 99;
#endif
     }
     catch (...)
     {
          return 99;
     }
}
