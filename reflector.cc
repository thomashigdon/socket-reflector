#include <err.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

typedef struct
{
  struct pollfd* pfd;
  timeval last_data;
  int enabled;
} connection_t;

static inline uint64_t get_cycles(void)
{
  uint64_t t;
  __asm__ __volatile__ ("rdtsc" : "=A"(t));
  return t;
}

double compute_average(uint64_t* buf, int length)
{
  double avg_accum = 0;

  for (int i = 0; i < length; i++)
  {
    avg_accum += (double)buf[i] / (double)length;
  }

  return avg_accum;
}


void client(char* host, int port, double interval, int num_connections)
{
  printf("Started client: host: %s, port %d, interval %f " \
         "num_connections: %d\n",
         host, port, interval, num_connections);

  static const int AVERAGE_LEN = 1000;

  uint64_t time_buffer[AVERAGE_LEN];

  struct pollfd pf[num_connections];

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(sockaddr_in));

  struct hostent* ptrh = gethostbyname(host);

  if (ptrh == NULL)
  {
    fprintf( stderr, "gethostbyname(): %s\n", hstrerror(h_errno) );
    exit(h_errno);
  }

  int sock[num_connections];

  for (int i = 0; i < num_connections; i++)
  {
    sock[i] = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock[i] < 0)
    {
      perror("socket");
      fprintf(stderr, "Failed to create socket\n");
      exit(1);
    }
  }

  memcpy(&addr.sin_addr, ptrh->h_addr, ptrh->h_length);

  addr.sin_family = PF_INET;
  addr.sin_port = htons(port);

  int i;
  for (i = 0; i < num_connections; i++)
  {
    //printf("connecting socket %d\n", sock[i]);
    if (connect(sock[i], (struct sockaddr *) &addr, sizeof(addr)) < 0)
    {
      perror("connect");
      fprintf(stderr, "Failed to connect()\n");
      exit(1);
    }


    pf[i].fd = sock[i];
    pf[i].events = POLLOUT;
  }

  printf("connected %d sockets\n", i);

  char send_buf[4] = { 0xde, 0xea, 0xbe, 0xef};

  uint64_t min = 0xffffffffffffffff;
  uint64_t max = 0;

  timeval tod;
  gettimeofday(&tod, 0);
  int cur_sec = tod.tv_sec;

  int cur_buffer = 0;

  while(1)
  {
    int ret = poll(pf, num_connections, 0);
    for (int i = 0; i < num_connections; i++)
    {
      if ((pf[i].revents & POLLOUT))
      {
        uint64_t before_time = get_cycles();
        int size = send(pf[i].fd, &before_time, sizeof(before_time), 0);
        //printf("sock %d sent %d bytes\n", *sock, size);
        uint64_t recvd_time;
        size = recv(pf[i].fd, &recvd_time, sizeof(recvd_time), 0);
        if (size > 0)
        {
          uint64_t cur_time = get_cycles();
          uint64_t diff_time = cur_time - recvd_time;
          if (diff_time > (1llu << 63))
          {
            diff_time = recvd_time + ((0 - 1)  - cur_time);
          }
          if (diff_time < min)
          {
            min = diff_time;
          }
          if (diff_time > max)
          {
            max = diff_time;
          }

          time_buffer[cur_buffer++ % AVERAGE_LEN] = diff_time;

        }
      }
      gettimeofday(&tod, 0);
      if (tod.tv_sec > cur_sec)
      {
        int length = cur_buffer < AVERAGE_LEN ? cur_buffer : AVERAGE_LEN;
        cur_sec = tod.tv_sec;
        printf("max: %llu min: %llu avg: %f\n",
               (unsigned long long)max,
               (unsigned long long)min,
               compute_average(time_buffer, length));
      }
    }

    usleep((int)(interval * (double)1e6));

  }

}

int accept_new_connections(int listening_socket,
                            connection_t* connections,
                            struct pollfd* pfd_array,
                            int nfds)
{
  struct timeval timeout;
  int connection;

  int i = nfds;

  struct pollfd listen_pf;
  listen_pf.fd = listening_socket;
  listen_pf.events = POLLIN;

  while (poll(&listen_pf, 1, 100))
  {
    //printf("accepting connection..\n");
    connection = accept(listening_socket, NULL, NULL);
    //printf("connection %d accepted\n", i);

    struct timeval tv;
    gettimeofday(&tv, 0);

    struct pollfd pf;
    pfd_array[i].fd = connection;
    pfd_array[i].events = POLLIN;
    connections[i].pfd = &pfd_array[i];
    connections[i].last_data = tv;
    connections[i].enabled = 1;
    i++;
  }
  return i;
}

void server(int port, int timeout_secs)
{
  printf("Started server: port %d, timeout: %d\n", port, timeout_secs);

  int nfds = 0;

  struct sockaddr_in server_address;

  struct timeval tv;

  connection_t connections[65536];
  struct pollfd pfd_array[65536];

  int listening_socket = socket(AF_INET, SOCK_STREAM, 0);

  int reuse_addr;
  setsockopt(listening_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_addr,
             sizeof(reuse_addr));
  memset((char *)  &server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = htonl(INADDR_ANY);
  server_address.sin_port = htons(port);

  if (bind(listening_socket, (struct sockaddr *) &server_address,
             sizeof(server_address)))
  {
    close(listening_socket);
    exit(1);
  }

  struct rlimit r;
  r.rlim_cur = 65536;
  r.rlim_max = 65536;
  if (setrlimit(RLIMIT_NOFILE, &r))
  {
    err(1, NULL);
  }

  listen(listening_socket, 5);

  char recv_buf[1024];

  gettimeofday(&tv, 0);

  int new_nfds = 0;

  nfds = new_nfds;

  int data_received = 0;

  while(1)
  {
    if (!data_received)
    {
      nfds = accept_new_connections(listening_socket,
          connections,
          pfd_array,
          nfds);
    }

    int ret = poll(pfd_array, nfds, 0);
    for (int i = 0; i < nfds; i++)
    {
      if (connections[i].enabled)
      {
        timeval my_time;
        gettimeofday(&my_time, 0);
        if (my_time.tv_sec > connections[i].last_data.tv_sec + timeout_secs)
        {
          printf("timeout: closing socket %d\n",connections[i].pfd->fd);
          close(connections[i].pfd->fd);
          connections[i].enabled = 0;
        }
        int size = -1;
        if ((connections[i].pfd->revents & POLLIN))
        {
          size = recv(connections[i].pfd->fd, recv_buf, 8, 0);
          //printf("sock %d received %d bytes\n", connections[i].pfd->fd, size);
        }
        if (size > 0)
        {
          connections[i].last_data = my_time;
          data_received = 1;
          size = send(connections[i].pfd->fd, recv_buf, size, 0);
        }
        else if (size == 0)
        {
          close(connections[i].pfd->fd);
          connections[i].enabled = 0;
          printf("socket %d closed by client\n",connections[i].pfd->fd);
        }
      }
    }
  }
}

void usage()
{
  printf("Usage: reflector (-c|-s) -h <host> -p <port> -n <num_connections> -t <timeout> -i <interval>\n");
}

int main(int argc, char *argv[])
{
  char* interval_value = 0;
  char* num_connections_value = 0;
  char* host_value = 0;
  char* port_value = 0;
  char* timeout_value = 0;

  int client_flag = 0;
  int server_flag = 0;

  int c;

  while ((c = getopt(argc, argv, "i:n:h:p:t:cs")) != -1)
  {
    switch (c)
    {
      case 'i':
        interval_value = optarg;
        break;
      case 'n':
        num_connections_value = optarg;
        break;
      case 'h':
        host_value = optarg;
        break;
      case 'p':
        port_value = optarg;
        break;
      case 't':
        timeout_value = optarg;
        break;
      case 'c':
        client_flag = 1;
        break;
      case 's':
        server_flag = 1;
        break;
      default:
        fprintf (stderr, "Unknown option '-%c'.\n", optopt);
        return 1;
    }
  }

  if (server_flag && client_flag)
  {
    usage();
    return 1;
  }
  else if (server_flag)
  {
    if (port_value && timeout_value)
    {
      int port = atoi(port_value);
      int timeout_secs = atoi(timeout_value);
      server(port, timeout_secs);
    }
    else
    {
      usage();
      return 1;
    }
  }
  else if (client_flag)
  {
    if (interval_value && num_connections_value && host_value && port_value)
    {
      double interval = atof(interval_value);
      int num_connections = atoi(num_connections_value);
      int port = atoi(port_value);
      client(host_value, port, interval, num_connections);
    }
    else
    {
      usage();
      return 1;
    }
  }
  else
  {
    usage();
    return 1;
  }
  return 0;
}
