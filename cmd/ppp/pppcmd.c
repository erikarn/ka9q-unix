/*
 *  PPPCMD.C	-- PPP related user commands
 *
 *	This implementation of PPP is declared to be in the public domain.
 *
 *	Jan 91	Bill_Simpson@um.cc.umich.edu
 *		Computer Systems Consulting Services
 *
 *	Acknowledgements and correction history may be found in PPP.C
 */
#include "top.h"

#include "lib/std/stdio.h"
#include "global.h"
#include "net/core/mbuf.h"
#include "net/core/iface.h"

#include "lib/util/cmdparse.h"

#include "net/ppp/ppp.h"
#include "net/ppp/pppfsm.h"
#include "net/ppp/ppplcp.h"
#include "net/ppp/ppppap.h"
#include "net/ppp/pppipcp.h"

static struct iface *ppp_lookup(char *ifname);

static int doppp_quick(int argc, char *argv[], void *p);
static int doppp_trace(int argc, char *argv[], void *p);

static int spot(uint work,uint want,uint will,uint mask);
static void genstat(struct ppp_s *ppp_p);
static void lcpstat(struct fsm_s *fsm_p);
static void papstat(struct fsm_s *fsm_p);
static void ipcpstat(struct fsm_s *fsm_p);

static int dotry_nak(int argc, char *argv[], void *p);
static int dotry_req(int argc, char *argv[], void *p);
static int dotry_terminate(int argc, char *argv[], void *p);


/* "ppp" subcommands */
static struct cmds Pppcmds[] = {
	{ "ipcp",	doppp_ipcp,	0,	0,	NULL },
	{ "lcp",	doppp_lcp,	0,	0,	NULL },
	{ "pap",	doppp_pap,	0,	0,	NULL },
	{ "quick",	doppp_quick,	0,	0,	NULL },
	{ "trace",	doppp_trace,	0,	0,	NULL },
	{ NULL },
};

/* "ppp <iface> <ncp> try" subcommands */
static struct cmds PppTrycmds[] = {
	{ "configure",	dotry_req,	0,	0,	NULL },
	{ "failure",	dotry_nak,	0,	0,	NULL },
	{ "terminate",	dotry_terminate,	0,	0,	NULL },
	{ NULL },
};

static char *PPPStatus[] = {
	"Physical Line Dead",
	"Establishment Phase",
	"Authentication Phase",
	"Network Protocol Phase",
	"Termination Phase"
};

static char *NCPStatus[] = {
	"Closed",
	"Listening -- waiting for remote host to attempt open",
	"Starting configuration exchange",
	"Remote host accepted our request; waiting for remote request",
	"We accepted remote request; waiting for reply to our request",
	"Opened",
	"Terminate request sent to remote host"
};

int PPPtrace;
struct iface *PPPiface;  /* iface for trace */


/****************************************************************************/

static struct iface *
ppp_lookup(ifname)
char *ifname;
{
	struct iface *ifp;

	if ((ifp = if_lookup(ifname)) == NULL) {
		kprintf("%s: Interface unknown\n",ifname);
		return(NULL);
	}
	if (ifp->iftype->type != CL_PPP) {
		kprintf("%s: not a PPP interface\n",ifp->name);
		return(NULL);
	}
	return(ifp);
}

/****************************************************************************/

int
doppp_commands(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	struct iface *ifp;

	if (argc < 2) {
		kprintf( "ppp <iface> required\n" );
		return -1;
	}
	if ((ifp = ppp_lookup(argv[1])) == NULL)
		return -1;

	if ( argc == 2 ) {
		ppp_show( ifp );
		return 0;
	}

	return subcmd(Pppcmds, argc - 1, &argv[1], ifp);
}


/* Close connection on PPP interface */
int
doppp_close(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	struct fsm_s *fsm_p = p;

	fsm_p->flags &= ~(FSM_ACTIVE | FSM_PASSIVE);

	fsm_close( fsm_p );
	return 0;
}


int
doppp_passive(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	struct fsm_s *fsm_p = p;

	fsm_p->flags &= ~FSM_ACTIVE;
	fsm_p->flags |= FSM_PASSIVE;

	fsm_start(fsm_p);
	return 0;
}


int
doppp_active(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	struct fsm_s *fsm_p = p;

	fsm_p->flags &= ~FSM_PASSIVE;
	fsm_p->flags |= FSM_ACTIVE;

	if ( fsm_p->state < fsmLISTEN ) {
		fsm_p->state = fsmLISTEN;
	}
	return 0;
}


static int
doppp_quick(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	struct iface *ifp = p;
	struct ppp_s *ppp_p = ifp->edv;
	struct lcp_s *lcp_p = ppp_p->fsm[Lcp].pdv;
	struct ipcp_s *ipcp_p = ppp_p->fsm[IPcp].pdv;

	lcp_p->local.want.accm = 0L;
	lcp_p->local.want.negotiate |= LCP_N_ACCM;
	lcp_p->local.want.magic_number += (long)&lcp_p->local.want.magic_number;
	lcp_p->local.want.negotiate |= LCP_N_MAGIC;
	lcp_p->local.want.negotiate |= LCP_N_ACFC;
	lcp_p->local.want.negotiate |= LCP_N_PFC;

	ipcp_p->local.want.compression = PPP_COMPR_PROTOCOL;
	ipcp_p->local.want.slots = 16;
	ipcp_p->local.want.slot_compress = 1;
	ipcp_p->local.want.negotiate |= IPCP_N_COMPRESS;
	doppp_active( 0, NULL, &(ppp_p->fsm[IPcp]) );

	return 0;
}


/****************************************************************************/

void
ppp_show(ifp)
struct iface *ifp;
{
	struct ppp_s *ppp_p = ifp->edv;

	genstat(ppp_p);
	if ( ppp_p->fsm[Lcp].pdv != NULL )
		lcpstat(&(ppp_p->fsm[Lcp]));
	if ( ppp_p->fsm[Pap].pdv != NULL )
		papstat(&(ppp_p->fsm[Pap]));
	if ( ppp_p->fsm[IPcp].pdv != NULL )
		ipcpstat(&(ppp_p->fsm[IPcp]));
}


static void
genstat(ppp_p)
struct ppp_s *ppp_p;
{

	kprintf("%s", PPPStatus[ppp_p->phase]);

	if (ppp_p->phase == pppREADY) {
		kprintf("\t(open for %s)",
			tformat(secclock() - ppp_p->upsince));
	}
	kprintf("\n");

	kprintf("%10lu In,  %10lu Flags,%6u ME, %6u FE, %6u CSE, %6u other\n",
		ppp_p->InRxOctetCount,
		ppp_p->InOpenFlag,
		ppp_p->InMemory,
		ppp_p->InFrame,
		ppp_p->InChecksum,
		ppp_p->InError);
	kprintf("\t\t%6u Lcp,%6u Pap,%6u IPcp,%6u Unknown\n",
		ppp_p->InNCP[Lcp],
		ppp_p->InNCP[Pap],
		ppp_p->InNCP[IPcp],
		ppp_p->InUnknown);
	kprintf("%10lu Out, %10lu Flags,%6u ME, %6u Fail\n",
		ppp_p->OutTxOctetCount,
		ppp_p->OutOpenFlag,
		ppp_p->OutMemory,
		ppp_p->OutError);
	kprintf("\t\t%6u Lcp,%6u Pap,%6u IPcp\n",
		ppp_p->OutNCP[Lcp],
		ppp_p->OutNCP[Pap],
		ppp_p->OutNCP[IPcp]);
}


static int
spot(work,want,will,mask)
uint work;
uint want;
uint will;
uint mask;
{
	char blot = ' ';
	int result = (work & mask);

	if ( !(will & mask) ) {
		blot = '*';
	} else if ( (want ^ work) & mask ) {
		blot = (result ? '+' : '-');
	}
	kprintf( "%c", blot );
	return result;
}

static void
lcpstat(fsm_p)
struct fsm_s *fsm_p;
{
	struct lcp_s *lcp_p = fsm_p->pdv;
	struct lcp_value_s *localp = &(lcp_p->local.work);
	uint  localwork = lcp_p->local.work.negotiate;
	uint  localwant = lcp_p->local.want.negotiate;
	uint  localwill = lcp_p->local.will_negotiate;
	struct lcp_value_s *remotep = &(lcp_p->remote.work);
	uint  remotework = lcp_p->remote.work.negotiate;
	uint  remotewant = lcp_p->remote.want.negotiate;
	uint  remotewill = lcp_p->remote.will_negotiate;

	kprintf("LCP %s\n",
		NCPStatus[fsm_p->state]);

	kprintf("\t\t MRU\t ACCM\t\t AP\t PFC  ACFC Magic\n");

	kprintf("\tLocal:\t");

	spot( localwork, localwant, localwill, LCP_N_MRU );
	kprintf( "%4d\t", localp->mru );

	spot( localwork, localwant, localwill, LCP_N_ACCM );
	kprintf( "0x%08lx\t", localp->accm );

	if ( spot( localwork, localwant, localwill, LCP_N_AUTHENT ) ) {
		switch ( localp->authentication ) {
		case PPP_PAP_PROTOCOL:
			kprintf( "Pap\t" );
			break;
		default:
			kprintf( "0x%04x\t", localp->authentication);
			break;
		};
	} else {
		kprintf( "None\t" );
	}

	kprintf( spot( localwork, localwant, localwill, LCP_N_PFC )
		 ? "Yes " : "No  " );
	kprintf( spot( localwork, localwant, localwill, LCP_N_ACFC )
		 ? "Yes " : "No  " );

	spot( localwork, localwant, localwill, LCP_N_MAGIC );
	if ( localp->magic_number != 0L ) {
		kprintf( "0x%08lx\n", localp->magic_number );
	} else {
		kprintf( "unused\n" );
	}

	kprintf("\tRemote:\t");

	spot( remotework, remotewant, remotewill, LCP_N_MRU );
	kprintf( "%4d\t", remotep->mru );

	spot( remotework, remotewant, remotewill, LCP_N_ACCM );
	kprintf( "0x%08lx\t", remotep->accm );

	if ( spot( remotework, remotewant, remotewill, LCP_N_AUTHENT ) ) {
		switch ( remotep->authentication ) {
		case PPP_PAP_PROTOCOL:
			kprintf( "Pap\t" );
			break;
		default:
			kprintf( "0x%04x\t", remotep->authentication);
			break;
		};
	} else {
		kprintf( "None\t" );
	}

	kprintf( spot( remotework, remotewant, remotewill, LCP_N_PFC )
		 ? "Yes " : "No  " );
	kprintf( spot( remotework, remotewant, remotewill, LCP_N_ACFC )
		 ? "Yes " : "No  " );

	spot( remotework, remotewant, remotewill, LCP_N_MAGIC );
	if ( remotep->magic_number != 0L ) {
		kprintf( "0x%08lx\n", remotep->magic_number );
	} else {
		kprintf( "unused\n" );
	}
}


static void
papstat(fsm_p)
struct fsm_s *fsm_p;
{
	struct pap_s *pap_p = fsm_p->pdv;

	kprintf("PAP %s\n",
		NCPStatus[fsm_p->state]);

	kprintf( "\tMessage: '%s'\n", (pap_p->message == NULL) ?
		"none" : pap_p->message );
}


static void
ipcpstat(fsm_p)
struct fsm_s *fsm_p;
{
	struct ipcp_s *ipcp_p = fsm_p->pdv;
	struct ipcp_value_s *localp = &(ipcp_p->local.work);
	uint  localwork = ipcp_p->local.work.negotiate;
	struct ipcp_value_s *remotep = &(ipcp_p->remote.work);
	uint  remotework = ipcp_p->remote.work.negotiate;

	kprintf("IPCP %s\n",
		NCPStatus[fsm_p->state]);
	kprintf("\tlocal IP address: %s",
		inet_ntoa(localp->address));
	kprintf("  remote IP address: %s\n",
		inet_ntoa(localp->other));

	if (localwork & IPCP_N_COMPRESS) {
		kprintf("    In\tTCP header compression enabled:"
			" slots = %d, flag = 0x%02x\n",
			localp->slots,
			localp->slot_compress);
		slhc_i_status(ipcp_p->slhcp);
	}

	if (remotework & IPCP_N_COMPRESS) {
		kprintf("    Out\tTCP header compression enabled:"
			" slots = %d, flag = 0x%02x\n",
			remotep->slots,
			remotep->slot_compress);
		slhc_o_status(ipcp_p->slhcp);
	}
}


/****************************************************************************/
/* Set timeout interval when waiting for response from remote peer */
int
doppp_timeout(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	struct fsm_s *fsm_p = p;
	struct timer *t = &(fsm_p->timer);

	if (argc < 2) {
		kprintf("%d\n",dur_timer(t)/1000L);
	} else {
		int x = (int)strtol( argv[1], NULL, 0 );

		if (x <= 0) {
			kprintf("Timeout value %s (%d) must be > 0\n",
				argv[1], x);
			return -1;
		} else {
			set_timer(t, x * 1000L);
		}
	}
	return 0;
}


int
doppp_try(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	return subcmd(PppTrycmds, argc, argv, p);
}


static int
dotry_nak(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	struct fsm_s *fsm_p = p;

	if (argc < 2) {
		kprintf("%d\n",fsm_p->try_nak);
	} else {
		int x = (int)strtol( argv[1], NULL, 0 );

		if (x <= 0) {
			kprintf("Value %s (%d) must be > 0\n",
				argv[1], x);
			return -1;
		} else {
			fsm_p->try_nak = x;
		}
	}
	return 0;
}


static int
dotry_req(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	struct fsm_s *fsm_p = p;

	if (argc < 2) {
		kprintf("%d\n",fsm_p->try_req);
	} else {
		int x = (int)strtol( argv[1], NULL, 0 );

		if (x <= 0) {
			kprintf("Value %s (%d) must be > 0\n",
				argv[1], x);
			return -1;
		} else {
			fsm_p->try_req = x;
		}
	}
	return 0;
}


static int
dotry_terminate(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	struct fsm_s *fsm_p = p;

	if (argc < 2) {
		kprintf("%d\n",fsm_p->try_terminate);
	} else {
		int x = (int)strtol( argv[1], NULL, 0 );

		if (x <= 0) {
			kprintf("Value %s (%d) must be > 0\n",
				argv[1], x);
			return -1;
		} else {
			fsm_p->try_terminate = x;
		}
	}
	return 0;
}


static int
doppp_trace(argc,argv,p)
int argc;
char *argv[];
void *p;
{
	struct iface *ifp = p;
	struct ppp_s *ppp_p = ifp->edv;
	int tracing = ppp_p->trace;
	int result = setint(&tracing,"PPP tracing",argc,argv);

	ppp_p->trace = tracing;
	return result;
}


