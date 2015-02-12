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

/* This file contains the common functions used by both the ring and int
 * formats.
 *
 * Typically these deal with the socket descriptor or common conversions.
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

int linuxcommon_probe_filename(const char *filename)
{
	/* Is this an interface? */
	return (if_nametoindex(filename) != 0);
}

/* Compiles a libtrace BPF filter for use with a linux native socket */
static int linuxnative_configure_bpf(libtrace_t *libtrace,
		libtrace_filter_t *filter) {
#ifdef HAVE_LIBPCAP
	struct ifreq ifr;
	unsigned int arphrd;
	libtrace_dlt_t dlt;
	libtrace_filter_t *f;
	int sock;
	pcap_t *pcap;

	/* Take a copy of the filter object as it was passed in */
	f = (libtrace_filter_t *) malloc(sizeof(libtrace_filter_t));
	memcpy(f, filter, sizeof(libtrace_filter_t));

	/* If we are passed a filter with "flag" set to zero, then we must
	 * compile the filterstring before continuing. This involves
	 * determining the linktype, passing the filterstring to libpcap to
	 * compile, and saving the result for trace_start() to push into the
	 * kernel.
	 * If flag is set to one, then the filter was probably generated using
	 * trace_create_filter_from_bytecode() and so we don't need to do
	 * anything (we've just copied it above).
	 */
	if (f->flag == 0) {
		sock = socket(PF_INET, SOCK_STREAM, 0);
		memset(&ifr, 0, sizeof(struct ifreq));
		strncpy(ifr.ifr_name, libtrace->uridata, IF_NAMESIZE);
		if (ioctl(sock, SIOCGIFHWADDR, &ifr) != 0) {
			perror("Can't get HWADDR for interface");
			return -1;
		}
		close(sock);

		arphrd = ifr.ifr_hwaddr.sa_family;
		dlt = libtrace_to_pcap_dlt(arphrd_type_to_libtrace(arphrd));

		pcap = pcap_open_dead(dlt,
				FORMAT_DATA->snaplen);

		if (pcap_compile(pcap, &f->filter, f->filterstring, 0, 0) == -1) {
			/* Filter didn't compile, set flag to 0 so we can
			 * detect this when trace_start() is called and
			 * produce a useful error
			 */
			f->flag = 0;
			trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
			              "Failed to compile BPF filter (%s): %s",
			              f->filterstring, pcap_geterr(pcap));
		} else {
			/* Set the "flag" to indicate that the filterstring
			 * has been compiled
			 */
			f->flag = 1;
		}

		pcap_close(pcap);

	}

	if (FORMAT_DATA->filter != NULL)
		free(FORMAT_DATA->filter);

	FORMAT_DATA->filter = f;

	return 0;
#else
	return -1
#endif
}

int linuxcommon_config_input(libtrace_t *libtrace,
		trace_option_t option,
		void *data)
{
	switch(option) {
		case TRACE_OPTION_SNAPLEN:
			FORMAT_DATA->snaplen=*(int*)data;
			return 0;
		case TRACE_OPTION_PROMISC:
			FORMAT_DATA->promisc=*(int*)data;
			return 0;
		case TRACE_OPTION_FILTER:
		 	return linuxnative_configure_bpf(libtrace,
					(libtrace_filter_t *) data);
		case TRACE_OPTION_META_FREQ:
			/* No meta-data for this format */
			break;
		case TRACE_OPTION_EVENT_REALTIME:
			/* Live captures are always going to be in trace time */
			break;
		/* Avoid default: so that future options will cause a warning
		 * here to remind us to implement it, or flag it as
		 * unimplementable
		 */
	}

	/* Don't set an error - trace_config will try to deal with the
	 * option and will set an error if it fails */
	return -1;
}

int linuxcommon_init_input(libtrace_t *libtrace)
{
	struct linux_per_stream_t stream_data = ZERO_LINUX_STREAM;

	libtrace->format_data = (struct linux_format_data_t *)
		malloc(sizeof(struct linux_format_data_t));
	assert(libtrace->format_data != NULL);

	FORMAT_DATA->per_stream =
		libtrace_list_init(sizeof(stream_data));
	assert(FORMAT_DATA->per_stream != NULL);

	libtrace_list_push_back(FORMAT_DATA->per_stream, &stream_data);

	FORMAT_DATA->promisc = -1;
	FORMAT_DATA->snaplen = LIBTRACE_PACKET_BUFSIZE;
	FORMAT_DATA->filter = NULL;
	FORMAT_DATA->stats_valid = 0;
	FORMAT_DATA->max_order = MAX_ORDER;
	FORMAT_DATA->fanout_flags = PACKET_FANOUT_LB;
	/* Some examples use pid for the group however that would limit a single
	 * application to use only int/ring format, instead using rand */
	FORMAT_DATA->fanout_group = (uint16_t) rand();
	return 0;
}

int linuxcommon_init_output(libtrace_out_t *libtrace)
{
	libtrace->format_data = (struct linux_format_data_out_t*)
		malloc(sizeof(struct linux_format_data_out_t));
	assert(libtrace->format_data != NULL);

	FORMAT_DATA_OUT->fd = -1;
	FORMAT_DATA_OUT->tx_ring = NULL;
	FORMAT_DATA_OUT->txring_offset = 0;
	FORMAT_DATA_OUT->queue = 0;
	FORMAT_DATA_OUT->max_order = MAX_ORDER;
	return 0;
}

/* Close an input stream, this is safe to be called part way through
 * initilisation as a cleanup function assuming streams were set to
 * ZERO_LINUX_STREAM to begin with.
 *
 * This works correctly with both int and ring
 */
void linuxcommon_close_input_stream(libtrace_t *libtrace,
                                    struct linux_per_stream_t *stream) {
	if (stream->fd != -1)
		close(stream->fd);
	stream->fd = -1;
	/* TODO maybe store size against stream XXX */
	if (stream->rx_ring != MAP_FAILED)
		munmap(stream->rx_ring,
		       FORMAT_DATA->req.tp_block_size *
		       FORMAT_DATA->req.tp_block_nr);
	stream->rx_ring = MAP_FAILED;
}

/* Start an input stream
 * - Opens the file descriptor
 * - Sets promiscuous correctly
 * - Sets socket option
 * - Add BPF filter
 *
 * The output is ready for int directly, for ring the conversion to ring still
 * needs to take place.
 */
int linuxcommon_start_input_stream(libtrace_t *libtrace,
                                   struct linux_per_stream_t *stream)
{
	struct sockaddr_ll addr;
	const int one = 1;
	memset(&addr,0,sizeof(addr));
	libtrace_filter_t *filter = FORMAT_DATA->filter;

	/* Create a raw socket for reading packets on */
	stream->fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (stream->fd==-1) {
		trace_set_err(libtrace, errno, "Could not create raw socket");
		return -1;
	}

	/* Bind to the capture interface */
	addr.sll_family = AF_PACKET;
	addr.sll_protocol = htons(ETH_P_ALL);
	if (strlen(libtrace->uridata)) {
		addr.sll_ifindex = if_nametoindex(libtrace->uridata);
		if (addr.sll_ifindex == 0) {
			linuxcommon_close_input_stream(libtrace, stream);
			trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
				      "Failed to find interface %s",
				      libtrace->uridata);
			return -1;
		}
	} else {
		addr.sll_ifindex = 0;
	}
	if (bind(stream->fd,
	         (struct sockaddr*)&addr,
	         (socklen_t)sizeof(addr))==-1) {
		linuxcommon_close_input_stream(libtrace, stream);
		trace_set_err(libtrace, errno,
			      "Failed to bind to interface %s",
			      libtrace->uridata);
		return -1;
	}

	/* If promisc hasn't been specified, set it to "true" if we're
	 * capturing on one interface, or "false" if we're capturing on
	 * all interfaces.
	 */
	if (FORMAT_DATA->promisc==-1) {
		if (addr.sll_ifindex!=0)
			FORMAT_DATA->promisc=1;
		else
			FORMAT_DATA->promisc=0;
	}

	/* Enable promiscuous mode, if requested */
	if (FORMAT_DATA->promisc) {
		struct packet_mreq mreq;
		socklen_t socklen = sizeof(mreq);
		memset(&mreq,0,sizeof(mreq));
		mreq.mr_ifindex = addr.sll_ifindex;
		mreq.mr_type = PACKET_MR_PROMISC;
		if (setsockopt(stream->fd,
			       SOL_PACKET,
			       PACKET_ADD_MEMBERSHIP,
			       &mreq,
			       socklen)==-1) {
			perror("setsockopt(PROMISC)");
		}
	}

	/* Set the timestamp option on the socket - aim for the most detailed
	 * clock resolution possible */
#ifdef SO_TIMESTAMPNS
	if (setsockopt(stream->fd,
		       SOL_SOCKET,
		       SO_TIMESTAMPNS,
		       &one,
		       (socklen_t)sizeof(one))!=-1) {
		FORMAT_DATA->timestamptype = TS_TIMESPEC;
	}
	else
	/* DANGER: This is a dangling else to only do the next setsockopt()
	 * if we fail the first! */
#endif
		if (setsockopt(stream->fd,
			       SOL_SOCKET,
			       SO_TIMESTAMP,
			       &one,
			       (socklen_t)sizeof(one))!=-1) {
			FORMAT_DATA->timestamptype = TS_TIMEVAL;
		}
		else
			FORMAT_DATA->timestamptype = TS_NONE;

	/* Push BPF filter into the kernel. At this stage we can safely assume
	 * that the filterstring has been compiled, or the filter was supplied
	 * pre-compiled.
	 */
	if (filter != NULL) {
		/* Check if the filter was successfully compiled. If not,
		 * it is probably a bad filter and we should return an error
		 * before the caller tries to read any packets */
		if (filter->flag == 0) {
			linuxcommon_close_input_stream(libtrace, stream);
			trace_set_err(libtrace, TRACE_ERR_BAD_FILTER,
				      "Cannot attach a bad filter to %s",
				      libtrace->uridata);
			return -1;
		}

		if (setsockopt(stream->fd,
			       SOL_SOCKET,
			       SO_ATTACH_FILTER,
			       &filter->filter,
			       sizeof(filter->filter)) == -1) {
			perror("setsockopt(SO_ATTACH_FILTER)");
		} else {
			/* The socket accepted the filter, so we need to
			 * consume any buffered packets that were received
			 * between opening the socket and applying the filter.
			 */
			void *buf = malloc((size_t)LIBTRACE_PACKET_BUFSIZE);
			while(recv(stream->fd,
				   buf,
				   (size_t)LIBTRACE_PACKET_BUFSIZE,
				   MSG_DONTWAIT) != -1) { }
			free(buf);
		}
	}

	FORMAT_DATA->stats_valid = 0;

	return 0;
}

/**
 * Converts a socket, either packet_mmap or standard raw socket into a
 * fanout socket.
 * NOTE: This means we can read from the socket with multiple queues,
 * each must be setup (identically) and then this called upon them
 *
 * @return 0 success, -1 error
 */
inline int linuxcommon_to_packet_fanout(libtrace_t *libtrace,
                                        struct linux_per_stream_t *stream)
{
	int fanout_opt = ((int)FORMAT_DATA->fanout_flags << 16) |
	                 (int)FORMAT_DATA->fanout_group;
	if (setsockopt(stream->fd, SOL_PACKET, PACKET_FANOUT,
			&fanout_opt, sizeof(fanout_opt)) == -1) {
		trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
		              "Converting the fd to a socket fanout failed %s",
		              libtrace->uridata);
		return -1;
	}
	return 0;
}

int linuxcommon_pause_input(libtrace_t *libtrace)
{
	size_t i;

	/* Stop and detach each stream */
	for (i = 0; i < libtrace_list_get_size(FORMAT_DATA->per_stream); ++i) {
		struct linux_per_stream_t *stream;
		stream = libtrace_list_get_index(FORMAT_DATA->per_stream, i)->data;
		linuxcommon_close_input_stream(libtrace, stream);
	}

	return 0;
}

int linuxcommon_fin_input(libtrace_t *libtrace)
{
	if (libtrace->format_data) {
		if (FORMAT_DATA->filter != NULL)
			free(FORMAT_DATA->filter);

		if (FORMAT_DATA->per_stream)
			libtrace_list_deinit(FORMAT_DATA->per_stream);

		free(libtrace->format_data);
	}

	return 0;
}

int linuxcommon_pregister_thread(libtrace_t *libtrace,
                                 libtrace_thread_t *t,
                                 bool reading) {
	fprintf(stderr, "registering thread %d!!\n", t->perpkt_num);
	if (reading) {
		/* XXX TODO remove this oneday make sure hasher thread still works */
		struct linux_per_stream_t *stream;
		stream = libtrace_list_get_index(FORMAT_DATA->per_stream,
		                                 t->perpkt_num)->data;
		t->format_data = stream;
		if (!stream) {
			/* This should never happen and indicates an
			 * internal libtrace bug */
			trace_set_err(libtrace, TRACE_ERR_INIT_FAILED,
				      "Failed to attached thread %d to a stream",
				      t->perpkt_num);
			return -1;
		}
	}
	return 0;
}

int linuxcommon_pconfig_input(libtrace_t *libtrace,
                              trace_parallel_option_t option,
                              void *data)
{
	switch(option) {
		case TRACE_OPTION_SET_HASHER:
			switch (*((enum hasher_types *)data)) {
				case HASHER_BALANCE:
					// Do fanout
					FORMAT_DATA->fanout_flags = PACKET_FANOUT_LB;
					// Or we could balance to the CPU
					return 0;
				case HASHER_BIDIRECTIONAL:
				case HASHER_UNIDIRECTIONAL:
					FORMAT_DATA->fanout_flags = PACKET_FANOUT_HASH;
					return 0;
				case HASHER_CUSTOM:
				case HASHER_HARDWARE:
					return -1;
			}
			break;
		/* Avoid default: so that future options will cause a warning
		 * here to remind us to implement it, or flag it as
		 * unimplementable
		 */
	}

	/* Don't set an error - trace_config will try to deal with the
	 * option and will set an error if it fails */
	return -1;
}

static void linuxcommon_update_statistics(libtrace_t *libtrace) {
	struct tpacket_stats stats;
	size_t i;
	socklen_t len = sizeof(stats);

	for (i = 0; i < libtrace_list_get_size(FORMAT_DATA->per_stream); ++i) {
		struct linux_per_stream_t *stream;
		stream = libtrace_list_get_index(FORMAT_DATA->per_stream, i)->data;
		if (stream->fd != -1) {
			if (getsockopt(stream->fd,
			           SOL_PACKET,
			           PACKET_STATISTICS,
			           &stats,
			           &len) == 0) {
				if (FORMAT_DATA->stats_valid==0) {
					FORMAT_DATA->stats.tp_drops = stats.tp_drops;
					FORMAT_DATA->stats.tp_packets = stats.tp_packets;
					FORMAT_DATA->stats_valid = 1;
				} else {
					FORMAT_DATA->stats.tp_drops += stats.tp_drops;
					FORMAT_DATA->stats.tp_drops += stats.tp_packets;
				}
			} else {
				perror("getsockopt PACKET_STATISTICS failed");
			}
		}
	}
}

/* Number of packets that passed filtering */
uint64_t linuxcommon_get_captured_packets(libtrace_t *libtrace) {
	if (libtrace->format_data == NULL)
		return UINT64_MAX;
	if (FORMAT_DATA_FIRST->fd == -1) {
		/* This is probably a 'dead' trace so obviously we can't query
		 * the socket for capture counts, can we? */
		return UINT64_MAX;
	}
	linuxcommon_update_statistics(libtrace);
	if (FORMAT_DATA->stats_valid)
		return FORMAT_DATA->stats.tp_packets;
	else
		return UINT64_MAX;
}

/* Number of packets that got past filtering and were then dropped because
 * of lack of space.
 *
 * We could also try read from /sys/class/net/ethX/statistics/ to get
 * real drop counters and stuff.
 */
uint64_t linuxcommon_get_dropped_packets(libtrace_t *libtrace) {
	if (libtrace->format_data == NULL)
		return UINT64_MAX;
	if (FORMAT_DATA_FIRST->fd == -1) {
		/* This is probably a 'dead' trace so obviously we can't query
		 * the socket for drop counts, can we? */
		return UINT64_MAX;
	}
	linuxcommon_update_statistics(libtrace);
	if (FORMAT_DATA->stats_valid)
		return FORMAT_DATA->stats.tp_drops;
	else
		return UINT64_MAX;
}

/* Linux doesn't keep track how many packets were seen before filtering
 * so we can't tell how many packets were filtered.  Bugger.  So annoying.
 *
 * Since we tell libtrace that we do support filtering, if we don't declare
 * this here as failing, libtrace will happily report for us that it didn't
 * filter any packets, so don't lie -- return that we don't know.
 */
uint64_t linuxcommon_get_filtered_packets(libtrace_t *trace UNUSED) {
	return UINT64_MAX;
}

int linuxcommon_get_fd(const libtrace_t *libtrace) {
	if (libtrace->format_data == NULL)
		return -1;
	return FORMAT_DATA_FIRST->fd;
}

int linuxcommon_pstart_input(libtrace_t *libtrace,
                             int (*start_stream)(libtrace_t *, struct linux_per_stream_t*)) {
	int i = 0;
	int tot = libtrace->perpkt_thread_count;
	int iserror = 0;
	struct linux_per_stream_t empty_stream = ZERO_LINUX_STREAM;

	printf("Calling native pstart packet\n");
	for (i = 0; i < tot; ++i)
	{
		struct linux_per_stream_t *stream;
		/* Add storage for another stream */
		if (libtrace_list_get_size(FORMAT_DATA->per_stream) <= (size_t) i)
			libtrace_list_push_back(FORMAT_DATA->per_stream, &empty_stream);

		stream = libtrace_list_get_index(FORMAT_DATA->per_stream, i)->data;
		if (start_stream(libtrace, stream) != 0) {
			iserror = 1;
			break;
		}
		if (linuxcommon_to_packet_fanout(libtrace, stream) != 0)
		{
			iserror = 1;
			close(stream->fd);
			stream->fd = -1;
			break;
		}
	}

	if (iserror) {
		/* Free those that succeeded */
		for (i = i - 1; i >= 0; i--) {
			struct linux_per_stream_t *stream;
			stream = libtrace_list_get_index(FORMAT_DATA->per_stream, i)->data;
			linuxcommon_close_input_stream(libtrace, stream);
		}
		libtrace_list_deinit(FORMAT_DATA->per_stream);
		free(libtrace->format_data);
		libtrace->format_data = NULL;
		return -1;
	}

	return 0;
}

#else /* HAVE_NETPACKET_PACKET_H */

/* No NETPACKET - So this format is not live */
uint64_t linuxcommon_get_filtered_packets(libtrace_t *trace UNUSED) {
	return UINT64_MAX;
}

uint64_t linuxcommon_get_captured_packets(libtrace_t *trace UNUSED) {
	return UINT64_MAX;
}

uint64_t linuxcommon_get_dropped_packets(libtrace_t *trace UNUSED) {
	return UINT64_MAX;
}

#endif /* HAVE_NETPACKET_PACKET_H */


inline libtrace_direction_t linuxcommon_get_direction(uint8_t pkttype)
{
	switch (pkttype) {
		case PACKET_OUTGOING:
		case PACKET_LOOPBACK:
			return TRACE_DIR_OUTGOING;
		case PACKET_OTHERHOST:
			return TRACE_DIR_OTHER;
		default:
			return TRACE_DIR_INCOMING;
	}
}

inline libtrace_direction_t
linuxcommon_set_direction(struct sockaddr_ll * skadr,
                          libtrace_direction_t direction)
{
	switch (direction) {
		case TRACE_DIR_OUTGOING:
			skadr->sll_pkttype = PACKET_OUTGOING;
			return TRACE_DIR_OUTGOING;
		case TRACE_DIR_INCOMING:
			skadr->sll_pkttype = PACKET_HOST;
			return TRACE_DIR_INCOMING;
		case TRACE_DIR_OTHER:
			skadr->sll_pkttype = PACKET_OTHERHOST;
			return TRACE_DIR_OTHER;
		default:
			return -1;
	}
}

inline libtrace_linktype_t linuxcommon_get_link_type(uint16_t linktype)
{
	/* Convert the ARPHRD type into an appropriate libtrace link type */
	switch (linktype) {
		case LIBTRACE_ARPHRD_ETHER:
		case LIBTRACE_ARPHRD_LOOPBACK:
			return TRACE_TYPE_ETH;
		case LIBTRACE_ARPHRD_PPP:
			return TRACE_TYPE_NONE;
		case LIBTRACE_ARPHRD_IEEE80211_RADIOTAP:
			return TRACE_TYPE_80211_RADIO;
		case LIBTRACE_ARPHRD_IEEE80211:
			return TRACE_TYPE_80211;
		case LIBTRACE_ARPHRD_SIT:
		case LIBTRACE_ARPHRD_NONE:
			return TRACE_TYPE_NONE;
		default: /* shrug, beyond me! */
			printf("unknown Linux ARPHRD type 0x%04x\n",linktype);
			return (libtrace_linktype_t)~0U;
	}
}
