#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <mutex>
#include <poll.h>
#include <sys/epoll.h>  
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

bool SocketEngine::HasPendingWork()
{
    /*
     * Check multiple sources of pending work:
     * 1. ActionList - queued actions waiting to be processed
     * 2. Pending writes - sockets with buffered data to send
     * 3. Pending messages - messages waiting to be delivered
     * 
     * If any of these have work pending, we should NOT block in poll
     * because we need to continue processing CPU-bound work.
     */
    
    /* Check if ActionList has pending actions*/

    if (ActionList::GetActionCount() > 0)
    {
        return true;
    }
    
    /* Check if we have pending writes (uses atomic counter for thread-safety)*/

    if (SocketEngine::pending_writes_count_.load(std::memory_order_relaxed) > 0)
    {
        return true;
    }
    
    /* Check if we have pending messages*/

    if (SocketEngine::pending_message_count_.load(std::memory_order_relaxed) > 0)
    {
        return true;
    }
    
    return false;
}
