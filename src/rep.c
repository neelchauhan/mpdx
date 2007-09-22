
/*
 * rep.c
 *
 * Written by Alexander Motin <mav@FreeBSD.org>
 */

#include "ppp.h"
#include "rep.h"
#include "msg.h"
#include "ngfunc.h"
#include "log.h"
#include "util.h"

#include <netgraph/ng_message.h>
#ifdef __DragonFly__
#include <netgraph/socket/ng_socket.h>
#include <netgraph/tee/ng_tee.h>
#else
#include <netgraph/ng_socket.h>
#include <netgraph/ng_tee.h>
#endif
#include <netgraph.h>

/*
 * DEFINITIONS
 */

  /* Set menu options */
  enum {
    SET_LINK,
    SET_ACCEPT,
    SET_DENY,
    SET_ENABLE,
    SET_DISABLE,
    SET_YES,
    SET_NO,
  };

/*
 * INTERNAL FUNCTIONS
 */

  static int	RepSetCommand(Context ctx, int ac, char *av[], void *arg);
  static void	RepShowLinks(Context ctx, Rep r);

/*
 * GLOBAL VARIABLES
 */

  struct discrim	self_discrim;

  const struct cmdtab RepSetCmds[] = {
    { "link {template}",		"Set link template",
	RepSetCommand, NULL, (void *) SET_LINK },
    { "accept [opt ...]",		"Accept option",
	RepSetCommand, NULL, (void *) SET_ACCEPT },
    { "deny [opt ...]",			"Deny option",
	RepSetCommand, NULL, (void *) SET_DENY },
    { "enable [opt ...]",		"Enable option",
	RepSetCommand, NULL, (void *) SET_ENABLE },
    { "disable [opt ...]",		"Disable option",
	RepSetCommand, NULL, (void *) SET_DISABLE },
    { "yes [opt ...]",			"Enable and accept option",
	RepSetCommand, NULL, (void *) SET_YES },
    { "no [opt ...]",			"Disable and deny option",
	RepSetCommand, NULL, (void *) SET_NO },
    { NULL },
  };

/*
 * INTERNAL VARIABLES
 */

  static const struct confinfo	gConfList[] = {
    { 0,	0,			NULL		},
  };

/*
 * RepIncoming()
 */

void
RepIncoming(Link l)
{
    Rep		r = l->rep;
    struct ngm_mkpeer       mkp;
    union {
        u_char buf[sizeof(struct ng_mesg) + sizeof(struct nodeinfo)];
        struct ng_mesg reply;
    } repbuf;
    struct ng_mesg *const reply = &repbuf.reply;
    struct nodeinfo *ninfo = (struct nodeinfo *)&reply->data;
    char	buf[64];
    
    Log(LG_REP, ("[%s] REP: INCOMING event from %s (0)",
	r->name, l->name));
	
    if (!r->links[1])
	PhysGetRep(r);
    if (!r->links[1]) {
	PhysClose(l);
	return;
    }

    if (r->csock <= 0) {
	/* Create a new netgraph node to control TCP ksocket node. */
	if (NgMkSockNode(NULL, &r->csock, NULL) < 0) {
    	    Log(LG_ERR, ("[%s] REP: can't create control socket: %s",
    		r->name, strerror(errno)));
    	    PhysClose(l);
	    return;
	}
	(void)fcntl(r->csock, F_SETFD, 1);
    }

    snprintf(mkp.type, sizeof(mkp.type), "%s", NG_TEE_NODE_TYPE);
    snprintf(mkp.ourhook, sizeof(mkp.ourhook), "tee");
    snprintf(mkp.peerhook, sizeof(mkp.peerhook), NG_TEE_HOOK_LEFT2RIGHT);
    if (NgSendMsg(r->csock, ".:", NGM_GENERIC_COOKIE,
        NGM_MKPEER, &mkp, sizeof(mkp)) < 0) {
    	Log(LG_ERR, ("[%s] REP: can't attach %s %s node: %s",
    	    l->name, NG_TEE_NODE_TYPE, mkp.ourhook, strerror(errno)));
	close(r->csock);
    	PhysClose(l);
	return;
    }

    /* Get tee node ID */
    if (NgSendMsg(r->csock, ".:tee",
	NGM_GENERIC_COOKIE, NGM_NODEINFO, NULL, 0) != -1) {
	    if (NgRecvMsg(r->csock, reply, sizeof(repbuf), NULL) != -1) {
	        r->node_id = ninfo->id;
	    }
    }
    
    PhysGetCallingNum(r->links[0], buf, sizeof(buf));
    PhysSetCallingNum(r->links[1], buf);

    PhysGetCalledNum(r->links[0], buf, sizeof(buf));
    PhysSetCalledNum(r->links[1], buf);

    PhysOpen(r->links[1]);
}

/*
 * RepUp()
 */

void
RepUp(Link l)
{
    Rep r = l->rep;
    int n = (r->links[0] == l)?0:1;
    
    Log(LG_REP, ("[%s] REP: UP event from %s (%d)",
	r->name, l->name, n));

    r->p_up |= (1 << n);
    
    if (n == 1)
	PhysOpen(r->links[1-n]);

    if (r->p_up == 3 && r->csock > 0 && r->node_id) {
	char path[NG_PATHLEN + 1];
	
	snprintf(path, sizeof(path), "[%x]:", r->node_id);
	NgFuncShutdownNode(r->csock, r->name, path);
	r->node_id = 0;
	close(r->csock);
	r->csock = -1;
    }
}

/*
 * RepDown()
 */

void
RepDown(Link l)
{
    Rep r = l->rep;
    int n = (r->links[0] == l)?0:1;

    Log(LG_REP, ("[%s] REP: DOWN event from %s (%d)",
	r->name, l->name, n));

    r->p_up &= ~(1 << n);

    if (r->links[1-n])
	PhysClose(r->links[1-n]);

    if (r->csock > 0 && r->node_id) {
	char path[NG_PATHLEN + 1];
	
	snprintf(path, sizeof(path), "[%x]:", r->node_id);
	NgFuncShutdownNode(r->csock, r->name, path);
	r->node_id = 0;
	close(r->csock);
	r->csock = -1;
    }
    
    if (r->links[1-n] == NULL && !r->stay)
	RepShutdown(r);
}

/*
 * RepIsSync()
 */

int
RepIsSync(Link l) {
    Rep r = l->rep;
    int n = (r->links[0] == l)?0:1;
    
    return (PhysIsSync(r->links[1-n]));
}

/*
 * RepSetAccm()
 */

void
RepSetAccm(Link l, u_int32_t xmit, u_int32_t recv) {
    Rep r = l->rep;
    int n = (r->links[0] == l)?0:1;
    
    Log(LG_REP, ("[%s] REP: SetAccm(0x%08x, 0x%08x) from %s (%d)",
	r->name, xmit, recv, l->name, n));

    PhysSetAccm(r->links[1-n], xmit, recv);
}

/*
 * RepGetHook()
 */

int
RepGetHook(Link l, char *path, char *hook)
{
    Rep r = l->rep;
    int n = (r->links[0] == l)?0:1;

    if (r->node_id == 0)
	return (0);

    snprintf(path, NG_PATHLEN, "[%lx]:", (u_long)r->node_id);
    if (n == 0)
	snprintf(hook, NG_HOOKLEN, NG_TEE_HOOK_LEFT);
    else
	snprintf(hook, NG_HOOKLEN, NG_TEE_HOOK_RIGHT);
    return (1);
}

/*
 * RepCommand()
 *
 * Show list of all bundles or set bundle
 */

int
RepCommand(Context ctx, int ac, char *av[], void *arg)
{
  Rep	r;
  int	k;

  switch (ac) {
    case 0:

      Printf("Defined repeaters:\r\n");

      for (k = 0; k < gNumReps; k++)
	if ((r = gReps[k]) != NULL) {
	  Printf("\t%-15s", r->name);
	  RepShowLinks(ctx, r);
	}
      break;

    case 1:

      /* Change bundle, and link also if needed */
      if ((r = RepFind(av[0])) != NULL) {
        RESETREF(ctx->rep, r);
        RESETREF(ctx->bund, NULL);
        RESETREF(ctx->lnk, r->links[0]);
      } else
	Printf("Repeater \"%s\" not defined.\r\n", av[0]);
      break;

    default:
      return(-1);
  }
  return(0);
}

/*
 * RepCreateCmd()
 *
 * Create a new repeater.
 */

int
RepCreate(Context ctx, int ac, char *av[], void *arg)
{
    Rep		r, rt = NULL;
    int		k;
    int         tmpl = 0;
    int         stay = 0;

    RESETREF(ctx->lnk, NULL);
    RESETREF(ctx->bund, NULL);
    RESETREF(ctx->rep, NULL);

    if (ac < 1)
	return(-1);

    if (strcmp(av[0], "template") == 0) {
	tmpl = 1;
	stay = 1;
    } else if (strcmp(av[0], "static") == 0)
	stay = 1;

    if (ac - stay < 1 || ac - stay > 2)
	return(-1);

    if (strlen(av[0 + stay])>16) {
	Log(LG_ERR, ("repeater name \"%s\" is too long", av[0 + stay]));
	return (-1);
    }

    /* See if repeater name already taken */
    if ((r = RepFind(av[0 + stay])) != NULL) {
	Log(LG_ERR, ("repeater \"%s\" already exists", av[0 + stay]));
	return (-1);
    }

    if (ac - stay == 2) {
	/* See if template name specified */
	if ((rt = RepFind(av[1 + stay])) == NULL) {
	    Log(LG_ERR, ("Repeater template \"%s\" not found", av[1 + stay]));
	    return (0);
	}
	if (!rt->tmpl) {
	    Log(LG_ERR, ("Repeater \"%s\" is not a template", av[1 + stay]));
	    return (0);
	}
    }

    /* Create and initialize new link */
    if (rt) {
	r = RepInst(rt, av[0 + stay], tmpl, stay);
    } else {
        /* Create a new repeater structure */
        r = Malloc(MB_REP, sizeof(*r));
	snprintf(r->name, sizeof(r->name), "%s", av[0 + stay]);
	r->tmpl = tmpl;
	r->stay = stay;
	r->csock = -1;

	/* Add repeater to the list of repeaters and make it the current active repeater */
	for (k = 0; k < gNumReps && gReps[k] != NULL; k++);
	if (k == gNumReps)			/* add a new repeater pointer */
	    LengthenArray(&gReps, sizeof(*gReps), &gNumReps, MB_REP);
	r->id = k;
	gReps[k] = r;
	REF(r);
    }
  
    RESETREF(ctx->rep, r);

    /* Done */
    return(0);
}

/*
 * RepInst()
 */

Rep
RepInst(Rep rt, char *name, int tmpl, int stay)
{
    Rep 	r;
    int		k;

    /* Create and initialize new link */
    r = Malloc(MB_REP, sizeof(*r));
    memcpy(r, rt, sizeof(*r));
    r->tmpl = tmpl;
    r->stay = stay;
    r->refs = 0;

    /* Find a free rep pointer */
    for (k = 0; k < gNumReps && gReps[k] != NULL; k++);
    if (k == gNumReps)			/* add a new repeater pointer */
        LengthenArray(&gReps, sizeof(*gReps), &gNumReps, MB_REP);
    r->id = k;

    if (name)
	strlcpy(r->name, name, sizeof(r->name));
    else
	snprintf(r->name, sizeof(r->name), "%s-%d", rt->name, k);
    gReps[k] = r;
    REF(r);

    return (r);
}

/*
 * RepShutdown()
 *
 */
 
void
RepShutdown(Rep r)
{
    int k;

    gReps[r->id] = NULL;

    Log(LG_REP, ("[%s] Repeater shutdown", r->name));
    for (k = 0; k < 2; k++) {
	Link	l;
	if ((l = r->links[k]) != NULL)
	    if (!l->stay)
		LinkShutdown(l);
    }

    if (r->csock > 0 && r->node_id) {
	char path[NG_PATHLEN + 1];
	
	snprintf(path, sizeof(path), "[%x]:", r->node_id);
	NgFuncShutdownNode(r->csock, r->name, path);
	r->node_id = 0;
	close(r->csock);
	r->csock = -1;
    }
    r->dead = 1;
    UNREF(r);
}

/*
 * RepStat()
 *
 * Show state of a repeater
 */

int
RepStat(Context ctx, int ac, char *av[], void *arg)
{
  Rep	r;

  /* Find repeater they're talking about */
  switch (ac) {
    case 0:
      r = ctx->rep;
      break;
    case 1:
      if ((r = RepFind(av[0])) == NULL) {
	Printf("Repeater \"%s\" not defined.\r\n", av[0]);
	return(0);
      }
      break;
    default:
      return(-1);
  }

  /* Show stuff about the repeater */
  Printf("Repeater %s%s:\r\n", r->name, r->tmpl?" (template)":(r->stay?" (static)":""));
  Printf("\tLink template   : %s\r\n", r->linkt);
  Printf("\tLinks           : ");
  RepShowLinks(ctx, r);

  return(0);
}

/*
 * RepShowLinks()
 */

static void
RepShowLinks(Context ctx, Rep r)
{
    int		j;

    for (j = 0; j < 2; j++) {
	if (r->links[j]) {
	    Printf("%s", r->links[j]->name);
	    if (!r->links[j]->type)
    		Printf("[no type/%s] ",
      		    gPhysStateNames[r->links[j]->state]);
	    else
    		Printf("[%s/%s] ", r->links[j]->type->name,
      		    gPhysStateNames[r->links[j]->state]);
	}
    }
    Printf("\r\n");
}

/*
 * RepFind()
 *
 * Find a repeater structure
 */

Rep
RepFind(char *name)
{
  int	k;

  for (k = 0;
    k < gNumReps && (!gReps[k] || strcmp(gReps[k]->name, name));
    k++);
  return((k < gNumReps) ? gReps[k] : NULL);
}

/*
 * RepSetCommand()
 */

static int
RepSetCommand(Context ctx, int ac, char *av[], void *arg)
{
    Rep		r = ctx->rep;

  if (ac == 0)
    return(-1);
  switch ((intptr_t)arg) {
    case SET_LINK:
	if (ac != 1)
	    return(-1);
	snprintf(ctx->rep->linkt, sizeof(ctx->rep->linkt), "%s", av[0]);
	break;

    case SET_ACCEPT:
      AcceptCommand(ac, av, &r->options, gConfList);
      break;

    case SET_DENY:
      DenyCommand(ac, av, &r->options, gConfList);
      break;

    case SET_ENABLE:
      EnableCommand(ac, av, &r->options, gConfList);
      break;

    case SET_DISABLE:
      DisableCommand(ac, av, &r->options, gConfList);
      break;

    case SET_YES:
      YesCommand(ac, av, &r->options, gConfList);
      break;

    case SET_NO:
      NoCommand(ac, av, &r->options, gConfList);
      break;

    default:
      assert(0);
  }
  return(0);
}

