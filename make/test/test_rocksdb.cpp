/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * RocksDB Vendor Header Test
 * Verifies that the vendored RocksDB headers are present and usable.
 */

#include "vendor/rocksdb/include/rocksdb/db.h"
#include "vendor/rocksdb/include/rocksdb/options.h"

int test_rocksdb()
{
     rocksdb::Options options;
     options.create_if_missing = true;

     return rocksdb::Status::OK().ok() ? 0 : 1;
}
