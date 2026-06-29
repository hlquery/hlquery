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

#include <memory>
#include <netinet/in.h>
#include <string>
#include <vector>

#include "core/socketengine.h"

/* 
 * Manages one listening socket for incoming client connections.
 * This event handler stores the bind target and reacts to socket
 * readiness notifications from the socket engine.
 */

class ListenManager : public EventHandler
{
   private:

     /* Address used when binding the listening socket */

     std::string BindAddr;

     /* Port used when binding the listening socket */

     int Port;

     /* Native socket address structure built from the bind settings */

     struct sockaddr_in ServerAddr;

   public:

     /* Construct one listen manager with address and port defaults. */

     ListenManager(const std::string& address = "0.0.0.0", int port_num = 9200);

     /* Destroy the listen manager and release listener resources. */

     ~ListenManager();

     /* Build listen managers for non-HTTP bind entries in the server config. */

     static std::vector<std::unique_ptr<ListenManager>> CreateCustomProtocolListeners();

     /* Bind the socket and start listening for incoming connections. */

     bool BindAndListen();

     /* 
      * Handle a readable event on the listening socket.
      * This is triggered when new client connections are ready to accept.
      */

     void OnEventHandlerRead() override;

     /* Handle an error event reported by the socket engine. */

     void OnEventHandlerError(int ErrorNum) override;
};
