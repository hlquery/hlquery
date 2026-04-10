/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 * Copyright (C) 2021-2026 Carlos F. Ferry <carlos.ferry@gmail.com>
 * Released under the BSD 3-Clause License. See https://docs.hlquery.com/
 */

/*
 * POSIX/System Header Tests
 * Covers system headers used across the codebase not already tested elsewhere
 */

#include <arpa/inet.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <termios.h>
#include <time.h>

int test_posix()
{
     try
     {
          /* <netinet/in.h>, <arpa/inet.h> */

          sockaddr_in addr{};
          addr.sin_family = AF_INET;
          if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1)
          {
               return 1;
          }

          /* <netdb.h> */

          addrinfo hints{};
          hints.ai_family = AF_INET;

          /* <netinet/tcp.h> */

          int tcp_opt = TCP_NODELAY;
          (void)tcp_opt;

          /* <poll.h> */

          pollfd pfd{};
          pfd.fd = -1;
          pfd.events = 0;

          /* <pthread.h> */

          pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
          if (pthread_mutex_lock(&mtx) != 0)
          {
               return 2;
          }
          pthread_mutex_unlock(&mtx);

          /* <sched.h> */

          int prio = sched_get_priority_max(SCHED_FIFO);
          if (prio <= 0)
          {
               return 3;
          }

          /* <sys/epoll.h> */

          epoll_event ev{};
          ev.events = EPOLLIN;

          /* <sys/eventfd.h> */

          eventfd_t ev_val = 0;
          (void)ev_val;

          /* <sys/ioctl.h> */

          int dummy = 0;
          (void)dummy;

          /* <sys/select.h> */

          fd_set set;
          FD_ZERO(&set);

          /* <sys/socket.h> */

          int s = socket(AF_INET, SOCK_STREAM, 0);
          if (s < 0)
          {
               return 4;
          }
          (void)s;

          /* <sys/sysinfo.h> */

          struct sysinfo info;
          if (sysinfo(&info) != 0)
          {
               return 5;
          }

          /* <sys/uio.h> */

          iovec iov{};
          iov.iov_base = nullptr;
          iov.iov_len = 0;

          /* <termios.h> */

          termios tio{};
          (void)tio;

          /* <time.h> */

          timespec ts{};
          ts.tv_sec = 0;
          ts.tv_nsec = 0;

          /* <getopt.h> */

          int (*getopt_ptr)(int, char* const[], const char*) = getopt;
          (void)getopt_ptr;

          return 0;
     }
     catch (...)
     {
          return 6;
     }
}
