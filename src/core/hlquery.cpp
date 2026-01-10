#include <chrono>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/hlquery.h"

hlquery* Instance = nullptr;

hlquery::hlquery(int argc, char** argv)
{
        Instance = this;
        Config = std::make_unique<ServerConfig>(argc, argv);
        ParseArgs();
}


HLManager::AdvancedRocksDBEngine* hlquery::GetDatabase() const {
    return Database.get();
}

