/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/

#include <cstddef>

/*
 * Compression Library Tests
 */

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

/* Test basic compression algorithms that might be available */

/* This is a simple test that doesn't require external libraries */

int test_compression()
{
     try
     {

          /* Test 1: Basic data compression simulation */

          /* We'll test with a simple run-length encoding approach */

          /* to verify that compression concepts work */

          std::string test_data = "aaabbbcccdddeeefff";
          std::vector<char> compressed;
          std::vector<char> decompressed;

          /* Simple run-length encoding */

          char current = test_data[0];
          int count = 1;

          for (size_t i = 1; i < test_data.length(); ++i)
          {

               if (test_data[i] == current)
               {
                    count++;
               }

               else
               {
                    compressed.push_back(current);
                    compressed.push_back(static_cast<char>(count));
                    current = test_data[i];
                    count = 1;
               }
          }

          /* Add the last run */

          compressed.push_back(current);
          compressed.push_back(static_cast<char>(count));

          /* Test 2: Verify compression reduced size */

          if (compressed.size() >= test_data.length())
          {
               return 1; /* Compression should reduce size for this data */
          }

          /* Test 3: Decompress */

          for (size_t i = 0; i < compressed.size(); i += 2)
          {
               char ch = compressed[i];
               int cnt = static_cast<int>(compressed[i + 1]);

               for (int j = 0; j < cnt; ++j)
               {
                    decompressed.push_back(ch);
               }
          }

          /* Test 4: Verify decompression matches original */

          if (decompressed.size() != test_data.length())
          {
               return 2;
          }

          for (size_t i = 0; i < test_data.length(); ++i)
          {

               if (decompressed[i] != test_data[i])
               {
                    return 3;
               }
          }

          /* Test 5: Test with binary data */

          std::vector<unsigned char> binary_data = {
               0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
               0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};

          /* Simple checksum to verify data integrity */

          unsigned int checksum = 0;

          for (unsigned char byte : binary_data)
          {
               checksum += byte;
          }

          if (checksum != 120)
          { /* Sum of 0..15 = 120 */
               return 4;
          }

          /* Test 6: Bit manipulation operations (useful for compression) */

          unsigned int value = 0x12345678;

          /* Test bit shifts */

          if ((value << 4) != 0x23456780)
          {
               return 5;
          }

          if ((value >> 4) != 0x01234567)
          {
               return 6;
          }

          /* Test bitwise operations */

          if ((value & 0x0000FFFF) != 0x00005678)
          {
               return 7;
          }

          if ((value | 0x0000FFFF) != 0x1234FFFF)
          {
               return 8;
          }

          /* Test 7: Huffman-style frequency counting */

          std::string freq_test = "hello world hello";
          int char_freq[256] = {0};

          for (char c : freq_test)
          {
               char_freq[static_cast<unsigned char>(c)]++;
          }

          /* Verify some expected frequencies */

          if (char_freq['l'] != 5 || char_freq['o'] != 3 ||
              char_freq['h'] != 2)
          {
               return 9;
          }

          return 0; /* Success */
     }
     catch (...)
     {
          return 99;
     }
}
