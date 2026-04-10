/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * Filesystem Operations Tests
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int test_filesystem()
{
     try
     {
          namespace fs = std::filesystem;

          const std::string test_dir = "/tmp/hlquery_fs_test";
          const std::string test_file = test_dir + "/test_file.txt";
          const std::string test_content =
               "hlquery filesystem test content";

          /* Test 1: Create directory */

          if (!fs::create_directories(test_dir))
          {

               if (!fs::exists(test_dir))
               {
                    return 1;
               }
          }

          /* Test 2: Check directory exists */

          if (!fs::exists(test_dir) || !fs::is_directory(test_dir))
          {
               return 2;
          }

          /* Test 3: Write file */

          {
               std::ofstream file(test_file);

               if (!file.is_open())
               {
                    return 3;
               }

               file << test_content;
               file.close();
          }

          /* Test 4: Check file exists and is regular file */

          if (!fs::exists(test_file) || !fs::is_regular_file(test_file))
          {
               return 4;
          }

          /* Test 5: Check file size */

          auto file_size = fs::file_size(test_file);

          if (file_size != test_content.length())
          {
               return 5;
          }

          /* Test 6: Read file content */

          {
               std::ifstream file(test_file);

               if (!file.is_open())
               {
                    return 6;
               }

               std::string content;
               std::getline(file, content);
               file.close();

               if (content != test_content)
               {
                    return 7;
               }
          }

          /* Test 7: Directory iteration */

          bool found_test_file = false;

          for (const auto& entry : fs::directory_iterator(test_dir))
          {

               if (entry.path().filename() == "test_file.txt")
               {
                    found_test_file = true;
                    break;
               }
          }

          if (!found_test_file)
          {
               return 8;
          }

          /* Test 8: File permissions */

          auto perms = fs::status(test_file).permissions();

          /* Just check that we can read permissions */

          if (perms == fs::perms::unknown)
          {
               return 9;
          }

          /* Test 9: Get current path */

          auto current_path = fs::current_path();

          if (current_path.empty())
          {
               return 10;
          }

          /* Test 10: Path operations */

          fs::path test_path(test_file);

          if (test_path.filename() != "test_file.txt" ||
              test_path.extension() != ".txt" ||
              test_path.parent_path() != test_dir)
          {
               return 11;
          }

          /* Cleanup */

          fs::remove(test_file);
          fs::remove(test_dir);

          return 0; /* Success */
     }
     catch (const std::filesystem::filesystem_error& e)
     {
          return 90 + static_cast<int>(e.code().value() % 10);
     }
     catch (...)
     {
          return 99;
     }
}
