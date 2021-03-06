#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"
#include "../port/netif.h"

#include "etherif.h"

static Ether *etherxx[MaxEther];

Chan*
etherattach(char* spec)
{
	ulong ctlrno;
	char *p;
	Chan *chan;
	Ether *ether;

	ctlrno = 0;
	if(spec && *spec){
		ctlrno = strtoul(spec, &p, 0);
		if((ctlrno == 0 && p == spec) || *p || (ctlrno >= MaxEther))
			error(Ebadarg);
	}
	if((ether = etherxx[ctlrno]) == 0)
		error(Enodev);
	rlock(ether);
	if(waserror()){
		runlock(ether);
		nexterror();
	}
	chan = devattach('l', spec);
	chan->dev = ctlrno;
	if(ether->attach)
		ether->attach(etherxx[ctlrno]);
	poperror();
	runlock(ether);
	return chan;
}

static void
ethershutdown(void)
{
	Ether *ether;
	int i;

	for(i=0; i<MaxEther; i++){
		ether = etherxx[i];
		if(ether != nil && ether->detach != nil)
			ether->detach(ether);
	}
}

static Walkqid*
etherwalk(Chan* chan, Chan *nchan, char **name, int nname)
{
	Walkqid *wq;
	Ether *ether;

	ether = etherxx[chan->dev];
	rlock(ether);
	if(waserror()) {
		runlock(ether);
		nexterror();
	}
	wq = netifwalk(etherxx[chan->dev], chan, nchan, name, nname);
	poperror();
	runlock(ether);
	return wq;
}

static int
etherstat(Chan* chan, uchar* dp, int n)
{
	int s;
	Ether *ether;

	ether = etherxx[chan->dev];
	rlock(ether);
	if(waserror()) {
		runlock(ether);
		nexterror();
	}
	s = netifstat(ether, chan, dp, n);
	poperror();
	runlock(ether);
	return s;
}

static Chan*
etheropen(Chan* chan, int omode)
{
	Chan *c;
	Ether *ether;

	ether = etherxx[chan->dev];
	rlock(ether);
	if(waserror()) {
		runlock(ether);
		nexterror();
	}
	c = netifopen(ether, chan, omode);
	poperror();
	runlock(ether);
	return c;
}

static void
ethercreate(Chan*, char*, int, ulong)
{
}

static void
etherclose(Chan* chan)
{
	Ether *ether;

	ether = etherxx[chan->dev];
	rlock(ether);
	if(waserror()) {
		runlock(ether);
		nexterror();
	}
	netifclose(ether, chan);
	poperror();
	runlock(ether);
}

static long
etherread(Chan* chan, void* buf, long n, vlong off)
{
	Ether *ether;
	ulong offset = off;
	long r;

	ether = etherxx[chan->dev];
	rlock(ether);
	if(waserror()) {
		runlock(ether);
		nexterror();
	}
	if((chan->qid.type & QTDIR) == 0 && ether->ifstat){
		/*
		 * With some controllers it is necessary to reach
		 * into the chip to extract statistics.
		 */
		if(NETTYPE(chan->qid.path) == Nifstatqid){
			r = ether->ifstat(ether, buf, n, offset);
			goto out;
		}
		if(NETTYPE(chan->qid.path) == Nstatqid)
			ether->ifstat(ether, buf, 0, offset);
	}
	r = netifread(ether, chan, buf, n, offset);
out:
	poperror();
	runlock(ether);
	return r;
}

static Block*
etherbread(Chan* chan, long n, ulong offset)
{
	Block *b;
	Ether *ether;

	ether = etherxx[chan->dev];
	rlock(ether);
	if(waserror()) {
		runlock(ether);
		nexterror();
	}
	b = netifbread(ether, chan, n, offset);
	poperror();
	runlock(ether);
	return b;
}

static int
etherwstat(Chan* chan, uchar* dp, int n)
{
	Ether *ether;
	int r;

	ether = etherxx[chan->dev];
	rlock(ether);
	if(waserror()) {
		runlock(ether);
		nexterror();
	}
	r = netifwstat(ether, chan, dp, n);
	poperror();
	runlock(ether);
	return r;
}

static void
etherrtrace(Netfile* f, Etherpkt* pkt, int len)
{
	int i, n;
	Block *bp;

	if(qwindow(f->in) <= 0)
		return;
	if(len > 58)
		n = 58;
	else
		n = len;
	bp = iallocb(64);
	if(bp == nil)
		return;
	memmove(bp->wp, pkt->d, n);
	i = TK2MS(MACHP(0)->ticks);
	bp->wp[58] = len>>8;
	bp->wp[59] = len;
	bp->wp[60] = i>>24;
	bp->wp[61] = i>>16;
	bp->wp[62] = i>>8;
	bp->wp[63] = i;
	bp->wp += 64;
	qpass(f->in, bp);
}

Block*
etheriq(Ether* ether, Block* bp, int fromwire)
{
	Etherpkt *pkt;
	ushort type;
	int len, multi, tome, fromme;
	Netfile **ep, *f, **fp, *fx;
	Block *xbp;

	ether->inpackets++;

	pkt = (Etherpkt*)bp->rp;
	len = BLEN(bp);
	type = (pkt->type[0]<<8)|pkt->type[1];
	fx = 0;
	ep = &ether->f[Ntypes];

	multi = pkt->d[0] & 1;
	/* check for valid multcast addresses */
	if(multi && memcmp(pkt->d, ether->bcast, sizeof(pkt->d)) != 0 && ether->prom == 0){
		if(!activemulti(ether, pkt->d, sizeof(pkt->d))){
			if(fromwire){
				freeb(bp);
				bp = 0;
			}
			return bp;
		}
	}

	/* is it for me? */
	tome = memcmp(pkt->d, ether->ea, sizeof(pkt->d)) == 0;
	fromme = memcmp(pkt->s, ether->ea, sizeof(pkt->s)) == 0;

	/*
	 * Multiplex the packet to all the connections which want it.
	 * If the packet is not to be used subsequently (fromwire != 0),
	 * attempt to simply pass it into one of the connections, thereby
	 * saving a copy of the data (usual case hopefully).
	 */
	for(fp = ether->f; fp < ep; fp++){
		if((f = *fp) && (f->type == type || f->type < 0))
		if(tome || multi || f->prom){
			/* Don't want to hear bridged packets */
			if(f->bridge && !fromwire && !fromme)
				continue;
			if(!f->headersonly){
				if(fromwire && fx == 0)
					fx = f;
				else if(xbp = iallocb(len)){
					memmove(xbp->wp, pkt, len);
					xbp->wp += len;
					if(qpass(f->in, xbp) < 0)
						ether->soverflows++;
				}
				else
					ether->soverflows++;
			}
			else
				etherrtrace(f, pkt, len);
		}
	}

	if(fx){
		if(qpass(fx->in, bp) < 0)
			ether->soverflows++;
		return 0;
	}
	if(fromwire){
		freeb(bp);
		return 0;
	}

	return bp;
}

static int
etheroq(Ether* ether, Block* bp)
{
	int len, loopback, s;
	Etherpkt *pkt;

	ether->outpackets++;

	/*
	 * Check if the packet has to be placed back onto the input queue,
	 * i.e. if it's a loopback or broadcast packet or the interface is
	 * in promiscuous mode.
	 * If it's a loopback packet indicate to etheriq that the data isn't
	 * needed and return, etheriq will pass-on or free the block.
	 * To enable bridging to work, only packets that were originated
	 * by this interface are fed back.
	 */
	pkt = (Etherpkt*)bp->rp;
	len = BLEN(bp);
	loopback = memcmp(pkt->d, ether->ea, sizeof(pkt->d)) == 0;
	if(loopback || memcmp(pkt->d, ether->bcast, sizeof(pkt->d)) == 0 || ether->prom){
		s = splhi();
		etheriq(ether, bp, 0);
		splx(s);
	}

	if(!loopback){
		qbwrite(ether->oq, bp);
		if(ether->transmit != nil)
			ether->transmit(ether);
	}else
		freeb(bp);

	return len;
}

static long
etherwrite(Chan* chan, void* buf, long n, vlong)
{
	Ether *ether;
	Block *bp;
	int onoff;
	Cmdbuf *cb;
	long l;

	ether = etherxx[chan->dev];
	rlock(ether);
	if(waserror()) {
		runlock(ether);
		nexterror();
	}
	if(NETTYPE(chan->qid.path) != Ndataqid) {
		l = netifwrite(ether, chan, buf, n);
		if(l >= 0)
			goto out;
		cb = parsecmd(buf, n);
		if(strcmp(cb->f[0], "nonblocking") == 0){
			if(cb->nf <= 1)
				onoff = 1;
			else
				onoff = atoi(cb->f[1]);
			qnoblock(ether->oq, onoff);
			free(cb);
			goto out;
		}
		free(cb);
		if(ether->ctl!=nil){
			l = ether->ctl(ether,buf,n);
			goto out;
		}
		error(Ebadctl);
	}

	if(n > ether->maxmtu)
		error(Etoobig);
	if(n < ether->minmtu)
		error(Etoosmall);
	bp = allocb(n);
	if(waserror()){
		freeb(bp);
		nexterror();
	}
	memmove(bp->rp, buf, n);
	memmove(bp->rp+Eaddrlen, ether->ea, Eaddrlen);
	bp->wp += n;
	poperror();

	l = etheroq(ether, bp);
out:
	poperror();
	runlock(ether);
	return l;
}

static long
etherbwrite(Chan* chan, Block* bp, ulong)
{
	Ether *ether;
	long n;

	n = BLEN(bp);
	if(NETTYPE(chan->qid.path) != Ndataqid){
		if(waserror()) {
			freeb(bp);
			nexterror();
		}
		n = etherwrite(chan, bp->rp, n, 0);
		poperror();
		freeb(bp);
		return n;
	}
	ether = etherxx[chan->dev];
	rlock(ether);
	if(waserror()) {
		runlock(ether);
		nexterror();
	}
	if(n > ether->maxmtu){
		freeb(bp);
		error(Etoobig);
	}
	if(n < ether->minmtu){
		freeb(bp);
		error(Etoosmall);
	}
	n = etheroq(ether, bp);
	poperror();
	runlock(ether);
	return n;
}

static struct {
	char*	type;
	int	(*reset)(Ether*);
} cards[MaxEther+1];

void
addethercard(char* t, int (*r)(Ether*))
{
	static int ncard;

	if(ncard == MaxEther)
		panic("too many ether cards");
	cards[ncard].type = t;
	cards[ncard].reset = r;
	ncard++;
}

int
parseether(uchar *to, char *from)
{
	char nip[4];
	char *p;
	int i;

	p = from;
	for(i = 0; i < Eaddrlen; i++){
		if(*p == 0)
			return -1;
		nip[0] = *p++;
		if(*p == 0)
			return -1;
		nip[1] = *p++;
		nip[2] = 0;
		to[i] = strtoul(nip, 0, 16);
		if(*p == ':')
			p++;
	}
	return 0;
}

static void
etherreset(void)
{
	Ether *ether;
	int i, n, ctlrno;
	char name[KNAMELEN], buf[128];

	for(ether = 0, ctlrno = 0; ctlrno < MaxEther; ctlrno++){
		if(ether == 0)
			ether = malloc(sizeof(Ether));
		memset(ether, 0, sizeof(Ether));
		ether->ctlrno = ctlrno;
		ether->mbps = 10;
		ether->minmtu = ETHERMINTU;
		ether->maxmtu = ETHERMAXTU;
		ether->tbdf = BUSUNKNOWN;

		if(archether(ctlrno, ether) <= 0)
			continue;

		for(n = 0; cards[n].type; n++){
			if(cistrcmp(cards[n].type, ether->type))
				continue;
			for(i = 0; i < ether->nopt; i++){
				if(cistrncmp(ether->opt[i], "ea=", 3) == 0){
					if(parseether(ether->ea, &ether->opt[i][3]) == -1)
						memset(ether->ea, 0, Eaddrlen);
				}else if(cistrcmp(ether->opt[i], "fullduplex") == 0 ||
					cistrcmp(ether->opt[i], "10BASE-TFD") == 0)
					ether->fullduplex = 1;
				else if(cistrcmp(ether->opt[i], "100BASE-TXFD") == 0)
					ether->mbps = 100;
			}
			if(cards[n].reset(ether))
				break;
			snprint(name, sizeof(name), "ether%d", ctlrno);

			if(ether->interrupt != nil)
				intrenable(ether->irq, ether->interrupt, ether, ether->tbdf, name);

			i = sprint(buf, "#l%d: %s: %dMbps port 0x%luX irq %lud",
				ctlrno, ether->type, ether->mbps, ether->port, ether->irq);
			if(ether->mem)
				i += sprint(buf+i, " addr 0x%luX", PADDR(ether->mem));
			if(ether->size)
				i += sprint(buf+i, " size 0x%luX", ether->size);
			i += sprint(buf+i, ": %2.2uX%2.2uX%2.2uX%2.2uX%2.2uX%2.2uX",
				ether->ea[0], ether->ea[1], ether->ea[2],
				ether->ea[3], ether->ea[4], ether->ea[5]);
			sprint(buf+i, "\n");
			print(buf);

			if(ether->mbps == 100){
				netifinit(ether, name, Ntypes, 256*1024);
				if(ether->oq == 0)
					ether->oq = qopen(256*1024, Qmsg, 0, 0);
			}
			else{
				netifinit(ether, name, Ntypes, 64*1024);
				if(ether->oq == 0)
					ether->oq = qopen(64*1024, Qmsg, 0, 0);
			}
			if(ether->oq == 0)
				panic("etherreset %s", name);
			ether->alen = Eaddrlen;
			memmove(ether->addr, ether->ea, Eaddrlen);
			memset(ether->bcast, 0xFF, Eaddrlen);

			etherxx[ctlrno] = ether;
			ether = 0;
			break;
		}
	}
	if(ether)
		free(ether);
}

static void
etherpower(int on)
{
	int i;
	Ether *ether;

	for(i = 0; i < MaxEther; i++){
		if((ether = etherxx[i]) == nil || ether->power == nil)
			continue;
		if(on){
			if(canrlock(ether))
				continue;
			if(ether->power != nil)
				ether->power(ether, on);
			wunlock(ether);
		}else{
			if(!canrlock(ether))
				continue;
			wlock(ether);
			if(ether->power != nil)
				ether->power(ether, on);
			/* Keep locked until power goes back on */
		}
	}
}

#define POLY 0xedb88320

/* really slow 32 bit crc for ethers */
ulong
ethercrc(uchar *p, int len)
{
	int i, j;
	ulong crc, b;

	crc = 0xffffffff;
	for(i = 0; i < len; i++){
		b = *p++;
		for(j = 0; j < 8; j++){
			crc = (crc>>1) ^ (((crc^b) & 1) ? POLY : 0);
			b >>= 1;
		}
	}
	return crc;
}

Dev etherdevtab = {
	'l',
	"ether",

	etherreset,
	devinit,
	ethershutdown,
	etherattach,
	etherwalk,
	etherstat,
	etheropen,
	ethercreate,
	etherclose,
	etherread,
	etherbread,
	etherwrite,
	etherbwrite,
	devremove,
	etherwstat,
	etherpower,
};
