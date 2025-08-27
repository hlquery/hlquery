#include <iostream>

HLQuery* Instance = nullptr;

HLQuery::HLQuery(int argc, char** argv) 
{
        Instance = this;

}

/* Main */

int main(int argc, char** argv)
{
        new hlquery(argc, argv);

        /*
         * Initialize the server.
         *
         * - This sets up configuration, logging, sockets, 
         *   databases, and any other subsystems required.
         * - If initialization fails, the instance is destroyed 
         *   and the program exits with error code 1.
         */

        if (!Instance->Initialize())
        {

                /*
                 * Fast-fail on initialization errors (e.g., port already in use).
                 * At this point Initialize() has already cleaned up transient state
                 * like the PID file when needed. Use QuickExit to bypass potentially
                 * blocking destructors/cleanup paths that can hang.
                 */

                Shutdown::QuickExit(EXIT_STATUS_CONFIG);
        }

        Instance->Run();
        delete Instance;
}

