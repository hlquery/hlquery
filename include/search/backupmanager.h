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

#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class BackupManager
{
   private:

     /* Mutex guards backup operations. */

     mutable std::mutex mutex;

     /* BackupDir stores the default backup directory. */

     std::string backup_dir;

   public:

     /* Constructor. */

     BackupManager();

     /* Destructor. */

     ~BackupManager();

     /* CreateBackup creates a backup for the database. */

     bool CreateBackup(int db_id, const std::string& backup_path);

     /* RestoreBackup restores a database backup. */

     bool RestoreBackup(int db_id, const std::string& backup_path);

     /* ListBackups lists available backups. */

     std::vector<std::string> ListBackups() const;

     /* ValidateBackupFile validates a backup file. */

     bool ValidateBackupFile(const std::string& backup_path) const;

     /* DeleteBackup removes a backup file. */

     bool DeleteBackup(const std::string& backup_path);

     /* GetBackupInfo returns metadata for a backup file. */

     std::unordered_map<std::string, std::string> GetBackupInfo(const std::string& backup_path) const;

     /* GenerateBackupPath builds a backup path for a database. */

     std::string GenerateBackupPath(int db_id) const;
};
