/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/

#include <cstddef>

/*
 * Memory Mapping Tests
 */

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int test_memory_mapping()
{
     try
     {
          const char* test_file = "/tmp/hlquery_mmap_test";
          const char* test_content = "hlquery memory mapping test data";
          const size_t content_size = strlen(test_content);

          /* Test 1: Create and write test file */

          int fd = open(test_file, O_CREAT | O_RDWR | O_TRUNC, 0644);

          if (fd < 0)
          {
               return 1;
          }

          if (write(fd, test_content, content_size) !=
              static_cast<ssize_t>(content_size))
          {
               close(fd);
               unlink(test_file);
               return 2;
          }

          /* Test 2: Get file size */

          struct stat file_stat;

          if (fstat(fd, &file_stat) != 0)
          {
               close(fd);
               unlink(test_file);
               return 3;
          }

          if (static_cast<size_t>(file_stat.st_size) != content_size)
          {
               close(fd);
               unlink(test_file);
               return 4;
          }

          /* Test 3: Memory map the file for reading */

          void* mapped_read =
               mmap(NULL, content_size, PROT_READ, MAP_PRIVATE, fd, 0);

          if (mapped_read == MAP_FAILED)
          {
               close(fd);
               unlink(test_file);
               return 5;
          }

          /* Test 4: Verify mapped content */

          if (memcmp(mapped_read, test_content, content_size) != 0)
          {
               munmap(mapped_read, content_size);
               close(fd);
               unlink(test_file);
               return 6;
          }

          /* Test 5: Unmap read-only mapping */

          if (munmap(mapped_read, content_size) != 0)
          {
               close(fd);
               unlink(test_file);
               return 7;
          }

          /* Test 6: Memory map for writing */

          void* mapped_write =
               mmap(NULL, content_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);

          if (mapped_write == MAP_FAILED)
          {
               close(fd);
               unlink(test_file);
               return 8;
          }

          /* Test 7: Modify mapped content */

          char* mapped_char = static_cast<char*>(mapped_write);
          const char* new_content = "Modified content for mmap test!";
          strncpy(mapped_char, new_content, content_size);

          /* Test 8: Sync changes to disk */

          if (msync(mapped_write, content_size, MS_SYNC) != 0)
          {
               munmap(mapped_write, content_size);
               close(fd);
               unlink(test_file);
               return 9;
          }

          /* Test 9: Unmap write mapping */

          if (munmap(mapped_write, content_size) != 0)
          {
               close(fd);
               unlink(test_file);
               return 10;
          }

          /* Test 10: Verify changes were written to file */

          lseek(fd, 0, SEEK_SET);
          char read_buffer[64];
          ssize_t bytes_read =
               read(fd, read_buffer, sizeof(read_buffer) - 1);

          if (bytes_read < 0)
          {
               close(fd);
               unlink(test_file);
               return 11;
          }

          read_buffer[bytes_read] = '\0';

          if (strncmp(read_buffer, new_content, strlen(new_content)) != 0)
          {
               close(fd);
               unlink(test_file);
               return 12;
          }

          /* Test 11: Anonymous memory mapping */

          void* anon_map =
               mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

          if (anon_map == MAP_FAILED)
          {
               close(fd);
               unlink(test_file);
               return 13;
          }

          /* Test 12: Write to anonymous mapping */

          strcpy(static_cast<char*>(anon_map), "Anonymous mapping test");

          /* Test 13: Unmap anonymous mapping */

          if (munmap(anon_map, 4096) != 0)
          {
               close(fd);
               unlink(test_file);
               return 14;
          }

          /* Cleanup */

          close(fd);
          unlink(test_file);

          return 0; /* Success */
     }
     catch (...)
     {
          return 99;
     }
}
