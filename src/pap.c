
/*
 * pap.c
 *
 * Written by Toshiharu OHNO <tony-o@iij.ad.jp>
 * Copyright (c) 1993, Internet Initiative Japan, Inc. All rights reserved.
 * See ``COPYRIGHT.iij''
 * 
 * Rewritten by Archie Cobbs <archie@freebsd.org>
 * Copyright (c) 1995-1999 Whistle Communications, Inc. All rights reserved.
 * See ``COPYRIGHT.whistle''
 */

#include "ppp.h"
#include "pap.h"
#include "radius.h"
#include "auth.h"
#include "ngfunc.h"

/*
 * INTERNAL FUNCTIONS
 */

  static void	PapSendRequest(PapInfo pap);
  static void	PapTimeout(void *ptr);

/*
 * PapStart()
 */

void
PapStart(PapInfo pap, int which)
{
  switch (which) {
    case AUTH_PEER_TO_SELF:	/* Just wait for peer's request */
      break;

    case AUTH_SELF_TO_PEER:

      /* Initialize retry counter and timer */
      pap->next_id = 1;
      pap->retry = AUTH_RETRIES;

      TimerInit(&pap->timer, "PapTimer",
	lnk->conf.retry_timeout * SECONDS, PapTimeout, (void *) pap);
      TimerStart(&pap->timer);

      /* Send first request */
      PapSendRequest(pap);
      break;

    default:
      assert(0);
  }
}

/*
 * PapStop()
 */

void
PapStop(PapInfo pap)
{
  TimerStop(&pap->timer);
}

/*
 * PapSendRequest()
 *
 * Send a PAP packet to peer.
 */

static void
PapSendRequest(PapInfo pap)
{
  struct authdata	auth;
  int			name_len, pass_len;
  u_char		*pkt;

  /* Get password corresponding to my authname */
  memset(&auth, 0, sizeof(auth));
  strlcpy(auth.authname, bund->conf.authname, sizeof(auth.authname));
  Log(LG_AUTH, ("[%s] PAP: using authname \"%s\"", lnk->name, auth.authname));
  if (AuthGetData(&auth, 1, NULL) < 0)
    Log(LG_AUTH, (" Warning: no secret for \"%s\" found", auth.authname));

  /* Build response packet */
  name_len = strlen(auth.authname);
  pass_len = strlen(auth.password);

  pkt = Malloc(MB_AUTH, 1 + name_len + 1 + pass_len);
  pkt[0] = name_len;
  memcpy(pkt + 1, auth.authname, name_len);
  pkt[1 + name_len] = pass_len;
  memcpy(pkt + 1 + name_len + 1, auth.password, pass_len);

  /* Send it off */
  AuthOutput(PROTO_PAP, PAP_REQUEST, pap->next_id++, pkt,
    1 + name_len + 1 + pass_len, 0, 0);
  Freee(MB_AUTH, pkt);
}

/*
 * PapInput()
 *
 * Accept an incoming PAP packet
 */

void
PapInput(u_char code, u_char id, const u_char *pkt, u_short len)
{
  Auth			const a = &lnk->lcp.auth;
  PapInfo		const pap = &a->pap;

  /* Deal with packet */
  Log(LG_AUTH, ("[%s] PAP: rec'd %s #%d",
    lnk->name, PapCode(code), id));
  switch (code) {
    case PAP_REQUEST:
      {
	struct authdata	auth;
	char		*name_ptr, name[256];
	char		*pass_ptr, pass[256];
	const char	*failMesg;
	int		name_len, pass_len;
	int		whyFail;
	int		radRes = RAD_NACK;

	/* Is this appropriate? */
	if (a->peer_to_self != PROTO_PAP) {
	  Log(LG_AUTH, ("[%s] PAP: %s not expected",
	    lnk->name, PapCode(code)));
	  whyFail = AUTH_FAIL_NOT_EXPECTED;
	  goto badRequest;
	}

	name_len = pkt[0];
	name_ptr = (char *)pkt + 1;

	/* Sanity check packet and extract fields */
	if (1 + name_len >= len
	  || ((pass_len = pkt[1 + name_len]) && FALSE)
	  || ((pass_ptr = (char *)pkt + 1 + name_len + 1) && FALSE)
	  || name_len + 1 + pass_len + 1 > len)
	{
	  Log(LG_AUTH, (" Bad packet"));
	  whyFail = AUTH_FAIL_INVALID_PACKET;
	  goto badRequest;
	}
	memcpy(name, name_ptr, name_len);
	name[name_len] = 0;
	memcpy(pass, pass_ptr, pass_len);
	pass[pass_len] = 0;

	/* Initialize 'auth' info */
	memset(&auth, 0, sizeof(auth));
	strlcpy(auth.authname, name, sizeof(auth.authname));

	/* perform pre authentication checks (single-login, etc.) */
	if (AuthPreChecks(&auth, 1, &whyFail) < 0) {
	  Log(LG_AUTH, (" AuthPreCheck failed for \"%s\"", auth.authname));
	  goto badRequest;
	}

	/* Try RADIUS auth if configured */
	if (Enabled(&bund->conf.options, BUND_CONF_RADIUSAUTH)) {
	  radRes = RadiusPAPAuthenticate(name, pass);
	  if (radRes == RAD_ACK) {
	    RadiusSetAuth(&auth);
	    goto goodRequest;
	  }
	  if (!Enabled(&bund->conf.options, BUND_CONF_RADIUSFALLBACK)) {
	    whyFail = AUTH_FAIL_INVALID_LOGIN;
	    goto badRequest;
	  }
	}

	/* Get auth data for this system */
	Log(LG_AUTH, (" Peer name: \"%s\"", auth.authname));
	if (AuthGetData(&auth, 1, &whyFail) < 0) {
	  Log(LG_AUTH, (" Can't get credentials for \"%s\"", auth.authname));
	  goto badRequest;
	}

	/* Do name & password match? */
	if (strcmp(auth.authname, name) || strcmp(auth.password, pass)) {
	  Log(LG_AUTH, (" Invalid response"));
	  whyFail = AUTH_FAIL_INVALID_LOGIN;
badRequest:
	  failMesg = AuthFailMsg(PROTO_PAP, 0, whyFail);
	  AuthOutput(PROTO_PAP, PAP_NAK, id, failMesg, strlen(failMesg), 1, 0);
	  AuthFinish(AUTH_PEER_TO_SELF, FALSE, &auth);
	  break;
	}

goodRequest:
	/* Login accepted */
	Log(LG_AUTH, (" Response is valid"));
	AuthOutput(PROTO_PAP, PAP_ACK, id, AUTH_MSG_WELCOME,
	  strlen(AUTH_MSG_WELCOME), 1, 0);
	AuthFinish(AUTH_PEER_TO_SELF, TRUE, &auth);
      }
      break;

    case PAP_ACK:
    case PAP_NAK:
      {
	char	*msg;
	int	msg_len;

	/* Is this appropriate? */
	if (a->self_to_peer != PROTO_PAP) {
	  Log(LG_AUTH, ("[%s] PAP: %s not expected",
	    lnk->name, PapCode(code)));
	  break;
	}

	/* Stop resend timer */
	TimerStop(&pap->timer);

	/* Show reply message */
	msg_len = pkt[0];
	msg = (char *) &pkt[1];
	if (msg_len < len - 1)
	  msg_len = len - 1;
	ShowMesg(LG_AUTH, msg, msg_len);

	/* Done with my auth to peer */
	AuthFinish(AUTH_SELF_TO_PEER, code == PAP_ACK, NULL);
      }
      break;

    default:
      Log(LG_AUTH, ("[%s] PAP: unknown code", lnk->name));
      break;
  }
}

/*
 * PapTimeout()
 *
 * Timer expired for reply to our request
 */

static void
PapTimeout(void *ptr)
{
  PapInfo	const pap = (PapInfo) ptr;

  TimerStop(&pap->timer);
  if (--pap->retry > 0) {
    TimerStart(&pap->timer);
    PapSendRequest(pap);
  }
}

/*
 * PapCode()
 */

const char *
PapCode(int code)
{
  static char	buf[12];

  switch (code) {
    case PAP_REQUEST:
      return("REQUEST");
    case PAP_ACK:
      return("ACK");
    case PAP_NAK:
      return("NAK");
    default:
      snprintf(buf, sizeof(buf), "code%d", code);
      return(buf);
  }
}

