/*
 * Copyright (c) 2004-2007 Voltaire Inc.  All rights reserved.
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <inttypes.h>

#define __BUILD_VERSION_TAG__ 1.2.1
#include <common.h>
#include <umad.h>
#include <mad.h>

#include "ibnetdiscover.h"
#include "grouping.h"
#include "ibdiag_common.h"

static char *node_type_str[] = {
	"???",
	"ca",
	"switch",
	"router",
	"iwarp rnic"
};

static int timeout = 2000;		/* ms */
static int dumplevel = 0;
static int verbose;
static FILE *f;

char *argv0 = "ibnetdiscover";
int ibdebug;

static char *switch_map = NULL;
static FILE *switch_map_fp = NULL;

Node *nodesdist[MAXHOPS+1];     /* last is Ca list */
Node *mynode;
int maxhops_discovered = 0;

struct ChassisList *chassis = NULL;

int
get_port(Port *port, int portnum, ib_portid_t *portid)
{
	char portinfo[64];
	void *pi = portinfo;

	port->portnum = portnum;

	if (!smp_query(pi, portid, IB_ATTR_PORT_INFO, portnum, timeout))
		return -1;

	mad_decode_field(pi, IB_PORT_LID_F, &port->lid);
	mad_decode_field(pi, IB_PORT_LMC_F, &port->lmc);
	mad_decode_field(pi, IB_PORT_STATE_F, &port->state);
	mad_decode_field(pi, IB_PORT_PHYS_STATE_F, &port->physstate);

	DEBUG("portid %s portnum %d: lid %d state %d physstate %d",
		portid2str(portid), portnum, port->lid, port->state, port->physstate);
	return 1;
}
/*
 * Returns 0 if non switch node is found, 1 if switch is found, -1 if error.
 */
int
get_node(Node *node, Port *port, ib_portid_t *portid)
{
	char portinfo[64];
	char switchinfo[64];
	void *pi = portinfo, *ni = node->nodeinfo, *nd = node->nodedesc;
	void *si = switchinfo;

	if (!smp_query(ni, portid, IB_ATTR_NODE_INFO, 0, timeout))
		return -1;

	mad_decode_field(ni, IB_NODE_GUID_F, &node->nodeguid);
	mad_decode_field(ni, IB_NODE_TYPE_F, &node->type);
	mad_decode_field(ni, IB_NODE_NPORTS_F, &node->numports);
	mad_decode_field(ni, IB_NODE_DEVID_F, &node->devid);
	mad_decode_field(ni, IB_NODE_VENDORID_F, &node->vendid);
	mad_decode_field(ni, IB_NODE_SYSTEM_GUID_F, &node->sysimgguid);

	mad_decode_field(ni, IB_NODE_PORT_GUID_F, &node->portguid);
	mad_decode_field(ni, IB_NODE_LOCAL_PORT_F, &node->localport);
	port->portnum = node->localport;
	port->portguid = node->nodeguid;

	if (!smp_query(nd, portid, IB_ATTR_NODE_DESC, 0, timeout))
		return -1;

	if (!smp_query(pi, portid, IB_ATTR_PORT_INFO, 0, timeout))
		return -1;

	mad_decode_field(pi, IB_PORT_LID_F, &port->lid);
	mad_decode_field(pi, IB_PORT_LMC_F, &port->lmc);
	mad_decode_field(pi, IB_PORT_STATE_F, &port->state);
	mad_decode_field(pi, IB_PORT_PHYS_STATE_F, &port->physstate);

	if (node->type != SWITCH_NODE)
		return 0;

	node->smalid = port->lid;
	node->smalmc = port->lmc;

        if (!smp_query(si, portid, IB_ATTR_SWITCH_INFO, 0, timeout))
                node->smaenhsp0 = 0;	/* assume base SP0 */
	else {
        	mad_decode_field(si, IB_SW_ENHANCED_PORT0_F, &node->smaenhsp0);
	}

	DEBUG("portid %s: got switch node %" PRIx64 " '%s'",
	      portid2str(portid), node->nodeguid, node->nodedesc);
	return 1;
}

static int
extend_dpath(ib_dr_path_t *path, int nextport)
{
	if (path->cnt+2 >= sizeof(path->p))
		return -1;
	++path->cnt;
	if (path->cnt > maxhops_discovered)
		maxhops_discovered = path->cnt;
	path->p[path->cnt] = nextport;
	return path->cnt;
}

static void
dump_endnode(ib_portid_t *path, char *prompt, Node *node, Port *port)
{
	if (!dumplevel)
		return;

	fprintf(f, "%s -> %s %s {%016" PRIx64 "} portnum %d lid %d-%d\"%s\"\n",
		portid2str(path), prompt,
		(node->type <= IB_NODE_MAX ? node_type_str[node->type] : "???"),
		node->nodeguid, node->type == SWITCH_NODE ? 0 : port->portnum,
		port->lid, port->lid + (1 << port->lmc) - 1,
		clean_nodedesc(node->nodedesc));
}

#define HASHGUID(guid)		((uint32_t)(((uint32_t)(guid) * 101) ^ ((uint32_t)((guid) >> 32) * 103)))
#define HTSZ 137

static Node *nodestbl[HTSZ];

static Node *
find_node(Node *new)
{
	int hash = HASHGUID(new->nodeguid) % HTSZ;
	Node *node;

	for (node = nodestbl[hash]; node; node = node->htnext)
		if (node->nodeguid == new->nodeguid)
			return node;

	return NULL;
}

static Node *
create_node(Node *temp, ib_portid_t *path, int dist)
{
	Node *node;
	int hash = HASHGUID(temp->nodeguid) % HTSZ;

	node = malloc(sizeof(*node));
	if (!node)
		return NULL;

	memcpy(node, temp, sizeof(*node));
	node->dist = dist;
	node->path = *path;

	node->htnext = nodestbl[hash];
	nodestbl[hash] = node;

	if (node->type != SWITCH_NODE)
		dist = MAXHOPS; 	/* special Ca list */

	node->dnext = nodesdist[dist];
	nodesdist[dist] = node;

	return node;
}

static Port *
find_port(Node *node, Port *port)
{
	Port *old;

	for (old = node->ports; old; old = old->next)
		if (old->portnum == port->portnum)
			return old;

	return NULL;
}

static Port *
create_port(Node *node, Port *temp)
{
	Port *port;

	port = malloc(sizeof(*port));
	if (!port)
		return NULL;

	memcpy(port, temp, sizeof(*port));
	port->node = node;
	port->next = node->ports;
	node->ports = port;

	return port;
}

static void
link_ports(Node *node, Port *port, Node *remotenode, Port *remoteport)
{
	DEBUG("linking: 0x%" PRIx64 " %p->%p:%u and 0x%" PRIx64 " %p->%p:%u",
		node->nodeguid, node, port, port->portnum,
		remotenode->nodeguid, remotenode, remoteport, remoteport->portnum);
	if (port->remoteport)
		port->remoteport->remoteport = NULL;
	if (remoteport->remoteport)
		remoteport->remoteport->remoteport = NULL;
	port->remoteport = remoteport;
	remoteport->remoteport = port;
}

static int
handle_port(Node *node, Port *port, ib_portid_t *path, int portnum, int dist)
{
	Node node_buf;
	Port port_buf;
	Node *remotenode, *oldnode;
	Port *remoteport, *oldport;

	memset(&node_buf, 0, sizeof(node_buf));
	memset(&port_buf, 0, sizeof(port_buf));

	DEBUG("handle node %p port %p:%d dist %d", node, port, portnum, dist);
	if (port->physstate != 5)	/* LinkUp */
		return -1;

	if (extend_dpath(&path->drpath, portnum) < 0)
		return -1;

	if (get_node(&node_buf, &port_buf, path) < 0) {
		IBWARN("NodeInfo on %s failed, skipping port",
			portid2str(path));
		path->drpath.cnt--;	/* restore path */
		return -1;
	}

	oldnode = find_node(&node_buf);
	if (oldnode)
		remotenode = oldnode;
	else if (!(remotenode = create_node(&node_buf, path, dist + 1)))
		IBERROR("no memory");

	oldport = find_port(remotenode, &port_buf);
	if (oldport) {
		remoteport = oldport;
		if (node != remotenode || port != remoteport)
			IBWARN("port moving...");
	} else if (!(remoteport = create_port(remotenode, &port_buf)))
		IBERROR("no memory");

	dump_endnode(path, oldnode ? "known remote" : "new remote",
		     remotenode, remoteport);

	link_ports(node, port, remotenode, remoteport);

	path->drpath.cnt--;	/* restore path */
	return 0;
}

/*
 * Return 1 if found, 0 if not, -1 on errors.
 */
static int
discover(ib_portid_t *from)
{
	Node node_buf;
	Port port_buf;
	Node *node;
	Port *port;
	int i;
	int dist = 0;
	ib_portid_t *path;

	DEBUG("from %s", portid2str(from));

	memset(&node_buf, 0, sizeof(node_buf));
	memset(&port_buf, 0, sizeof(port_buf));

	if (get_node(&node_buf, &port_buf, from) < 0) {
		IBWARN("can't reach node %s", portid2str(from));
		return -1;
	}

	node = create_node(&node_buf, from, 0);
	if (!node)
		IBERROR("out of memory");

	mynode = node;

	port = create_port(node, &port_buf);
	if (!port)
		IBERROR("out of memory");

	if (node->type != SWITCH_NODE &&
	    handle_port(node, port, from, node->localport, 0) < 0)
		return 0;

	for (dist = 0; dist < MAXHOPS; dist++) {

		for (node = nodesdist[dist]; node; node = node->dnext) {

			path = &node->path;

			DEBUG("dist %d node %p", dist, node);
			dump_endnode(path, "processing", node, port);

			for (i = 1; i <= node->numports; i++) {
				if (i == node->localport)
					continue;

				if (get_port(&port_buf, i, path) < 0) {
					IBWARN("can't reach node %s port %d", portid2str(path), i);
					return 0;
				}

				port = find_port(node, &port_buf);
				if (port)
					continue;

				port = create_port(node, &port_buf);
				if (!port)
					IBERROR("out of memory");

				/* If switch, set port GUID to node GUID */
				if (node->type == SWITCH_NODE)
					port->portguid = node->portguid;

				handle_port(node, port, path, i, dist);
			}
		}
	}

	return 0;
}

char *
node_name(Node *node)
{
	static char buf[256];

	switch(node->type) {
	case SWITCH_NODE:
		sprintf(buf, "\"%s", "S");
		break;
	case CA_NODE:
		sprintf(buf, "\"%s", "H");
		break;
	case ROUTER_NODE:
		sprintf(buf, "\"%s", "R");
		break;
	default:
		sprintf(buf, "\"%s", "?");
		break;
	}
	sprintf(buf+2, "-%016" PRIx64 "\"", node->nodeguid);

	return buf;
}

void
list_node(Node *node)
{
	char *node_type;
	char *nodename = NULL;

	if (node->type == SWITCH_NODE)
		nodename = lookup_switch_name(switch_map_fp, node->nodeguid,
						node->nodedesc);
	else
		nodename = clean_nodedesc(node->nodedesc);
	switch(node->type) {
	case SWITCH_NODE:
		node_type = "Switch";
		break;
	case CA_NODE:
		node_type = "Ca";
		break;
	case ROUTER_NODE:
		node_type = "Router";
		break;
	default:
		node_type = "???";
		break;
	}
	fprintf(f, "%s\t : 0x%016" PRIx64 " ports %d devid 0x%x vendid 0x%x \"%s\"\n",
		node_type,
		node->nodeguid, node->numports, node->devid, node->vendid,
		nodename);

	if (nodename && (node->type == SWITCH_NODE))
		free(nodename);
}

void
out_ids(Node *node)
{
	fprintf(f, "\nvendid=0x%x\ndevid=0x%x\n", node->vendid, node->devid);
	if (node->sysimgguid)
		fprintf(f, "sysimgguid=0x%" PRIx64 "\n", node->sysimgguid);
}

void
out_chassis(int chassisnum)
{
	uint64_t guid;

	fprintf(f, "\nChassis %d", chassisnum);
	guid = get_chassis_guid(chassisnum);
	if (guid)
		fprintf(f, " (guid 0x%" PRIx64 ")", guid);
	fprintf(f, "\n");
}

void
out_switch(Node *node, int group)
{
	char *str;
	char *nodename = NULL;

	out_ids(node);
	fprintf(f, "switchguid=0x%" PRIx64, node->nodeguid);
	if (group) {
		if (node->chrecord) {
			if (node->chrecord->chassisnum) {
				fprintf(f, "\t\t# Chassis %d ", node->chrecord->chassisnum);
				/* Currently, only if Voltaire chassis */
				if (node->vendid == VTR_VENDOR_ID) {
					str = get_chassis_type(node->chrecord->chassistype);
					if (str)
						fprintf(f, "%s ", str);
					str = get_chassis_slot(node->chrecord->chassisslot);
					if (str)
						fprintf(f, "%s ", str);
					fprintf(f, "%d Chip %d", node->chrecord->slotnum, node->chrecord->anafanum);
				}
			}
		}
	}

	if (node->type == SWITCH_NODE)
		nodename = lookup_switch_name(switch_map_fp, node->nodeguid,
				node->nodedesc);
	else
		nodename = clean_nodedesc(node->nodedesc);
	fprintf(f, "\nSwitch\t%d %s\t\t# \"%s\" %s port 0 lid %d lmc %d\n",
		node->numports, node_name(node),
		nodename,
		node->smaenhsp0 ? "enhanced" : "base",
		node->smalid, node->smalmc);
	if (nodename && (node->type == SWITCH_NODE))
		free(nodename);
}

void
out_ca(Node *node)
{
	char *node_type;
	char *node_type2;

	out_ids(node);
	switch(node->type) {
	case CA_NODE:
		node_type = "ca";
		node_type2 = "Ca";
		break;
	case ROUTER_NODE:
		node_type = "rt";
		node_type2 = "Rt";
		break;
	default:
		node_type = "???";
		node_type2 = "???";
		break;
	}

	fprintf(f, "%sguid=0x%" PRIx64 "\n", node_type, node->nodeguid);
	fprintf(f, "%s\t%d %s\t\t# \"%s\"\n",
		node_type2, node->numports, node_name(node),
		clean_nodedesc(node->nodedesc));
}

static char *
out_ext_port(Port *port, int group)
{
	char *str = NULL;

	if (group) {
		if (port->node->chrecord && port->node->vendid == VTR_VENDOR_ID) {
			/* Currently, only if Voltaire chassis */
			str = portmapstring(port);
		}
	}
	return (str);
}

void
out_switch_port(Port *port, int group)
{
	char *ext_port_str = NULL;
	char *rem_nodename = NULL;

	DEBUG("port %p:%d remoteport %p", port, port->portnum, port->remoteport);
	fprintf(f, "[%d]", port->portnum);

	ext_port_str = out_ext_port(port, group);
	if (ext_port_str)
		fprintf(f, "%s", ext_port_str);

	if (port->remoteport->node->type == SWITCH_NODE)
		rem_nodename = lookup_switch_name(switch_map_fp,
				port->remoteport->node->nodeguid,
				port->remoteport->node->nodedesc);
	else
		rem_nodename = clean_nodedesc(port->remoteport->node->nodedesc);

	ext_port_str = out_ext_port(port->remoteport, group);
	fprintf(f, "\t%s[%d]%s\t\t# \"%s\" lid %d\n",
		node_name(port->remoteport->node),
		port->remoteport->portnum,
		ext_port_str ? ext_port_str : "",
		rem_nodename,
		port->remoteport->node->type == SWITCH_NODE ? port->remoteport->node->smalid : port->remoteport->lid);

	if (rem_nodename && (port->remoteport->node->type == SWITCH_NODE))
		free(rem_nodename);
}

void
out_ca_port(Port *port, int group)
{
	char *str = NULL;
	char *rem_nodename = NULL;

	fprintf(f, "[%d]\t%s[%d]", port->portnum,
		node_name(port->remoteport->node),
		port->remoteport->portnum);
	str = out_ext_port(port->remoteport, group);
	if (str)
		fprintf(f, "%s", str);

	if (port->remoteport->node->type == SWITCH_NODE)
		rem_nodename = lookup_switch_name(switch_map_fp,
				port->remoteport->node->nodeguid,
				port->remoteport->node->nodedesc);
	else
		rem_nodename = clean_nodedesc(port->remoteport->node->nodedesc);
	fprintf(f, "\t\t# lid %d lmc %d \"%s\" lid %d\n",
		port->lid, port->lmc, rem_nodename,
		port->remoteport->node->type == SWITCH_NODE ? port->remoteport->node->smalid : port->remoteport->lid);
	if (rem_nodename && (port->remoteport->node->type == SWITCH_NODE))
		free(rem_nodename);
}

int
dump_topology(int listtype, int group)
{
	Node *node;
	Port *port;
	int i = 0, dist = 0;
	time_t t = time(0);

	if (!listtype) {
		fprintf(f, "#\n# Topology file: generated on %s#\n", ctime(&t));
		fprintf(f, "# Max of %d hops discovered\n", maxhops_discovered);
		fprintf(f, "# Initiated from node %016" PRIx64 " port %016" PRIx64 "\n", mynode->nodeguid, mynode->portguid);
	}

	/* Make pass on switches */
	if (group && !listtype) {
		ChassisList *ch = NULL;

		/* Chassis based switches first */
		for (ch = chassis; ch; ch = ch->next) {
			int n = 0;

			if (!ch->chassisnum)
				continue;
			out_chassis(ch->chassisnum);
			fprintf(f, "\n# Spine Nodes");
			for (n = 1; n <= (SPINES_MAX_NUM+1); n++) {
				if (ch->spinenode[n]) {
					out_switch(ch->spinenode[n], group);
					for (port = ch->spinenode[n]->ports; port; port = port->next, i++)
						if (port->remoteport)
							out_switch_port(port, group);
				}
			}
			fprintf(f, "\n# Line Nodes");
			for (n = 1; n <= (LINES_MAX_NUM+1); n++) {
				if (ch->linenode[n]) {
					out_switch(ch->linenode[n], group);
					for (port = ch->linenode[n]->ports; port; port = port->next, i++)
						if (port->remoteport)
							out_switch_port(port, group);
				}
			}

		}

		for (dist = 0; dist <= maxhops_discovered; dist++) {

			for (node = nodesdist[dist]; node; node = node->dnext) {

				/* Non Voltaire chassis */
				if (node->vendid == VTR_VENDOR_ID)
					continue;
				if (node->chrecord) {
					if (!node->chrecord->chassisnum)
						continue;
				} else
					continue;

				out_switch(node, group);
				for (port = node->ports; port; port = port->next, i++)
					if (port->remoteport)
						out_switch_port(port, group);

			}
		}

	} else {
		for (dist = 0; dist <= maxhops_discovered; dist++) {

			for (node = nodesdist[dist]; node; node = node->dnext) {

				DEBUG("SWITCH: dist %d node %p", dist, node);
				if (!listtype) {
					out_switch(node, group);
				} else {
					if (listtype & SWITCH_NODE)
						list_node(node);
					continue;
				}

				for (port = node->ports; port; port = port->next, i++)
					if (port->remoteport)
						out_switch_port(port, group);
			}
		}
	}

	if (group && !listtype) {

		fprintf(f, "\nNon-Chassis Nodes\n");

		for (dist = 0; dist <= maxhops_discovered; dist++) {

			for (node = nodesdist[dist]; node; node = node->dnext) {

				DEBUG("SWITCH: dist %d node %p", dist, node);
				/* Now, skip chassis based switches */
				if (node->chrecord)
					if (node->chrecord->chassisnum)
						continue;
				out_switch(node, group);

				for (port = node->ports; port; port = port->next, i++)
					if (port->remoteport)
						out_switch_port(port, group);
			}

		}

	}

	/* Make pass on CAs */
	for (node = nodesdist[MAXHOPS]; node; node = node->dnext) {

		DEBUG("CA: dist %d node %p", dist, node);
		if (!listtype)
			out_ca(node);
		else {
			if (listtype & CA_NODE)
				list_node(node);
			continue;
		}

		for (port = node->ports; port; port = port->next, i++)
			if (port->remoteport)
				out_ca_port(port, group);
	}

	return i;
}

void
usage(void)
{
	fprintf(stderr, "Usage: %s [-d(ebug)] -e(rr_show) -v(erbose) -s(how) -l(ist) -g(rouping) -H(ca_list) -S(witch_list) -V(ersion) -C ca_name -P ca_port "
			"-t(imeout) timeout_ms --switch-map switch-map] [<topology-file>]\n",
			argv0);
	fprintf(stderr, "       --switch-map <switch-map> specify a switch-map file\n");
	exit(-1);
}

int
main(int argc, char **argv)
{
	int mgmt_classes[2] = {IB_SMI_CLASS, IB_SMI_DIRECT_CLASS};
	ib_portid_t my_portid = {0};
	int udebug = 0, list = 0;
	char *ca = 0;
	int ca_port = 0;
	int group = 0;

	static char const str_opts[] = "C:P:t:devslgHSVhu";
	static const struct option long_opts[] = {
		{ "C", 1, 0, 'C'},
		{ "P", 1, 0, 'P'},
		{ "debug", 0, 0, 'd'},
		{ "err_show", 0, 0, 'e'},
		{ "verbose", 0, 0, 'v'},
		{ "show", 0, 0, 's'},
		{ "list", 0, 0, 'l'},
		{ "grouping", 0, 0, 'g'},
		{ "Hca_list", 0, 0, 'H'},
		{ "Switch_list", 0, 0, 'S'},
		{ "timeout", 1, 0, 't'},
		{ "switch-map", 1, 0, 1},
		{ "Version", 0, 0, 'V'},
		{ "help", 0, 0, 'h'},
		{ "usage", 0, 0, 'u'},
		{ }
	};

	f = stdout;

	argv0 = argv[0];

	while (1) {
		int ch = getopt_long(argc, argv, str_opts, long_opts, NULL);
		if ( ch == -1 )
			break;
		switch(ch) {
		case 1:
			switch_map = strdup(optarg);
			break;
		case 'C':
			ca = optarg;
			break;
		case 'P':
			ca_port = strtoul(optarg, 0, 0);
			break;
		case 'd':
			ibdebug++;
			madrpc_show_errors(1);
			umad_debug(udebug);
			udebug++;
			break;
		case 't':
			timeout = strtoul(optarg, 0, 0);
			break;
		case 'v':
			verbose++;
			dumplevel++;
			break;
		case 's':
			dumplevel = 1;
			break;
		case 'e':
			madrpc_show_errors(1);
			break;
		case 'l':
			list = CA_NODE | SWITCH_NODE;
			break;
		case 'g':
			group = 1;
			break;
		case 'S':
			list = SWITCH_NODE;
			break;
		case 'H':
			list = CA_NODE;
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

	if (argc)
		if (!(f = fopen(argv[0], "w")))
			IBERROR("can't open file %s for writing", argv[0]);

	madrpc_init(ca, ca_port, mgmt_classes, 2);
	switch_map_fp = open_switch_map(switch_map);

	if (discover(&my_portid) < 0)
		IBERROR("discover");

	if (group)
		chassis = group_nodes();

	dump_topology(list, group);

	close_switch_map(switch_map_fp);
	exit(0);
}