/* LAPB (AX25) timer recovery routines
 * Copyright 1991 Phil Karn, KA9Q
 */
#include "top.h"

#include "global.h"
#include "net/core/mbuf.h"
#include "core/timer.h"

#include "net/ax25/ax25.h"
#include "net/ax25/lapb.h"

static void tx_enq(struct ax25_cb *axp);

/* Called whenever timer T1 expires */
void
recover(void *p)
{
	struct ax25_cb *axp = (struct ax25_cb *)p;

	axp->flags.retrans = 1;
	axp->retries++;
	if((1L << axp->retries) < Blimit) {
		/* Back off retransmit timer */
		ax25_set_t1_timer(axp, dur_timer(&axp->t1)*2);
	}

	switch(axp->state){
	case LAPB_SETUP:
		if(axp->n2 != 0 && axp->retries > axp->n2){
			free_q(&axp->txq);
			axp->reason = LB_TIMEOUT;
			lapbstate(axp,LAPB_DISCONNECTED);
		} else {
			sendctl(axp,LAPB_COMMAND,SABM|PF);
			start_timer(&axp->t1);
		}
		break;
	case LAPB_DISCPENDING:
		if(axp->n2 != 0 && axp->retries > axp->n2){
			axp->reason = LB_TIMEOUT;
			lapbstate(axp,LAPB_DISCONNECTED);
		} else {
			sendctl(axp,LAPB_COMMAND,DISC|PF);
			start_timer(&axp->t1);
		}
		break;
	case LAPB_CONNECTED:
	case LAPB_RECOVERY:
		if(axp->n2 != 0 && axp->retries > axp->n2){
			/* Give up */
			sendctl(axp,LAPB_RESPONSE,DM|PF);
			free_q(&axp->txq);
			axp->reason = LB_TIMEOUT;
			lapbstate(axp,LAPB_DISCONNECTED);
		} else {
			/* Transmit poll */
			tx_enq(axp);
			lapbstate(axp,LAPB_RECOVERY);
		}
		break;
	default:
		break;
	}
}

/* Deferred lapb send */
void
defer_lapb_send(void *p)
{
	struct ax25_cb *axp = p;

	(void) dlapb_output(axp);
}

/* Send a poll (S-frame command with the poll bit set) */
void
pollthem(void *p)
{
	struct ax25_cb *axp;

	axp = (struct ax25_cb *)p;
	if(axp->proto == V1)
		return;	/* Not supported in the old protocol */
	switch(axp->state){
	case LAPB_CONNECTED:
		axp->retries = 0;
		tx_enq(axp);
		lapbstate(axp,LAPB_RECOVERY);
		break;
	default:
		break;
	}
}
/* Transmit query */
static void
tx_enq(struct ax25_cb *axp)
{
	char ctl;

#if 0
	struct mbuf *bp;

	/* I believe that retransmitting the oldest unacked
	 * I-frame tends to give better performance than polling,
	 * as long as the frame isn't too "large", because
	 * chances are that the I frame got lost anyway.
	 * This is an option in LAPB, but not in the official AX.25.
	 */
	if(axp->txq != NULL
	 && (len_p(axp->txq) < axp->pthresh || axp->proto == V1)){
		/* Retransmit oldest unacked I-frame */
		dup_p(&bp,axp->txq,0,len_p(axp->txq));
		ctl = PF | I | (((axp->vs - axp->unack) & MMASK) << 1)
		 | (axp->vr << 5);
		sendframe(axp,LAPB_COMMAND,ctl,&bp);
	} else {
		ctl = len_p(axp->rxq) >= axp->window ? RNR|PF : RR|PF;	
		sendctl(axp,LAPB_COMMAND,ctl);
	}
#else
	/*
	 * Just send a poll request, don't try to retransmit an older I-frame.
	 * For stacks that don't implement T2 doing the above may result in
	 * a lot of retransmitted packets as the receiver may see RRs followed
	 * by an I-frame poll for an earlier sequence number, leading to a REJ.
	 */
	ctl = len_p(axp->rxq) >= axp->window ? RNR|PF : RR|PF;
	sendctl(axp,LAPB_COMMAND,ctl);
#endif
	axp->response = 0;
	stop_timer(&axp->t3);
	start_timer(&axp->t1);
}
