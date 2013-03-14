/*
 * Copyright (c) 2012, Thingsquare, http://www.thingsquare.com/.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "contiki-net.h"
#include "websocket-http-client.h"
#include "cfs/cfs.h"
#include "lib/petsciiconv.h"

#include "mdns.h"
#include "websocket.h"

PROCESS(websocket_process, "Websockets process");

#define MAX_HOSTLEN 16
#define MAX_PATHLEN 100

LIST(websocketlist);

static uint8_t inited = 0;

#define WEBSOCKET_FIN_BIT       0x80

#define WEBSOCKET_OPCODE_MASK   0x0f
#define WEBSOCKET_OPCODE_CONT   0x00
#define WEBSOCKET_OPCODE_TEXT   0x01
#define WEBSOCKET_OPCODE_BIN    0x02
#define WEBSOCKET_OPCODE_CLOSE  0x08
#define WEBSOCKET_OPCODE_PING   0x09
#define WEBSOCKET_OPCODE_PONG   0x0a

#define WEBSOCKET_MASK_BIT      0x80
#define WEBSOCKET_LEN_MASK      0x7f
struct websocket_frame_hdr {
  uint8_t opcode;
  uint8_t len;
  uint8_t extlen[4];
};

struct websocket_frame_mask {
  uint8_t mask[4];
};

/*---------------------------------------------------------------------------*/
static int
parse_url(const char *url, char *host, uint16_t *portptr, char *path)
{
  const char *urlptr;
  int i;
  const char *file;
  uint16_t port;

  if(url == NULL) {
    return 0;
  }

  /* Don't even try to go further if the URL is empty. */
  if(strlen(url) == 0) {
    return 0;
  }

  /* See if the URL starts with http:// or ws:// and remove it. */
  if(strncmp(url, http_http, strlen(http_http)) == 0) {
    urlptr = url + strlen(http_http);
  } else if(strncmp(url, http_ws, strlen(http_ws)) == 0) {
    urlptr = url + strlen(http_ws);
  } else {
    urlptr = url;
  }

  /* Find host part of the URL. */
  for(i = 0; i < MAX_HOSTLEN; ++i) {
    if(*urlptr == 0 ||
       *urlptr == '/' ||
       *urlptr == ' ' ||
       *urlptr == ':') {
      if(host != NULL) {
	host[i] = 0;
      }
      break;
    }
    if(host != NULL) {
      host[i] = *urlptr;
    }
    ++urlptr;
  }

  /* Find the port. Default is 80. */
  port = 80;
  if(*urlptr == ':') {
    port = 0;
    do {
      ++urlptr;
      if(*urlptr >= '0' && *urlptr <= '9') {
	port = (10 * port) + (*urlptr - '0');
      }
    } while(*urlptr >= '0' &&
	    *urlptr <= '9');
  }
  if(portptr != NULL) {
    *portptr = port;
  }
  /* Find file part of the URL. */
  while(*urlptr != '/' && *urlptr != 0) {
    ++urlptr;
  }
  if(*urlptr == '/') {
    file = urlptr;
  } else {
    file = "/";
  }
  if(path != NULL) {
    strncpy(path, file, MAX_PATHLEN);
  }
  return 1;
}
/*---------------------------------------------------------------------------*/
static int
start_get(struct websocket *s)
{
  uip_ipaddr_t addr;
  char host[MAX_HOSTLEN];
  char path[MAX_PATHLEN];
  uint16_t port;
  
  if(parse_url(s->url, host, &port, path)) {
    
    /* First check if the host is an IP address. */
    if(uiplib_ip4addrconv(host, (uip_ip4addr_t *)&addr) == 0 &&
       uiplib_ip6addrconv(host, (uip_ip6addr_t *)&addr) == 0) {
      /* Try to lookup the hostname. If it fails, we initiate a hostname
	 lookup and print out an informative message on the
	 statusbar. */
      if(mdns_lookup(host) == NULL) {
	mdns_query(host);
	s->state = WEBSOCKET_STATE_DNS_REQUEST_SENT;
	printf("Resolving host...\n");
	return WEBSOCKET_OK;
      }
    }

    /* The hostname we present in the hostname table, so we send out the
       initial GET request. */
    if(websocket_http_client_get(&(s->s), host, port, path,
                                 s->subprotocol) == 0) {
      printf("Out of memory error\n");
      s->state = WEBSOCKET_STATE_CLOSED;
      return WEBSOCKET_ERR;
    } else {
      printf("Connecting...\n");
      s->state = WEBSOCKET_STATE_HTTP_REQUEST_SENT;
      return WEBSOCKET_OK;
    }
  }
  return WEBSOCKET_ERR;
}
/*---------------------------------------------------------------------------*/
void
call(struct websocket *s, websocket_result r,
     uint8_t *data, uint16_t datalen)
{
  if(s != NULL && s->callback != NULL) {
    s->callback(s, r, data, datalen);
  }
} 
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(websocket_process, ev, data)
{
  PROCESS_BEGIN();

  while(1) {

    PROCESS_WAIT_EVENT();

    if(ev == tcpip_event) {
      websocket_http_client_appcall(data);
    } else if(ev == mdns_event_found && data != NULL) {
      struct websocket *s;
      const char *name = data;
      /* Either found a hostname, or not. We need to go through the
	 list of websocketsand figure out to which connection this
	 reply corresponds, then either restart the HTTP get, or kill
	 it (if no hostname was found). */
      for(s = list_head(websocketlist);
	  s != NULL;
	  s = list_item_next(s)) {
	char host[MAX_HOSTLEN];
	if(parse_url(s->url, host, NULL, NULL) &&
	   strcmp(name, host) == 0) {
	  if(mdns_lookup(name) != NULL) {
	    /* Hostname found, restart get. */
	    printf("Restarting get\n");
	    start_get(s);
	  } else {
	    /* Hostname not found, kill connection. */
            /*	    printf("XXX killing connection\n");*/
            call(s, WEBSOCKET_HOSTNAME_NOT_FOUND, NULL, 0);
	  }
	}
      }
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
/* Callback function. Called from the webclient when the HTTP
 * connection was abruptly aborted.
 */
void
websocket_http_client_aborted(struct websocket_http_client_state *client_state)
{
  if(client_state != NULL) {
    struct websocket *s = (struct websocket *)
      ((char *)client_state - offsetof(struct websocket, s));
    printf("Websocket reset\n");
    s->state = WEBSOCKET_STATE_CLOSED;
    call(s, WEBSOCKET_RESET, NULL, 0);
  }
}
/*---------------------------------------------------------------------------*/
/* Callback function. Called from the webclient when the HTTP
 * connection timed out.
 */
void
websocket_http_client_timedout(struct websocket_http_client_state *client_state)
{
  struct websocket *s = (struct websocket *)
    ((char *)client_state - offsetof(struct websocket, s));
  printf("Websocket timed out\n");
  s->state = WEBSOCKET_STATE_CLOSED;
  call(s, WEBSOCKET_TIMEDOUT, NULL, 0);
}
/*---------------------------------------------------------------------------*/
/* Callback function. Called from the webclient when the HTTP
 * connection was closed after a request from the "websocket_http_client_close()"
 * function. .
 */
void
websocket_http_client_closed(struct websocket_http_client_state *client_state)
{
  struct websocket *s = (struct websocket *)
    ((char *)client_state - offsetof(struct websocket, s));
  printf("Websocket closed.\n");
  s->state = WEBSOCKET_STATE_CLOSED;
  call(s, WEBSOCKET_CLOSED, NULL, 0);
}
/*---------------------------------------------------------------------------*/
/* Callback function. Called from the webclient when the HTTP
 * connection is connected.
 */
void
websocket_http_client_connected(struct websocket_http_client_state *client_state)
{
  struct websocket *s = (struct websocket *)
    ((char *)client_state - offsetof(struct websocket, s));

  printf("Websocket connected\n");
  s->state = WEBSOCKET_STATE_CONNECTED;
  call(s, WEBSOCKET_CONNECTED, NULL, 0);
}
/*---------------------------------------------------------------------------*/
/* Callback function. Called from the webclient module when HTTP data
 * has arrived.
 */
void
websocket_http_client_datahandler(struct websocket_http_client_state *client_state,
				  uint8_t *data, uint16_t datalen)
{
  struct websocket *s = (struct websocket *)
    ((char *)client_state - offsetof(struct websocket, s));
  struct websocket_frame_hdr *hdr;
  struct websocket_frame_mask *maskptr;
  uint8_t *user_data;
  int appdata_len;
  int header_len;

  if(data == NULL) {
    call(s, WEBSOCKET_CLOSED, NULL, 0);
  } else {

    if(s->state == WEBSOCKET_STATE_CONNECTED) {
      /* If this is the start of an incoming websocket data frame, we
         decode the header and check if we should act on in. If not, we
         pipe the data to the application through a callback handler. If
         data arrives in multiple packets, it is up to the application to
         put it back together again. */


      /* The websocket header is at the start of the incoming data. */
      hdr = (struct websocket_frame_hdr *)data;

      /* The s->len field holds the length of the application data
       * chunk that we are about to receive. */
      s->len = 0;

      /* The s->mask field holds the bitmask of the data chunk, if
       * any. */
      memset(s->mask, 0, sizeof(s->mask));

      /* We first read out the length of the application data
         chunk. The length may be encoded over multiple bytes. If the
         length is >= 126 bytes, it is encoded as two or more
         bytes. The first length field determines if it is in 2 or 4
         bytes. We also keep track of where the bitmask is held - its
         place also differs depending on how the length is encoded.  */
      maskptr = (struct websocket_frame_mask *)hdr->extlen;
      if((hdr->len & WEBSOCKET_LEN_MASK) < 126) {
        s->len = hdr->len & WEBSOCKET_LEN_MASK;
      } else if(hdr->len == 126) {
        s->len = (hdr->extlen[0] << 8) + hdr->extlen[1];
        maskptr = (struct websocket_frame_mask *)&hdr->extlen[2];
      } else if(hdr->len == 127) {
        s->len = ((uint32_t)hdr->extlen[0] << 24) +
          ((uint32_t)hdr->extlen[1] << 16) +
          ((uint32_t)hdr->extlen[2] << 8) +
          hdr->extlen[3];
        maskptr = (struct websocket_frame_mask *)&hdr->extlen[4];
      }

      /* Set user_data to point to the first byte of application data.
         See if the application data chunk is masked or not. If it is,
         we copy the bitmask into the s->mask field. */
      if((hdr->len & WEBSOCKET_MASK_BIT) == 0) {
        user_data = (uint8_t *)maskptr;
        /*        printf("No mask\n");*/
      } else {
        user_data = (uint8_t *)&maskptr->mask[4];
        memcpy(s->mask, &maskptr->mask, sizeof(s->mask));
        /*        printf("There was a mask, %02x %02x %02x %02x\n",
                  s->mask[0], s->mask[1], s->mask[2], s->mask[3]);*/
      }

      /* Remember the opcode of the application chunk, put it in the
       * s->opcode field. */
      s->opcode = hdr->opcode & WEBSOCKET_OPCODE_MASK;

      header_len = (int)(user_data - data);
      appdata_len = datalen - header_len;

      if(s->opcode == WEBSOCKET_OPCODE_PING) {
        /* If the opcode is ping, we change the opcode to a pong, and
         * send the data back. */
        hdr->opcode = (hdr->opcode & (~WEBSOCKET_OPCODE_MASK)) |
          WEBSOCKET_OPCODE_PONG;
        websocket_http_client_send(&s->s, data, s->len + 2);
        printf("Got ping\n");
      } else if(s->opcode == WEBSOCKET_OPCODE_BIN ||
                s->opcode == WEBSOCKET_OPCODE_TEXT) {

        /* If the opcode is bin or text, and there is application
         * layer data in the packet, we call the application to
         * process it. */

        /*        printf("s->len %d, datalen is %d, appdata_len %d\n",
               s->len, datalen,
               appdata_len);*/
        if(s->len > 0) {
          s->state = WEBSOCKET_STATE_RECEIVING_DATA;
          if(appdata_len > 0) {
            int len;
#define MIN(a, b) ((a) < (b)? (a): (b))
            len = MIN(s->len, appdata_len);
            /* XXX todo: mask if needed. */
            call(s, WEBSOCKET_DATA, user_data, len);
            user_data += len;
            s->len -= len;
            appdata_len -= len;
          }
        }
      }

      if(s->len <= 0) {
        s->state = WEBSOCKET_STATE_CONNECTED;
      }

      /* Need to keep parsing the incoming data to check for more
       frames, if the incoming datalen is > than s->len. */
      if(s->len == 0 && appdata_len > 0) {
        printf("XXX 1 again\n");
        websocket_http_client_datahandler(client_state,
                                          user_data, appdata_len);
      }
    } else if(s->state == WEBSOCKET_STATE_RECEIVING_DATA) {
      /* XXX todo: mask if needed. */
      /*      printf("Calling with s->len %d datalen %d\n",
              s->len, datalen);*/
      if(datalen < s->len) {
        call(s, WEBSOCKET_DATA, data, datalen);
        s->len -= datalen;
      } else {
        call(s, WEBSOCKET_DATA, data, s->len);
        data += s->len;
        datalen -= s->len;
        s->len = 0;
      }
      if(s->len <= 0) {
        s->state = WEBSOCKET_STATE_CONNECTED;
      }
      /* Need to keep parsing the incoming data to check for more
         frames, if the incoming datalen is > than len. */
      if(s->len == 0 && datalen > 0) {
        printf("XXX 2 again\n");
        websocket_http_client_datahandler(client_state,
                                          data, datalen);

      }
    }
  }
}
/*---------------------------------------------------------------------------*/
void
websocket_init(void)
{
  process_start(&websocket_process, NULL);
  list_init(websocketlist);
  inited = 1;
}
/*---------------------------------------------------------------------------*/
websocket_result
websocket_open(struct websocket *s, const char *url,
               const char *subprotocol,
	       websocket_callback c)
{
  int ret;

  if(!inited) {
    websocket_init();
  }

  if(s == NULL) {
    return WEBSOCKET_ERR;
  }

  if(s->state != WEBSOCKET_STATE_CLOSED) {
    printf("websocket_open: closing websocket before opening it again.\n");
    websocket_close(s);
  }
  s->callback = c;
  list_add(websocketlist, s);
  strncpy(s->url, url, WEBSOCKET_MAX_URLLEN);
  strncpy(s->subprotocol, subprotocol, WEBSOCKET_CONF_MAX_SUBPROTOLEN);
  PROCESS_CONTEXT_BEGIN(&websocket_process);
  ret = start_get(s);
  PROCESS_CONTEXT_END();
  return ret;
}
/*---------------------------------------------------------------------------*/
void
websocket_close(struct websocket *s)
{
  websocket_http_client_close(&s->s);
  s->state = WEBSOCKET_CLOSED;
}
/*---------------------------------------------------------------------------*/
static int
send_data(struct websocket *s, const void *data,
          uint16_t datalen, uint8_t data_type_opcode)
{
  struct websocket_frame_hdr hdr;
  struct websocket_frame_mask mask;

  if(datalen > 126) {
    printf("websocket_send: data len > 126 not implemented\n");
    return 0;
  }

  if(2 + 4 + datalen > websocket_http_client_sendbuflen(&s->s)) {
    return -1;
  }

  hdr.opcode = WEBSOCKET_FIN_BIT | data_type_opcode;

  /* Data from client must always have the mask bit set, and a data
     mask sent right after the header. */
  hdr.len = datalen | WEBSOCKET_MASK_BIT;

  /* XXX: We just set a dummy mask of 0 for now and hope that this
     works. */
  mask.mask[0] =
    mask.mask[1] =
    mask.mask[2] =
    mask.mask[3] = 0;
  websocket_http_client_send(&s->s, (uint8_t *)&hdr, 2);
  websocket_http_client_send(&s->s, (uint8_t *)&mask, 4);
  websocket_http_client_send(&s->s, data, datalen);
  return datalen;
}
/*---------------------------------------------------------------------------*/
int
websocket_send_str(struct websocket *s, const char *str)
{
  printf("websocket_send_str %s\n", str);
  return send_data(s, str, strlen(str), WEBSOCKET_OPCODE_TEXT);
}
/*---------------------------------------------------------------------------*/
int
websocket_send(struct websocket *s, const uint8_t *data,
	       uint16_t datalen)
{
  return send_data(s, data, datalen, WEBSOCKET_OPCODE_BIN);
}
/*---------------------------------------------------------------------------*/
int
websocket_ping(struct websocket *s)
{
  struct websocket_frame_hdr hdr;
  struct websocket_frame_mask mask;

  if(2 + 4 > websocket_http_client_sendbuflen(&s->s)) {
    return -1;
  }

  hdr.opcode = WEBSOCKET_FIN_BIT | WEBSOCKET_OPCODE_PING;

  /* Data from client must always have the mask bit set, and a data
     mask sent right after the header. */
  hdr.len = 0 | WEBSOCKET_MASK_BIT;

  /* XXX: We just set a dummy mask of 0 for now and hope that this
     works. */
  mask.mask[0] =
    mask.mask[1] =
    mask.mask[2] =
    mask.mask[3] = 0;
  websocket_http_client_send(&s->s, (uint8_t *)&hdr, 2);
  websocket_http_client_send(&s->s, (uint8_t *)&mask, 4);
  return 1;
}
/*---------------------------------------------------------------------------*/
