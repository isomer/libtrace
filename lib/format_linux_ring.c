/*
 * This file is part of libtrace
 *
 * Copyright (c) 2007,2008,2009,2010 The University of Waikato, Hamilton,
 * New Zealand.
 *
 * Authors: Daniel Lawson
 *          Perry Lorier
 *          Shane Alcock
 *          Richard Sanger
 *
 * All rights reserved.
 *
 * This code has been developed by the University of Waikato WAND
 * research group. For further information please see http://www.wand.net.nz/
 *
 * libtrace is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * libtrace is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libtrace; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id$
 *
 */

/* This format module deals with using the Linux Ring capture format (also
 * known as PACKET_MMAP).
 *
 * Linux Ring is a LIVE capture format.
 *
 * This format also supports writing which will write packets out to the
 * network as a form of packet replay. This should not be confused with the
 * RT protocol which is intended to transfer captured packet records between
 * RT-speaking programs.
 */

#include "config.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_helper.h"
#include "libtrace_arphrd.h"
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#ifdef HAVE_INTTYPES_H
#  include <inttypes.h>
#else
# error "Can't find inttypes.h"
#endif

#include "format_linux_common.h"

#ifdef HAVE_NETPACKET_PACKET_H
/* Get current frame in the ring buffer*/
#define GET_CURRENT_BUFFER(libtrace, stream) \
	((void *)stream->rx_ring +				\
	 (stream->rxring_offset *				\
	  FORMAT_DATA->req.tp_frame_size))
#endif

/* Get the start of the captured data. I'm not sure if tp_mac (link layer) is
 * always guaranteed. If it's not there then just use tp_net.
 */
#define TP_TRACE_START(mac, net, hdrend) \
	((mac) > (hdrend) && (mac) < (net) ? (mac) : (net))
/* Cached page size, the page size shouldn't be changing */
static int pagesize = 0;

/*
 * Try figure out the best sizes for the ring buffer. Ensure that:
 * - max(Block_size) == page_size << max_order
 * - Frame_size == page_size << x (so that block_size%frame_size == 0)
 *   This means that there will be no wasted space between blocks
 * - Frame_size < block_size
 * - Frame_size is as close as possible to LIBTRACE_PACKET_BUFSIZE, but not
 *   bigger
 * - Frame_nr = Block_nr * (frames per block)
 * - CONF_RING_FRAMES is used a minimum number of frames to hold
 * - Calculates based on max_order and buf_min
 */
static void calculate_buffers(struct tpacket_req * req, int fd, char * uri,
		uint32_t max_order)
{
	struct ifreq ifr;
	unsigned max_frame = LIBTRACE_PACKET_BUFSIZE;
	pagesize = getpagesize();

	strcpy(ifr.ifr_name, uri);
	/* Don't bother trying to set frame size above mtu linux will drop
	 * these anyway.
	 *
	 * Remember, that our frame also has to include a TPACKET header!
	 */
	if (ioctl(fd, SIOCGIFMTU, (caddr_t)&ifr) >= 0)
		max_frame = ifr.ifr_mtu + TPACKET_ALIGN(TPACKET2_HDRLEN);
	if (max_frame > LIBTRACE_PACKET_BUFSIZE)
		max_frame = LIBTRACE_PACKET_BUFSIZE;

	/* Calculate frame size */
	req->tp_frame_size = pagesize;
	while (req->tp_frame_size < max_frame &&
	      req->tp_frame_size < LIBTRACE_PACKET_BUFSIZE) {
		req->tp_frame_size <<= 1;
	}
	if (req->tp_frame_size > LIBTRACE_PACKET_BUFSIZE)
		req->tp_frame_size >>= 1;

	/* Calculate block size */
	req->tp_block_size = pagesize << max_order;
	do {
		req->tp_block_size >>= 1;
	} while ((CONF_RING_FRAMES * req->tp_frame_size) <= req->tp_block_size);
	req->tp_block_size <<= 1;

	/* Calculate number of blocks */
	req->tp_block_nr = (CONF_RING_FRAMES * req->tp_frame_size)
		/ req->tp_block_size;
	if((CONF_RING_FRAMES * req->tp_frame_size) % req->tp_block_size != 0)
		req->tp_block_nr++;

	/* Calculate packets such that we use all the space we have to
	 * allocated */
	req->tp_frame_nr = req->tp_block_nr *
		(req->tp_block_size / req->tp_frame_size);

	/*
	printf("MaxO 0x%x BS 0x%x BN 0x%x FS 0x%x FN 0x%x\n",
		max_order,
		req->tp_block_size,
		req->tp_block_nr,
		req->tp_frame_size,
		req->tp_frame_nr);
	*/

	/* In case we have some silly values*/
	assert(req->tp_block_size);
	assert(req->tp_block_nr);
	assert(req->tp_frame_size);
	assert(req->tp_frame_nr);
	assert(req->tp_block_size % req->tp_frame_size == 0);
}

static inline int socket_to_packetmmap(char * uridata, int ring_type,
					int fd,
					struct tpacket_req * req,
					char ** ring_location,
					uint32_t *max_order,
					char *error) {
	int val;

	/* Switch to TPACKET header version 2, we only try support v2 because
	 * v1 had problems with data type consistancy */
	val = TPACKET_V2;
	if (setsockopt(fd,
		       SOL_PACKET,
		       PACKET_VERSION,
		       &val,
		       sizeof(val)) == -1) {
		strncpy(error, "TPACKET2 not supported", 2048);
		return -1;
	}

	/* Try switch to a ring buffer. If it fails we assume the the kernel
	 * cannot allocate a block of that size, so decrease max_block and
	 * retry.
	 */
	while(1) {
		fprintf(stderr, "max_order=%d\n", *max_order);
		if (*max_order <= 0) {
			strncpy(error,
				"Cannot allocate enough memory for ring buffer",
				2048);
			return -1;
		}
		calculate_buffers(req, fd, uridata, *max_order);
		if (setsockopt(fd,
			       SOL_PACKET,
			       ring_type,
			       req,
			       sizeof(struct tpacket_req)) == -1) {
			if(errno == ENOMEM) {
				(*max_order)--;
			} else {
				strncpy(error,
					"Error setting the ring buffer size",
					2048);
				return -1;
			}

		} else break;
	}
	fprintf(stderr, "max_order=%d\n", *max_order);

	/* Map the ring buffer into userspace */
	*ring_location = mmap(NULL,
			      req->tp_block_size * req->tp_block_nr,
			      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(*ring_location == MAP_FAILED) {
		strncpy(error, "Failed to map memory for ring buffer", 2048);
		return -1;
	}

	return 0;
}

/* Release a frame back to the kernel or free() if it's a malloc'd buffer
 */
inline static void ring_release_frame(libtrace_t *libtrace UNUSED,
				      libtrace_packet_t *packet)
{
	/* Free the old packet */
	if(packet->buffer == NULL)
		return;

	if(packet->buf_control == TRACE_CTRL_PACKET){
		free(packet->buffer);
		packet->buffer = NULL;
	}

	if(packet->buf_control == TRACE_CTRL_EXTERNAL) {
		//struct linux_format_data_t *ftd = FORMAT_DATA;
		/* Check it's within our buffer first - consider the pause
		 * resume case it might have already been free'd lets hope we
		 * get another buffer */
		// TODO: For now let any one free anything
		/*if(LIBTRACE_BETWEEN((char *) packet->buffer,
				(char *) ftd->rx_ring,
				ftd->rx_ring +
				ftd->req.tp_block_size *
				ftd->req.tp_block_nr)){*/
		TO_TP_HDR2(packet->buffer)->tp_status = 0;
		packet->buffer = NULL;
		/*}*/
	}
}

static inline int linuxring_start_input_stream(libtrace_t *libtrace,
                                               struct linux_per_stream_t *stream) {
	char error[2048];

	/* We set the socket up the same and then convert it to PACKET_MMAP */
	if (linuxcommon_start_input_stream(libtrace, stream) < 0)
		return -1;

	strncpy(error, "No known error", 2048);

	/* Make it a packetmmap */
	if(socket_to_packetmmap(libtrace->uridata, PACKET_RX_RING,
	                        stream->fd,
	                        &FORMAT_DATA->req,
	                        &stream->rx_ring,
	                        &FORMAT_DATA->max_order,
	                        error) != 0) {
		trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
		              "Initialisation of packet MMAP failed: %s",
		              error);
		linuxcommon_close_input_stream(libtrace, stream);
		return -1;
	}

	return 0;
}

static int linuxring_start_input(libtrace_t *libtrace)
{
	int ret = linuxring_start_input_stream(libtrace, FORMAT_DATA_FIRST);
	if (ret != 0) {
		libtrace_list_deinit(FORMAT_DATA->per_stream);
		free(libtrace->format_data);
		libtrace->format_data = NULL;
	}
	return ret;
}

static int linuxring_pstart_input(libtrace_t *libtrace) {
	return linuxcommon_pstart_input(libtrace, linuxring_start_input_stream);
}

static int linuxring_start_output(libtrace_out_t *libtrace)
{
	char error[2048];
	FORMAT_DATA_OUT->fd = socket(PF_PACKET, SOCK_RAW, 0);
	if (FORMAT_DATA_OUT->fd==-1) {
		free(FORMAT_DATA_OUT);
		trace_set_err_out(libtrace, errno, "Failed to create raw socket");
		return -1;
	}

	/* Make it a packetmmap */
	if(socket_to_packetmmap(libtrace->uridata, PACKET_TX_RING,
				FORMAT_DATA_OUT->fd,
				&FORMAT_DATA_OUT->req,
				&FORMAT_DATA_OUT->tx_ring,
				&FORMAT_DATA_OUT->max_order,
				error) != 0) {
		trace_set_err_out(libtrace, TRACE_ERR_INIT_FAILED,
				  "Initialisation of packet MMAP failed: %s",
				  error);
		close(FORMAT_DATA_OUT->fd);
		free(FORMAT_DATA_OUT);
		libtrace->format_data = NULL;
		return -1;
	}

	FORMAT_DATA_OUT->sock_hdr.sll_family = AF_PACKET;
	FORMAT_DATA_OUT->sock_hdr.sll_protocol = 0;
	FORMAT_DATA_OUT->sock_hdr.sll_ifindex =
		if_nametoindex(libtrace->uridata);
	FORMAT_DATA_OUT->sock_hdr.sll_hatype = 0;
	FORMAT_DATA_OUT->sock_hdr.sll_pkttype = 0;
	FORMAT_DATA_OUT->sock_hdr.sll_halen = 0;
	FORMAT_DATA_OUT->queue = 0;

	return 0;
}

static int linuxring_fin_output(libtrace_out_t *libtrace)
{
	/* Make sure any remaining frames get sent */
	sendto(FORMAT_DATA_OUT->fd,
	       NULL,
	       0,
	       0,
	       (void *) &FORMAT_DATA_OUT->sock_hdr,
	       sizeof(FORMAT_DATA_OUT->sock_hdr));

	/* Unmap our data area */
	munmap(FORMAT_DATA_OUT->tx_ring,
	       FORMAT_DATA_OUT->req.tp_block_size *
	       FORMAT_DATA_OUT->req.tp_block_nr);

	/* Free the socket */
	close(FORMAT_DATA_OUT->fd);
	FORMAT_DATA_OUT->fd=-1;
	free(libtrace->format_data);
	return 0;
}

static libtrace_linktype_t
linuxring_get_link_type(const struct libtrace_packet_t *packet)
{
	uint16_t linktype = GET_SOCKADDR_HDR(packet->buffer)->sll_hatype;
	return linuxcommon_get_link_type(linktype);
}

static libtrace_direction_t
linuxring_get_direction(const struct libtrace_packet_t *packet) {
	return linuxcommon_get_direction(GET_SOCKADDR_HDR(packet->buffer)->
	                                 sll_pkttype);
}

static libtrace_direction_t
linuxring_set_direction(libtrace_packet_t *packet,
			libtrace_direction_t direction) {
	return linuxcommon_set_direction(GET_SOCKADDR_HDR(packet->buffer), direction);
}

static struct timeval linuxring_get_timeval(const libtrace_packet_t *packet)
{
	struct timeval tv;
	tv.tv_sec = TO_TP_HDR2(packet->buffer)->tp_sec;
	tv.tv_usec = TO_TP_HDR2(packet->buffer)->tp_nsec / 1000;
	return tv;
}

static struct timespec linuxring_get_timespec(const libtrace_packet_t *packet)
{
	struct timespec ts;
	ts.tv_sec = TO_TP_HDR2(packet->buffer)->tp_sec;
	ts.tv_nsec = TO_TP_HDR2(packet->buffer)->tp_nsec;
	return ts;
}

static int linuxring_get_capture_length(const libtrace_packet_t *packet)
{
	return TO_TP_HDR2(packet->buffer)->tp_snaplen;
}

static int linuxring_get_wire_length(const libtrace_packet_t *packet)
{
	int wirelen = TO_TP_HDR2(packet->buffer)->tp_len;

	/* Include the missing FCS */
	if (trace_get_link_type(packet) == TRACE_TYPE_ETH)
		wirelen += 4;

	return wirelen;
}

static int linuxring_get_framing_length(const libtrace_packet_t *packet)
{
	/*
	 * Need to make frame_length + capture_length = complete capture length
	 * so include alignment whitespace. So reverse calculate from packet.
	 */
	return (char *)packet->payload - (char *)packet->buffer;
}

static size_t linuxring_set_capture_length(libtrace_packet_t *packet,
					   size_t size)
{
	assert(packet);
	if (size > trace_get_capture_length(packet)) {
		/* We should avoid making a packet larger */
		return trace_get_capture_length(packet);
	}

	/* Reset the cached capture length */
	packet->capture_length = -1;

	TO_TP_HDR2(packet->buffer)->tp_snaplen = size;

	return trace_get_capture_length(packet);
}

static int linuxring_prepare_packet(libtrace_t *libtrace UNUSED,
				    libtrace_packet_t *packet, void *buffer,
				    libtrace_rt_types_t rt_type, uint32_t flags)
{
	if (packet->buffer != buffer &&
	    packet->buf_control == TRACE_CTRL_PACKET) {
		free(packet->buffer);
	}

	if ((flags & TRACE_PREP_OWN_BUFFER) == TRACE_PREP_OWN_BUFFER)
		packet->buf_control = TRACE_CTRL_PACKET;
	else
		packet->buf_control = TRACE_CTRL_EXTERNAL;


	packet->buffer = buffer;
	packet->header = buffer;
	packet->payload = (char *)buffer +
		TP_TRACE_START(TO_TP_HDR2(packet->header)->tp_mac,
			       TO_TP_HDR2(packet->header)->tp_net,
			       TPACKET2_HDRLEN);
	packet->type = rt_type;

	return 0;
}
#define LIBTRACE_MIN(a,b) ((a)<(b) ? (a) : (b))
inline static int linuxring_read_stream(libtrace_t *libtrace,
                                        libtrace_packet_t *packet,
                                        struct linux_per_stream_t *stream,
                                        libtrace_message_queue_t *queue) {

	struct tpacket2_hdr *header;
	int ret;
	unsigned int snaplen;
	struct pollfd pollset[2];

	ring_release_frame(libtrace, packet);
	
	packet->buf_control = TRACE_CTRL_EXTERNAL;
	packet->type = TRACE_RT_DATA_LINUX_RING;
	
	/* Fetch the current frame */
	header = GET_CURRENT_BUFFER(libtrace, stream);
	assert((((unsigned long) header) & (pagesize - 1)) == 0);

	/* TP_STATUS_USER means that we can use the frame.
	 * When a slot does not have this flag set, the frame is not
	 * ready for consumption.
	 */
	while (!(header->tp_status & TP_STATUS_USER)) {
		pollset[0].fd = stream->fd;
		pollset[0].events = POLLIN;
		pollset[0].revents = 0;
		if (queue) {
			pollset[1].fd = libtrace_message_queue_get_fd(queue);
			pollset[1].events = POLLIN;
			pollset[1].revents = 0;
		}
		/* Wait for more data or a message*/
		ret = poll(pollset, (queue ? 2 : 1), -1);
		if (ret > 0) {
			if (pollset[0].revents)
				continue;
			else
				return READ_MESSAGE;
		} else if (ret < 0) {
			if (errno != EINTR)
			trace_set_err(libtrace,errno,"poll()");
			return -1;
		} else {
			/* Poll timed out - check if we should exit */
			if (libtrace_halt)
				return 0;
			continue;
		}
	}

	packet->buffer = header;

	/* If a snaplen was configured, automatically truncate the packet to
	 * the desired length.
	 */
	snaplen=LIBTRACE_MIN(
			(int)LIBTRACE_PACKET_BUFSIZE-(int)sizeof(*header),
			(int)FORMAT_DATA->snaplen);
	
	TO_TP_HDR2(packet->buffer)->tp_snaplen = LIBTRACE_MIN((unsigned int)snaplen, TO_TP_HDR2(packet->buffer)->tp_len);

	/* Move to next buffer */
  	stream->rxring_offset++;
	stream->rxring_offset %= FORMAT_DATA->req.tp_frame_nr;

	/* We just need to get prepare_packet to set all our packet pointers
	 * appropriately */
	if (linuxring_prepare_packet(libtrace, packet, packet->buffer,
				packet->type, 0))
		return -1;
	return  linuxring_get_framing_length(packet) + 
				linuxring_get_capture_length(packet);

}

static int linuxring_read_packet(libtrace_t *libtrace, libtrace_packet_t *packet) {
	return linuxring_read_stream(libtrace, packet, FORMAT_DATA_FIRST, NULL);
}

static int linuxring_pread_packets(libtrace_t *libtrace,
                                   libtrace_thread_t *t,
                                   libtrace_packet_t *packets[],
                                   UNUSED size_t nb_packets) {
	/* For now just read one packet */
	packets[0]->error = linuxring_read_stream(libtrace, packets[0],
	                                          t->format_data, &t->messages);
	if (packets[0]->error >= 1)
		return 1;
	else
		return packets[0]->error;
}

/* Non-blocking read */
static libtrace_eventobj_t linuxring_event(libtrace_t *libtrace,
					   libtrace_packet_t *packet)
{
	struct tpacket2_hdr *header;
	libtrace_eventobj_t event = {0,0,0.0,0};

	/* We must free the old packet, otherwise select() will instantly
	 * return */
	ring_release_frame(libtrace, packet);

	/* Fetch the current frame */
	header = GET_CURRENT_BUFFER(libtrace, FORMAT_DATA_FIRST);
	if (header->tp_status & TP_STATUS_USER) {
		/* We have a frame waiting */
		event.size = trace_read_packet(libtrace, packet);
		event.type = TRACE_EVENT_PACKET;
	} else {
		/* Ok we don't have a packet waiting */
		event.type = TRACE_EVENT_IOWAIT;
		event.fd = FORMAT_DATA_FIRST->fd;
	}

	return event;
}

/**
 * Free any resources being kept for this packet, Note: libtrace
 * will ensure all fields are zeroed correctly.
 */
static void linuxring_fin_packet(libtrace_packet_t *packet)
{
	libtrace_t *libtrace = packet->trace;

	if (packet->buffer == NULL)
		return;
	assert(packet->trace);

	/* If we own the packet (i.e. it's not a copy), we need to free it */
	if (packet->buf_control == TRACE_CTRL_EXTERNAL) {
		/* Started should always match the existence of the rx_ring
		 * in the parallel case still just check the first ring */
		assert(!!FORMAT_DATA_FIRST->rx_ring ==
		       !!packet->trace->started);
		/* If we don't have a ring its already been destroyed */
		if (FORMAT_DATA_FIRST->rx_ring != MAP_FAILED)
			ring_release_frame(packet->trace, packet);
		else
			packet->buffer = NULL;
	}
}

static int linuxring_write_packet(libtrace_out_t *libtrace,
				  libtrace_packet_t *packet)
{
	struct tpacket2_hdr *header;
	struct pollfd pollset;
	struct socket_addr;
	int ret;
	unsigned max_size;
	void * off;

	if (trace_get_link_type(packet) == TRACE_TYPE_NONDATA)
		return 0;

	max_size = FORMAT_DATA_OUT->req.tp_frame_size -
		TPACKET2_HDRLEN + sizeof(struct sockaddr_ll);

	header = (void *)FORMAT_DATA_OUT->tx_ring +
		(FORMAT_DATA_OUT->txring_offset *
		 FORMAT_DATA_OUT->req.tp_frame_size);

	while(header->tp_status != TP_STATUS_AVAILABLE) {
		/* if none available: wait on more data */
		pollset.fd = FORMAT_DATA_OUT->fd;
		pollset.events = POLLOUT;
		pollset.revents = 0;
		ret = poll(&pollset, 1, 1000);
		if (ret < 0 && errno != EINTR) {
			perror("poll");
			return -1;
		}
		if(ret == 0)
			/* Timeout something has gone wrong - maybe the queue is
			 * to large so try issue another send command
			 */
			ret = sendto(FORMAT_DATA_OUT->fd,
				     NULL,
				     0,
				     0,
				     (void *)&FORMAT_DATA_OUT->sock_hdr,
				     sizeof(FORMAT_DATA_OUT->sock_hdr));
			if (ret < 0) {
				trace_set_err_out(libtrace, errno,
						  "sendto after timeout "
						  "failed");
				return -1;
			}
	}

	header->tp_len = trace_get_capture_length(packet);

	/* We cannot write the whole packet so just write part of it */
	if (header->tp_len > max_size)
		header->tp_len = max_size;

	/* Fill packet - no sockaddr_ll in header when writing to the TX_RING */
	off = ((void *)header) + (TPACKET2_HDRLEN - sizeof(struct sockaddr_ll));
	memcpy(off, (char *)packet->payload, header->tp_len);

	/* 'Send it' and increase ring pointer to the next frame */
	header->tp_status = TP_STATUS_SEND_REQUEST;
	FORMAT_DATA_OUT->txring_offset = (FORMAT_DATA_OUT->txring_offset + 1) %
		FORMAT_DATA_OUT->req.tp_frame_nr;

	/* Notify kernel there are frames to send */
	FORMAT_DATA_OUT->queue ++;
	FORMAT_DATA_OUT->queue %= TX_MAX_QUEUE;
	if(FORMAT_DATA_OUT->queue == 0){
		ret = sendto(FORMAT_DATA_OUT->fd,
				NULL,
				0,
				MSG_DONTWAIT,
				(void *)&FORMAT_DATA_OUT->sock_hdr,
				sizeof(FORMAT_DATA_OUT->sock_hdr));
		if (ret < 0) {
			trace_set_err_out(libtrace, errno, "sendto failed");
			return -1;
		}
	}
	return header->tp_len;

}

#ifdef HAVE_NETPACKET_PACKET_H

static void linuxring_help(void)
{
	printf("linuxring format module: $Revision: 1793 $\n");
	printf("Supported input URIs:\n");
	printf("\tring:eth0\n");
	printf("\n");
	printf("Supported output URIs:\n");
	printf("\tring:eth0\n");
	printf("\n");
	return;
}

static struct libtrace_format_t linuxring = {
	"ring",
	"$Id$",
	TRACE_FORMAT_LINUX_RING,
	linuxcommon_probe_filename,	/* probe filename */
	NULL,				/* probe magic */
	linuxcommon_init_input,	 	/* init_input */
	linuxcommon_config_input,	/* config_input */
	linuxring_start_input,		/* start_input */
	linuxcommon_pause_input,	/* pause_input */
	linuxcommon_init_output,	/* init_output */
	NULL,				/* config_output */
	linuxring_start_output,		/* start_ouput */
	linuxcommon_fin_input,		/* fin_input */
	linuxring_fin_output,		/* fin_output */
	linuxring_read_packet,		/* read_packet */
	linuxring_prepare_packet,	/* prepare_packet */
	linuxring_fin_packet,		/* fin_packet */
	linuxring_write_packet,		/* write_packet */
	linuxring_get_link_type,	/* get_link_type */
	linuxring_get_direction,	/* get_direction */
	linuxring_set_direction,	/* set_direction */
	NULL,				/* get_erf_timestamp */
	linuxring_get_timeval,		/* get_timeval */
	linuxring_get_timespec,		/* get_timespec */
	NULL,				/* get_seconds */
	NULL,				/* seek_erf */
	NULL,				/* seek_timeval */
	NULL,				/* seek_seconds */
	linuxring_get_capture_length,	/* get_capture_length */
	linuxring_get_wire_length,	/* get_wire_length */
	linuxring_get_framing_length,	/* get_framing_length */
	linuxring_set_capture_length,	/* set_capture_length */
	NULL,				/* get_received_packets */
	linuxcommon_get_filtered_packets,/* get_filtered_packets */
	linuxcommon_get_dropped_packets,/* get_dropped_packets */
	linuxcommon_get_captured_packets,/* get_captured_packets */
	linuxcommon_get_fd,		/* get_fd */
	linuxring_event,		/* trace_event */
	linuxring_help,			/* help */
	NULL,				/* next pointer */
	{true, -1},			/* Live, no thread limit */
	linuxring_pstart_input,		/* pstart_input */
	linuxring_pread_packets,	/* pread_packets */
	linuxcommon_pause_input,	/* ppause */
	linuxcommon_fin_input,		/* p_fin */
        linuxcommon_pconfig_input,	/* pconfig input */
	linuxcommon_pregister_thread,
	NULL
};
#else

static void linuxring_help(void)
{
	printf("linuxring format module: $Revision: 1793 $\n");
	printf("Not supported on this host\n");
}

static struct libtrace_format_t linuxring = {
	"ring",
	"$Id$",
	TRACE_FORMAT_LINUX_RING,
	NULL,				/* probe filename */
	NULL,				/* probe magic */
	NULL,	 			/* init_input */
	NULL,				/* config_input */
	NULL,				/* start_input */
	NULL,				/* pause_input */
	NULL,				/* init_output */
	NULL,				/* config_output */
	NULL,				/* start_ouput */
	NULL,				/* fin_input */
	NULL,				/* fin_output */
	NULL,				/* read_packet */
	linuxring_prepare_packet,	/* prepare_packet */
	NULL,				/* fin_packet */
	NULL,				/* write_packet */
	linuxring_get_link_type,	/* get_link_type */
	linuxring_get_direction,	/* get_direction */
	linuxring_set_direction,	/* set_direction */
	NULL,				/* get_erf_timestamp */
	linuxring_get_timeval,		/* get_timeval */
	linuxring_get_timespec,		/* get_timespec */
	NULL,				/* get_seconds */
	NULL,				/* seek_erf */
	NULL,				/* seek_timeval */
	NULL,				/* seek_seconds */
	linuxring_get_capture_length,	/* get_capture_length */
	linuxring_get_wire_length,	/* get_wire_length */
	linuxring_get_framing_length,	/* get_framing_length */
	linuxring_set_capture_length,	/* set_capture_length */
	NULL,				/* get_received_packets */
	NULL,				/* get_filtered_packets */
	NULL,				/* get_dropped_packets */
	NULL,				/* get_captured_packets */
	NULL,				/* get_fd */
	NULL,				/* trace_event */
	linuxring_help,			/* help */
	NULL,				/* next pointer */
	NON_PARALLEL(true)
};
#endif

/* TODO: Figure out how to give this format preference over the linux native
 * formate if the user only specifies an interface */
void linuxring_constructor(void)
{
	register_format(&linuxring);
}
