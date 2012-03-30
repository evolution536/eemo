/* $Id$ */

/*
 * Copyright (c) 2010-2012 SURFnet bv
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
#include "utlist.h"
#include "eemo_handlefactory.h"
#include "eemo_log.h"
#include "ip_handler.h"
#include "tcp_handler.h"

/* The handle for the IP handler for TCP packets */
static unsigned long tcp_ip_handler_handle = 0;

/* The linked list of TCP packet handlers */
static eemo_tcp_handler* tcp_handlers = NULL;

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
	eemo_tcp_handler* handler_it = NULL;
	eemo_rv rv = ERV_SKIPPED;

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
	LL_FOREACH(tcp_handlers, handler_it)
	{
		eemo_rv handler_rv = ERV_SKIPPED;

		if ((handler_it->handler_fn != NULL) &&
		    (((handler_it->srcport == TCP_ANY_PORT) || (handler_it->srcport == hdr->tcp_srcport)) &&
		     ((handler_it->dstport == TCP_ANY_PORT) || (handler_it->dstport == hdr->tcp_dstport))))
		{
			size_t delta_ofs = ((hdr->tcp_ofs & 0xf0) >> 4) * 4; /* header length in 32-bit words */
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
			handler_rv = (handler_it->handler_fn)(tcp_data, ip_info, tcp_info);

			eemo_pbuf_free(tcp_data);
		}

		if (rv != ERV_HANDLED)
		{
			rv = handler_rv;
		}
	}

	return rv;
}

/* Register a TCP handler */
eemo_rv eemo_reg_tcp_handler(u_short srcport, u_short dstport, eemo_tcp_handler_fn handler_fn, unsigned long* handle)
{
	eemo_tcp_handler* new_handler = NULL;

	/* Check parameters */
	if ((handler_fn == NULL) || (handle == NULL))
	{
		return ERV_PARAM_INVALID;
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
	new_handler->handle = eemo_get_new_handle();

	/* Register the new handler */
	LL_APPEND(tcp_handlers, new_handler);

	*handle = new_handler->handle;

	DEBUG_MSG("Registered TCP handler with handle 0x%08X and handler function at 0x%08X", *handle, handler_fn);

	return ERV_OK;
}

/* Unregister a TCP handler */
eemo_rv eemo_unreg_tcp_handler(unsigned long handle)
{
	eemo_tcp_handler* to_delete = NULL;

	LL_SEARCH_SCALAR(tcp_handlers, to_delete, handle, handle);

	if (to_delete != NULL)
	{
		LL_DELETE(tcp_handlers, to_delete);

		DEBUG_MSG("Unregistered TCP handler with handle 0x%08X and handler function at 0x%08X", handle, to_delete->handler_fn);

		free(to_delete);

		eemo_recycle_handle(handle);

		return ERV_OK;
	}
	else
	{
		return ERV_NOT_FOUND;
	}
}

/* Initialise IP handling */
eemo_rv eemo_init_tcp_handler(void)
{
	tcp_handlers = NULL;

	/* Register TCP packet handler */
	if (eemo_reg_ip_handler(IP_TCP, &eemo_handle_tcp_packet, &tcp_ip_handler_handle) != ERV_OK)
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
	eemo_tcp_handler* handler_it = NULL;
	eemo_tcp_handler* handler_tmp = NULL;

	/* Clean up the list of TCP packet handlers */
	LL_FOREACH_SAFE(tcp_handlers, handler_it, handler_tmp)
	{
		LL_DELETE(tcp_handlers, handler_it);

		free(handler_it);
	}

	/* Unregister the IP handler for TCP packets */
	eemo_unreg_ip_handler(tcp_ip_handler_handle);

	INFO_MSG("Uninitialised TCP handling");
}

