/* $Id$ */

/*
 * Copyright (c) 2010-2011 SURFnet bv
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
 * 3. Neither the name of SURFnet bv nor the names of its contributors 
 *    may be used to endorse or promote products derived from this 
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * The Extensible Ethernet Monitor (EEMO)
 * TCP packet handling
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "eemo.h"
#include "eemo_list.h"
#include "eemo_log.h"
#include "ip_handler.h"
#include "tcp_handler.h"

/* The linked list of TCP packet handlers */
static eemo_ll_entry* tcp_handlers = NULL;

/* TCP handler comparison data type */
typedef struct
{
	u_short srcport;
	u_short dstport;
}
eemo_tcp_handler_comp_t;

/* TCP handler comparison */
int eemo_tcp_handler_compare(void* elem_data, void* comp_data)
{
	eemo_tcp_handler_comp_t* comp = (eemo_tcp_handler_comp_t*) comp_data;
	eemo_tcp_handler* elem = (eemo_tcp_handler*) elem_data;

	if ((elem_data == NULL) || (comp_data == NULL))
	{
		return 0;
	}

	if (((elem->srcport == TCP_ANY_PORT) || (elem->srcport == comp->srcport)) &&
	    ((elem->dstport == TCP_ANY_PORT) || (elem->dstport == comp->dstport)))
	{
		return 1;
	}

	return 0;
}

/* Find a TCP handler for the specified type */
eemo_tcp_handler* eemo_find_tcp_handler(u_short srcport, u_short dstport)
{
	eemo_tcp_handler* rv = NULL;
	eemo_tcp_handler_comp_t comp;

	comp.srcport = srcport;
	comp.dstport = dstport;

	if (eemo_ll_find(tcp_handlers, (void*) &rv, &eemo_tcp_handler_compare, (void*) &comp) != ERV_OK)
	{
		/* FIXME: log this */
	}

	return rv;
}

/* Convert TCP packet header to host byte order */
void eemo_tcp_ntoh(eemo_hdr_tcp* hdr)
{
	hdr->tcp_srcport	= ntohs(hdr->tcp_srcport);
	hdr->tcp_dstport	= ntohs(hdr->tcp_dstport);
	hdr->tcp_seqno		= ntohl(hdr->tcp_seqno);
	hdr->tcp_ackno		= ntohl(hdr->tcp_ackno);
	hdr->tcp_win		= ntohs(hdr->tcp_win);
	hdr->tcp_chksum		= ntohs(hdr->tcp_chksum);
	hdr->tcp_urgent		= ntohs(hdr->tcp_urgent);
}

/* Handle a TCP packet */
eemo_rv eemo_handle_tcp_packet(eemo_packet_buf* packet, eemo_ip_packet_info ip_info)
{
	eemo_hdr_tcp* hdr = NULL;
	size_t hdr_len = 0;
	eemo_tcp_handler* handler = NULL;

	/* Check minimum length */
	if (packet->len < sizeof(eemo_hdr_tcp))
	{
		/* TCP packet is malformed */
		return ERV_MALFORMED;
	}

	/* Take the header from the packet */
	hdr = (eemo_hdr_tcp*) packet->data;

	/* Convert the header to host byte order */
	eemo_tcp_ntoh(hdr);

	/* Determine the true header length */
	hdr_len = (hdr->tcp_ofs) >> 4;
	hdr_len *= 4; /* size in packet in 32-bit words */

	if (packet->len < hdr_len)
	{
		return ERV_MALFORMED;
	}

	/* See if there is a handler given the source and destination port for this packet */
	handler = eemo_find_tcp_handler(hdr->tcp_srcport, hdr->tcp_dstport);

	if ((handler != NULL) && (handler->handler_fn != NULL))
	{
		size_t delta_ofs = ((hdr->tcp_ofs & 0xf0) >> 4) * 4; /* header length in 32-bit words */
		eemo_rv rv = ERV_OK;
		eemo_packet_buf* tcp_data = NULL; 
		eemo_tcp_packet_info tcp_info;

		if (delta_ofs > packet->len)
		{
			return ERV_MALFORMED;
		}

		tcp_data = eemo_pbuf_new(&packet->data[delta_ofs], packet->len - delta_ofs);

		if (tcp_data == NULL)
		{
			return ERV_MEMORY;
		}

		/* Copy TCP information */
		tcp_info.srcport 	= hdr->tcp_srcport;
		tcp_info.dstport	= hdr->tcp_dstport;
		tcp_info.seqno		= hdr->tcp_seqno;
		tcp_info.ackno		= hdr->tcp_ackno;
		tcp_info.flags		= hdr->tcp_flags;
		tcp_info.winsize	= hdr->tcp_win;
		tcp_info.urgptr		= hdr->tcp_urgent;

		/* Call handler */
		rv = (handler->handler_fn)(tcp_data, ip_info, tcp_info);

		eemo_pbuf_free(tcp_data);

		return rv;
	}

	return ERV_SKIPPED;
}

/* Register a TCP handler */
eemo_rv eemo_reg_tcp_handler(u_short srcport, u_short dstport, eemo_tcp_handler_fn handler_fn)
{
	eemo_tcp_handler* new_handler = NULL;
	eemo_rv rv = ERV_OK;

	/* Check if a handler for the specified ports already exists */
	if (eemo_find_tcp_handler(srcport, dstport) != NULL)
	{
		/* A handler for this type has already been registered */
		return ERV_HANDLER_EXISTS;
	}

	/* Create a new handler entry */
	new_handler = (eemo_tcp_handler*) malloc(sizeof(eemo_tcp_handler));

	if (new_handler == NULL)
	{
		/* Not enough memory */
		return ERV_MEMORY;
	}

	new_handler->srcport = srcport;
	new_handler->dstport = dstport;
	new_handler->handler_fn = handler_fn;

	/* Register the new handler */
	if ((rv = eemo_ll_append(&tcp_handlers, (void*) new_handler)) != ERV_OK)
	{
		/* FIXME: log this */
	}

	return rv;
}

/* Unregister a TCP handler */
eemo_rv eemo_unreg_tcp_handler(u_short srcport, u_short dstport)
{
	eemo_tcp_handler_comp_t comp;

	comp.srcport = srcport;
	comp.dstport = dstport;

	return eemo_ll_remove(&tcp_handlers, &eemo_tcp_handler_compare, (void*) &comp);
}

/* Initialise IP handling */
eemo_rv eemo_init_tcp_handler(void)
{
	tcp_handlers = NULL;

	/* Register TCP packet handler */
	if (eemo_reg_ip_handler(IP_TCP, &eemo_handle_tcp_packet) != ERV_OK)
	{
		ERROR_MSG("Failed to register handler for TCP packets");

		return ERV_GENERAL_ERROR;
	}

	INFO_MSG("Initialised TCP handling");

	return ERV_OK;
}

/* Clean up */
void eemo_tcp_handler_cleanup(void)
{
	/* Clean up the list of TCP packet handlers */
	if (eemo_ll_free(&tcp_handlers) != ERV_OK)
	{
		ERROR_MSG("Failed to free list of TCP handlers");
	}

	/* Unregister the IP handler for TCP packets */
	eemo_unreg_ip_handler(IP_TCP);

	INFO_MSG("Uninitialised TCP handling");
}
