/* dbScan.c */
/* share/src/db  $Id$ */

/* tasks and subroutines to scan the database */
/*
 *      Original Author:        Bob Dalesio
 *      Current Author:		Marty Kraimer
 *      Date:   	        07/18/91
 *
 *      Experimental Physics and Industrial Control System (EPICS)
 *
 *      Copyright 1991, the Regents of the University of California,
 *      and the University of Chicago Board of Governors.
 *
 *      This software was produced under  U.S. Government contracts:
 *      (W-7405-ENG-36) at the Los Alamos National Laboratory,
 *      and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *      Initial development by:
 *              The Controls and Automation Group (AT-8)
 *              Ground Test Accelerator
 *              Accelerator Technology Division
 *              Los Alamos National Laboratory
 *
 *      Co-developed with
 *              The Controls and Computing Group
 *              Accelerator Systems Division
 *              Advanced Photon Source
 *              Argonne National Laboratory
 *
 * Modification Log:
 * -----------------
 * .18  07-18-91	mrk	major revision
 * .19  02-05-92	jba	Changed function arguments from paddr to precord 
 * .20	05-19-92	mrk	Changes for internal database structure changes
 */

#include	<vxWorks.h>
#include	<stdlib.h>
#include	<types.h>
#include	<semLib.h>
#include 	<rngLib.h>
#include 	<lstLib.h>
#include 	<string.h>

#include	<dbDefs.h>
#include	<dbAccess.h>
#include	<dbScan.h>
#include	<taskwd.h>
#include	<callback.h>
#include	<dbBase.h>
#include	<dbCommon.h>
#include	<dbRecords.h>
#include	<devSup.h>
#include	<task_params.h>
#include	<fast_lock.h>
#include	<dbManipulate.h>

extern struct dbBase *pdbBase;
extern volatile int interruptAccept;

struct scan_list{
	FAST_LOCK	lock;
	LIST		list;
	short		modified;/*has list been modified?*/
	long		ticks;	/*ticks per period for periodic*/
};
/*scan_elements are allocated and the address stored in dbCommon.spvt*/
struct scan_element{
	NODE			node;
	struct scan_list	*pscan_list;
	struct dbCommon		*precord;
};

int volatile scanRestart=FALSE;

/* PERIODIC SCANNER */
static int nPeriodic=0;
static struct scan_list **papPeriodic; /* pointer to array of pointers*/
static int *periodicTaskId;		/*array of integers after allocation*/

/* EVENT */
#define MAX_EVENTS 256
#define EVENT_QUEUE_SIZE 1000
static struct scan_list *papEvent[MAX_EVENTS];/*array of pointers*/
static SEM_ID eventSem;
static RING_ID eventQ;
static int eventTaskId;

/* IO_EVENT*/
struct io_scan_list {
	CALLBACK		callback;
	struct scan_list	scan_list;
	struct io_scan_list	*next;
};

static struct io_scan_list *iosl_head[NUM_CALLBACK_PRIORITIES]={NULL,NULL,NULL};

/* Private routines */
void periodicTask();	/*Periodic scan task			*/
void initPeriodic();	/*Initialize the periodic variables	*/
void spawnPeriodic();	/*Spawn the periodTasks			*/
void wdPeriodic();	/*watchdog callback for periodicTasks	*/
void eventTask();	/*Periodic scan task			*/
void initEvent();	/*Initialize the event variables	*/
void spawnEvent();	/*Spawn the eventTask			*/
void wdEvent();		/*watchdog callback for eventTask	*/
void ioeventCallback();	/*ioevent callback 			*/
void printList();	/*print a scan list			*/

void scanList();	/*Scan a scan list			*/
void buildScanLists();	/*Build scan lists			*/
void addToList();	/*add element to a list			*/
void deleteFromList();	/*delete element from a list		*/

long scanInit()
{
	int i;

	initPeriodic();
	initEvent();
	scan_init(); /*old IO_EVENT_SCAN*/
	buildScanLists();
	for (i=0;i<nPeriodic; i++) spawnPeriodic(i);
	spawnEvent();
	return(0);
}

void post_event(int event)
{
	unsigned char evnt;
	int status;

	if (!interruptAccept) return;     /* not awake yet */
	if(event<0 || event>=MAX_EVENTS) {
		logMsg("illegal event passed to post_event\n");
		return;
	}
	evnt = (unsigned)event;
	/*multiple writers can exist. Thus if evnt is ever changed to use*/
	/*something bigger than a character interrupts will have to be blocked*/
	if(rngBufPut(eventQ,(void *)&evnt,sizeof(unsigned char))!=sizeof(unsigned char))
	    logMsg("rngBufPut overflow in post_event\n");
	if((status=semGive(eventSem))!=OK){
/*semGive randomly returns garbage value*/
/*
   		 logMsg("semGive returned error in post_event status= %d\n",status);
*/
        }
}


void scanAdd(struct dbCommon *precord)
{
	short		scan;
	short		phase;
	short		event;
	long		status;
	struct scan_list *psl;

	/* get the list on which this record belongs */
	scan = precord->scan;
	if(scan==SCAN_PASSIVE) return;
	if(scan<0 || scan>= nPeriodic+SCAN_1ST_PERIODIC) {
	    recGblRecordError(-1,precord,"scanAdd detected illegal SCAN value");
	}else if(scan==SCAN_EVENT) {
	    unsigned char evnt;

	    if(precord->evnt<0 || precord->evnt>=MAX_EVENTS) {
		recGblRecordError(S_db_badField,precord,"scanAdd detected illegal EVNT value");
		return;
	    }
	    evnt = (signed)precord->evnt;
	    psl = papEvent[evnt];
	    if(psl==NULL) {
		psl = calloc(1,sizeof(struct scan_list));
		papEvent[precord->evnt] = psl;
		FASTLOCKINIT(&psl->lock);
		lstInit(&psl->list);
	    }
	    addToList(precord,psl);
	} else if(scan==SCAN_IO_EVENT) {
	    short		cmd=0;
	    struct io_scan_list *piosl;
	    int			priority,dummy1,dummy2;
	    DEVSUPFUN get_ioint_info=precord->dset->get_ioint_info;

	    if(get_ioint_info==NULL) return;
	    if(get_ioint_info(&cmd,precord,&piosl,&dummy1,&dummy2)) return;/*return if error*/
	    if(cmd==-1) {
		add_to_scan_list(precord); /*old IO_EVENT_SCAN*/
	    } else {
		if(piosl==NULL) return;
		priority = precord->prio;
		if(priority<0 || priority>=NUM_CALLBACK_PRIORITIES) {
		    recGblRecordError(-1,precord,"scanAdd: illegal prio field");
		    return;
		}
		piosl += priority; /* get piosl for correct priority*/
		addToList(precord,&piosl->scan_list);
	    }
	} else if(scan>=SCAN_1ST_PERIODIC) {
	    int	ind;

	    ind = scan - SCAN_1ST_PERIODIC;
	    psl = papPeriodic[ind];
	    addToList(precord,psl);
	}
	return;
}

void scanDelete(struct dbCommon *precord)
{
	short		scan;
	short		phase;
	short		event;
	long		status;
	struct scan_list *psl;

	/* get the list on which this record belongs */
	scan = precord->scan;
	if(scan==SCAN_PASSIVE) return;
	if(scan<0 || scan>= nPeriodic+SCAN_1ST_PERIODIC) {
	   recGblRecordError(-1,precord,"scanDelete detected illegal SCAN value");
	}else if(scan==SCAN_EVENT) {
	    unsigned char evnt;

	    if(precord->evnt<0 || precord->evnt>=MAX_EVENTS) {
		recGblRecordError(S_db_badField,precord,"scanDelete detected illegal EVNT value");
		return;
	    }
	    evnt = (signed)precord->evnt;
	    psl = papEvent[evnt];
	    if(psl==NULL) 
		 recGblRecordError(-1,precord,"scanDelete for bad evnt");
	    else
		deleteFromList(precord,psl);
	} else if(scan==SCAN_IO_EVENT) {
	    short		cmd=1;
	    struct io_scan_list *piosl;
	    int			priority,dummy1,dummy2;
	    DEVSUPFUN get_ioint_info=precord->dset->get_ioint_info;

	    if(get_ioint_info==NULL) return;
	    if(get_ioint_info(&cmd,precord,&piosl,&dummy1,&dummy2)) return;/*return if error*/
	    if(cmd==-1) {
		delete_from_scan_list(precord); /*old IO_EVENT_SCAN*/
	    } else {
		priority = precord->prio;
		if(priority<0 || priority>=NUM_CALLBACK_PRIORITIES) {
		    recGblRecordError(-1,precord,"scanDelete: get_ioint_info returned illegal priority");
		    return;
		}
		piosl += priority; /*get piosl for correct priority*/
		deleteFromList(precord,&piosl->scan_list);
	    }
	} else if(scan>=SCAN_1ST_PERIODIC) {
	    int	ind;

	    ind = scan - SCAN_1ST_PERIODIC;
	    psl = papPeriodic[ind];
	    deleteFromList(precord,psl);
	}
	return;
}

int scanppl()	/*print periodic list*/
{
    struct scan_list	*psl;
    char	message[80];
    double	period;
    int		i;

    for (i=0; i<nPeriodic; i++) {
	psl = papPeriodic[i];
	if(psl==NULL) continue;
	period = psl->ticks;
	period /= vxTicksPerSecond;
	sprintf(message,"Scan Period= %f seconds\n",period);
	printList(psl,message);
    }
    return(0);
}

int scanpel()  /*print event list */
{
    struct scan_list	*psl;
    char	message[80];
    int		i;

    for (i=0; i<MAX_EVENTS; i++) {
	psl = papEvent[i];
	if(psl==NULL) continue;
	sprintf(message,"Event %d\n",i);
	printList(psl,message);
    }
    return(0);
}

int scanpiol()  /* print io_event list */
{
    struct io_scan_list *piosl;
    int			priority;
    char		message[80];

    for(priority=0; priority<NUM_CALLBACK_PRIORITIES; priority++) {
	piosl=iosl_head[priority];
	if(piosl==NULL)continue;
	sprintf(message,"IO Event: Priority=%d",priority);
	while(piosl != NULL) {
	    printList(&piosl->scan_list,message);
	    piosl=piosl->next;
	}
    }
    return(0);
}

void scanIoInit(IOSCANPVT *ppioscanpvt)
{
    struct io_scan_list *piosl;
    int priority;

    /* allocate an array of io_scan_lists. One for each priority	*/
    /* IOSCANPVT will hold the address of this array of structures	*/
    *ppioscanpvt=calloc(NUM_CALLBACK_PRIORITIES,sizeof(struct io_scan_list));
    for(priority=0, piosl=*ppioscanpvt;
    priority<NUM_CALLBACK_PRIORITIES; priority++, piosl++){
	piosl->callback.callback = ioeventCallback;
	piosl->callback.priority = priority;
	lstInit(&piosl->scan_list.list);
	FASTLOCKINIT(&piosl->scan_list.lock);
	piosl->next=iosl_head[priority];
	iosl_head[priority]=piosl;
    }
    
}


void scanIoRequest(IOSCANPVT pioscanpvt)
{
    struct io_scan_list *piosl;
    int priority;

    if(!interruptAccept) return;
    for(priority=0, piosl=pioscanpvt;
    priority<NUM_CALLBACK_PRIORITIES; priority++, piosl++){
	if(lstCount(&piosl->scan_list.list)>0) callbackRequest((void *)piosl);
    }
}

static void periodicTask(struct scan_list *psl)
{

    unsigned long	start_time,end_time;
    long		delay;
    struct scan_element *pse,*prev,*next;

    start_time = tickGet();
    while(TRUE) {
	if(interruptAccept)scanList(psl);
	end_time = tickGet();
	delay = psl->ticks - (end_time - start_time);
	if(delay<=0) delay=1;
	taskDelay(delay);
	start_time = end_time + delay;
    }
}


static void initPeriodic()
{
	struct {
	    DBRenumStrs
	} scanChoices;
	struct scan_list *psl;
	struct dbAddr		dbAddr;		/* database address */
	struct recHeader	*precHeader;
	struct recLoc		*precLoc;
	RECNODE			*precNode;
	struct dbCommon		*precord=NULL;	/* pointer to record	*/
	long			status,nRequest,options;
	void			*pfl=NULL;
	int			i;
	char name[PVNAME_SZ+FLDNAME_SZ+2];
	float temp;

	if(!(precHeader = pdbBase->precHeader)) {
	   errMessage(S_record_noRecords, "initPeriodic");
	   exit(1);
	}
	/* look for first record */
	for (i=0; i<precHeader->number; i++) {
		if((precLoc=precHeader->papRecLoc[i])==NULL) continue;
		for(precNode=(RECNODE *)lstFirst(precLoc->preclist);
		precNode; precNode = (RECNODE *)lstNext(&precNode->next)) {
			precord = precNode->precord;
			if(precord->name[0]!=0) goto got_record;
		}
	}
	errMessage(S_record_noRecords,"initPeriodic");
	return;
got_record:
	/* get database address of SCAN field */
	name[PVNAME_SZ+1] = 0;
	strncpy(name,precord->name,PVNAME_SZ);
	strcat(name,".SCAN");
	if ((status=dbNameToAddr(name,&dbAddr)) != 0){
		recGblDbaddrError(status,&dbAddr,"initPeriodic");
		exit(1);
	}
	options = DBR_ENUM_STRS;
	nRequest = 0;
	status = dbGetField(&dbAddr,DBR_ENUM,&scanChoices,&options,&nRequest,pfl);
	if(status) {
		recGblDbaddrError(status,&dbAddr,"initPeriodic");
		exit(1);
	}
	nPeriodic = scanChoices.no_str - SCAN_1ST_PERIODIC;
	papPeriodic = calloc(nPeriodic,sizeof(struct scan_list *));
	if(papPeriodic==NULL) {
		errMessage(-1,"initPeriodic calloc failure");
		exit(1);
	}
	periodicTaskId = calloc(nPeriodic,sizeof(int));
	if(periodicTaskId==NULL) {
		errMessage(-1,"initPeriodic calloc failure");
		exit(1);
	}
	for(i=0; i<nPeriodic; i++) {
		psl = calloc(1,sizeof(struct scan_list));
		if(psl==NULL) {
			errMessage(-1,"initPeriodic calloc failure");
			exit(1);
		}
		papPeriodic[i] = psl;
		FASTLOCKINIT(&psl->lock);
		lstInit(&psl->list);
		sscanf(scanChoices.strs[i+SCAN_1ST_PERIODIC],"%f",&temp);
		psl->ticks = temp * vxTicksPerSecond;
	}
}

static void spawnPeriodic(int ind)
{
    struct scan_list *psl;

    psl = papPeriodic[ind];
    periodicTaskId[ind] = taskSpawn(PERIODSCAN_NAME,PERIODSCAN_PRI-ind,
				PERIODSCAN_OPT,PERIODSCAN_STACK,
				(FUNCPTR )periodicTask,psl);
    taskwdInsert(periodicTaskId[ind],wdPeriodic,(void *)(long)ind);
}

static void wdPeriodic(long ind)
{
    struct scan_list *psl;

    psl = papPeriodic[ind];
    taskwdRemove(periodicTaskId[ind]);
    if(!scanRestart)return;
    FASTUNLOCK(&psl->lock);
    spawnPeriodic(ind);
}

static void eventTask()
{
    unsigned char	event;
    struct scan_element *pse,*prev,*next;
    struct scan_list *psl;

    while(TRUE) {
        if(semTake(eventSem,WAIT_FOREVER)!=OK)
	    logMsg("semTake returned error in eventTask\n");
        while (rngNBytes(eventQ)>=sizeof(unsigned char)){
	    if(rngBufGet(eventQ,(void *)&event,sizeof(unsigned char))!=sizeof(unsigned char))
		logMsg("rngBufGet returned error in eventTask\n");
	    if(event<0 || event>MAX_EVENTS-1) {
		errMessage(-1,"eventTask received an illegal event");
		continue;
	    }
	    if(papEvent[event]==NULL) continue;
	    psl = papEvent[event];
	    if(psl) scanList(psl);
	}
    }
}

static void initEvent()
{
	int i;

	for(i=0; i<MAX_EVENTS; i++) papEvent[i] = 0;
	eventQ = rngCreate(sizeof(unsigned char) * EVENT_QUEUE_SIZE);
	if(eventQ==NULL) {
		errMessage(0,"initEvent failed");
		exit(1);
	}
	if((eventSem=semBCreate(SEM_Q_FIFO,SEM_EMPTY))==NULL)
		logMsg("semBcreate failed in initEvent\n");
}

static void spawnEvent()
{

    eventTaskId = taskSpawn(EVENTSCAN_NAME,EVENTSCAN_PRI,EVENTSCAN_OPT,
			EVENTSCAN_STACK,(FUNCPTR)eventTask);
    taskwdInsert(eventTaskId,wdEvent,0L);
}

static void wdEvent()
{
    int i;
    struct scan_list *psl;

    taskwdRemove(eventTaskId);
    if(!scanRestart) return;
    if(semFlush(eventSem)!=OK)
	logMsg("semFlush failed while restarting eventTask\n");
    rngFlush(eventQ);
    for (i=0; i<MAX_EVENTS; i++) {
	psl = papEvent[i];
	if(psl==NULL) continue;
	FASTUNLOCK(&psl->lock);
    }
    spawnEvent();
}

static void ioeventCallback(struct io_scan_list *piosl)
{
    struct scan_list *psl=&piosl->scan_list;

    scanList(psl);
}


static void printList(struct scan_list *psl,char *message)
{
    struct scan_element *pse;

    FASTLOCK(&psl->lock);
    (void *)pse = lstFirst(&psl->list);
    FASTUNLOCK(&psl->lock);
    if(pse==NULL) return;
    printf("%s\n",message);
    while(pse!=NULL) {
	printf("    %-28s\n",pse->precord->name);
	FASTLOCK(&psl->lock);
	if(pse->pscan_list != psl) {
	    FASTUNLOCK(&psl->lock);
	    printf("Returning because list changed while processing.");
	    return;
	}
	(void *)pse = lstNext((void *)pse);
	FASTUNLOCK(&psl->lock);
    }
}

static void scanList(struct scan_list *psl)
{
    /*In reading this code remember that the call to dbProcess can result*/
    /*in the SCAN field being changed in an arbitrary number of records  */

    struct scan_element *pse,*prev,*next;

    FASTLOCK(&psl->lock);
	psl->modified = FALSE;
	(void *)pse = lstFirst(&psl->list);
	prev = NULL;
	(void *)next = lstNext((void *)pse);
    FASTUNLOCK(&psl->lock);
    while(pse!=NULL) {
	struct dbCommon *precord = pse->precord;

	dbScanLock(precord);
	dbProcess(precord);
	dbScanUnlock(precord);
	FASTLOCK(&psl->lock);
	    if(!psl->modified) {
		prev = pse;
		(void *)pse = lstNext((void *)pse);
		if(pse!=NULL) (void *)next = lstNext((void *)pse);
	    } else if (pse->pscan_list==psl) {
		/*This scan element is still in same scan list*/
		prev = pse;
		(void *)pse = lstNext((void *)pse);
		if(pse!=NULL) (void *)next = lstNext((void *)pse);
		psl->modified = FALSE;
	    } else if (prev!=NULL && prev->pscan_list==psl) {
		/*Previous scan element is still in same scan list*/
		(void *)pse = lstNext((void *)prev);
		if(pse!=NULL) {
		    (void *)prev = lstPrevious((void *)pse);
		    (void *)next = lstNext((void *)pse);
		}
		psl->modified = FALSE;
	    } else if (next!=NULL && next->pscan_list==psl) {
		/*Next scan element is still in same scan list*/
		pse = next;
		(void *)prev = lstPrevious((void *)pse);
		(void *)next = lstNext((void *)pse);
		psl->modified = FALSE;
	    } else {
		/*Too many changes. Just wait till next period*/
		FASTUNLOCK(&psl->lock);
		return;
	    }
	FASTUNLOCK(&psl->lock);
    }
}

static void buildScanLists()
{
	struct recHeader	*precHeader;
	struct recLoc		*precLoc;
	RECNODE			*precNode;
	struct dbCommon		*precord;	/* pointer to record	*/
	int			i;

	if(!(precHeader = pdbBase->precHeader)) {
		errMessage(S_record_noRecords,
			"Error detected in build_scan_lists");
		exit(1);
	}
	/* look through all of the database records and place them on lists */
	for (i=0; i<precHeader->number; i++) {
		if((precLoc=precHeader->papRecLoc[i])==NULL) continue;
		for(precNode=(RECNODE *)lstFirst(precLoc->preclist);
		precNode; precNode = (RECNODE *)lstNext(&precNode->next)) {
			precord = precNode->precord;
			if(precord->name[0]==0) continue;
			scanAdd(precord);
		}
	}
}

static void addToList(struct dbCommon *precord,struct scan_list *psl)
{
	struct scan_element	*pse,*ptemp;

	FASTLOCK(&psl->lock);
	pse = (struct scan_element *)(precord->spvt);
	if(pse==NULL) {
		pse = calloc(1,sizeof(struct scan_element));
		if(pse==NULL) {
		    recGblRecordError(-1,precord,"addToList calloc error");
		    exit(1);
		}
		precord->spvt = (void *)pse;
		(void *)pse->precord = precord;
	}
	pse ->pscan_list = psl;
	(void *)ptemp = lstFirst(&psl->list);
	while(ptemp!=NULL) {
		if(ptemp->precord->phas>precord->phas) {
			lstInsert(&psl->list,
				lstPrevious((void *)ptemp),(void *)pse);
			break;
		}
		(void *)ptemp = lstNext((void *)ptemp);
	}
	if(ptemp==NULL) lstAdd(&psl->list,(void *)pse);
	psl->modified = TRUE;
	FASTUNLOCK(&psl->lock);
	return;
}

static void deleteFromList(struct dbCommon *precord,struct scan_list *psl)
{
	struct scan_element	*pse;

	FASTLOCK(&psl->lock);
	if(precord->spvt==NULL) {
		FASTUNLOCK(&psl->lock);
		return;
	}
	pse = (struct scan_element *)(precord->spvt);
	if(pse==NULL || pse->pscan_list!=psl) {
	    FASTUNLOCK(&psl->lock);
	    errMessage(-1,"deleteFromList failed");
	    return;
	}
	pse->pscan_list = NULL;
	lstDelete(&psl->list,(void *)pse);
	psl->modified = TRUE;
	FASTUNLOCK(&psl->lock);
	return;
}
