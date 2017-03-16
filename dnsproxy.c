#include"dnsproxy.h"
#include"config_parser.h"
#include"blist.h"
//TODO if searching domain is in the blacklist send response with ip taken from config


#define PACKAGE_SIZE 8192
#define CACHE_CLEAN_TIME (MIN_TTL / 2 + 1)

typedef struct {
  SOCKET sock;
  char buffer[PACKAGE_SIZE + sizeof(unsigned short)];
} LOCAL_DNS;

typedef struct {
  int tcp;
  SOCKET sock;
  struct sockaddr_in addr;
  unsigned int head;
  unsigned int rear;
  unsigned int capacity;
  char buffer[PACKAGE_SIZE * 3];
} REMOTE_DNS;

typedef struct {
  LOCAL_DNS local;
  REMOTE_DNS remote;
  config cfg;
} PROXY_ENGINE;

static const int enable = 1;
static int disable_cache = 0;

static void process_query(PROXY_ENGINE *engine)
{
  blist bl=engine->cfg->blacklist;
  LOCAL_DNS *ldns;
  REMOTE_DNS *rdns;
  DNS_HDR *hdr, *rhdr;
  DNS_QDS *qds;
  DNS_RRS *rrs;
  DOMAIN_CACHE *dcache;
  TRANSPORT_CACHE *tcache;
  socklen_t addrlen;
  struct sockaddr_in source;
  char *pos, *head, *rear;
  char *buffer, domain[PACKAGE_SIZE], rbuffer[PACKAGE_SIZE];
  int size, dlen;
  time_t current;
  unsigned char qlen;
  unsigned int ttl, ttl_tmp;
  unsigned short index, q_len;

  ldns = &engine->local;
  rdns = &engine->remote;
  buffer = ldns->buffer + sizeof(unsigned short);

  addrlen = sizeof(struct sockaddr_in);
  size = recvfrom(ldns->sock, buffer, PACKAGE_SIZE, 0, (struct sockaddr*)&source, &addrlen);
  if(size <= (int)sizeof(DNS_HDR))
    return;

  hdr = (DNS_HDR*)buffer;
  rhdr = (DNS_HDR*)rbuffer;
  memset(rbuffer, 0, sizeof(DNS_HDR));

  rhdr->id = hdr->id;
  rhdr->qr = 1;
  q_len = 0;
  qds = NULL;
  head = buffer + sizeof(DNS_HDR);
  rear = buffer + size;
  if(hdr->qr != 0 || hdr->tc != 0 || ntohs(hdr->qd_count) != 1)
    rhdr->rcode = 1;
  else {
      dlen = 0;
      pos = head;
      while(pos < rear) {
          qlen = (unsigned char)*pos++;
          if(qlen > 63 || (pos + qlen) > (rear - sizeof(DNS_QDS))) {
              rhdr->rcode = 1;
              break;
            }
          if(qlen > 0) {
              if(dlen > 0)
                domain[dlen++] = '.';
              while(qlen-- > 0)
                domain[dlen++] = (char)tolower(*pos++);
            }
          else {
              qds = (DNS_QDS*) pos;
              if(ntohs(qds->classes) != 0x01)
                rhdr->rcode = 4;
              else {
                  pos += sizeof(DNS_QDS);
                  q_len = pos - head;
                }
              break;
            }
        }
      domain[dlen] = '\0';
    }
  if(!(blist_check(bl,domain)))
    {
      if(rhdr->rcode == 0 && ntohs(qds->type) == 0x01) {
          dcache = domain_cache_search(domain);
          if(dcache) {
              rhdr->qd_count = htons(1);
              rhdr->an_count = htons(dcache->an_count);
              pos = rbuffer + sizeof(DNS_HDR);
              memcpy(pos, head, q_len);
              pos += q_len;
              memcpy(pos, dcache->answer, dcache->an_length);
              rear = pos + dcache->an_length;
              if(dcache->expire > 0) {
                  if(time(&current) <= dcache->timestamp)
                    ttl = 1;
                  else
                    ttl = (unsigned int)(current - dcache->timestamp);
                  index = 0;
                  while(pos < rear && index++ < dcache->an_count) {
                      rrs = NULL;
                      if((unsigned char)*pos == 0xc0) {
                          pos += 2;
                          rrs = (DNS_RRS*) pos;
                        }
                      else {
                          while(pos < rear) {
                              qlen = (unsigned char)*pos++;
                              if(qlen > 0)
                                pos += qlen;
                              else {
                                  rrs = (DNS_RRS*) pos;
                                  break;
                                }
                            }
                        }
                      ttl_tmp = ntohl(rrs->ttl);
                      if(ttl_tmp <= ttl)
                        ttl_tmp = 1;
                      else
                        ttl_tmp -= ttl;
                      rrs->ttl = htonl(ttl_tmp);
                      pos += sizeof(DNS_RRS) + ntohs(rrs->rd_length);
                    }
                }
              sendto(ldns->sock, rbuffer, rear - rbuffer, 0, (struct sockaddr*)&source, sizeof(struct sockaddr_in));
              return;
            }
        }

      if(rhdr->rcode == 0) {
          tcache = transport_cache_insert(ntohs(hdr->id), &source, ldns);
          if(tcache == NULL)
            rhdr->rcode = 2;
          else {
              hdr->id = htons(tcache->new_id);
              if(!rdns->tcp) {
                  if(sendto(rdns->sock, buffer, size, 0, (struct sockaddr*)&rdns->addr, sizeof(struct sockaddr_in)) != size)
                    rhdr->rcode = 2;
                }
              else {
                  if(rdns->sock == INVALID_SOCKET) {
                      rdns->head = 0;
                      rdns->rear = 0;
                      rdns->sock = socket(AF_INET, SOCK_STREAM, 0);
                      if(rdns->sock != INVALID_SOCKET) {
                          setsockopt(rdns->sock, IPPROTO_TCP, TCP_NODELAY, (void*)&enable, sizeof(enable));
                          if(connect(rdns->sock, (struct sockaddr*)&rdns->addr, sizeof(struct sockaddr_in)) != 0) {
                              closesocket(rdns->sock);
                              rdns->sock = INVALID_SOCKET;
                            }
                        }
                    }
                  if(rdns->sock == INVALID_SOCKET)
                    rhdr->rcode = 2;
                  else{
                      pos = ldns->buffer;
                      *(unsigned short*)pos = htons((unsigned short)size);
                      size += sizeof(unsigned short);
                      if(send(rdns->sock, ldns->buffer, size, 0) != size) {
                          rdns->head = 0;
                          rdns->rear = 0;
                          closesocket(rdns->sock);
                          rdns->sock = INVALID_SOCKET;
                          rhdr->rcode = 2;
                        }
                    }
                }
              if(rhdr->rcode != 0)
                transport_cache_delete(tcache);
            }
        }
      if(rhdr->rcode != 0)
        sendto(ldns->sock, rbuffer, sizeof(DNS_HDR), 0, (struct sockaddr*)&source, sizeof(struct sockaddr_in));
    }
  else
    //set rcode!!! i.e.2(SERVFAIL) or 5 (refused)
    {

      rhdr->rcode = 5;
    sendto(ldns->sock, rbuffer, sizeof(DNS_HDR), 0, (struct sockaddr*)&source, sizeof(struct sockaddr_in));

    }
}

static void process_response(char* buffer, int size)
{
  DNS_HDR *hdr;
  DNS_QDS *qds;
  DNS_RRS *rrs;
  LOCAL_DNS *ldns;
  TRANSPORT_CACHE *cache;
  char domain[PACKAGE_SIZE];
  char *pos, *rear, *answer;
  int badfmt, dlen, length;
  unsigned char qlen;
  unsigned int ttl, ttl_tmp;
  unsigned short index, an_count;

  length = size;
  hdr = (DNS_HDR*)buffer;
  an_count = ntohs(hdr->an_count);
  if(hdr->qr == 1 && hdr->tc == 0 && ntohs(hdr->qd_count) == 1 && an_count > 0) {
      dlen = 0;
      qds = NULL;
      pos = buffer + sizeof(DNS_HDR);
      rear = buffer + size;
      while(pos < rear) {
          qlen = (unsigned char)*pos++;
          if(qlen > 63 || (pos + qlen) > (rear - sizeof(DNS_QDS)))
            break;
          if(qlen > 0) {
              if(dlen > 0)
                domain[dlen++] = '.';
              while(qlen-- > 0)
                domain[dlen++] = (char)tolower(*pos++);
            }
          else {
              qds = (DNS_QDS*) pos;
              if(ntohs(qds->classes) != 0x01)
                qds = NULL;
              else
                pos += sizeof(DNS_QDS);
              break;
            }
        }
      domain[dlen] = '\0';

      if(qds && ntohs(qds->type) == 0x01) {
          ttl = MAX_TTL;
          index = 0;
          badfmt = 0;
          answer = pos;
          while(badfmt == 0 && pos < rear && index++ < an_count) {
              rrs = NULL;
              if((unsigned char)*pos == 0xc0) {
                  pos += 2;
                  rrs = (DNS_RRS*) pos;
                }
              else {
                  while(pos < rear) {
                      qlen = (unsigned char)*pos++;
                      if(qlen > 63 || (pos + qlen) > (rear - sizeof(DNS_RRS)))
                        break;
                      if(qlen > 0)
                        pos += qlen;
                      else {
                          rrs = (DNS_RRS*) pos;
                          break;
                        }
                    }
                }
              if(rrs == NULL || ntohs(rrs->classes) != 0x01)
                badfmt = 1;
              else {
                  ttl_tmp = ntohl(rrs->ttl);
                  if(ttl_tmp < ttl)
                    ttl = ttl_tmp;
                  pos += sizeof(DNS_RRS) + ntohs(rrs->rd_length);
                }
            }
          if(badfmt == 0) {
              hdr->nr_count = 0;
              hdr->ns_count = 0;
              length = pos - buffer;
              if(!disable_cache)
                domain_cache_append(domain, dlen, ttl, an_count, pos - answer, answer);
            }
        }
    }

  cache = transport_cache_search(ntohs(hdr->id));
  if(cache) {
      ldns = (LOCAL_DNS*)cache->context;
      hdr->id = htons(cache->old_id);
      sendto(ldns->sock, buffer, length, 0, (struct sockaddr*)&cache->source, sizeof(struct sockaddr_in));
      transport_cache_delete(cache);
    }
}

static void process_response_udp(REMOTE_DNS *rdns)
{
  int size;
  socklen_t addrlen;
  struct sockaddr_in source;

  addrlen = sizeof(struct sockaddr_in);
  size = recvfrom(rdns->sock, rdns->buffer, PACKAGE_SIZE, 0, (struct sockaddr*)&source, &addrlen);
  if(size < (int)sizeof(DNS_HDR))
    return;

  if(source.sin_addr.s_addr != rdns->addr.sin_addr.s_addr)
    return;

  process_response(rdns->buffer, size);
}

static void process_response_tcp(REMOTE_DNS *rdns)
{
  char *pos;
  int to_down, size;
  unsigned int len, buflen;

  to_down = 0;
  size = recv(rdns->sock, rdns->buffer + rdns->rear, rdns->capacity - rdns->rear, 0);
  if(size < 1)
    to_down = 1;
  else {
      rdns->rear += size;
      while((buflen = rdns->rear - rdns->head) > sizeof(unsigned short)) {
          pos = rdns->buffer + rdns->head;
          len = ntohs(*(unsigned short*)pos);
          if(len > PACKAGE_SIZE) {
              to_down = 1;
              break;
            }
          if(len + sizeof(unsigned short) > buflen)
            break;
          process_response(pos + sizeof(unsigned short), len);
          rdns->head += len + sizeof(unsigned short);
        }
      if(!to_down) {
          if(rdns->head == rdns->rear) {
              rdns->head = 0;
              rdns->rear = 0;
            }
          else if(rdns->head > PACKAGE_SIZE) {
              len = rdns->rear - rdns->head;
              memmove(rdns->buffer, rdns->buffer + rdns->head, len);
              rdns->head = 0;
              rdns->rear = len;
            }
        }
    }

  if(to_down){
      rdns->head = 0;
      rdns->rear = 0;
      closesocket(rdns->sock);
      rdns->sock = INVALID_SOCKET;
    }
}

static PROXY_ENGINE g_engine;


static int dnsproxy(unsigned short local_port, const char* remote_addr, unsigned short remote_port, int remote_tcp, config cfg)
{
  int maxfd, fds;
  fd_set readfds;
  struct timeval timeout;
  struct sockaddr_in addr;
  time_t current, last_clean;

  PROXY_ENGINE *engine = &g_engine;
  g_engine.cfg=cfg;
  LOCAL_DNS *ldns = &engine->local;
  REMOTE_DNS *rdns = &engine->remote;

  ldns->sock = socket(AF_INET, SOCK_DGRAM, 0);
  if(ldns->sock == INVALID_SOCKET) {
      perror("create socket");
      return -1;
    }
  setsockopt(ldns->sock, SOL_SOCKET, SO_REUSEADDR, (char*)&enable, sizeof(enable));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(local_port);
  if(bind(ldns->sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
      perror("bind service port");
      return -1;
    }

  rdns->tcp = remote_tcp;
  rdns->sock = INVALID_SOCKET;
  rdns->addr.sin_family = AF_INET;
  rdns->addr.sin_addr.s_addr = inet_addr(remote_addr);
  rdns->addr.sin_port = htons(remote_port);
  rdns->head = 0;
  rdns->rear = 0;
  rdns->capacity = sizeof(rdns->buffer);
  if(!rdns->tcp) {
      rdns->sock = socket(AF_INET, SOCK_DGRAM, 0);
      if(rdns->sock == INVALID_SOCKET) {
          perror("create socket");
          return -1;
        }

    }

  last_clean = time(&current);
  while(1) {
      FD_ZERO(&readfds);
      FD_SET(ldns->sock, &readfds);
      maxfd = (int)ldns->sock;
      if(rdns->sock != INVALID_SOCKET) {
          FD_SET(rdns->sock, &readfds);
          if(maxfd < (int)rdns->sock)
            maxfd = (int)rdns->sock;
        }
      timeout.tv_sec = CACHE_CLEAN_TIME;
      timeout.tv_usec = 0;
      fds = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
      if(fds > 0) {
          if(rdns->sock != INVALID_SOCKET
             && FD_ISSET(rdns->sock, &readfds)) {
              if(rdns->tcp)
                process_response_tcp(rdns);
              else
                process_response_udp(rdns);
            }
          if(FD_ISSET(ldns->sock, &readfds))
            process_query(engine);
        }
      if(time(&current) - last_clean > CACHE_CLEAN_TIME || fds == 0) {
          last_clean = current;
          domain_cache_clean(current);
          transport_cache_clean(current);
        }
    }
  return 0;
}





int main(int argc, char* argv[])
{


  int use_daemon = 1;
  int remote_tcp = 1;
  int transport_timeout = 5;
  const char *hosts_file = NULL;
  char *remote_addr;
  unsigned short local_port = 53, remote_port = 53;

  char *help_message;
  char* config_file;

  config cfg;

  FILE *fp;

  help_message="Usage: %s -c <path to config file> - Start dns proxy with config file.\n"
               "\t\t-h\t\t\t - Print help message and exit.\n";
  char *program_name=argv[0];
  char oc;





  if(argc < 2)
    {
      fprintf(stderr, help_message, program_name);
      exit(EXIT_FAILURE);
    }
  while((oc=getopt(argc, argv, ":hc:")) != -1)
    {
      switch (oc)
        {
        case  'h':
          fprintf(stderr, help_message, program_name);
          exit(EXIT_FAILURE);

        case 'c':
          config_file=optarg;
          break;
        case '?':
          fprintf(stderr,help_message,program_name);
          exit(EXIT_FAILURE);
        case ':':
          fprintf(stderr, "Option -%c requires config filename\n",optopt);
          exit(EXIT_FAILURE);
        default:
          fprintf(stderr,help_message,program_name);
          exit(EXIT_FAILURE);
        }
    }
  if (optind != argc)
    {
      fprintf(stderr,help_message,program_name);
      exit(EXIT_FAILURE);
    }



  if((fp=fopen(config_file,"r")) == NULL)
    {
      perror(config_file);
      exit(1);
    }

  /* parses config and makes blist */

  if((cfg=parse_config(fp)) == NULL)
    return 1;





  if(use_daemon) {
      int fd;
      pid_t pid = fork();
      if(pid < 0) {
          perror("fork");
          return -1;
        }
      if(pid != 0)
        exit(0);
      pid = setsid();
      if(pid < -1) {
          perror("setsid");
          return -1;
        }
      chdir("/");
      fd = open ("/dev/null", O_RDWR, 0);
      if(fd != -1) {
          dup2 (fd, 0);
          dup2 (fd, 1);
          dup2 (fd, 2);
          if(fd > 2)
            close (fd);
        }
      umask(0);
    }
  signal(SIGPIPE, SIG_IGN);




  srand((unsigned int)time(NULL));
  domain_cache_init(hosts_file);
  transport_cache_init(transport_timeout);

  remote_addr=cfg->forward;


  dnsproxy(local_port, remote_addr, remote_port, remote_tcp, cfg);
  remove_config(cfg);

  return 0;

}

