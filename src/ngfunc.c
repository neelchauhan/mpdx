
/*
 * ngfunc.c
 *
 * Written by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1995-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 *
 * TCP MSSFIX contributed by Sergey Korolew <dsATbittu.org.ru>
 *
 * Routines for doing netgraph stuff
 *
 */

#include "defs.h"
#include "ppp.h"
#include "bund.h"
#include "ngfunc.h"
#include "input.h"
#include "ccp.h"
#include "netgraph.h"
#include "command.h"
#include "util.h"

#ifdef USE_NG_BPF
#include <net/bpf.h>
#endif
#include <arpa/inet.h>

#include <netgraph/ng_message.h>

#include <netgraph/ng_socket.h>
#include <netgraph/ng_ksocket.h>
#include <netgraph/ng_iface.h>
#ifdef USE_NG_VJC
#include <netgraph/ng_vjc.h>
#endif
#ifdef USE_NG_BPF
#include <netgraph/ng_bpf.h>
#endif
#include <netgraph/ng_tee.h>
#ifdef USE_NG_TCPMSS
#include <netgraph/ng_tcpmss.h>
#endif
#ifdef USE_NG_NETFLOW
#include <netgraph/netflow/ng_netflow.h>
#include <netgraph/netflow/netflow.h>
#if NGM_NETFLOW_COOKIE >= 1309868867
#include <netgraph/netflow/netflow_v9.h>
#endif
#endif
#ifdef USE_NG_PRED1
#include <netgraph/ng_pred1.h>
#endif

#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>

/*
 * DEFINITIONS
 */

  #define TEMPHOOK		"temphook"
  #define MAX_IFACE_CREATE	128

  /* Set menu options */
  enum {
    SET_PEER,
    SET_SELF,
    SET_TIMEOUTS,
#if NGM_NETFLOW_COOKIE >= 1309868867
    SET_TEMPLATE,
    SET_MTU,
    SET_VERSION,
#endif
    SET_NODE,
    SET_HOOK
  };

/*
 * INTERNAL FUNCTIONS
 */

#ifdef USE_NG_NETFLOW
  static int	NetflowSetCommand(Context ctx, int ac, const char *const av[], const void *arg);
#endif

/*
 * GLOBAL VARIABLES
 */

#ifdef USE_NG_NETFLOW
  const struct cmdtab NetflowSetCmds[] = {
    { "peer {ip} {port}",	"Set export destination" ,
        NetflowSetCommand, NULL, 2, (void *) SET_PEER },
    { "self {ip} [{port}]",	"Set export source" ,
        NetflowSetCommand, NULL, 2, (void *) SET_SELF },
    { "timeouts {inactive} {active}", "Set NetFlow timeouts" ,
        NetflowSetCommand, NULL, 2, (void *) SET_TIMEOUTS },
#if NGM_NETFLOW_COOKIE >= 1309868867
    { "template {time} {packets}", "Set NetFlow v9 template" ,
        NetflowSetCommand, NULL, 2, (void *) SET_TEMPLATE },
    { "mtu {mtu}", "Set NetFlow v9 MTU" ,
        NetflowSetCommand, NULL, 2, (void *) SET_MTU },
    { "version {version}", "Set version to export" ,
        NetflowSetCommand, NULL, 2, (void *) SET_VERSION },
#endif
    { "node {name}", "Set node name to use" ,
        NetflowSetCommand, NULL, 2, (void *) SET_NODE },
    { "hook {number}", "Set initial hook number" ,
        NetflowSetCommand, NULL, 2, (void *) SET_HOOK },
    { NULL, NULL, NULL, NULL, 0, NULL },
  };
#endif

/*
 * INTERNAL VARIABLES
 */

#ifdef USE_NG_TCPMSS
  u_char gTcpMSSNode = FALSE;
#endif
#ifdef USE_NG_NETFLOW
  u_char gNetflowNode = FALSE;
  u_char gNetflowNodeShutdown = TRUE;
  char gNetflowNodeName[64] = "mpd-nf";
  ng_ID_t gNetflowNodeID = 0;
  u_int gNetflowIface = 0;
  struct sockaddr_storage gNetflowExport;
  struct sockaddr_storage gNetflowSource;
  uint32_t gNetflowInactive = 0;
  uint32_t gNetflowActive = 0;
#if NGM_NETFLOW_COOKIE >= 1309868867
  static uint16_t gNetflowTime = 0;
  static uint16_t gNetflowPackets = 0;
  static uint16_t gNetflowMTU = 0;
  static u_int gNetflowVer = 5;
#endif
#endif
  
  static int	gNgStatSock=0;


#ifdef USE_NG_NETFLOW
int
NgFuncInitGlobalNetflow(void)
{
    char		path[NG_PATHSIZ];
    struct ngm_mkpeer	mp;
    struct ngm_rmhook	rm;
    struct ngm_name	nm;
    int			csock;

    /* Create a netgraph socket node */
    if (NgMkSockNode(NULL, &csock, NULL) < 0) {
	Perror("NETFLOW: Can't create %s node", NG_SOCKET_NODE_TYPE);
        return (-1);
    }

    /* If node exist just get it's ID. */
    if (gNetflowNode) {
	snprintf(path, sizeof(path), "%s:", gNetflowNodeName);
	if ((gNetflowNodeID = NgGetNodeID(csock, path)) == 0) {
	    Perror("NETFLOW: Cannot get %s node id", NG_NETFLOW_NODE_TYPE);
	    goto fail;
	}
	close(csock);
	return (0);
    }

    snprintf(gNetflowNodeName, sizeof(gNetflowNodeName), "mpd%d-nf", gPid);

    /* Create a global netflow node. */
    strcpy(mp.type, NG_NETFLOW_NODE_TYPE);
    strcpy(mp.ourhook, TEMPHOOK);
    strcpy(mp.peerhook, NG_NETFLOW_HOOK_DATA "0");
    if (NgSendMsg(csock, ".:",
      NGM_GENERIC_COOKIE, NGM_MKPEER, &mp, sizeof(mp)) < 0) {
	Perror("NETFLOW: Can't create %s node at \"%s\"->\"%s\"", 
	    mp.type, ".:", mp.ourhook);
	goto fail;
    }
    
    /* Get new node ID. */
    if ((gNetflowNodeID = NgGetNodeID(csock, TEMPHOOK)) == 0) {
	Perror("NETFLOW: Cannot get %s node id", NG_NETFLOW_NODE_TYPE);
	goto fail;
    }

    /* Set the new node's name. */
    strcpy(nm.name, gNetflowNodeName);
    if (NgSendMsg(csock, TEMPHOOK,
      NGM_GENERIC_COOKIE, NGM_NAME, &nm, sizeof(nm)) < 0) {
	Perror("NETFLOW: Can't name %s node", NG_NETFLOW_NODE_TYPE);
	goto fail;
    }

    /* Connect ng_ksocket(4) node for export. */
    strcpy(mp.type, NG_KSOCKET_NODE_TYPE);
#if NGM_NETFLOW_COOKIE >= 1309868867
    if (gNetflowVer == 5) {
#endif
	strcpy(mp.ourhook, NG_NETFLOW_HOOK_EXPORT);
#if NGM_NETFLOW_COOKIE >= 1309868867
    } else {
	strcpy(mp.ourhook, NG_NETFLOW_HOOK_EXPORT9);
    }
#endif
    if (gNetflowExport.ss_family==AF_INET6) {
	snprintf(mp.peerhook, sizeof(mp.peerhook), "%d/%d/%d", PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    } else {
        snprintf(mp.peerhook, sizeof(mp.peerhook), "inet/dgram/udp");
    }
    snprintf(path, sizeof(path), "[%x]:", gNetflowNodeID);
    if (NgSendMsg(csock, path,
      NGM_GENERIC_COOKIE, NGM_MKPEER, &mp, sizeof(mp)) < 0) {
	Perror("NETFLOW: Can't create %s node at \"%s\"->\"%s\"",
	    mp.type, path, mp.ourhook);
	goto fail;
    }

    /* Configure timeouts for ng_netflow(4). */
    if (gNetflowInactive != 0 && gNetflowActive != 0) {
	struct ng_netflow_settimeouts nf_settime;

	nf_settime.inactive_timeout = gNetflowInactive;
	nf_settime.active_timeout = gNetflowActive;

	if (NgSendMsg(csock, path, NGM_NETFLOW_COOKIE,
	  NGM_NETFLOW_SETTIMEOUTS, &nf_settime, sizeof(nf_settime)) < 0) {
	    Perror("NETFLOW: Can't set timeouts on netflow %s node",
		NG_NETFLOW_NODE_TYPE);
	    goto fail2;
	}
    }

#if NGM_NETFLOW_COOKIE >= 1309868867
    if (gNetflowTime != 0 && gNetflowPackets != 0) {
	struct ng_netflow_settemplate nf_settempl;

	nf_settempl.time = gNetflowTime;
	nf_settempl.packets = gNetflowPackets;
	if (NgSendMsg(csock, path, NGM_NETFLOW_COOKIE,
	  NGM_NETFLOW_SETTEMPLATE, &nf_settempl, sizeof(nf_settempl)) < 0) {
	    Perror("NETFLOW: Can't set NetFlow v9 template on netflow %s node",
		NG_NETFLOW_NODE_TYPE);
	    goto fail2;
	}
    }

    if (gNetflowMTU != 0) {
	struct ng_netflow_setmtu nf_setmtu;

	nf_setmtu.mtu = gNetflowMTU;
	if (NgSendMsg(csock, path, NGM_NETFLOW_COOKIE,
	  NGM_NETFLOW_SETMTU, &nf_setmtu, sizeof(nf_setmtu)) < 0) {
	    Perror("NETFLOW: Can't set NetFlow v9 MTU on netflow %s node",
		NG_NETFLOW_NODE_TYPE);
	  goto fail2;
	}
    }
#endif

    /* Configure export destination and source on ng_ksocket(4). */
#if NGM_NETFLOW_COOKIE >= 1309868867
    if (gNetflowVer == 5) {
#endif
	strlcat(path, NG_NETFLOW_HOOK_EXPORT, sizeof(path));
#if NGM_NETFLOW_COOKIE >= 1309868867
    } else {
	strlcat(path, NG_NETFLOW_HOOK_EXPORT9, sizeof(path));
    }
#endif
    if (gNetflowSource.ss_len != 0) {
	if (NgSendMsg(csock, path, NGM_KSOCKET_COOKIE,
	  NGM_KSOCKET_BIND, &gNetflowSource, sizeof(gNetflowSource)) < 0) {
	    Perror("NETFLOW: Can't bind export %s node", NG_KSOCKET_NODE_TYPE);
	    goto fail2;
	}
    }
    if (gNetflowExport.ss_len != 0) {
	if (NgSendMsg(csock, path, NGM_KSOCKET_COOKIE,
	  NGM_KSOCKET_CONNECT, &gNetflowExport, sizeof(gNetflowExport)) < 0) {
	    Perror("NETFLOW: Can't connect export %s node", NG_KSOCKET_NODE_TYPE);
	    goto fail2;
	}
    }

    /* Set the new node's name. */
    snprintf(nm.name, sizeof(nm.name), "mpd%d-nfso", gPid);
    if (NgSendMsg(csock, path,
      NGM_GENERIC_COOKIE, NGM_NAME, &nm, sizeof(nm)) < 0) {
	Perror("NETFLOW: Can't name %s node", NG_KSOCKET_NODE_TYPE);
	goto fail2;
    }

    /* Disconnect temporary hook. */
    memset(&rm, 0, sizeof(rm));
    strncpy(rm.ourhook, TEMPHOOK, sizeof(rm.ourhook));
    if (NgSendMsg(csock, ".:",
      NGM_GENERIC_COOKIE, NGM_RMHOOK, &rm, sizeof(rm)) < 0) {
	Perror("can't remove hook %s", TEMPHOOK);
	goto fail2;
    }
    gNetflowNode = TRUE;
    close(csock);

    return (0);
fail2:
    snprintf(path, sizeof(path), "[%x]:", gNetflowNodeID);
    NgFuncShutdownNode(csock, "netflow", path);
fail:
    gNetflowNodeID = 0;
    close(csock);
    return (-1);
}
#endif

/*
 * NgFuncCreateIface()
 *
 * Create a new netgraph interface.
 */

int
NgFuncCreateIface(Bund b, char *buf, int max)
{
    union {
        u_char		buf[sizeof(struct ng_mesg) + sizeof(struct nodeinfo)];
        struct ng_mesg	reply;
    }			u;
    struct nodeinfo	*const ni = (struct nodeinfo *)(void *)u.reply.data;
    struct ngm_rmhook	rm;
    struct ngm_mkpeer	mp;
    int			rtn = 0;

    /* Create iface node (as a temporary peer of the socket node) */
    strcpy(mp.type, NG_IFACE_NODE_TYPE);
    strcpy(mp.ourhook, TEMPHOOK);
    strcpy(mp.peerhook, NG_IFACE_HOOK_INET);
    if (NgSendMsg(gLinksCsock, ".:",
      NGM_GENERIC_COOKIE, NGM_MKPEER, &mp, sizeof(mp)) < 0) {
	Log(LG_ERR, ("[%s] can't create %s node at \"%s\"->\"%s\": %s %d",
    	    b->name, NG_IFACE_NODE_TYPE, ".:", mp.ourhook, strerror(errno), gLinksCsock));
	return(-1);
    }

    /* Get the new node's name */
    if (NgSendMsg(gLinksCsock, TEMPHOOK,
      NGM_GENERIC_COOKIE, NGM_NODEINFO, NULL, 0) < 0) {
	Perror("[%s] %s", b->name, "NGM_NODEINFO");
	rtn = -1;
	goto done;
    }
    if (NgRecvMsg(gLinksCsock, &u.reply, sizeof(u), NULL) < 0) {
	Perror("[%s] reply from %s", b->name, NG_IFACE_NODE_TYPE);
	rtn = -1;
	goto done;
    }
    strlcpy(buf, ni->name, max);

done:
    /* Disconnect temporary hook */
    strcpy(rm.ourhook, TEMPHOOK);
    if (NgSendMsg(gLinksCsock, ".:",
      NGM_GENERIC_COOKIE, NGM_RMHOOK, &rm, sizeof(rm)) < 0) {
	Perror("[%s] can't remove hook %s", b->name, TEMPHOOK);
	rtn = -1;
    }

    /* Done */
    return(rtn);
}

/*
 * NgFuncShutdownGlobal()
 *
 * Shutdown nodes, that are shared between bundles.
 */

void
NgFuncShutdownGlobal(void)
{
#ifdef USE_NG_NETFLOW
    char	path[NG_PATHSIZ];
    int		csock;

    if (gNetflowNode == FALSE || gNetflowNodeShutdown==FALSE)
	return;

    /* Create a netgraph socket node */
    if (NgMkSockNode(NULL, &csock, NULL) < 0) {
	Perror("NgFuncShutdownGlobal: can't create %s node", NG_SOCKET_NODE_TYPE);
	return;
    }

    snprintf(path, sizeof(path), "[%x]:", gNetflowNodeID);
    NgFuncShutdownNode(csock, "netflow", path);
    
    close(csock);
#endif
}

/*
 * NgFuncShutdownNode()
 */

int
NgFuncShutdownNode(int csock, const char *label, const char *path)
{
    int rtn, retry = 10, delay = 1000;

retry:
    if ((rtn = NgSendMsg(csock, path,
      NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0)) < 0) {
	if (errno == ENOBUFS && retry > 0) {
    	    Log(LG_ERR, ("[%s] shutdown \"%s\": %s, retrying...",
	      label, path, strerror(errno)));
	    usleep(delay);
	    retry--;
	    delay *= 2;
	    goto retry;
	}
	if (errno != ENOENT) {
	    Perror("[%s] can't shutdown \"%s\"", label, path);
	}
    }
    return(rtn);
}

/*
 * NgFuncSetConfig()
 */

void
NgFuncSetConfig(Bund b)
{
    char	path[NG_PATHSIZ];
    snprintf(path, sizeof(path), "[%x]:", b->nodeID);
    if (NgSendMsg(gLinksCsock, path, NGM_PPP_COOKIE,
    	    NGM_PPP_SET_CONFIG, &b->pppConfig, sizeof(b->pppConfig)) < 0) {
	Perror("[%s] can't config %s", b->name, path);
	DoExit(EX_ERRDEAD);
    }
}

/*
 * NgFuncSendQuery()
 */

int
NgFuncSendQuery(const char *path, int cookie, int cmd, const void *args,
	size_t arglen, struct ng_mesg *rbuf, size_t replen, char *raddr)
{

    if (!gNgStatSock) {
	char		name[NG_NODESIZ];
	
	/* Create a netgraph socket node */
	snprintf(name, sizeof(name), "mpd%d-stats", gPid);
	if (NgMkSockNode(name, &gNgStatSock, NULL) < 0) {
	    Perror("NgFuncSendQuery: can't create %s node", NG_SOCKET_NODE_TYPE);
	    return(-1);
	}
	(void) fcntl(gNgStatSock, F_SETFD, 1);
    }

    /* Send message */
    if (NgSendMsg(gNgStatSock, path, cookie, cmd, args, arglen) < 0) {
	Perror("NgFuncSendQuery: can't send message");
	return (-1);
    }

    /* Read message */
    if (NgRecvMsg(gNgStatSock, rbuf, replen, raddr) < 0) {
	Perror("NgFuncSendQuery: can't read unexpected message");
	return (-1);
    }

    return (0);
}

/*
 * NgFuncConnect()
 */

int
NgFuncConnect(int csock, char *label, const char *path, const char *hook,
	const char *path2, const char *hook2)
{
    struct ngm_connect	cn;

    strlcpy(cn.path, path2, sizeof(cn.path));
    strlcpy(cn.ourhook, hook, sizeof(cn.ourhook));
    strlcpy(cn.peerhook, hook2, sizeof(cn.peerhook));
    if (NgSendMsg(csock, path,
      NGM_GENERIC_COOKIE, NGM_CONNECT, &cn, sizeof(cn)) < 0) {
	Perror("[%s] can't connect \"%s\"->\"%s\" and \"%s\"->\"%s\"",
	    label, path, hook, path2, hook2);
	return(-1);
    }
    return(0);
}

/*
 * NgFuncDisconnect()
 */

int
NgFuncDisconnect(int csock, char *label, const char *path, const char *hook)
{
    struct ngm_rmhook	rm;
    int		retry = 10, delay = 1000;

    /* Disconnect hook */
    memset(&rm, 0, sizeof(rm));
    strlcpy(rm.ourhook, hook, sizeof(rm.ourhook));
retry:
    if (NgSendMsg(csock, path,
      NGM_GENERIC_COOKIE, NGM_RMHOOK, &rm, sizeof(rm)) < 0) {
	if (errno == ENOBUFS && retry > 0) {
    	    Log(LG_ERR, ("[%s] remove hook %s from node \"%s\": %s, retrying...",
	      label, hook, path, strerror(errno)));
	    usleep(delay);
	    retry--;
	    delay *= 2;
	    goto retry;
	}
	Perror("[%s] can't remove hook %s from node \"%s\"", label, hook, path);
	return(-1);
    }
    return(0);
}

/*
 * NgFuncWritePppFrame()
 *
 * Consumes the mbuf.
 */

int
NgFuncWritePppFrame(Bund b, int linkNum, int proto, Mbuf bp)
{
    u_int16_t	temp;

    /* Prepend ppp node bypass header */
    temp = htons(linkNum);
    bp = mbcopyback(bp, -4, &temp, 2);
    temp = htons(proto);
    bp = mbcopyback(bp, 2, &temp, 2);

    /* Debugging */
    LogDumpBp(LG_FRAME, bp,
	"[%s] xmit bypass frame link=%d proto=0x%04x",
	b->name, (int16_t)linkNum, proto);

    if ((linkNum == NG_PPP_BUNDLE_LINKNUM && b->n_up == 0) ||
	(linkNum != NG_PPP_BUNDLE_LINKNUM &&
	    (b->links[linkNum] == NULL ||
	    b->links[linkNum]->state != PHYS_STATE_UP))) {
	Log(LG_FRAME, ("[%s] Bundle: No links ready to send packet", b->name));
	mbfree(bp);
	return (-1);
    }

    /* Write frame */
    return NgFuncWriteFrame(gLinksDsock, b->hook, b->name, bp);
}

/*
 * NgFuncWritePppFrameLink()
 *
 * Consumes the mbuf.
 */

int
NgFuncWritePppFrameLink(Link l, int proto, Mbuf bp)
{
    u_int16_t	temp;

    if (l->joined_bund) {
	return (NgFuncWritePppFrame(l->bund, l->bundleIndex, proto, bp));
    }

    /* Prepend framing */
    temp = htons(0xff03);
    bp = mbcopyback(bp, -4, &temp, 2);
    temp = htons(proto);
    bp = mbcopyback(bp, 2, &temp, 2);

    /* Debugging */
    LogDumpBp(LG_FRAME, bp,
	"[%s] xmit frame to link proto=0x%04x",
	l->name, proto);

    if (l->state != PHYS_STATE_UP) {
	Log(LG_FRAME, ("[%s] Link: Not ready to send packet", l->name));
	mbfree(bp);
	return (-1);
    }

    /* Write frame */
    return NgFuncWriteFrame(gLinksDsock, l->hook, l->name, bp);
}

/*
 * NgFuncWriteFrame()
 *
 * Consumes the mbuf.
 */

int
NgFuncWriteFrame(int dsock, const char *hookname, const char *label, Mbuf bp)
{
    union {
        u_char          buf[sizeof(struct sockaddr_ng) + NG_HOOKSIZ];
	struct sockaddr_ng sa_ng;
    }                   u;
    struct sockaddr_ng	*ng = &u.sa_ng;
    int			rtn;

    /* Write frame */
    if (bp == NULL)  
	return (-1);

    /* Set dest address */
    memset(&u.buf, 0, sizeof(u.buf));
    strlcpy(ng->sg_data, hookname, NG_HOOKSIZ);
    ng->sg_family = AF_NETGRAPH;
    ng->sg_len = 3 + strlen(ng->sg_data);

    rtn = sendto(dsock, MBDATAU(bp), MBLEN(bp),
	0, (struct sockaddr *)ng, ng->sg_len);

    /* ENOBUFS can be expected on some links, e.g., ng_pptpgre(4) */
    if (rtn < 0 && errno != ENOBUFS) {
	Perror("[%s] error writing len %d frame to %s",
	    label, (int)MBLEN(bp), hookname);
    }
    mbfree(bp);
    return (rtn);
}

/*
 * NgFuncClrStats()
 *
 * Clear link or whole bundle statistics
 */

int
NgFuncClrStats(Bund b, u_int16_t linkNum)
{
    char	path[NG_PATHSIZ];

    /* Get stats */
    snprintf(path, sizeof(path), "[%x]:", b->nodeID);
    if (NgSendMsg(gLinksCsock, path, 
	NGM_PPP_COOKIE, NGM_PPP_CLR_LINK_STATS, &linkNum, sizeof(linkNum)) < 0) {
	    Perror("[%s] can't clear stats, link=%d", b->name, linkNum);
	    return (-1);
    }
    return(0);
}

#ifndef NG_PPP_STATS64

/*
 * NgFuncGetStats()
 *
 * Get link or whole bundle statistics
 */

int
NgFuncGetStats(Bund b, u_int16_t linkNum, struct ng_ppp_link_stat *statp)
{
    union {
        u_char			buf[sizeof(struct ng_mesg)
				  + sizeof(struct ng_ppp_link_stat)];
        struct ng_mesg		reply;
    }				u;
    char			path[NG_PATHSIZ];

    /* Get stats */
    snprintf(path, sizeof(path), "[%x]:", b->nodeID);
    if (NgFuncSendQuery(path, NGM_PPP_COOKIE, NGM_PPP_GET_LINK_STATS,
      &linkNum, sizeof(linkNum), &u.reply, sizeof(u), NULL) < 0) {
	Perror("[%s] can't get stats, link=%d", b->name, linkNum);
	return -1;
    }
    if (statp != NULL)
	memcpy(statp, u.reply.data, sizeof(*statp));
    return(0);
}

#else
/*
 * NgFuncGetStats64()
 *
 * Get 64bit link or whole bundle statistics
 */

int
NgFuncGetStats64(Bund b, u_int16_t linkNum, struct ng_ppp_link_stat64 *statp)
{
    union {
        u_char			buf[sizeof(struct ng_mesg)
				  + sizeof(struct ng_ppp_link_stat64)];
        struct ng_mesg		reply;
    }				u;
    char			path[NG_PATHSIZ];

    /* Get stats */
    snprintf(path, sizeof(path), "[%x]:", b->nodeID);
    if (NgFuncSendQuery(path, NGM_PPP_COOKIE, NGM_PPP_GET_LINK_STATS64,
      &linkNum, sizeof(linkNum), &u.reply, sizeof(u), NULL) < 0) {
	Perror("[%s] can't get stats, link=%d", b->name, linkNum);
	return -1;
    }
    if (statp != NULL)
	memcpy(statp, u.reply.data, sizeof(*statp));
    return(0);
}
#endif

/*
 * NgFuncErrx()
 */

void
NgFuncErrx(const char *fmt, ...)
{
    char	buf[100];
    va_list	args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log(LG_ERR, ("netgraph: %s", buf));
}

/*
 * NgFuncErr()
 */

void
NgFuncErr(const char *fmt, ...)
{
    char	buf[100];
    va_list	args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Perror("netgraph: %s", buf);
}

#ifdef USE_NG_NETFLOW
/*
 * NetflowSetCommand()
 */
       
static int
NetflowSetCommand(Context ctx, int ac, const char *const av[], const void *arg)
{
    struct sockaddr_storage *sin;

    switch ((intptr_t)arg) {
	case SET_PEER: 
    	    if (ac != 2)
		return (-1);
    	    if ((sin = ParseAddrPort(ac, av, ALLOW_IPV4|ALLOW_IPV6)) == NULL)
		return (-1);
    	    gNetflowExport = *sin;
    	    break;
	case SET_SELF:
	    if (ac != 1 && ac != 2)
		return (-1);
    	    if ((sin = ParseAddrPort(ac, av, ALLOW_IPV4|ALLOW_IPV6)) == NULL)
		return (-1);
    	    gNetflowSource = *sin;
    	    break;
	case SET_TIMEOUTS:
    	    if (ac != 2)
		return (-1);
    	    if (atoi(av[0]) <= 0 || atoi(av[1]) <= 0)
		Error("Bad netflow timeouts \"%s %s\"", av[0], av[1]);
    	    gNetflowInactive = atoi(av[0]);
    	    gNetflowActive = atoi(av[1]);
    	    break;
#if NGM_NETFLOW_COOKIE >= 1309868867
	case SET_TEMPLATE:
    	    if (ac != 2)
		return (-1);
	    /*
	     * RFC 3954 clause 7.3
	     * "Both options MUST be configurable by the user on the Exporter."
	     */
    	    if (atoi(av[0]) <= 0 || atoi(av[1]) <= 0)
		Error("Bad netflow v9 template values \"%s %s\"", av[0], av[1]);
    	    gNetflowTime = atoi(av[0]);		/* Default 600 */
    	    gNetflowPackets = atoi(av[1]);	/* Default 500 */
    	    break;
	case SET_MTU:
    	    if (ac != 1)
		return (-1);
    	    if (atoi(av[0]) < (int)MIN_MTU || atoi(av[0]) > (int)MAX_MTU)
		Error("Bad netflow v9 MTU \"%s\"", av[0]);
    	    gNetflowMTU = atoi(av[0]);		/* Default 1500 */
    	    break;
	case SET_VERSION:
    	    if (ac != 1)
		return (-1);
    	    if (atoi(av[0]) != 5 && atoi(av[0]) != 9)
		Error("Bad netflow export version \"%s\"", av[0]);
    	    gNetflowVer = atoi(av[0]);		/* Default 5 */
    	    break;
#endif
	case SET_NODE:
    	    if (ac != 1)
		return (-1);
    	    if (strlen(av[0]) == 0 || strlen(av[0]) > 63)
		Error("Bad netflow node name \"%s\"", av[0]);
    	    strlcpy(gNetflowNodeName, av[0], sizeof(gNetflowNodeName));
    	    gNetflowNode=TRUE;
    	    gNetflowNodeShutdown=FALSE;
    	    break;
	case SET_HOOK:
    	    if (ac != 1)
		return (-1);
    	    if (atoi(av[0]) <= 0 || atoi(av[0]) >= NG_NETFLOW_MAXIFACES)
		Error("Bad netflow hook number \"%s\"", av[0]);
    	    gNetflowIface = atoi(av[0])-1;
    	    break;

	default:
	    return (-1);
    }

    return (0);
}

/*
 * ShowNetflow()
 *
 * Show state of a Netflow
 */

int
ShowNetflow(Context ctx, int ac, const char *const av[], const void *arg)
{
    struct u_addr addr;
    in_port_t port;
    char buf[64];
    char path[NG_PATHSIZ];
    union {
        u_char buf[sizeof(struct ng_mesg) + sizeof(struct ng_netflow_info)];
        struct ng_mesg reply;
    } u;
    struct ng_netflow_info *const ni = \
        (struct ng_netflow_info *)(void *)u.reply.data;
#ifdef NGM_NETFLOW_V9_COOKIE
    union {
        u_char buf[sizeof(struct ng_mesg) + sizeof(struct ng_netflow_v9info)];
        struct ng_mesg reply;
    } uv9;
    struct ng_netflow_v9info *const niv9 = \
        (struct ng_netflow_v9info *)(void *)uv9.reply.data;
#endif /* NGM_NETFLOW_V9_COOKIE */

    (void)ac;
    (void)av;
    (void)arg;

    if (gNetflowNodeID>0) {
        snprintf(path, sizeof(path), "[%x]:", gNetflowNodeID);
        if (NgFuncSendQuery(path, NGM_NETFLOW_COOKIE, NGM_NETFLOW_INFO,
        NULL, 0, &u.reply, sizeof(u), NULL) < 0)
            return(-7);
#ifdef NGM_NETFLOW_V9_COOKIE
        if (NgFuncSendQuery(path, NGM_NETFLOW_COOKIE, NGM_NETFLOW_V9INFO,
        NULL, 0, &uv9.reply, sizeof(uv9), NULL) < 0)
            return(-7);
#endif /* NGM_NETFLOW_V9_COOKIE */
    }

    Printf("Netflow status:\r\n");
    Printf("\tNode created   : %s\r\n", gNetflowNodeID ? "Yes" : "No");
    Printf("Netflow settings:\r\n");
    Printf("\tNode name      : %s\r\n", gNetflowNodeName);
    Printf("\tInitial hook   : %d\r\n", gNetflowIface);
    Printf("\tTimeouts, sec:\r\n");
    Printf("\t  Active       : %u\r\n",
        (gNetflowNodeID>0) ? ni->nfinfo_act_t :
        (gNetflowActive ? gNetflowActive : ACTIVE_TIMEOUT));
    Printf("\t  Inactive     : %u\r\n",
        (gNetflowNodeID>0) ? ni->nfinfo_inact_t :
        (gNetflowInactive ? gNetflowInactive : INACTIVE_TIMEOUT));
    sockaddrtou_addr(&gNetflowExport, &addr, &port);
    Printf("\tExport address : %s port %d\r\n",
        u_addrtoa(&addr, buf, sizeof(buf)), (int)port);
    sockaddrtou_addr(&gNetflowSource, &addr, &port);
    Printf("\tSource address : %s port %d\r\n",
        u_addrtoa(&addr, buf, sizeof(buf)), (int)port);
#if NGM_NETFLOW_COOKIE >= 1309868867
    Printf("\tExport version : v%d\r\n", gNetflowVer);
    Printf("Netflow v9 configuration:\r\n");
    Printf("\tTemplate:\r\n");
#ifdef NGM_NETFLOW_V9_COOKIE
    Printf("\t  Time         : %d\r\n",
        (gNetflowNodeID>0) ? niv9->templ_time :
        (gNetflowTime ? gNetflowTime : NETFLOW_V9_MAX_TIME_TEMPL));
    Printf("\t  Packets      : %d\r\n",
        (gNetflowNodeID>0) ? niv9->templ_packets :
        (gNetflowPackets ? gNetflowPackets : NETFLOW_V9_MAX_PACKETS_TEMPL));
    Printf("\tNetflow v9 MTU : %d\r\n",
        (gNetflowNodeID>0) ? niv9->mtu :
        (gNetflowMTU ? gNetflowMTU : BASE_MTU));
#else
    Printf("\t  Time         : %d\r\n",
        gNetflowTime ? gNetflowTime : NETFLOW_V9_MAX_TIME_TEMPL);
    Printf("\t  Packets      : %d\r\n",
        gNetflowPackets ? gNetflowPackets : NETFLOW_V9_MAX_PACKETS_TEMPL);
    Printf("\tNetflow v9 MTU : %d\r\n",
        gNetflowMTU ? gNetflowMTU : BASE_MTU);
#endif /* NGM_NETFLOW_V9_COOKIE */
#endif
    if (gNetflowNodeID>0) {
        Printf("Traffic stats:\r\n");
#if NGM_NETFLOW_COOKIE >= 1365756954
        Printf("\tAccounted IPv4 octets  : %llu\r\n", (unsigned long long)ni->nfinfo_bytes);
        Printf("\tAccounted IPv4 packets : %llu\r\n", (unsigned long long)ni->nfinfo_packets);
        Printf("\tAccounted IPv6 octets  : %llu\r\n", (unsigned long long)ni->nfinfo_bytes6);
        Printf("\tAccounted IPv6 packets : %llu\r\n", (unsigned long long)ni->nfinfo_packets6);
        Printf("\tSkipped IPv4 octets    : %llu\r\n", (unsigned long long)ni->nfinfo_sbytes);
        Printf("\tSkipped IPv4 packets   : %llu\r\n", (unsigned long long)ni->nfinfo_spackets);
        Printf("\tSkipped IPv6 octets    : %llu\r\n", (unsigned long long)ni->nfinfo_sbytes6);
        Printf("\tSkipped IPv6 packets   : %llu\r\n", (unsigned long long)ni->nfinfo_spackets6);
        Printf("\tActive expiries        : %llu\r\n", (unsigned long long)ni->nfinfo_act_exp);
        Printf("\tInactive expiries      : %llu\r\n", (unsigned long long)ni->nfinfo_inact_exp);
        Printf("\tUsed IPv4 cache records: %u\r\n", ni->nfinfo_used);
        Printf("\tUsed IPv6 cache records: %u\r\n", ni->nfinfo_used6);
        Printf("\tFailed allocations     : %u\r\n", ni->nfinfo_alloc_failed);
        Printf("\tFailed v5 export       : %u\r\n", ni->nfinfo_export_failed);
        Printf("\tFailed v9 export       : %u\r\n", ni->nfinfo_export9_failed);
        Printf("\tRallocated mbufs       : %u\r\n", ni->nfinfo_realloc_mbuf);
        Printf("\tFibs allocated         : %u\r\n", ni->nfinfo_alloc_fibs);
#else /* NGM_NETFLOW_COOKIE >= 1365756954 */
        Printf("\tAccounted IPv4 octets  : %llu\r\n", (unsigned long long)ni->nfinfo_bytes);
        Printf("\tAccounted IPv4 packets : %u\r\n", ni->nfinfo_packets);
#if NGM_NETFLOW_COOKIE >= 1309868867
        Printf("\tAccounted IPv6 octets  : %llu\r\n", (unsigned long long)ni->nfinfo_bytes6);
        Printf("\tAccounted IPv6 packets : %u\r\n", ni->nfinfo_packets6);
        Printf("\tSkipped IPv4 octets    : %llu\r\n", (unsigned long long)ni->nfinfo_sbytes);
        Printf("\tSkipped IPv4 packets   : %u\r\n", ni->nfinfo_spackets);
        Printf("\tSkipped IPv6 octets    : %llu\r\n", (unsigned long long)ni->nfinfo_sbytes6);
        Printf("\tSkipped IPv6 packets   : %u\r\n", ni->nfinfo_spackets6);
#endif
        Printf("\tUsed IPv4 cache records: %u\r\n", ni->nfinfo_used);
#if NGM_NETFLOW_COOKIE >= 1309868867
        Printf("\tUsed IPv6 cache records: %u\r\n", ni->nfinfo_used6);
#endif
        Printf("\tFailed allocations     : %u\r\n", ni->nfinfo_alloc_failed);
        Printf("\tFailed v5 export       : %u\r\n", ni->nfinfo_export_failed);
#if NGM_NETFLOW_COOKIE >= 1309868867
        Printf("\tFailed v9 export       : %u\r\n", ni->nfinfo_export9_failed);
        Printf("\tRallocated mbufs       : %u\r\n", ni->nfinfo_realloc_mbuf);
        Printf("\tFibs allocated         : %u\r\n", ni->nfinfo_alloc_fibs);
#endif
        Printf("\tActive expiries        : %u\r\n", ni->nfinfo_act_exp);
        Printf("\tInactive expiries      : %u\r\n", ni->nfinfo_inact_exp);
#endif /* NGM_NETFLOW_COOKIE >= 1365756954 */
    }
    return(0);
}
#endif /* USE_NG_NETFLOW */

ng_ID_t
NgGetNodeID(int csock, const char *path)
{
    union {
        u_char          buf[sizeof(struct ng_mesg) + sizeof(struct nodeinfo)];
	struct ng_mesg  reply;
    }                   u;
    struct nodeinfo     *const ni = (struct nodeinfo *)(void *)u.reply.data;
    
    if (csock < 0) {
	if (!gNgStatSock) {
	    char		name[NG_NODESIZ];
	
	    /* Create a netgraph socket node */
	    snprintf(name, sizeof(name), "mpd%d-stats", gPid);
	    if (NgMkSockNode(name, &gNgStatSock, NULL) < 0) {
    		Perror("NgMkSockNode: can't create %s node",
    		     NG_SOCKET_NODE_TYPE);
    		return (0);
	    }
	    (void) fcntl(gNgStatSock, F_SETFD, 1);
	}
	csock = gNgStatSock;
    }

    if (NgSendMsg(csock, path,
      NGM_GENERIC_COOKIE, NGM_NODEINFO, NULL, 0) < 0) {
	Perror("NgSendMsg to %s", path);
	return (0);
    }
    if (NgRecvMsg(csock, &u.reply, sizeof(u), NULL) < 0) {
	Perror("NgRecvMsg from %s", path);
	return (0);
    }

    return (ni->id);
}

