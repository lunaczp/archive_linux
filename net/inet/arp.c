/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		This file implements the Address Resolution Protocol (ARP),
 *		which is used by TCP/IP to map the IP addresses from a host
 *		to a low-level hardware address (like an Ethernet address)
 *		which it can use to talk to that host.
 *
 * NOTE:	This module will be rewritten completely in the near future,
 *		because I want it to become a multi-address-family address
 *		resolver, like it should be.  It will be put in a separate
 *		directory under 'net', being a protocol of its own. -FvK
 *
 * Version:	@(#)arp.c	1.0.15	05/25/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Stephen A. Wood, <saw@hallc1.cebaf.gov>
 *		Arnt Gulbrandsen, <agulbra@pvv.unit.no>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/config.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/errno.h>
#include <linux/if_arp.h>
#include <linux/in.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <stdarg.h>
#include "inet.h"
#include "timer.h"
#include "dev.h"
#include "eth.h"
#include "ip.h"
#include "route.h"
#include "protocol.h"
#include "tcp.h"
#include "skbuff.h"
#include "sock.h"
#include "arp.h"


#define ARP_MAX_TRIES	3


static char *unk_print(unsigned char *, int);
static char *eth_aprint(unsigned char *, int);


static char *arp_cmds[] = {
  "0x%04X",
  "REQUEST",
  "REPLY",
  "REVERSE REQUEST",
  "REVERSE REPLY",
  NULL
};
#define	ARP_MAX_CMDS	(sizeof(arp_cmds) / sizeof(arp_cmds[0]))

static struct {
  char	*name;
  char	*(*print)(unsigned char *ptr, int len);
} arp_types[] = {
  { "0x%04X",			unk_print	},
  { "10 Mbps Ethernet", 	eth_aprint	},
  { "3 Mbps Ethernet",		eth_aprint	},
  { "AX.25",			unk_print	},
  { "Pronet",			unk_print	},
  { "Chaos",			unk_print	},
  { "IEEE 802.2 Ethernet (?)",	eth_aprint	},
  { "Arcnet",			unk_print	},
  { "AppleTalk",		unk_print	},
  { NULL,			NULL		}
};
#define	ARP_MAX_TYPE	(sizeof(arp_types) / sizeof(arp_types[0]))


struct arp_table *arp_tables[ARP_TABLE_SIZE] = {
  NULL,
};
struct sk_buff *arp_q = NULL;


/* Dump the ADDRESS bytes of an unknown hardware type. */
static char *
unk_print(unsigned char *ptr, int len)
{
  static char buff[32];
  char *bufp = buff;
  int i;

  for (i = 0; i < len; i++)
	bufp += sprintf(bufp, "%02X ", (*ptr++ & 0377));
  return(buff);
}


/* Dump the ADDRESS bytes of an Ethernet hardware type. */
static char *
eth_aprint(unsigned char *ptr, int len)
{
  if (len != ETH_ALEN) return("");
  return(eth_print(ptr));
}


/* Dump an ARP packet. Not complete yet for non-Ethernet packets. */
static void
arp_print(struct arphdr *arp)
{
  int len, idx;
  unsigned char *ptr;

  if (inet_debug != DBG_ARP) return;

  printk("ARP: ");
  if (arp == NULL) {
	printk("(null)\n");
	return;
  }

  /* Print the opcode name. */
  len = htons(arp->ar_op);
  if (len < ARP_MAX_CMDS) idx = len;
    else idx = 0;
  printk("op ");
  printk(arp_cmds[idx], len);

  /* Print the ARP header. */
  len = htons(arp->ar_hrd);
  if (len < ARP_MAX_TYPE) idx = len;
    else idx = 0;
  printk("   hrd = "); printk(arp_types[idx].name, len);
  printk("   pro = 0x%04X\n", htons(arp->ar_pro));
  printk("   hlen = %d plen = %d\n", arp->ar_hln, arp->ar_pln);

  /*
   * Print the variable data.
   * When ARP gets redone (after the formal introduction of NET-2),
   * this part will be redone.  ARP will then be a multi-family address
   * resolver, and the code below will be made more general. -FvK
   */
  ptr = ((unsigned char *) &arp->ar_op) + sizeof(u_short);
  printk("   sender HA = %s ", arp_types[idx].print(ptr, arp->ar_hln));
  ptr += arp->ar_hln;
  printk("  PA = %s\n", in_ntoa(*(unsigned long *) ptr));
  ptr += arp->ar_pln;
  printk("   target HA = %s ", arp_types[idx].print(ptr, arp->ar_hln));
  ptr += arp->ar_hln;
  printk("  PA = %s\n", in_ntoa(*(unsigned long *) ptr));
}


/* This will try to retransmit everything on the queue. */
static void
arp_send_q(void)
{
  struct sk_buff *skb;
  struct sk_buff *next;

  cli();
  next = arp_q;
  arp_q = NULL;
  sti();
  while ((skb = next) != NULL) {
	if (skb->magic != ARP_QUEUE_MAGIC) {
		printk("ARP: *** Bug: skb with bad magic %X: squashing queue\n",
								skb->magic);
		return;
	}

	/* Extra consistency check. */
	if (skb->next == NULL
#ifdef CONFIG_MAX_16M
		|| ((unsigned long)(skb->next) > 16*1024*1024)
#endif
								) {
		printk("ARP: *** Bug: bad skb->next, squashing queue\n");
		return;
	}

	/* First remove skb from the queue. */
	next = skb->next;
	if (next != skb) {
		skb->prev->next = next;
		next->prev = skb->prev;
	} else {
		next = NULL;
	}

	skb->magic = 0;
	skb->next = NULL;
	skb->prev = NULL;

	/* Decrement the 'tries' counter. */
	cli();
	skb->tries--;
	if (skb->tries == 0) {
		/*
		 * Grmpf.
		 * We have tried ARP_MAX_TRIES to resolve the IP address
		 * from this datagram.  This means that the machine does
		 * not listen to our ARP requests.  Perhaps someone tur-
		 * ned off the thing?
		 * In any case, trying further is useless.  So, we kill
		 * this packet from the queue.  (grinnik) -FvK
		 */
		skb->sk = NULL;
		kfree_skb(skb, FREE_WRITE);

		sti();
		continue;
	}

	/* Can we now complete this packet? */
	sti();
	if (!skb->dev->rebuild_header(skb+1, skb->dev)) {
		/* Yes, so send it out. */
		skb->next = NULL;
		skb->prev = NULL;
		skb->arp  = 1;
		skb->dev->queue_xmit(skb, skb->dev, 0);
	} else {
		/* Alas.  Re-queue it... */
		cli();
		skb->magic = ARP_QUEUE_MAGIC;      
		if (arp_q == NULL) {
			skb->next = skb;
			skb->prev = skb;
			arp_q = skb;
		} else {
			skb->next = arp_q;
			skb->prev = arp_q->prev;  
			arp_q->prev->next = skb;
			arp_q->prev = skb;
		}
		sti();
	}
  }
}


/* Create and send our response to an ARP request. */
static int
arp_response(struct arphdr *arp1, struct device *dev)
{
  struct arphdr *arp2;
  struct sk_buff *skb;
  unsigned long src, dst;
  unsigned char *ptr1, *ptr2;
  int hlen;

  /* Get some mem and initialize it for the return trip. */
  skb = (struct sk_buff *) kmalloc(sizeof(struct sk_buff) +
  		sizeof(struct arphdr) +
		(2 * arp1->ar_hln) + (2 * arp1->ar_pln) +
		dev->hard_header_len, GFP_ATOMIC);
  if (skb == NULL) {
	printk("ARP: no memory available for ARP REPLY!\n");
	return(1);
  }

  /* Decode the source (REQUEST) message. */
  ptr1 = ((unsigned char *) &arp1->ar_op) + sizeof(u_short);
  src = *((unsigned long *) (ptr1 + arp1->ar_hln));
  dst = *((unsigned long *) (ptr1 + (arp1->ar_hln * 2) + arp1->ar_pln));

  skb->lock     = 0;
  skb->mem_addr = skb;
  skb->len      = sizeof(struct arphdr) + (2 * arp1->ar_hln) + 
		  (2 * arp1->ar_pln) + dev->hard_header_len;
  skb->mem_len  = sizeof(struct sk_buff) + skb->len;
  hlen = dev->hard_header((unsigned char *)(skb+1), dev,
			 ETH_P_ARP, src, dst, skb->len);
  if (hlen < 0) {
	printk("ARP: cannot create HW frame header for REPLY !\n");
	return(1);
  }

  /*
   * Fill in the ARP REPLY packet.
   * This looks ugly, but we have to deal with the variable-length
   * ARP packets and such.  It is not as bad as it looks- FvK
   */
  arp2 = (struct arphdr *) ((unsigned char *) (skb+1) + hlen);
  ptr2 = ((unsigned char *) &arp2->ar_op) + sizeof(u_short);
  arp2->ar_hrd = arp1->ar_hrd;
  arp2->ar_pro = arp1->ar_pro;
  arp2->ar_hln = arp1->ar_hln;
  arp2->ar_pln = arp1->ar_pln;
  arp2->ar_op = htons(ARPOP_REPLY);
  memcpy(ptr2, dev->dev_addr, arp2->ar_hln);
  ptr2 += arp2->ar_hln;
  memcpy(ptr2, ptr1 + (arp1->ar_hln * 2) + arp1->ar_pln, arp2->ar_pln);
  ptr2 += arp2->ar_pln;
  memcpy(ptr2, ptr1, arp2->ar_hln);
  ptr2 += arp2->ar_hln;
  memcpy(ptr2, ptr1 + arp1->ar_hln, arp2->ar_pln);

  skb->free = 1;
  skb->arp = 1;
  skb->sk = NULL;
  skb->next = NULL;

  DPRINTF((DBG_ARP, ">>"));
  arp_print(arp2);

  /* Queue the packet for transmission. */
  dev->queue_xmit(skb, dev, 0);
  return(0);
}


/* This will find an entry in the ARP table by looking at the IP address. */
static struct arp_table *
arp_lookup(unsigned long paddr)
{
  struct arp_table *apt;
  unsigned long hash;

  DPRINTF((DBG_ARP, "ARP: lookup(%s)\n", in_ntoa(paddr)));

  /* We don't want to ARP ourselves. */
  if (chk_addr(paddr) == IS_MYADDR) {
	printk("ARP: ARPing my own IP address %s !\n", in_ntoa(paddr));
	return(NULL);
  }

  /* Loop through the table for the desired address. */
  hash = htonl(paddr) & (ARP_TABLE_SIZE - 1);
  cli();
  apt = arp_tables[hash];
  while(apt != NULL) {
	if (apt->ip == paddr) {
		sti();
		return(apt);
	}
	apt = apt->next;
  }
  sti();
  return(NULL);
}


/* Delete an ARP mapping entry in the cache. */
void
arp_destroy(unsigned long paddr)
{
  struct arp_table *apt;
  struct arp_table **lapt;
  unsigned long hash;

  DPRINTF((DBG_ARP, "ARP: destroy(%s)\n", in_ntoa(paddr)));

  /* We cannot destroy our own ARP entry. */
  if (chk_addr(paddr) == IS_MYADDR) {
	DPRINTF((DBG_ARP, "ARP: Destroying my own IP address %s !\n",
							in_ntoa(paddr)));
	return;
  }
  hash = htonl(paddr) & (ARP_TABLE_SIZE - 1);

  cli();
  lapt = &arp_tables[hash];
  while ((apt = *lapt) != NULL) {
	if (apt->ip == paddr) {
		*lapt = apt->next;
		kfree_s(apt, sizeof(struct arp_table));
		sti();
		return;
	}
	lapt = &apt->next;
  }
  sti();
}


/* Create an ARP entry.  The caller should check for duplicates! */
static struct arp_table *
arp_create(unsigned long paddr, unsigned char *addr, int hlen, int htype)
{
  struct arp_table *apt;
  unsigned long hash;

  DPRINTF((DBG_ARP, "ARP: create(%s, ", in_ntoa(paddr)));
  DPRINTF((DBG_ARP, "%s, ", eth_print(addr)));
  DPRINTF((DBG_ARP, "%d, %d)\n", hlen, htype));

  apt = (struct arp_table *) kmalloc(sizeof(struct arp_table), GFP_ATOMIC);
  if (apt == NULL) {
	printk("ARP: no memory available for new ARP entry!\n");
	return(NULL);
  }

  /* Fill in the allocated ARP cache entry. */
  hash = htonl(paddr) & (ARP_TABLE_SIZE - 1);
  apt->ip = paddr;
  apt->hlen = hlen;
  apt->htype = htype;
  apt->flags = (ATF_INUSE | ATF_COM);	/* USED and COMPLETED entry */
  memcpy(apt->ha, addr, hlen);
  apt->last_used = timer_seq;
  cli();
  apt->next = arp_tables[hash];
  arp_tables[hash] = apt;
  sti();
  return(apt);
}


/*
 * An ARP REQUEST packet has arrived.
 * We try to be smart here, and fetch the data of the sender of the
 * packet- we might need it later, so fetching it now can save us a
 * broadcast later.
 * Then, if the packet was meant for us (i.e. the TARGET address was
 * one of our own IP addresses), we set up and send out an ARP REPLY
 * packet to the sender.
 */
int
arp_rcv(struct sk_buff *skb, struct device *dev, struct packet_type *pt)
{
  struct arphdr *arp;
  struct arp_table *tbl;
  unsigned long src, dst;
  unsigned char *ptr;
  int ret;

  DPRINTF((DBG_ARP, "<<\n"));
  arp = skb->h.arp;
  arp_print(arp);

  /* If this test doesn't pass, something fishy is going on. */
  if (arp->ar_hln != dev->addr_len || dev->type != NET16(arp->ar_hrd)) {
	printk("ARP: Bad packet received on device \"%s\" !\n", dev->name);
	kfree_skb(skb, FREE_READ);
	return(0);
  }

  /* For now we will only deal with IP addresses. */
  if (arp->ar_pro != NET16(ETH_P_IP) || arp->ar_pln != 4) {
	if (arp->ar_op != NET16(ARPOP_REQUEST))
		printk("ARP: Non-IP request on device \"%s\" !\n", dev->name);
	kfree_skb(skb, FREE_READ);
	return(0);
  }

  /*
   * As said before, we try to be smart by using the
   * info already present in the packet: the sender's
   * IP and hardware address.
   */
  ptr = ((unsigned char *) &arp->ar_op) + sizeof(u_short);
  memcpy(&src, ptr + arp->ar_hln, arp->ar_pln);
  tbl = arp_lookup(src);
  if (tbl != NULL) {
	DPRINTF((DBG_ARP, "ARP: udating entry for %s\n", in_ntoa(src)));
	memcpy(tbl->ha, ptr, arp->ar_hln);
	tbl->hlen = arp->ar_hln;
	tbl->flags |= ATF_COM;
	tbl->last_used = timer_seq;
  } else {
	memcpy(&dst, ptr + (arp->ar_hln * 2) + arp->ar_pln, arp->ar_pln);
	if (chk_addr(dst) != IS_MYADDR) {
		kfree_skb(skb, FREE_READ);
		return(0);
	} else {
		tbl = arp_create(src, ptr, arp->ar_hln, arp->ar_hrd);
		if (tbl == NULL) {
			kfree_skb(skb, FREE_READ);
			return(0);
		}
	}
  }

  /*
   * Since we updated the ARP cache, we might have enough
   * information to send out some previously queued IP
   * datagrams....
   */
  arp_send_q();

  /*
   * OK, we used that part of the info.  Now check if the
   * request was an ARP REQUEST for one of our own addresses...
   */
  if (arp->ar_op != NET16(ARPOP_REQUEST)) {
	kfree_skb(skb, FREE_READ);
	return(0);
  }
  memcpy(&dst, ptr + (arp->ar_hln * 2) + arp->ar_pln, arp->ar_pln);
  if (chk_addr(dst) != IS_MYADDR) {
	DPRINTF((DBG_ARP, "ARP: request was not for me!\n"));
	kfree_skb(skb, FREE_READ);
	return(0);
  }

  /*
   * Yes, it is for us.
   * Allocate, fill in and send an ARP REPLY packet.
   */
  ret = arp_response(arp, dev);
  kfree_skb(skb, FREE_READ);
  return(ret);
}


/* Create and send an ARP REQUEST packet. */
void
arp_send(unsigned long paddr, struct device *dev, unsigned long saddr)
{
  struct sk_buff *skb;
  struct arphdr *arp;
  unsigned char *ptr;
  int tmp;

  DPRINTF((DBG_ARP, "ARP: send(paddr=%s, ", in_ntoa(paddr)));
  DPRINTF((DBG_ARP, "dev=%s, ", dev->name));
  DPRINTF((DBG_ARP, "saddr=%s)\n", in_ntoa(saddr)));

  skb = (struct sk_buff *) kmalloc(sizeof(struct sk_buff) +
  		sizeof(struct arphdr) + (2 * dev->addr_len) +
		(2 * 4 /* arp->plen */), GFP_ATOMIC);
  if (skb == NULL) {
	printk("ARP: No memory available for REQUEST %s\n", in_ntoa(paddr));
	return;
  }
  
  /* Fill in the request. */
  skb->lock = 0;
  skb->sk = NULL;
  skb->mem_addr = skb;
  skb->len = sizeof(struct arphdr) +
	     dev->hard_header_len + (2 * dev->addr_len) + 8;
  skb->mem_len = sizeof(struct sk_buff) + skb->len;
  skb->arp = 1;
  skb->dev = dev;
  skb->next = NULL;
  tmp = dev->hard_header((unsigned char *)(skb+1), dev,
			  ETH_P_ARP, 0, saddr, skb->len);
  if (tmp < 0) {
	kfree_s(skb->mem_addr, skb->mem_len);
	return;
  }
  arp = (struct arphdr *) ((unsigned char *) (skb+1) + tmp);
  arp->ar_hrd = htons(dev->type);
  arp->ar_pro = htons(ETH_P_IP);
  arp->ar_hln = dev->addr_len;
  arp->ar_pln = 4;
  arp->ar_op = htons(ARPOP_REQUEST);

  ptr = ((unsigned char *) &arp->ar_op) + sizeof(u_short);
  memcpy(ptr, dev->dev_addr, arp->ar_hln);
  ptr += arp->ar_hln;
  memcpy(ptr, &saddr, arp->ar_pln);
  ptr += arp->ar_pln;
  memcpy(ptr, dev->broadcast, arp->ar_hln);
  ptr += arp->ar_hln;
  memcpy(ptr, &paddr, arp->ar_pln);

  DPRINTF((DBG_ARP, ">>\n"));
  arp_print(arp);
  dev->queue_xmit(skb, dev, 0);
}


/* Find an ARP mapping in the cache. If not found, post a REQUEST. */
int
arp_find(unsigned char *haddr, unsigned long paddr, struct device *dev,
	   unsigned long saddr)
{
  struct arp_table *apt;

  DPRINTF((DBG_ARP, "ARP: find(haddr=%s, ", eth_print(haddr)));
  DPRINTF((DBG_ARP, "paddr=%s, ", in_ntoa(paddr)));
  DPRINTF((DBG_ARP, "dev=%s, saddr=%s)\n", dev->name, in_ntoa(saddr)));

  switch(chk_addr(paddr)) {
	case IS_MYADDR:
		memcpy(haddr, dev->dev_addr, dev->addr_len);
		return(0);
	case IS_BROADCAST:
		memcpy(haddr, dev->broadcast, dev->addr_len);
		return(0);
  }
		
  apt = arp_lookup(paddr);
  if (apt != NULL) {
	/*
	 * Make sure it's not too old. If it is too old, we will
	 * just pretend we did not find it, and then arp_send will
	 * verify the address for us.
	 */
        if ((!(apt->flags & ATF_PERM)) ||
	    (!before(apt->last_used, timer_seq+ARP_TIMEOUT) && apt->hlen != 0)) {
		apt->last_used = timer_seq;
		memcpy(haddr, apt->ha, dev->addr_len);
		return(0);
	} else {
		DPRINTF((DBG_ARP, "ARP: find: found expired entry for %s\n",
							in_ntoa(apt->ip)));
	}
  }

  /*
   * This assume haddr are at least 4 bytes.
   * If this isn't true we can use a lookup table, one for every dev.
   * NOTE: this bit of code still looks fishy to me- FvK
   */
  *(unsigned long *)haddr = paddr;

  /* If we didn't find an entry, we will try to send an ARP packet. */
  arp_send(paddr, dev, saddr);

  return(1);
}


/* Add an entry to the ARP cache.  Check for dupes! */
void
arp_add(unsigned long addr, unsigned char *haddr, struct device *dev)
{
  struct arp_table *apt;

  DPRINTF((DBG_ARP, "ARP: add(%s, ", in_ntoa(addr)));
  DPRINTF((DBG_ARP, "%s, ", eth_print(haddr)));
  DPRINTF((DBG_ARP, "%d, %d)\n", dev->hard_header_len, dev->type));

  /* This is probably a good check... */
  if (addr == 0) {
	printk("ARP: add: will not add entry for 0.0.0.0 !\n");
	return;
  }

  /* First see if the address is already in the table. */
  apt = arp_lookup(addr);
  if (apt != NULL) {
	DPRINTF((DBG_ARP, "ARP: updating entry for %s\n", in_ntoa(addr)));
	apt->last_used = timer_seq;
	memcpy(apt->ha, haddr , dev->addr_len);
	return;
  }
  arp_create(addr, haddr, dev->addr_len, dev->type);
}


/* Create an ARP entry for a device's broadcast address. */
void
arp_add_broad(unsigned long addr, struct device *dev)
{
  struct arp_table *apt;

  arp_add(addr, dev->broadcast, dev);
  apt = arp_lookup(addr);
  if (apt != NULL) {
	apt->flags |= ATF_PERM;
  }
}


/* Queue an IP packet, while waiting for the ARP reply packet. */
void
arp_queue(struct sk_buff *skb)
{
  cli();
  skb->tries = ARP_MAX_TRIES;

  if (skb->next != NULL) {
	sti();
	printk("ARP: arp_queue skb already on queue magic=%X.\n", skb->magic);
	return;
  }
  if (arp_q == NULL) {
	arp_q = skb;
	skb->next = skb;
	skb->prev = skb;
  } else {
	skb->next = arp_q;
	skb->prev = arp_q->prev;
	skb->next->prev = skb;
	skb->prev->next = skb;
  }
  skb->magic = ARP_QUEUE_MAGIC;
  sti();
}


/*
 * Write the contents of the ARP cache to a PROCfs file.
 * This is not by long perfect, as the internal ARP table doesn't
 * have all the info we would like to have.  Oh well, it works for
 * now, eh? - FvK
 * Also note, that due to space limits, we cannot generate more than
 * 4Kbyte worth of data.  This usually is enough, but I have seen
 * machines die from under me because of a *very* large ARP cache.
 * This can be simply tested by doing:
 *
 *	# ping 255.255.255.255
 *	# arp -a
 *
 * Perhaps we should redo PROCfs to handle larger buffers?  Michael?
 */
int
arp_get_info(char *buffer)
{
  struct arpreq *req;
  struct arp_table *apt;
  int i;
  char *pos;

  /* Loop over the ARP table and copy structures to the buffer. */
  pos = buffer;
  i = 0;
  for (i = 0; i < ARP_TABLE_SIZE; i++) {
	cli();
	apt = arp_tables[i];
	sti();
	while (apt != NULL) {
		if (pos < (buffer + 4000)) {
			req = (struct arpreq *) pos;
			memset((char *) req, 0, sizeof(struct arpreq));
			req->arp_pa.sa_family = AF_INET;
			memcpy((char *) req->arp_pa.sa_data, (char *) &apt->ip, 4);
				req->arp_ha.sa_family = apt->htype;
			memcpy((char *) req->arp_ha.sa_data,
		       		(char *) &apt->ha, apt->hlen);
		}
		pos += sizeof(struct arpreq);
		cli();
		apt = apt->next;
		sti();
	}
  }
  return(pos - buffer);
}


/* Set (create) an ARP cache entry. */
static int
arp_req_set(struct arpreq *req)
{
  struct arpreq r;
  struct arp_table *apt;
  struct sockaddr_in *si;
  int htype, hlen;

  /* We only understand about IP addresses... */
  memcpy_fromfs(&r, req, sizeof(r));
  if (r.arp_pa.sa_family != AF_INET) return(-EPFNOSUPPORT);

  /*
   * Find out about the hardware type.
   * We have to be compatible with BSD UNIX, so we have to
   * assume that a "not set" value (i.e. 0) means Ethernet.
   */
  si = (struct sockaddr_in *) &r.arp_pa;
  switch(r.arp_ha.sa_family) {
	case 0:
	case ARPHRD_ETHER:
		htype = ARPHRD_ETHER;
		hlen = ETH_ALEN;
		break;
	default:
		return(-EPFNOSUPPORT);
  }

  /* Is there an existing entry for this address? */
  if (si->sin_addr.s_addr == 0) {
	printk("ARP: SETARP: requested PA is 0.0.0.0 !\n");
	return(-EINVAL);
  }
  apt = arp_lookup(si->sin_addr.s_addr);
  if (apt == NULL) {
	apt = arp_create(si->sin_addr.s_addr,
		(unsigned char *) r.arp_ha.sa_data, hlen, htype);
	if (apt == NULL) return(-ENOMEM);
  }

  /* We now have a pointer to an ARP entry.  Update it! */
  memcpy((char *) &apt->ha, (char *) &r.arp_ha.sa_data, hlen);
  apt->last_used = timer_seq;
  apt->flags = r.arp_flags;

  return(0);
}


/* Get an ARP cache entry. */
static int
arp_req_get(struct arpreq *req)
{
  struct arpreq r;
  struct arp_table *apt;
  struct sockaddr_in *si;

  /* We only understand about IP addresses... */
  memcpy_fromfs(&r, req, sizeof(r));
  if (r.arp_pa.sa_family != AF_INET) return(-EPFNOSUPPORT);

  /* Is there an existing entry for this address? */
  si = (struct sockaddr_in *) &r.arp_pa;
  apt = arp_lookup(si->sin_addr.s_addr);
  if (apt == NULL) return(-ENXIO);

  /* We found it; copy into structure. */
  memcpy((char *) r.arp_ha.sa_data, (char *) &apt->ha, apt->hlen);
  r.arp_ha.sa_family = apt->htype;

  /* Copy the information back */
  memcpy_tofs(req, &r, sizeof(r));
  return(0);
}


/* Delete an ARP cache entry. */
static int
arp_req_del(struct arpreq *req)
{
  struct arpreq r;
  struct sockaddr_in *si;

  /* We only understand about IP addresses... */
  memcpy_fromfs(&r, req, sizeof(r));
  if (r.arp_pa.sa_family != AF_INET) return(-EPFNOSUPPORT);

  si = (struct sockaddr_in *) &r.arp_pa;
  arp_destroy(si->sin_addr.s_addr);

  return(0);
}


/* Handle an ARP layer I/O control request. */
int
arp_ioctl(unsigned int cmd, void *arg)
{
  switch(cmd) {
	case DDIOCSDBG:
		return(dbg_ioctl(arg, DBG_ARP));
	case SIOCDARP:
		if (!suser()) return(-EPERM);
		return(arp_req_del((struct arpreq *)arg));
	case SIOCGARP:
		return(arp_req_get((struct arpreq *)arg));
	case SIOCSARP:
		if (!suser()) return(-EPERM);
		return(arp_req_set((struct arpreq *)arg));
	default:
		return(-EINVAL);
  }
  /*NOTREACHED*/
  return(0);
}
