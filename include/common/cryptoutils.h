/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/config.h"

std::vector<uint8_t> SHA256(const void *data, size_t len);

std::vector<uint8_t> HMACSHA256(const void *key, size_t keylen, const void *data, size_t len);

std::vector<uint8_t> RandomBytes(size_t len);

std::string Hex(const std::vector<uint8_t> &bytes);

std::string MD5(const std::string &input);

/* AES256-CBC encryption/decryption */

std::string AES256Encrypt(const std::string &plaintext, const std::string &key);

std::string AES256Decrypt(const std::string &ciphertext, const std::string &key);

/* Fast xorshift RNG for internal sampling (non-crypto) */

class FastRNG
{
     uint64_t S;

   public:
     explicit FastRNG(uint64_t seed = 88172645463325252ull) : S(seed)
     {
     }

     inline uint64_t Next()
     {
          uint64_t x = S;
          x ^= x << 7;
          x ^= x >> 9;
          x ^= x << 8;
          S = x;
          return x;
     }

     inline uint32_t Next32()
     {
          return static_cast<uint32_t>(Next());
     }

     inline uint64_t Uniform(uint64_t n)
     {
          return n ? (Next() % n) : 0;
     }
};
