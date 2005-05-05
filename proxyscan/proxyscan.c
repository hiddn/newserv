
#include "proxyscan.h"

#include <sys/poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "../core/error.h"
#include "../core/events.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "../nick/nick.h"
#include "../core/hooks.h"
#include "../lib/sstring.h"
#include "../irc/irc_config.h"
#include "../localuser/localuser.h"
#include "../core/config.h"
#include <unistd.h>
#include "../core/schedule.h"
#include <string.h>
#include "../irc/irc.h"
#include "../lib/irc_string.h"

#define SCANTIMEOUT      60

#define SCANHOSTHASHSIZE 1000
#define SCANHASHSIZE     400

/* It's unlikely you'll get 100k of preamble before a connect... */
#define READ_SANITY_LIMIT 102400

scan *scantable[SCANHASHSIZE];

int listenfd;
int activescans;
int maxscans;
int queuedhosts;
int scansdone;
int rescaninterval;
int warningsent;
int glinedhosts;
time_t starttime;

int numscans; /* number of scan types currently valid */
scantype thescans[PSCAN_MAXSCANS];

unsigned int hitsbyclass[10];
unsigned int scansbyclass[10];

unsigned int myip;
sstring *myipstr;
unsigned short listenport;
int brokendb;

unsigned int ps_mailip;
unsigned int ps_mailport;
sstring *ps_mailname;

nick *proxyscannick;

FILE *ps_logfile;

/* Local functions */
void handlescansock(int fd, short events);
void timeoutscansock(void *arg);
void proxyscan_newnick(int hooknum, void *arg);
void proxyscan_lostnick(int hooknum, void *arg);
void proxyscanuserhandler(nick *target, int message, void **params);
void registerproxyscannick();
void killsock(scan *sp, int outcome);
void killallscans();
void proxyscanstats(int hooknum, void *arg);
void sendlagwarning();
void proxyscandostatus(nick *np);
void proxyscandebug(nick *np);
void proxyscan_newip(nick *np, unsigned long ip);
int proxyscan_addscantype(int type, int port);
int proxyscan_delscantype(int type, int port);

int proxyscan_addscantype(int type, int port) {
  /* Check we have a spare scan slot */
  
  if (numscans>=PSCAN_MAXSCANS)
    return 1;

  thescans[numscans].type=type;
  thescans[numscans].port=port;
  thescans[numscans].hits=0;
  
  numscans++;
  
  return 0;
}

int proxyscan_delscantype(int type, int port) {
  int i;
  
  for (i=0;i<numscans;i++)
    if (thescans[i].type==type && thescans[i].port==port)
      break;
      
  if (i>=numscans)
    return 1;
    
  memmove(thescans+i, thescans+(i+1), (PSCAN_MAXSCANS-(i+1)) * sizeof(scantype));
  numscans--;
  
  return 0;
}

void _init(void) {
  sstring *cfgstr;
  int ipbits[4];

  memset(scantable,0,sizeof(scantable));
  maxscans=200;
  activescans=0;
  queuedhosts=0;
  scansdone=0;
  warningsent=0;
  starttime=time(NULL);
  glinedhosts=0;

  /* Listen port */
  cfgstr=getcopyconfigitem("proxyscan","port","9999",6);
  listenport=strtol(cfgstr->content,NULL,10);
  freesstring(cfgstr);
  
  /* Max concurrent scans */
  cfgstr=getcopyconfigitem("proxyscan","maxscans","200",5);
  maxscans=strtol(cfgstr->content,NULL,10);
  freesstring(cfgstr);

  /* Clean host timeout */
  cfgstr=getcopyconfigitem("proxyscan","rescaninterval","3600",7);
  rescaninterval=strtol(cfgstr->content,NULL,10);
  cachehostinit(rescaninterval);
  freesstring(cfgstr);

  /* this default will NOT work well */
  myipstr=getcopyconfigitem("proxyscan","ip","127.0.0.1",16);
  
  sscanf(myipstr->content,"%d.%d.%d.%d",&ipbits[0],&ipbits[1],&ipbits[2],&ipbits[3]);
  
  myip=((ipbits[0]&0xFF)<<24)+((ipbits[1]&0xFF)<<16)+
    ((ipbits[2]&0xFF)<<8)+(ipbits[3]&0xFF);

  /* Mailer host */
  cfgstr=getcopyconfigitem("proxyscan","mailerip","",16);
  
#if defined(PROXYSCAN_MAIL)
  psm_mailerfd=-1;
  if (cfgstr) {
    sscanf(cfgstr->content,"%d.%d.%d.%d",&ipbits[0],&ipbits[1],&ipbits[2],&ipbits[3]);
    ps_mailip = ((ipbits[0]&0xFF)<<24)+((ipbits[1]&0xFF)<<16)+
                 ((ipbits[2]&0xFF)<<8)+(ipbits[3]&0xFF);
    ps_mailport=25;
    freesstring(cfgstr);
    
    ps_mailname=getcopyconfigitem("proxyscan","mailname","some.mail.server",HOSTLEN);
    Error("proxyscan",ERR_INFO,"Proxyscan mailer enabled; mailing to %s as %s.",IPtostr(ps_mailip),ps_mailname->content);
  } else {
    ps_mailport=0;
    ps_mailname=NULL;
  }
#endif

  proxyscannick=NULL;
  /* Set up our nick on the network */
  scheduleoneshot(time(NULL),&registerproxyscannick,NULL);

  registerhook(HOOK_NICK_NEWNICK,&proxyscan_newnick);

  registerhook(HOOK_CORE_STATSREQUEST,&proxyscanstats);

  /* Read in the clean hosts */
  loadcachehosts();

  /* Set up the database */
  if ((proxyscandbinit())!=0) {
    brokendb=1;
  } else {
    brokendb=0;
  }

  /* Default scan types */
  proxyscan_addscantype(STYPE_HTTP, 8080);
  proxyscan_addscantype(STYPE_HTTP, 80);
  proxyscan_addscantype(STYPE_HTTP, 6588);
  proxyscan_addscantype(STYPE_HTTP, 8000);
  proxyscan_addscantype(STYPE_HTTP, 3128);
  proxyscan_addscantype(STYPE_HTTP, 3802);
  proxyscan_addscantype(STYPE_HTTP, 5490);
  proxyscan_addscantype(STYPE_HTTP, 7441);
  proxyscan_addscantype(STYPE_HTTP, 808);
  proxyscan_addscantype(STYPE_HTTP, 3332);
  proxyscan_addscantype(STYPE_HTTP, 2282);
  proxyscan_addscantype(STYPE_SOCKS4, 1080);
  proxyscan_addscantype(STYPE_SOCKS5, 1080);
  proxyscan_addscantype(STYPE_SOCKS4, 1075);
  proxyscan_addscantype(STYPE_SOCKS5, 1075);
  proxyscan_addscantype(STYPE_SOCKS4, 2280);
  proxyscan_addscantype(STYPE_SOCKS5, 2280);
  proxyscan_addscantype(STYPE_SOCKS4, 1180);
  proxyscan_addscantype(STYPE_SOCKS5, 1180);
  proxyscan_addscantype(STYPE_WINGATE, 23);
  proxyscan_addscantype(STYPE_CISCO, 23);
  proxyscan_addscantype(STYPE_WINGATE, 1181);
  proxyscan_addscantype(STYPE_SOCKS5, 1978);
  proxyscan_addscantype(STYPE_SOCKS5, 1029);
  proxyscan_addscantype(STYPE_SOCKS5, 3801);
  proxyscan_addscantype(STYPE_SOCKS5, 3331);
  proxyscan_addscantype(STYPE_HTTP, 65506);
  proxyscan_addscantype(STYPE_HTTP, 63809);

  /* Schedule saves */
  schedulerecurring(time(NULL)+3600,0,3600,&dumpcachehosts,NULL);

  ps_logfile=fopen("proxyscan.log","a");
}

void registerproxyscannick(void *arg) {
  sstring *psnick,*psuser,*pshost,*psrealname;
  /* Set up our nick on the network */

  psnick=getcopyconfigitem("proxyscan","nick","P",NICKLEN);
  psuser=getcopyconfigitem("proxyscan","user","proxyscan",USERLEN);
  pshost=getcopyconfigitem("proxyscan","host","some.host",HOSTLEN);
  psrealname=getcopyconfigitem("proxyscan","realname","Proxyscan",REALLEN);

  proxyscannick=registerlocaluser(psnick->content,psuser->content,pshost->content,
				  psrealname->content,
				  NULL,UMODE_OPER|UMODE_SERVICE|UMODE_DEAF,
				  &proxyscanuserhandler);

  freesstring(psnick);
  freesstring(psuser);
  freesstring(pshost);
  freesstring(psrealname);
}

void _fini(void) {

  deregisterlocaluser(proxyscannick,NULL);

  deregisterhook(HOOK_NICK_NEWNICK,&proxyscan_newnick);

  deregisterhook(HOOK_CORE_STATSREQUEST,&proxyscanstats);

  deleteschedule(NULL,&dumpcachehosts,NULL);
  
  /* Kill any scans in progress */
  killallscans();

  /* Dump the database - AFTER killallscans() which prunes it */
  dumpcachehosts(NULL);

  /* free() all our structures */
  sfreeall();
  
  freesstring(ps_mailname);
#if defined(PROXYSCAN_MAIL)
  if (psm_mailerfd!=-1)
    deregisterhandler(psm_mailerfd,1);
#endif
    
  if (ps_logfile)
    fclose(ps_logfile);
}

void proxyscanuserhandler(nick *target, int message, void **params) {
  nick *sender;
  char *msg;
  int i;

  switch(message) {
  case LU_KILLED:
    scheduleoneshot(time(NULL)+1,&registerproxyscannick,NULL);
    proxyscannick=NULL;
    break;

  case LU_PRIVMSG:
  case LU_SECUREMSG:
    sender=(nick *)params[0];
    msg=(char *)params[1];
    
    if (IsOper(sender)) {
      if (!ircd_strncmp(msg,"listopen",8)) {
	proxyscandolistopen(proxyscannick,sender,time(NULL)-rescaninterval);
      }
      
      if (!ircd_strncmp(msg,"status",6)) {
	proxyscandostatus(sender);
      }
      
      if (!ircd_strncmp(msg,"save",4)) {
	dumpcachehosts(NULL);
	sendnoticetouser(proxyscannick,sender,"Done.");
      }
      
      if (!ircd_strncmp(msg,"debug",5)) {
	proxyscandebug(sender);
      }
      
      if (!ircd_strncmp(msg,"scan ",5)) {
        unsigned long a,b,c,d;
        if (4 != sscanf(&msg[5],"%lu.%lu.%lu.%lu",&a,&b,&c,&d)) {
          sendnoticetouser(proxyscannick,sender,"Usage: scan a.b.c.d");
        } else {
          sendnoticetouser(proxyscannick,sender,"Forcing scan of %lu.%lu.%lu.%lu",a,b,c,d);
	  /* Just queue the scans directly here.. plonk them on the priority queue */
	  for(i=0;i<numscans;i++) {
	    queuescan((a<<24)+(b<<16)+(c<<8)+d,thescans[i].type,thescans[i].port,SCLASS_NORMAL,time(NULL));
	  }
        }      
      }

      if (!ircd_strncmp(msg,"addscan ",8)) {
	unsigned int a,b;
	if (sscanf(msg+8,"%u %u",&a,&b) != 2) {
	  sendnoticetouser(proxyscannick,sender,"Usage: addscan <type> <port>");
	} else {
	  sendnoticetouser(proxyscannick,sender,"Added scan type %u port %u",a,b);
	  proxyscan_addscantype(a,b);
	  scanall(a,b);
	}
      }

      if (!ircd_strncmp(msg,"delscan ",8)) {
	unsigned int a,b;
	if (sscanf(msg+8,"%u %u",&a,&b) != 2) {
	  sendnoticetouser(proxyscannick,sender,"Usage: delscan <type> <port>");
	} else {
	  sendnoticetouser(proxyscannick,sender,"Delete scan type %u port %u",a,b);
	  proxyscan_delscantype(a,b);
	}
      }	  

      if (!ircd_strncmp(msg,"help",4)) {
	sendnoticetouser(proxyscannick,sender,"Proxyscan commands:");
	sendnoticetouser(proxyscannick,sender,"-------------------------------------------");
	sendnoticetouser(proxyscannick,sender,"help      Shows this help");
	sendnoticetouser(proxyscannick,sender,"status    Prints status information");
	sendnoticetouser(proxyscannick,sender,"listopen  Shows open proxies found recently");
	sendnoticetouser(proxyscannick,sender,"save      Saves the clean host database");
      }
    }

  default:
    break;
  }
}

void addscantohash(scan *sp) {
  int hash;
  hash=(sp->fd)%SCANHASHSIZE;
  
  sp->next=scantable[hash];
  scantable[hash]=sp;
  
  activescans++;
}

void delscanfromhash(scan *sp) {
  int hash;
  scan **sh;
  
  hash=(sp->fd)%SCANHASHSIZE;
  
  for (sh=&(scantable[hash]);*sh;sh=&((*sh)->next)) {
    if (*sh==sp) {
      (*sh)=sp->next;
      break;
    }
  } 
  
  activescans--;
}

scan *findscan(int fd) {
  int hash;
  scan *sp;
  
  hash=fd%SCANHASHSIZE;
  
  for (sp=scantable[hash];sp;sp=sp->next)
    if (sp->fd==fd)
      return sp;
  
  return NULL;
}

void startscan(unsigned int IP, int type, int port, int class) {
  scan *sp;
  
  sp=getscan();
  
  sp->outcome=SOUTCOME_INPROGRESS;
  sp->port=port;
  sp->IP=IP;
  sp->type=type;
  sp->class=class;
  sp->bytesread=0;
  sp->totalbytesread=0;
  memset(sp->readbuf, '\0', PSCAN_READBUFSIZE);

  sp->fd=createconnectsocket(sp->IP,sp->port);
  sp->state=SSTATE_CONNECTING;
  if (sp->fd<0) {
    /* Couldn't set up the socket? */
    freescan(sp);
    return;
  }
  /* Wait until it is writeable */
  registerhandler(sp->fd,POLLERR|POLLHUP|POLLOUT,&handlescansock);
  /* And set a timeout */
  sp->sch=scheduleoneshot(time(NULL)+SCANTIMEOUT,&timeoutscansock,(void *)sp);
  addscantohash(sp);
}

void timeoutscansock(void *arg) {
  scan *sp=(scan *)arg;

  killsock(sp, SOUTCOME_CLOSED);
}

void killsock(scan *sp, int outcome) {
  int i;
  cachehost *chp;
  foundproxy *fpp;

  scansdone++;
  scansbyclass[sp->class]++;

  /* Remove the socket from the schedule/event lists */
  deregisterhandler(sp->fd,1);  /* this will close the fd for us */
  deleteschedule(sp->sch,&timeoutscansock,(void *)sp);

  sp->outcome=outcome;
  delscanfromhash(sp);

  /* See if we need to queue another scan.. */
  if (sp->outcome==SOUTCOME_CLOSED &&
      ((sp->class==SCLASS_CHECK) ||
       (sp->class==SCLASS_NORMAL && (sp->state==SSTATE_SENTREQUEST || sp->state==SSTATE_GOTRESPONSE))))
    queuescan(sp->IP, sp->type, sp->port, SCLASS_PASS2, time(NULL)+300);

  if (sp->outcome==SOUTCOME_CLOSED && sp->class==SCLASS_PASS2)
    queuescan(sp->IP, sp->type, sp->port, SCLASS_PASS3, time(NULL)+300);

  if (sp->outcome==SOUTCOME_CLOSED && sp->class==SCLASS_PASS3)
    queuescan(sp->IP, sp->type, sp->port, SCLASS_PASS4, time(NULL)+300);

  if (sp->outcome==SOUTCOME_OPEN) {
    hitsbyclass[sp->class]++;
  
    /* Lets try and get the cache record.  If there isn't one, make a new one. */
    if (!(chp=findcachehost(sp->IP)))
      chp=addcleanhost(sp->IP, time(NULL));
    
    /* Stick it on the cache's list of proxies, if necessary */
    for (fpp=chp->proxies;fpp;fpp=fpp->next)
      if (fpp->type==sp->type && fpp->port==sp->port)
	break;

    if (!fpp) {
      fpp=getfoundproxy();
      fpp->type=sp->type;
      fpp->port=sp->port;
      fpp->next=chp->proxies;
      chp->proxies=fpp;
    }
    
    if (!chp->glineid) {
      glinedhosts++;
      loggline(chp);
      irc_send("%s GL * +*@%s 1800 :Open Proxy, see http://www.quakenet.org/openproxies.html - ID: %d",
	       mynumeric->content,IPtostr(sp->IP),chp->glineid);
      Error("proxyscan",ERR_DEBUG,"Found open proxy on host %s",IPtostr(sp->IP));
    } else {
      loggline(chp);  /* Update log only */
    }

    /* Update counter */
    for(i=0;i<numscans;i++) {
      if (thescans[i].type==sp->type && thescans[i].port==sp->port) {
	thescans[i].hits++;
	break;
      }
    }
  }

  freescan(sp);

  /* kick the queue.. */
  startqueuedscans();
}

void handlescansock(int fd, short events) {
  scan *sp;
  char buf[512];
  int res;
  int i;
  unsigned long netip;
  unsigned short netport;

  if ((sp=findscan(fd))==NULL) {
    /* Not found; return and hope it goes away */
    Error("proxyscan",ERR_ERROR,"Unexpected message from fd %d",fd);
    return;
  }

  /* It woke up, delete the alarm call.. */
  deleteschedule(sp->sch,&timeoutscansock,(void *)sp);

  if (events & (POLLERR|POLLHUP)) {
    /* Some kind of error; give up on this socket */
    if (sp->state==SSTATE_GOTRESPONSE) {
      /* If the error occured while we were waiting for a response, we might have
       * received the "OPEN PROXY!" message and the EOF at the same time, so continue
       * processing */
/*      Error("proxyscan",ERR_DEBUG,"Got error in GOTRESPONSE state for %s, continuing.",IPtostr(sp->host->IP)); */
    } else {
      killsock(sp, SOUTCOME_CLOSED);
      return;
    }
  }  

  /* Otherwise, we got what we wanted.. */

  switch(sp->state) {
  case SSTATE_CONNECTING:
    /* OK, we got activity while connecting, so we're going to send some
     * request depending on scan type.  However, we can reregister everything
     * here to save duplicate code: This code is common for all handlers */

    /* Delete the old handler */
    deregisterhandler(fd,0);
    /* Set the new one */
    registerhandler(fd,POLLERR|POLLHUP|POLLIN,&handlescansock);
    sp->sch=scheduleoneshot(time(NULL)+SCANTIMEOUT,&timeoutscansock,(void *)sp);
    /* Update state */
    sp->state=SSTATE_SENTREQUEST;

    switch(sp->type) {
    case STYPE_HTTP:
      sprintf(buf,"CONNECT %s:%d HTTP/1.0\r\n\r\n",myipstr->content,listenport);
      if ((write(fd,buf,strlen(buf)))<strlen(buf)) {
	/* We didn't write the full amount, DIE */
	killsock(sp,SOUTCOME_CLOSED);
	return;
      }
      break;

    case STYPE_SOCKS4:
      /* set up the buffer */
      netip=htonl(myip);
      netport=htons(listenport);
      memcpy(&buf[4],&netip,4);
      memcpy(&buf[2],&netport,2);
      buf[0]=4;
      buf[1]=1;
      buf[8]=0;
      if ((write(fd,buf,9))<9) {
	/* Didn't write enough, give up */
	killsock(sp,SOUTCOME_CLOSED);
	return;
      }
      break;

    case STYPE_SOCKS5:
      /* Set up initial request buffer */
      buf[0]=5;
      buf[1]=1;
      buf[2]=0;
      if ((write(fd,buf,3))>3) {
	/* Didn't write enough, give up */
	killsock(sp,SOUTCOME_CLOSED);
	return;
      }
      
      /* Now the actual connect request */
      buf[0]=5;
      buf[1]=1;
      buf[2]=0;
      buf[3]=1;      
      netip=htonl(myip);
      netport=htons(listenport);
      memcpy(&buf[4],&netip,4);
      memcpy(&buf[8],&netport,2);
      res=write(fd,buf,10);
      if (res<10) {
	killsock(sp,SOUTCOME_CLOSED);
	return;
      }
      break;
      
    case STYPE_WINGATE:
      /* Send wingate request */
      sprintf(buf,"%s:%d\r\n",myipstr->content,listenport);
      if((write(fd,buf,strlen(buf)))<strlen(buf)) {
	killsock(sp,SOUTCOME_CLOSED);
	return;
      }
      break;
    
    case STYPE_CISCO:
      /* Send cisco request */
      sprintf(buf,"cisco\r\n");
      if ((write(fd,buf,strlen(buf)))<strlen(buf)) {
	killsock(sp, SOUTCOME_CLOSED);
        return;
      }
      
      sprintf(buf,"telnet %s %d\r\n",myipstr->content,listenport);
      if ((write(fd,buf,strlen(buf)))<strlen(buf)) {
	killsock(sp, SOUTCOME_CLOSED);
        return;
      }
      
      break;    
    }                
    break;
    
  case SSTATE_SENTREQUEST:
    res=read(fd, sp->readbuf+sp->bytesread, PSCAN_READBUFSIZE-sp->bytesread);
    
    if (res<=0) {
      if ((errno!=EINTR && errno!=EWOULDBLOCK) || res==0) {
        /* EOF, forget it */
        killsock(sp, SOUTCOME_CLOSED);
        return;
      }
    }
    
    sp->bytesread+=res;
    sp->totalbytesread+=res;
    for (i=0;i<sp->bytesread - MAGICSTRINGLENGTH;i++) {
      if (!strncmp(sp->readbuf+i, MAGICSTRING, MAGICSTRINGLENGTH)) {
        /* Found the magic string */
        /* If the offset is 0, this means it was the first thing we got from the socket, 
         * so it's an actual IRCD (sheesh).  Note that when the buffer is full and moved,
         * the thing moved to offset 0 would previously have been tested as offset 
         * PSCAN_READBUFSIZE/2. */
        if (i==0) {
          killsock(sp, SOUTCOME_CLOSED);
          return;
        }
        
        killsock(sp, SOUTCOME_OPEN);
        return;
      }
    }
    
    /* If the buffer is full, move half of it along to make room */
    if (sp->bytesread == PSCAN_READBUFSIZE) {
      memcpy(sp->readbuf, sp->readbuf + (PSCAN_READBUFSIZE)/2, PSCAN_READBUFSIZE/2);
      sp->bytesread = PSCAN_READBUFSIZE/2;
    }
    
    /* Don't read data forever.. */
    if (sp->totalbytesread > READ_SANITY_LIMIT) {
      killsock(sp, SOUTCOME_CLOSED);
      return;
    }
    
    /* No magic string yet, we schedule another timeout in case it comes later. */
    sp->sch=scheduleoneshot(time(NULL)+SCANTIMEOUT,&timeoutscansock,(void *)sp);
    return;    
  }
}

void killallscans() {
  int i;
  scan *sp;
  cachehost *chp;
  
  for(i=0;i<SCANHASHSIZE;i++) {
    for(sp=scantable[i];sp;sp=sp->next) {
      /* If there is a pending scan, delete it's clean host record.. */
      if ((chp=findcachehost(sp->IP)) && !chp->proxies)
        delcachehost(chp);
        
      if (sp->fd!=-1) {
	deregisterhandler(sp->fd,1);
	deleteschedule(sp->sch,&timeoutscansock,(void *)(sp));
      }
    }
  }
}

void proxyscanstats(int hooknum, void *arg) {
  char buf[512];
  
  sprintf(buf, "Proxyscn: %6d/%4d scans complete/in progress.  %d hosts queued.",
	  scansdone,activescans,queuedhosts);
  triggerhook(HOOK_CORE_STATSREPLY,buf);
  sprintf(buf, "Proxyscn: %6u known clean hosts",cleancount());
  triggerhook(HOOK_CORE_STATSREPLY,buf);  
}

void sendlagwarning() {
  int i,j;
  nick *np;

  for (i=0;i<MAXSERVERS;i++) {
    if (serverlist[i].maxusernum>0) {
      for(j=0;j<serverlist[i].maxusernum;j++) {
	np=servernicks[i][j];
	if (np!=NULL && IsOper(np)) {
	  sendnoticetouser(proxyscannick,np,"Warning: More than 20,000 hosts to scan - I'm lagging behind badly!");
	}
      }
    }
  }
}

void proxyscandostatus(nick *np) {
  int i;
  int totaldetects=0;
  
  sendnoticetouser(proxyscannick,np,"Service uptime: %s",longtoduration(time(NULL)-starttime, 1));
  sendnoticetouser(proxyscannick,np,"Total scans completed:  %d",scansdone);
  sendnoticetouser(proxyscannick,np,"Total hosts glined:     %d",glinedhosts);

  sendnoticetouser(proxyscannick,np,"Currently active scans: %d/%d",activescans,maxscans);
  sendnoticetouser(proxyscannick,np,"Normal queued scans:    %d",normalqueuedscans);
  sendnoticetouser(proxyscannick,np,"Timed queued scans:     %d",prioqueuedscans);
  sendnoticetouser(proxyscannick,np,"'Clean' cached hosts:   %d",cleancount());
  sendnoticetouser(proxyscannick,np,"'Dirty' cached hosts:   %d",dirtycount());
  
  for (i=0;i<5;i++)
    sendnoticetouser(proxyscannick,np,"Open proxies, class %1d:  %d/%d (%.2f%%)",i,hitsbyclass[i],scansbyclass[i],((float)hitsbyclass[i]*100)/scansbyclass[i]);
  
  for (i=0;i<numscans;i++)
    totaldetects+=thescans[i].hits;
  
  sendnoticetouser(proxyscannick,np,"Scan type Port  Detections");
  for (i=0;i<numscans;i++)
    sendnoticetouser(proxyscannick,np,"%-9s %-5d %d (%.2f%%)",
                     scantostr(thescans[i].type), thescans[i].port, thescans[i].hits, ((float)thescans[i].hits*100)/totaldetects);
  
  sendnoticetouser(proxyscannick,np,"End of list.");
}

void proxyscandebug(nick *np) {
  /* Dump all scans.. */
  int i;
  int activescansfound=0;
  int totalscansfound=0;
  scan *sp;

  sendnoticetouser(proxyscannick,np,"Active scans : %d",activescans);
  
  for (i=0;i<SCANHASHSIZE;i++) {
    for (sp=scantable[i];sp;sp=sp->next) {
      if (sp->outcome==SOUTCOME_INPROGRESS) {
	activescansfound++;
      }
      totalscansfound++;
      sendnoticetouser(proxyscannick,np,"fd: %d type: %d port: %d state: %d outcome: %d IP: %s",
		       sp->fd,sp->type,sp->port,sp->state,sp->outcome,IPtostr(sp->IP));
    }
  }

  sendnoticetouser(proxyscannick,np,"Total %d scans actually found (%d active)",totalscansfound,activescansfound);
}
