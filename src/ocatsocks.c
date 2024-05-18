/* Copyright 2008-2024 Bernhard R. Fischer.
 *
 * This file is part of OnionCat.
 *
 * OnionCat is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * OnionCat is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OnionCat. If not, see <http://www.gnu.org/licenses/>.
 */

/*! \file ocatsocks.c
 *  Contains functions for connecting to TOR via SOCKS.
 *
 *  \author Bernhard Fischer <bf@abenteuerland.at>
 *  \date 2024/05/18
 */

/* SOCKS5 is defined in RFC1928 */

#include "ocat.h"
#include "ocat_netdesc.h"
#include "ocathosts.h"
#include "ocatresolv.h"
#include <netdb.h>


// SOCKS connector queue vars
static SocksQueue_t *socks_queue_ = NULL;

#define SOCKS_MIN_BUFLEN (sizeof(SocksHdr_t) + NDESC(name_size) + strlen(CNF(usrname)) + 2)
#define SOCKS_BUFLEN (SOCKS_MIN_BUFLEN + NI_MAXHOST + 32)


static int get_hostname(const SocksQueue_t *sq, char *onion, int onion_size)
{
   int ret = -1;

   // Do a hostname lookup if option set.
   // This is done in order to be able to retrieve a 256 bit base32 
   // host from e.g. /etc/hosts.
   if (CNF(hosts_lookup))
   {
      hosts_check();
      ret = hosts_get_name(&sq->addr, onion, onion_size);
   }

   // If no hostname was found above or network type is Tor
   // do usual OnionCat name transformation.
   if (ret == -1 && onion != NULL)
   {
      ipv6tonion(&sq->addr, onion);
      strlcat(onion, CNF(domain), onion_size);
   }

   return ret;
}


#define DIRECT_CONNECTIONS
#ifdef DIRECT_CONNECTIONS
static int hostname_addr(const char *name, struct sockaddr *addr, socklen_t *len)
{
   char portname[16];
   struct addrinfo hints, *res;
   int e;

   /* safety check */
   if (name == NULL || addr == NULL)
   {
      log_msg(LOG_EMERG, "name || addr == NULL");
      return -1;
   }

   memset(&hints, 0, sizeof(struct addrinfo));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_flags = 0;
   hints.ai_protocol = IPPROTO_TCP;

   snprintf(portname, sizeof(portname), "%d", CNF(ocat_dest_port));
   if ((e = getaddrinfo(name, portname, &hints, &res)) != 0)
   {
      log_msg(LOG_ERR, "getaddrinfo() failed: %s", gai_strerror(e));
      return -1;
   }

   if (res == NULL)
   {
      log_msg(LOG_ERR, "getaddrinfo() returned empty result structure");
      return -1;
   }

   log_debug("family = %d", res->ai_addr->sa_family);
   memcpy(addr, res->ai_addr, res->ai_addrlen > *len ? *len : res->ai_addrlen);
   *len = res->ai_addrlen;
   freeaddrinfo(res);

   return 0;
}
#endif


int socks_send_request(const SocksQueue_t *sq)
{
   int len, ret = -1;
   char buf[SOCKS_BUFLEN], onion[NI_MAXHOST];
   SocksHdr_t *shdr = (SocksHdr_t*) buf;

   get_hostname(sq, onion, sizeof(onion));

   log_debug("SOCKS_BUFLEN = %d, NI_MAXHOST = %d", (int) SOCKS_BUFLEN, NI_MAXHOST);
   if (inet_ntop(AF_INET6, &sq->addr, buf, sizeof(buf)) == NULL)
   {
      log_msg(LOG_WARNING, "inet_ntop failed: \"%s\"", strerror(errno));
      buf[0] = '\0';
   }
   log_msg(LOG_INFO, "trying to connect to \"%s\" [%s] on %d", onion, buf, sq->fd);

   log_debug("doing SOCKS4a handshake");
   shdr->ver = 4;
   shdr->cmd = 1;
   shdr->port = htons(CNF(ocat_dest_port));
   shdr->addr.s_addr = htonl(0x00000001);
   memcpy(buf + sizeof(SocksHdr_t), CNF(usrname), strlen(CNF(usrname)) + 1);
   memcpy(buf + sizeof(SocksHdr_t) + strlen(CNF(usrname)) + 1, onion, strlen(onion) + 1);
   len = sizeof(SocksHdr_t) + strlen(CNF(usrname)) + strlen(onion) + 2;
   if ((ret = write(sq->fd, shdr, len)) == -1)
   {
      log_msg(LOG_ERR, "error writing %d bytes to fd %d: \"%s\"", len, sq->fd, strerror(errno));
      return -1;
   }
   if (ret < len)
   {
      log_msg(LOG_ERR, "SOCKS request truncated to %d of %d bytes", ret, len);
      return -1;
   }
   log_debug("SOCKS request sent successfully");
   return 0;
}


int socks_rec_response(SocksQueue_t *sq)
{
   SocksHdr_t shdr;
   int ret, len;

   len = sizeof(SocksHdr_t);
   if ((ret = read(sq->fd, &shdr, len)) == -1)
   {
      log_msg(LOG_ERR, "reading SOCKS response on fd %d failed: \"%s\"", sq->fd, strerror(errno));
      return -1;
   }
   if (ret < len)
   {
      log_msg(LOG_ERR, "SOCKS response truncated to %d of %d bytes", ret, len);
      return -1;
   }

   log_debug("SOCKS response received");
   if (shdr.ver || (shdr.cmd != 90))
   {
      log_msg(LOG_ERR, "SOCKS request failed, reason = %d", shdr.cmd);
      return -1;
   }

   log_msg(LOG_INFO | LOG_FCONN, "SOCKS connection successfully opened on fd %d", sq->fd);
   return 0;
}


int socks_activate_peer(SocksQueue_t *sq)
{
   OcatPeer_t *peer;

   insert_peer(sq->fd, sq, time(NULL) - sq->connect_time);

   // Send first keepalive immediately
   lock_peers();
   if ((peer = search_peer(&sq->addr)))
      lock_peer(peer);
   else
      log_msg(LOG_EMERG, "newly inserted peer not found, fd = %d", sq->fd);
   unlock_peers();
   if (peer)
   {
      send_keepalive(peer);
      unlock_peer(peer);
   }

   return 0;
}


/*! This writes a SocksQueue element to the socks connector pipe.
 * @param sq Filled out SocksQueue_t struct.
 */
void socks_pipe_request(const SocksQueue_t *sq)
{
   fd_set wset;
   int maxfd;
   int len = sizeof(*sq), ret;

   for (maxfd = 0; !maxfd;)
   {
      FD_ZERO(&wset);
      FD_SET(CNF(socksfd[1]), &wset);
      if ((maxfd = oc_select(CNF(socksfd[1]) + 1, NULL, &wset, NULL)) == -1)
         return;
   }

   if (FD_ISSET(CNF(socksfd[1]), &wset))
   {
      log_debug("writing %d bytes to fd %d", len, CNF(socksfd[1]));
      if ((ret = write(CNF(socksfd[1]), sq, len)) == -1)
      {
         log_msg(LOG_WARNING, "error writing to SOCKS request pipe fd %d: \"%s\"", CNF(socksfd[1]), strerror(errno));
      }
      else if (ret < len)
      {
         log_msg(LOG_WARNING, "write to SOCKS request pipe fd %d truncated to %d bytes of %d", CNF(socksfd[1]), ret, len);
      }
      else
      {
         log_debug("wrote %d bytes to SOCKS request pipe fd %d", len, CNF(socksfd[1]));
      }
   }
   else
      log_msg(LOG_WARNING, "fd %d not in write set", CNF(socksfd[1]));
}


/*! Send a wakeup request to the connector thread.
 */
void sig_socks_connector(void)
{
   SocksQueue_t sq;

   memset(&sq, 0, sizeof(sq));
   socks_pipe_request(&sq);
}


/*! This is a wrapper function for sig_socks_connector() to be suitable for the
 * DNS query callback.
 */
#ifdef DEBUG
void socks_query_callback(void *UNUSED(p), struct in6_addr in6, int code)
#else
void socks_query_callback(void *UNUSED(p), struct in6_addr UNUSED(in6), int UNUSED(code))
#endif
{
#ifdef DEBUG
   char astr[INET6_ADDRSTRLEN];
   log_debug("query callback received for %s, code = %d", inet_ntop(AF_INET6, &in6, astr, sizeof(astr)), code);
#endif
   sig_socks_connector();
}


/*! Check if address addr exists within SOCKS request queue.
 * @param addr IPv6 Address to check for.
 * @return If the request for the address exists a pointer to the queued
 * element is returned. Otherwise NULL is returned.
 */
SocksQueue_t *socks_get_req(const struct in6_addr *addr)
{
   SocksQueue_t *squeue;

   for (squeue = socks_queue_; squeue; squeue = squeue->next)
      if (IN6_ARE_ADDR_EQUAL(&squeue->addr, addr))
         return squeue;
   return NULL;
}


/*! Add and link a SOCKS request to the SOCKS queue.
 *  @param sq Request structure to add.
 */
void socks_enqueue(const SocksQueue_t *sq)
{
   SocksQueue_t *squeue;

   log_debug("queueing new SOCKS connection request");
   if (socks_get_req(&sq->addr))
   {
      log_debug("SOCKS request exists");
      return;
   }
   if (!(squeue = malloc(sizeof(SocksQueue_t))))
      log_msg(LOG_EMERG, "could not get memory for SocksQueue entry: \"%s\"", strerror(errno)), exit(1);
   memcpy(squeue, sq, sizeof(*squeue));

   squeue->next = socks_queue_;
   socks_queue_ = squeue;
}


/*! Initialize a new SOCKS request and send it to the request pipe in order to
 *  get added to the SOCKS queue with socks_enqueue().
 *  @param addr IPv6 address to be requested
 *  @param perm 1 if connection should kept opened inifitely after successful request, 0 else.
 */
void socks_queue(struct in6_addr addr, int perm)
{
   SocksQueue_t *squeue, sq;

   // dont queue if SOCKS is disabled (-t none)
   if (!CNF(socks_dst)->sin_family)
      return;

   if ((squeue = socks_get_req(&addr)) != NULL)
   {
      log_debug("connection already exists, not queueing SOCKS connection");
      return;
   }

   log_debug("queueing new SOCKS connection request");
   memset(&sq, 0, sizeof(sq));
   IN6_ADDR_COPY(&sq.addr, &addr);
   sq.perm = perm;
   log_debug("signalling connector");
   socks_pipe_request(&sq);
}


/*! Remove SocksQueue_t element from SOCKS queue.
 *  @param sq Pointer to element to remove.
 */
void socks_unqueue(SocksQueue_t *squeue)
{
   SocksQueue_t **sq;

   for (sq = &socks_queue_; *sq; sq = &(*sq)->next)
      if (*sq == squeue)
      {
         *sq = (*sq)->next;
         log_debug("freeing SOCKS queue element at %p", squeue);
         free(squeue);
         break;
      }
}


void print_socks_queue(int fd)
{
   SocksQueue_t sq;

   memset(&sq, 0, sizeof(sq));
   sq.next = (SocksQueue_t*) (intptr_t) fd;
   socks_pipe_request(&sq);
}


void socks_output_queue(int fd)
{
   int i;
   char addrstr[INET6_ADDRSTRLEN], onstr[NDESC(name_size)];
   SocksQueue_t *squeue;

   for (squeue = socks_queue_, i = 0; squeue; squeue = squeue->next, i++)
   {
      if (!inet_ntop(AF_INET6, &squeue->addr, addrstr, INET6_ADDRSTRLEN))
      {
         log_msg(LOG_ERR, "inet_ntop returned NULL pointer: \"%s\"", strerror(errno));
         strlcpy(addrstr, "ERROR", INET6_ADDRSTRLEN);
      }

      dprintf(fd, "%d: %39s, %s%s, state = %d, %s(%d), retry = %d, connect_time = %d, restart_time = %d\n",
            i, 
            addrstr, 
            ipv6tonion(&squeue->addr, onstr),
            CNF(domain),
            squeue->state,
            squeue->perm ? "PERMANENT" : "TEMPORARY",
            squeue->perm,
            squeue->retry,
            (int) squeue->connect_time,
            (int) squeue->restart_time
            );
   }
   i = 0;
   oe_write(fd, &i, 1);
}


int socks5_greet(const SocksQueue_t *sq)
{
   char buf[] = {5, 1, 0}; // version 5, 1 auth method, method no_auth (0)
   int ret, len = sizeof(buf);

   if ((ret = write(sq->fd, buf, len)) == -1)
   {
      log_msg(LOG_ERR, "error writing %d bytes to fd %d: \"%s\"", len, sq->fd, strerror(errno));
      return -1;
   }
   if (ret < len)
   {
      log_msg(LOG_ERR, "SOCKS5 greeting truncated to %d of %d bytes", ret, len);
      return -1;
   }
   log_debug("SOCKS5 greeting sent successfully");
   return 0;
}


int socks5_greet_response(const SocksQueue_t *sq)
{
   char buf[2];
   int ret, len = sizeof(buf);

   if ((ret = read(sq->fd, buf, len)) == -1)
   {
      log_msg(LOG_ERR, "reading SOCKS5 greet response on fd %d faile: \"%s\"", sq->fd, strerror(errno));
      return -1;

   }
   if (ret < len)
   {
      log_msg(LOG_ERR, "SOCKS5 greet response truncated to %d of %d bytes", ret, len);
      return -1;
   }
   log_debug("SOCKS5 greet response received");
   if (buf[0] != 5 || buf[1] != 0)
   {
      log_msg(LOG_ERR, "unexpected SOCKS5 greet response: ver = %d, method = %d", buf[0], buf[1]);
      return -1;
   }
   log_msg(LOG_INFO | LOG_FCONN, "SOCKS5 greeting handshake on fd %d successful", sq->fd);
   return 0;
}


int socks5_send_request(const SocksQueue_t *sq)
{
   char buf[sizeof(Socks5Hdr_t) + sizeof(uint16_t) + NI_MAXHOST];
   char onion[NI_MAXHOST];
   Socks5Hdr_t *s5hdr = (Socks5Hdr_t*) buf;
   int len, ret;

   get_hostname(sq, onion, sizeof(onion));
   s5hdr->ver = 5;
   s5hdr->cmd = 1;   // CONNECT
   s5hdr->rsv = 0;   // reserved
   s5hdr->atyp = 3;  // DOMAIN
   s5hdr->addr = strlen(onion);
   memcpy(buf + sizeof(*s5hdr), onion, strlen(onion));
   *((uint16_t*) &buf[sizeof(*s5hdr) + strlen(onion)]) = htons(CNF(ocat_dest_port));

   len = sizeof(*s5hdr) + strlen(onion) + sizeof(uint16_t);
   if ((ret = write(sq->fd, s5hdr, len)) == -1)
   {
      log_msg(LOG_ERR, "error writing %d bytes to fd %d: \"%s\"", len, sq->fd, strerror(errno));
      return -1;
   }
   if (ret < len)
   {
      log_msg(LOG_ERR, "SOCKS5 request truncated to %d of %d bytes", ret, len);
      return -1;
   }
   log_debug("SOCKS5 request sent successfully");
   return 0;
}


int socks5_rec_response(SocksQueue_t *sq)
{
   char buf[sizeof(Socks5Hdr_t) + sizeof(uint16_t) + NI_MAXHOST];
   Socks5Hdr_t *s5hdr = (Socks5Hdr_t*) buf;
   int len, ret;

   len = sizeof(buf);
   if ((ret = read(sq->fd, s5hdr, len)) == -1)
   {
      log_msg(LOG_ERR, "reading SOCKS5 response on fd %d failed: \"%s\"", sq->fd, strerror(errno));
      return -1;
   }

   log_debug("got %d bytes as SOCKS5 response", ret);
   if (ret < (int) sizeof(*s5hdr))
   {
      log_msg(LOG_ERR, "SOCKS5 response seems truncated to %d of at least %d bytes", ret, (int) sizeof(*s5hdr));
      return -1;
   }

   if (s5hdr->ver != 5 || s5hdr->rsv != 0)
   {
      log_msg(LOG_ERR, "unexpected SOCKS5 response");
      return -1;
   }
   if (s5hdr->cmd != 0)
   {
      log_msg(LOG_ERR, "SOCKS5 server returned error %d", s5hdr->cmd);
      return -1;
   }
   log_msg(LOG_INFO | LOG_FCONN, "SOCKS5 connection successfully opened on fd %d", sq->fd);
   return 0;
}


int socks_tcp_connect(int fd, struct sockaddr *addr, int len)
{
   char astr[INET6_ADDRSTRLEN];
   if (connect(fd, addr, len) == -1)
   {
      if (errno != EINPROGRESS)
      {
         log_msg(LOG_ERR, "connect() to SOCKS port %s:%d failed: \"%s\". Sleeping for %d seconds.", 
            inet_ntop(addr->sa_family, 
               addr->sa_family == AF_INET ? (char*) &((struct sockaddr_in*) addr)->sin_addr : (char*) &((struct sockaddr_in6*) addr)->sin6_addr, astr, sizeof(astr)), 
            ntohs(addr->sa_family == AF_INET ? ((struct sockaddr_in*) addr)->sin_port : ((struct sockaddr_in6*) addr)->sin6_port),
            strerror(errno), TOR_SOCKS_CONN_TIMEOUT);
         return -1;
      }
      log_debug("connection in progress");
   }
   else
   {
      log_debug("connected");
   }

   return 0;
}


void socks_reset(SocksQueue_t *squeue)
{
   log_debug("resetting SOCKS request");
   if (squeue->fd > 0)
   {
      oe_close(squeue->fd);
      squeue->fd = 0;
   }
   squeue->restart_time = 0;
   squeue->state = SOCKS_NEW;
}


void socks_reschedule(SocksQueue_t *squeue)
{
   log_msg(LOG_INFO, "rescheduling SOCKS request");
   socks_reset(squeue);
   squeue->restart_time = time(NULL) + TOR_SOCKS_CONN_TIMEOUT;
}

 
#ifdef WITH_DNS_LOOKUP
/*! Send out a DNS reverse lookup for the addess found in sq.
 * @param sq Pointer to the queue message.
 * @return On success, the function returns a value >= 0. On error, -1 is
 * returned.
 */
int socks_dns_req(SocksQueue_t *sq)
{
   //struct sockaddr_in6 saddr;
   char buf[PACKETSZ];
   socklen_t slen;
   int n, len;

   memset(&sq->ns_addr, 0, sizeof(sq->ns_addr));
   if (hosts_get_ns(&sq->ns_addr.sin6_addr, &sq->ns_src) == -1)
   {
      log_msg(LOG_WARNING, "no DNS server available");
      return -1;
   }

   slen = sizeof(sq->ns_addr);
   sq->ns_addr.sin6_family = AF_INET6;
#ifdef HAVE_SIN_LEN
   sq->ns_addr.sin6_len = slen;
#endif
   sq->ns_addr.sin6_port = htons(CNF(ocat_ns_port));

   len = oc_mk_ptrquery((char*) &sq->addr, buf, sizeof(buf), sq->id);

   if ((n = sendto(sq->fd, buf, len, 0, (struct sockaddr*) &sq->ns_addr, slen)) == -1)
   {
      log_msg(LOG_ERR, "sendto() failed: %s", strerror(errno));
      return -1;
   }

   if (n < len)
      log_msg(LOG_WARNING, "message was truncated: %d < %d", n, len);

   log_msg(LOG_INFO, "DNS request sent to nameserver %s", inet_ntop(AF_INET6, &sq->ns_addr.sin6_addr, buf, sizeof(buf)));
   return n;
}


int socks_dns_recv(SocksQueue_t *sq)
{
   struct sockaddr_in6 saddr;
   char buf[PACKETSZ];
   socklen_t slen;
   int len;

   slen = sizeof(saddr);
   if ((len = recvfrom(sq->fd, buf, sizeof(buf), 0, (struct sockaddr*) &saddr, &slen)) == -1)
   {
      log_msg(LOG_ERR, "failed to receive DNS data on fd %d", sq->fd);
      return -1;
   }

   log_debug("received %d bytes on fd %d, checking identity", len, sq->fd);

   if (saddr.sin6_port != sq->ns_addr.sin6_port || !IN6_ARE_ADDR_EQUAL(&saddr.sin6_addr, &sq->ns_addr.sin6_addr))
   {
      log_msg(LOG_WARNING, "sender socket address does not match");
      return -1;
   }

   return oc_proc_response(buf, sizeof(buf), sq->id, &sq->addr, sq->ns_src);
}
#endif


void *socks_connector_sel(void *UNUSED(p))
{
   fd_set rset, wset;
   int maxfd = 0, len, so_err;
   SocksQueue_t *squeue, sq;
   time_t t;
   socklen_t err_len;
   struct sockaddr_storage ss;
   char name[NI_MAXHOST];

   for (;;)
   {
      update_thread_activity();
      if (term_req())
         return NULL;

      FD_ZERO(&rset);
      FD_ZERO(&wset);
      MFD_SET(CNF(socksfd[0]), &rset, maxfd);
      t = time(NULL);

      for (squeue = socks_queue_; squeue; squeue = squeue->next)
      {
         switch (squeue->state)
         {
            case SOCKS_NEW:
               /*if (!squeue->fd)
               {
                  log_msg(LOG_CRIT, "SOCKS_NEW and fd = %d, but should be 0", squeue->fd);
                  squeue->state = SOCKS_DELETE;
                  continue;
               }*/

               if (t < squeue->restart_time)
               {
                  log_debug("SOCKS request is scheduled for connection not before %lds", squeue->restart_time - t);
                  continue;
               }

               // check and increase retry counter
               squeue->retry++;
               if (!squeue->perm && (squeue->retry > SOCKS_MAX_RETRY))
               {
                  log_msg(LOG_NOTICE, "temporary request failed %d times and will be removed", squeue->retry - 1);
                  squeue->state = SOCKS_DELETE;
                  continue;
               }

#ifdef WITH_DNS_LOOKUP
               // send a DNS lookup if configured and no hostname in DB yet and it is the first try
               if (CNF(dns_lookup) && get_hostname(squeue, NULL, 0) == -1 && squeue->retry <= 1)
               {
                  // create anonymous UDP socket
                  if ((squeue->fd = socket(AF_INET6, SOCK_DGRAM, 0)) != -1)
                  {
                     log_debug("created UDP fd %d for DNS lookup", squeue->fd);
                     set_nonblock(squeue->fd);
                     squeue->id = rand();

                     if (socks_dns_req(squeue) != -1)
                     {
                        log_msg(LOG_INFO, "DNS request sent to fd %d", squeue->fd);
                        squeue->state = SOCKS_DNS_SENT;
                        squeue->retry = 0;
                        squeue->restart_time = t + SOCKS_DNS_RETRY_TIMEOUT;
                        MFD_SET(squeue->fd, &rset, maxfd);
                        continue;
                     }
                     else
                     {
                        log_msg(LOG_ERR, "could not send DNS request");
                        oe_close(squeue->fd);
                     }
                  }
                  else
                     log_msg(LOG_ERR, "could not create UDP socket: %s", strerror(errno));
               }
#endif

#ifdef WITH_DNS_RESOLVER
               // request hostname from resolver if not already available
               if (CNF(dns_lookup) && get_hostname(squeue, NULL, 0) == -1 && squeue->retry <= 1)
               {
                  log_msg(LOG_INFO, "signalling resolver");
                  if (ocres_query_callback(&squeue->addr, socks_query_callback, NULL) > 0)
                  {
                     squeue->state = SOCKS_DNS_SENT;
                     squeue->retry = 0;
                     squeue->restart_time = t + SOCKS_DNS_RETRY_TIMEOUT;
                     continue;
                  }
               }
#endif

#ifdef DIRECT_CONNECTIONS
               if (CNF(socks5) == CONNTYPE_DIRECT)
               {
                  if (get_hostname(squeue, name, sizeof(name)) == -1)
                  {
                     log_msg(LOG_ERR, "no valid destination name found for DIRECT connection");
                     continue;
                  }
                  err_len = sizeof(ss);
                  if (hostname_addr(name, (struct sockaddr*) &ss, &err_len))
                  {
                     log_msg(LOG_ERR, "no IP for hostname \"%s\" found", name);
                     continue;
                  }
               }
               else
#endif
               {
                  err_len = SOCKADDR_SIZE(CNF(socks_dst));
                  memcpy(&ss, CNF(socks_dst), err_len);
               }

               log_debug("creating socket for unconnected SOCKS request");
               if ((squeue->fd = socket(ss.ss_family, SOCK_STREAM, 0)) == -1)
               {
                  log_msg(LOG_ERR, "cannot create socket for new SOCKS request: \"%s\"", strerror(errno));
                  continue;
               }

               set_nonblock(squeue->fd);
               log_debug("queueing fd %d for connect", squeue->fd);
               squeue->connect_time = t;
               if (socks_tcp_connect(squeue->fd, (struct sockaddr*) &ss, err_len) == -1)
               {
                  socks_reschedule(squeue);
                  continue;
               }

               squeue->state = SOCKS_CONNECTING;
               MFD_SET(squeue->fd, &wset, maxfd);

               break;

            case SOCKS_4AREQ_SENT:
            case SOCKS_5GREET_SENT:
            case SOCKS_5REQ_SENT:
               MFD_SET(squeue->fd, &rset, maxfd);
               break;

#ifdef WITH_DNS_LOOKUP
            case SOCKS_DNS_SENT:
               // check dns timeout
               if (t < squeue->restart_time)
               {
                  // and wait for response if timeout not elapsed
                  log_debug("DNS re-request is scheduled not before %ds, awaiting response", squeue->restart_time - t);
                  MFD_SET(squeue->fd, &rset, maxfd);
                  continue;
               }
               // resend request after timeout
               if (squeue->retry < SOCKS_DNS_RETRY && socks_dns_req(squeue) != -1)
               {
                  log_msg(LOG_INFO, "DNS request re-sent to fd %d, retry = %d", squeue->fd, squeue->retry);
                  squeue->retry++;
                  squeue->restart_time = t + SOCKS_DNS_RETRY_TIMEOUT;
                  MFD_SET(squeue->fd, &rset, maxfd);
               }
               else
               {
                  // FIXME: not sure if this is working, have a look at the retry counters...
                  log_msg(LOG_INFO, "trying request with V2 hostname");
                  oe_close(squeue->fd);
                  squeue->state = SOCKS_NEW;
                  squeue->restart_time = 0;
                  squeue->retry = 1;                  // do this to get lookup skipped in ´case SOCKS_NEW´.
               }
               break;
#endif

#ifdef WITH_DNS_RESOLVER
            case SOCKS_DNS_SENT:
               // do a local lookup anyway
               if (get_hostname(squeue, NULL, 0) >= 0)
               {
                  // hostname found, reinit socksqueue struct
                  log_debug("hostname found");
                  squeue->state = SOCKS_NEW;
                  squeue->restart_time = 0;
                  squeue->retry = 0;

                  // restart queue search
                  sq.next = socks_queue_;
                  squeue = &sq;
                  continue;
               }

               // check dns timeout
               if (t < squeue->restart_time)
                  continue;

                // wait another period
               if (squeue->retry < SOCKS_DNS_RETRY)
               {
                  squeue->retry++;
                  squeue->restart_time = t + SOCKS_DNS_RETRY_TIMEOUT;
               }
               else
               {
                  // FIXME: not sure if this is working, have a look at the retry counters...
                  log_msg(LOG_INFO, "trying request with V2 hostname");
                  squeue->state = SOCKS_NEW;
                  squeue->restart_time = 0;
                  squeue->retry = 1;                  // do this to get lookup skipped in ´case SOCKS_NEW´.
               }
               break;
#endif

            case SOCKS_DELETE:
               log_debug("ignoring queued element marked for deletion");
               break;

            default:
               log_msg(LOG_CRIT, "ignoring unknown state %d", squeue->state);
               socks_reset(squeue);
         }
      }

      // select all file descriptors
      if ((maxfd = oc_select0(maxfd + 1, &rset, &wset, NULL, SOCKS_DNS_RETRY_TIMEOUT)) == -1)
         continue;

      // check socks request pipe
      if (FD_ISSET(CNF(socksfd[0]), &rset))
      {
         maxfd--;
         if ((len = read(CNF(socksfd[0]), &sq, sizeof(sq))) == -1)
            log_msg(LOG_ERR, "failed to read from SOCKS request pipe, fd = %d: \"%s\"", 
                  CNF(socksfd[0]), strerror(errno));
         if (len < (int) sizeof(sq))
            log_msg(LOG_ERR, "read from SOCKS request pipe truncated to %d of %d bytes, ignoring.", 
                  len, (int) sizeof(sq));
         else
         {
            log_debug("received %d bytes on SOCKS request pipe fd %d", len, CNF(socksfd[0]));
            if (sq.next)
            {
               log_debug("output of SOCKS request queue triggered");
               socks_output_queue((intptr_t) sq.next);
            }
            else if (IN6_IS_ADDR_UNSPECIFIED(&sq.addr))
            {
               log_debug("wakeup request on SOCKS request pipe received");
            }
            else
            {
               log_debug("SOCKS queuing request received");
               socks_enqueue(&sq);
            }
         }
      }

      // handle all other file descriptors
      t = time(NULL);
      for (squeue = socks_queue_; maxfd && squeue; squeue = squeue->next)
      {
         // check write set, this is valid after connect()
         if (FD_ISSET(squeue->fd, &wset))
         {
            maxfd--;
            if (squeue->state == SOCKS_CONNECTING)
            {
               // test if connect() worked
               log_debug("check socket error");
               err_len = sizeof(so_err);
               if (getsockopt(squeue->fd, SOL_SOCKET, SO_ERROR, &so_err, &err_len) == -1)
               {
                  log_msg(LOG_ERR, "getsockopt failed: \"%s\", rescheduling request", strerror(errno));
                  socks_reschedule(squeue);
                  continue;
               }
               if (so_err)
               {
                  log_msg(LOG_ERR, "getsockopt returned %d (\"%s\")", so_err, strerror(so_err));
                  socks_reschedule(squeue);
                  continue;
               }
               // SOCKS4A
               if (CNF(socks5) == CONNTYPE_SOCKS4A)
               {
                  // everything seems to be ok, now check request status
                  if (socks_send_request(squeue) == -1)
                  {
                     log_msg(LOG_ERR, "SOCKS request failed");
                     socks_reschedule(squeue);
                     continue;
                  }
                  // request successfully sent, advance state machine
                  squeue->state = SOCKS_4AREQ_SENT;
               }
               else if (CNF(socks5) == CONNTYPE_SOCKS5)
               {
                  // everything seems to be ok, now check request status
                  if (socks5_greet(squeue) == -1)
                  {
                     log_msg(LOG_ERR, "SOCKS5 request failed");
                     socks_reschedule(squeue);
                     continue;
                  }
                  // request successfully sent, advance state machine
                  squeue->state = SOCKS_5GREET_SENT;
               }
               else if (CNF(socks5) == CONNTYPE_DIRECT)
               {
                  // no further handshake required for direct peers
                  log_debug("activating peer fd %d", squeue->fd);
                  socks_activate_peer(squeue);
                  squeue->state = SOCKS_DELETE;
               }
               else
               {
                  log_msg(LOG_EMERG, "unknown connection type %d (this should never happen...)", CNF(socks5));
                  exit(1);
               }
            }
            else
            {
               log_debug("unknown state %d in write set", squeue->state);
            }
         }

         // check read set, this is valid after write, i.e. receiving SOCKS response
         if (FD_ISSET(squeue->fd, &rset))
         {
            maxfd--;
            switch (squeue->state)
            {
               case SOCKS_4AREQ_SENT:
                  if (socks_rec_response(squeue) == -1)
                  {
                     socks_reschedule(squeue);
                     continue;
                  }
                  // success
                  log_debug("activating peer fd %d", squeue->fd);
                  socks_activate_peer(squeue);
                  squeue->state = SOCKS_DELETE;
                  break;

               case SOCKS_5GREET_SENT:
                  // check greet response
                  if (socks5_greet_response(squeue) == -1)
                  {
                     socks_reschedule(squeue);
                     continue;
                  }
                  // greeting was successful, send request
                  if (socks5_send_request(squeue) == -1)
                  {
                     log_msg(LOG_ERR, "sending SOCKS5 request failed");
                     socks_reschedule(squeue);
                     continue;
                  }
                  // request successfully sent, advance state machine
                  squeue->state = SOCKS_5REQ_SENT;
                  break;

               case SOCKS_5REQ_SENT:
                  if (socks5_rec_response(squeue) == -1)
                  {
                     socks_reschedule(squeue);
                     continue;
                  }
                  // success
                  log_debug("activating peer fd %d", squeue->fd);
                  socks_activate_peer(squeue);
                  squeue->state = SOCKS_DELETE;
                  break;

#ifdef WITH_DNS_LOOKUP
               case SOCKS_DNS_SENT:
                  log_debug("received UDP response");

                  // handle response
                  if (socks_dns_recv(squeue) != -1)
                  {
                     log_msg(LOG_NOTICE, "got valid DNS response, now reconnecting");
                     oe_close(squeue->fd);
                     squeue->state = SOCKS_NEW;
                     squeue->retry = 0;
                     squeue->restart_time = 0;
                  }
                  else
                  {
                     log_debug("closing UDP fd %d", squeue->fd);
                     oe_close(squeue->fd);
                     squeue->state = SOCKS_DELETE;
                  }
                  break;
#endif
               case SOCKS_DELETE:
                  log_debug("element was marked for deletion");
                  break;

               default:
                  log_msg(LOG_CRIT, "unknown state %d in read set", squeue->state);
                  socks_reset(squeue);
            }
         }
      }

      // delete requests from queue which are marked for deletion
      for (squeue = socks_queue_; squeue; squeue = squeue->next)
         if (squeue->state == SOCKS_DELETE)
         {
            socks_unqueue(squeue);
            // restart loop
            squeue = socks_queue_;
            if (!squeue)
            {
               log_debug("last entry deleted, breaking loop");
               break;
            }
         }
   }
}


int synchron_socks_connect(const struct in6_addr *addr)
{
   SocksQueue_t sq;

   memset(&sq, 0, sizeof(sq));
   sq.addr = *addr;
   sq.state = SOCKS_NEW;
   sq.fd = -1;

   while (sq.state != SOCKS_READY)
   {
      if (term_req())
      {
         log_debug("termination request");
         goto rlr_exit;
      }

      switch (sq.state)
      {
         case SOCKS_NEW:
            log_debug("creating socket");
            if ((sq.fd = socket(CNF(socks_dst)->sin_family == AF_INET ? PF_INET : PF_INET6, SOCK_STREAM, 0)) == -1)
            {
               log_msg(LOG_ERR, "Failed to create socket for SOCKS test request: \"%s\"", strerror(errno));
               goto rlr_exit;
            }

            log_debug("connecting fd %d", sq.fd);
            if (!socks_tcp_connect(sq.fd, (struct sockaddr*) CNF(socks_dst), SOCKADDR_SIZE(CNF(socks_dst))))
            {
               log_msg(LOG_INFO, "Successfully connected to SOCKS!");
               if (CNF(rand_addr))
               {
                  log_msg(LOG_INFO, "Remote loopback not possible with random address (-R)");
                  goto rlr_exit;
               }

               sq.state = SOCKS_CONNECTING;
               continue;
            }

            log_msg(LOG_ERR, "Could not connect to SOCKS server (i.e. Tor/I2P). Please check!");
            break;

         case SOCKS_CONNECTING:
            // SOCKS4A
            if (CNF(socks5) == CONNTYPE_SOCKS4A)
            {
               // everything seems to be ok, now check request status
               if (socks_send_request(&sq) == -1)
               {
                  log_msg(LOG_ERR, "SOCKS request failed");
                  sq.state = SOCKS_DELETE;
                  continue;
               }
               // request successfully sent, advance state machine
               sq.state = SOCKS_4AREQ_SENT;
            }
            else if (CNF(socks5) == CONNTYPE_SOCKS5)
            {
               // everything seems to be ok, now check request status
               if (socks5_greet(&sq) == -1)
               {
                  log_msg(LOG_ERR, "SOCKS5 request failed");
                  sq.state = SOCKS_DELETE;
                  continue;
               }
               // request successfully sent, advance state machine
               sq.state = SOCKS_5GREET_SENT;
            }
            continue;

         case SOCKS_4AREQ_SENT:
            if (socks_rec_response(&sq) == -1)
            {
               sq.state = SOCKS_DELETE;
               continue;
            }
            // success
            log_debug("activating peer fd %d", sq.fd);
            sq.state = SOCKS_READY;
            continue;

         case SOCKS_5GREET_SENT:
            // check greet response
            if (socks5_greet_response(&sq) == -1)
            {
               sq.state = SOCKS_DELETE;
               continue;
            }
            // greeting was successful, send request
            if (socks5_send_request(&sq) == -1)
            {
               log_msg(LOG_ERR, "sending SOCKS5 request failed");
               sq.state = SOCKS_DELETE;
               continue;
            }
            // request successfully sent, advance state machine
            sq.state = SOCKS_5REQ_SENT;
            continue;

         case SOCKS_5REQ_SENT:
            if (socks5_rec_response(&sq) == -1)
            {
               sq.state = SOCKS_DELETE;
               continue;
            }
            // success
            log_debug("activating peer fd %d", sq.fd);
            sq.state = SOCKS_READY;
            continue;

         case SOCKS_DELETE:
            oe_close(sq.fd);
            sq.fd = -1;
            sq.state = SOCKS_NEW;
            break;

         default:
            log_msg(LOG_CRIT, "unhandled state %d", sq.state);
            sq.state = SOCKS_DELETE;
            continue;
      }

      log_msg(LOG_INFO, "Restarting in a moment...");
      oc_select(0, NULL, NULL, NULL);
   }

rlr_exit:
   return sq.fd;
}

