/*
 * Copyright (c) 2004-2006 Voltaire Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <getopt.h>

#define __BUILD_VERSION_TAG__ 1.2
#include <common.h>
#include <umad.h>
#include <mad.h>

static uint8_t pc[1024];

static char *argv0 = "perfquery";

#define IBERROR(fmt, args...)	iberror(__FUNCTION__, fmt, ## args)

static void
iberror(const char *fn, char *msg, ...)
{
	char buf[512];
	va_list va;
	int n;

	va_start(va, msg);
	n = vsprintf(buf, msg, va);
	va_end(va);
	buf[n] = 0;

	if (ibdebug)
		printf("%s: iberror: [pid %d] %s: failed: %s\n", argv0, getpid(), fn, buf);
	else
		printf("%s: iberror: failed: %s\n", argv0, buf);

	exit(-1);
}

static void
usage(void)
{
	char *basename;

	if (!(basename = strrchr(argv0, '/')))
		basename = argv0;
	else
		basename++;

	fprintf(stderr, "Usage: %s [-d(ebug) -G(uid) -a(ll_ports) -r(eset_after_read) -C ca_name -P ca_port "
			"-R(eset_only) -t(imeout) timeout_ms -V(ersion) -h(elp)] [<lid|guid> [[port] [reset_mask]]]\n",
			basename);
	fprintf(stderr, "\tExamples:\n");
	fprintf(stderr, "\t\t%s\t\t# read local port's performance counters\n", basename);
	fprintf(stderr, "\t\t%s 32 1\t\t# read performance counters from lid 32, port 1\n", basename);
	fprintf(stderr, "\t\t%s -e 32 1\t# read extended performance counters from lid 32, port 1\n", basename);
	fprintf(stderr, "\t\t%s -a 32\t\t# read performance counters from lid 32, all ports\n", basename);
	fprintf(stderr, "\t\t%s -r 32 1\t# read performance counters and reset\n", basename);
	fprintf(stderr, "\t\t%s -e -r 32 1\t# read extended performance counters and reset\n", basename);
	fprintf(stderr, "\t\t%s -R 0x20 1\t# reset performance counters of port 1 only\n", basename);
	fprintf(stderr, "\t\t%s -e -R 0x20 1\t# reset extended performance counters of port 1 only\n", basename);
	fprintf(stderr, "\t\t%s -R -a 32\t# reset performance counters of all ports\n", basename);
	fprintf(stderr, "\t\t%s -R 32 2 0x0fff\t# reset only error counters of port 2\n", basename);
	fprintf(stderr, "\t\t%s -R 32 2 0xf000\t# reset only non-error counters of port 2\n", basename);
	exit(-1);
}

int
main(int argc, char **argv)
{
	int mgmt_classes[4] = {IB_SMI_CLASS, IB_SMI_DIRECT_CLASS, IB_SA_CLASS, IB_PERFORMANCE_CLASS};
	ib_portid_t *sm_id = 0, sm_portid = {0};
	ib_portid_t portid = {0};
	extern int ibdebug;
	int dest_type = IB_DEST_LID;
	int timeout = 0;	/* use default */
	int mask = 0xffff, all = 0;
	int reset = 0, reset_only = 0;
	int port = 0;
	char buf[1024];
	int udebug = 0;
	char *ca = 0;
	int ca_port = 0;
	int extended = 0;
	uint16_t cap_mask, *pc2;

	static char const str_opts[] = "C:P:s:t:dGearRVhu";
	static const struct option long_opts[] = {
		{ "C", 1, 0, 'C'},
		{ "P", 1, 0, 'P'},
		{ "debug", 0, 0, 'd'},
		{ "Guid", 0, 0, 'G'},
		{ "extended", 0, 0, 'e'},
		{ "all_ports", 0, 0, 'a'},
		{ "reset_after_read", 0, 0, 'r'},
		{ "Reset_only", 0, 0, 'R'},
		{ "sm_portid", 1, 0, 's'},
		{ "timeout", 1, 0, 't'},
		{ "Version", 0, 0, 'V'},
		{ "help", 0, 0, 'h'},
		{ "usage", 0, 0, 'u'},
		{ }
	};

	argv0 = argv[0];

	while (1) {
		int ch = getopt_long(argc, argv, str_opts, long_opts, NULL);
		if ( ch == -1 )
			break;
		switch(ch) {
		case 'C':
			ca = optarg;
			break;
		case 'P':
			ca_port = strtoul(optarg, 0, 0);
			break;
		case 'e':
			extended = 1;
			break;
		case 'a':
			all++;
			port = 0xff;
			break;
		case 'd':
			ibdebug++;
			madrpc_show_errors(1);
			umad_debug(udebug);
			udebug++;
			break;
		case 'G':
			dest_type = IB_DEST_GUID;
			break;
		case 's':
			if (ib_resolve_portid_str(&sm_portid, optarg, IB_DEST_LID, 0) < 0)
				IBERROR("can't resolve SM destination port %s", optarg);
			sm_id = &sm_portid;
			break;
		case 'r':
			reset++;
			break;
		case 'R':
			reset_only++;
			break;
		case 't':
			timeout = strtoul(optarg, 0, 0);
			madrpc_set_timeout(timeout);
			break;
		case 'V':
			fprintf(stderr, "%s %s\n", argv0, get_build_version() );
			exit(-1);
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 1)
		port = strtoul(argv[1], 0, 0);
	if (argc > 2)
		mask = strtoul(argv[2], 0, 0);

	madrpc_init(ca, ca_port, mgmt_classes, 4);

	if (argc) {
		if (ib_resolve_portid_str(&portid, argv[0], dest_type, sm_id) < 0)
			IBERROR("can't resolve destination port %s", argv[0]);
	} else {
		if (ib_resolve_self(&portid, &port, 0) < 0)
			IBERROR("can't resolve self port %s", argv[0]);
	}

	/* PerfMgt ClassPortInfo is a required attribute */
	if (!perf_classportinfo_query(pc, &portid, port, timeout))
		IBERROR("classportinfo query");

	if (reset_only)
		goto do_reset;

	if (extended != 1) {
		if (!port_performance_query(pc, &portid, port, timeout))
			IBERROR("perfquery");

		mad_dump_perfcounters(buf, sizeof buf, pc, sizeof pc);
	} else {
		/* Should ClassPortInfo be implemented in libibmad ? */
		pc2 = (uint16_t *)&pc[2];	/* CapabilityMask */
		cap_mask = *pc2;
		if (!(cap_mask & 0x100)) /* 1.2 errata: bit 9 is extended counter support */
			IBWARN("PerfMgt ClassPortInfo 0x%x extended counters not indicated\n", cap_mask);

		if (!port_performance_ext_query(pc, &portid, port, timeout))
			IBERROR("perfextquery");

		mad_dump_perfcounters_ext(buf, sizeof buf, pc, sizeof pc);
	}

	printf("# Port counters: %s port %d\n%s", portid2str(&portid), port, buf);

	if (!reset)
		exit(0);

do_reset:
	if (extended != 1) {
		if (!port_performance_reset(pc, &portid, port, mask, timeout))
			IBERROR("perf reset");
	} else {
		if (!port_performance_ext_reset(pc, &portid, port, mask, timeout))
			IBERROR("perf ext reset");
	}

	exit(0);
}