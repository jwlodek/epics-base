/**************************************************************************
			GTA PROJECT   AT division
	Copyright, 1990-1994, The Regents of the University of California
	and the University of Chicago.
	Los Alamos National Laboratory

 	seq_main.c,v 1.2 1995/06/27 15:25:58 wright Exp

	DESCRIPTION: Seq() initiates a sequence as a group of cooperating
	tasks.  An optional string parameter specifies the values for
	macros.  The channel access context and task are shared by all state
	programs.

	ENVIRONMENT: VxWorks

	HISTORY:
23apr91,ajk	Fixed problem with state program invoking the sequencer.
01jul91,ajk	Added ANSI functional prototypes.
05jul91,ajk	Changed semCreate() in three places to semBCreate.
		Modified semTake() second param. to WAIT_FOREVER.
		These provide VX5.0 compatability.  
16aug91,ajk	Improved "magic number" error message.
25oct91,ajk	Code to create semaphores "pSS->getSemId" was left out.
		Added this code to init_sscb().
25nov91,ajk	Removed obsolete seqLog() code dealing with global locking.
04dec91,ajk	Implemented state program linked list, eliminating need for
		task variables.
11dec91,ajk	Cleaned up comments.
05feb92,ajk	Decreased minimum allowable stack size to SPAWN_STACK_SIZE/2.
24feb92,ajk	Print error code for log file failure.
28apr92,ajk	Implemented new event flag mode.
29apr92,ajk	Now alocates private program structures, even when reentry option
		is not specified.  This avoids problems with seqAddTask().
29apr92,ajk	Implemented mutual exclusion lock in seq_log().
16feb93,ajk	Converted to single channel access task for all state programs.
16feb93,ajk	Removed VxWorks pre-v5 stuff.
17feb93,ajk	Evaluation of channel names moved here from seq_ca.c.
19feb93,ajk	Fixed time stamp format for seq_log().
16jun93,ajk	Fixed taskSpawn() to have 15 args per vx5.1.
20jul93,ajk	Replaced obsolete delete() with remove() per vx5.1 release notes.
20jul93,ajk	Removed #define ANSI
15mar94,ajk	Implemented i/f to snc through structures in seqCom.h.
15mar94,ajk	Allowed assignment of array elements to db.
15mar94,ajk	Rearranged code that builds program structures.
02may94,ajk	Performed initialization when sequencer is evoked, even w/o
		parameters.
19jul95,ajk	Added unsigned types (unsigned char, short, int, long).
20jul95,ajk	Added priority specification at run time. 
03aug95,ajk	Fixed problem with +r option: user variable space (pSP->pVar)
		was not being allocated.
03jun96,ajk	Now compiles with -wall and -pedantic switches.
***************************************************************************/
/*#define	DEBUG	1*/

#include 	<string.h>

#include	"seqCom.h"
#include	"seq.h"
#include	"taskLib.h"
#include	"taskHookLib.h"
#include	"logLib.h"
#include	"errnoLib.h"
#include	"usrLib.h"

#ifdef		DEBUG
#undef		LOCAL
#define		LOCAL
#endif		/*DEBUG*/

/* ANSI functional prototypes for local routines */
LOCAL	SPROG *seqInitTables(struct seqProgram *);
LOCAL	VOID init_sprog(struct seqProgram *, SPROG *);
LOCAL	VOID init_sscb(struct seqProgram *, SPROG *);
LOCAL	VOID init_chan(struct seqProgram *, SPROG *);
LOCAL	VOID init_mac(SPROG *);

LOCAL	VOID seq_logInit(SPROG *);
LOCAL	VOID seqChanNameEval(SPROG *);
LOCAL	VOID selectDBtype(char *, short *, short *, short *, short *);

#define	SCRATCH_SIZE	(MAX_MACROS*(MAX_STRING_SIZE+1)*12)
/*	Globals */
/*	Flag to indicate that "taskDeleteHookAdd()" was called */
int	seqDeleteHookAdded = FALSE;

/*	Auxillary sequencer task id; used to share CA context. */
int	seqAuxTaskId = 0;

/*
 * seq: User-callable routine to initiate a state program.
 * Usage:  seq(<pSP>, <macros string>, <stack size>)
 *	pSP is the ptr to the state program structure.
 *	Example:  seq(&myprog, "logfile=mylog", 0)
 * When called from the shell, the 2nd & 3rd parameters are optional.
 *
 * Creates the initial state program task and returns its task id.
 * Most initialization is performed here.
 */
long seq (pSeqProg, macro_def, stack_size)
struct seqProgram	*pSeqProg;	/* state program info generated by snc */
char			*macro_def;	/* optional macro def'n string */
long			stack_size;	/* optional stack size (bytes) */
{
	int		tid;
	extern		sprog_delete();	/* Task delete routine */
	extern char	*seqVersion;
	SPROG		*pSP;
	char		*pValue, *ptask_name;
	extern		seqAuxTask();

	/* Print version & date of sequencer */
	printf("%s\n", seqVersion);

	/* Spawn the sequencer auxillary task */
	if (seqAuxTaskId == 0)
	{
		taskSpawn("seqAux", SPAWN_PRIORITY-1, VX_FP_TASK, 4000, seqAuxTask,
		 0,0,0,0,0,0,0,0,0,0);
		while (seqAuxTaskId == 0)
			taskDelay(5); /* wait for task to init. ch'l access */
#ifdef	DEBUG
	logMsg("task seqAux spawned, tid=0x%x\n", seqAuxTaskId, 0,0,0,0,0);
#endif	/*DEBUG*/
	}

	/* Specify a routine to run at task delete */
	if (!seqDeleteHookAdded)
	{
		taskDeleteHookAdd(sprog_delete);
		seqDeleteHookAdded = TRUE;
	}

	/* Exit if no parameters specified */
	if (pSeqProg == 0)
	{
		return 0;
	}

	/* Check for correct state program format */
	if (pSeqProg->magic != MAGIC)
	{	/* Oops */
		logMsg("Illegal magic number in state program.\n", 0,0,0,0,0,0);
		logMsg(" - Possible mismatch between SNC & SEQ versions\n", 0,0,0,0,0,0);
		logMsg(" - Re-compile your program?\n", 0,0,0,0,0,0);
		return -1;
	}

	/* Initialize the sequencer tables */
	pSP = seqInitTables(pSeqProg);

	/* Parse the macro definitions from the "program" statement */
	seqMacParse(pSeqProg->pParams, pSP);

	/* Parse the macro definitions from the command line */
	seqMacParse(macro_def, pSP);

	/* Do macro substitution on channel names */
	seqChanNameEval(pSP);

	/* Initialize sequencer logging */
	seq_logInit(pSP);

	/* Specify stack size */
	if (stack_size == 0)
		stack_size = SPAWN_STACK_SIZE;
	pValue = seqMacValGet(pSP->pMacros, "stack");
	if (pValue != NULL && strlen(pValue) > 0)
	{
		sscanf(pValue, "%ld", &stack_size);
	}
	if (stack_size < SPAWN_STACK_SIZE/2)
		stack_size = SPAWN_STACK_SIZE/2;

	/* Specify task name */
	pValue = seqMacValGet(pSP->pMacros, "name");
	if (pValue != NULL && strlen(pValue) > 0)
		ptask_name = pValue;
	else
		ptask_name = pSP->pProgName;

	/* Spawn the initial sequencer task */
#ifdef	DEBUG
	logMsg("Spawning task %s, stack_size=%d\n", ptask_name, stack_size, 0,0,0,0);
#endif	/*DEBUG*/
	/* Specify task priority */
	pSP->taskPriority = SPAWN_PRIORITY;
	pValue = seqMacValGet(pSP->pMacros, "priority");
	if (pValue != NULL && strlen(pValue) > 0)
	{
		sscanf(pValue, "%ld", &(pSP->taskPriority));
	}
	if (pSP->taskPriority < SPAWN_PRIORITY)
		pSP->taskPriority = SPAWN_PRIORITY;
	if (pSP->taskPriority > 255)
		pSP->taskPriority = 255;

	tid = taskSpawn(ptask_name, pSP->taskPriority, SPAWN_OPTIONS,
	 stack_size, (FUNCPTR)sequencer, (int)pSP, stack_size, (int)ptask_name,
	 0,0,0,0,0,0,0);

	seq_log(pSP, "Spawning state program \"%s\", task name = \"%s\"\n",
	 pSP->pProgName, ptask_name);
	seq_log(pSP, "  Task id = %d = 0x%x\n", tid, tid);

	/* Return task id to calling program */
	return tid;
}
/* seqInitTables - initialize sequencer tables */
LOCAL SPROG *seqInitTables(pSeqProg)
struct seqProgram	*pSeqProg;
{
	SPROG		*pSP;

	pSP = (SPROG *)calloc(1, sizeof (SPROG));

	/* Initialize state program block */
	init_sprog(pSeqProg, pSP);

	/* Initialize state set control blocks */
	init_sscb(pSeqProg, pSP);

	/* Initialize database channel blocks */
	init_chan(pSeqProg, pSP);

	/* Initialize the macro table */
	init_mac(pSP);


	return pSP;
}
/*
 * Copy data from seqCom.h structures into this task's dynamic structures as defined
 * in seq.h.
 */
LOCAL VOID init_sprog(pSeqProg, pSP)
struct seqProgram	*pSeqProg;
SPROG			*pSP;
{
	int		i, nWords;

	/* Copy information for state program */
	pSP->numSS = pSeqProg->numSS;
	pSP->numChans = pSeqProg->numChans;
	pSP->numEvents = pSeqProg->numEvents;
	pSP->options = pSeqProg->options;
	pSP->pProgName = pSeqProg->pProgName;
	pSP->exitFunc = (EXIT_FUNC)pSeqProg->exitFunc;
	pSP->varSize = pSeqProg->varSize;
	/* Allocate user variable area if reentrant option (+r) is set */
	if ((pSP->options & OPT_REENT) != 0)
		pSP->pVar = (char *)calloc(pSP->varSize, 1);

#ifdef	DEBUG
	logMsg("init_sprog: num SS=%d, num Chans=%d, num Events=%d, Prog Name=%s, var Size=%d\n",
	 pSP->numSS, pSP->numChans, pSP->numEvents, pSP->pProgName, pSP->varSize);
#endif	/*DEBUG*/

	/* Create a semaphore for resource locking on CA events */
	pSP->caSemId = semBCreate(SEM_Q_FIFO, SEM_FULL);
	if (pSP->caSemId == NULL)
	{
		logMsg("can't create caSemId\n", 0,0,0,0,0,0);
		return;
	}

	pSP->task_is_deleted = FALSE;
	pSP->connCount = 0;
	pSP->assignCount = 0;
	pSP->logFd = 0;

	/* Allocate an array for event flag bits */
	nWords = (pSP->numEvents + NBITS - 1) / NBITS;
	if (nWords == 0)
		nWords = 1;
	pSP->pEvents = (bitMask *)calloc(nWords,  sizeof(bitMask));
	for (i = 0; i < nWords; i++)
		pSP->pEvents[i] = 0;

	return;
}
/*
 * Initialize the state set control blocks
 */
LOCAL VOID init_sscb(pSeqProg, pSP)
struct seqProgram	*pSeqProg;
SPROG			*pSP;
{
	SSCB		*pSS;
	STATE		*pState;
	int		nss, nstates;
	struct seqSS	*pSeqSS;
	struct seqState	*pSeqState;


	/* Allocate space for the SSCB structures */
	pSP->pSS = pSS = (SSCB *)calloc(pSeqProg->numSS, sizeof(SSCB));

	/* Copy information for each state set and state */
	pSeqSS = pSeqProg->pSS;
	for (nss = 0; nss < pSeqProg->numSS; nss++, pSS++, pSeqSS++)
	{
		/* Fill in SSCB */
		pSS->pSSName = pSeqSS->pSSName;
		pSS->numStates = pSeqSS->numStates;
		pSS->errorState = pSeqSS->errorState;
		pSS->currentState = 0; /* initial state */
		pSS->nextState = 0;
		pSS->prevState = 0;
		pSS->taskId = 0;
		pSS->sprog = pSP;
#ifdef	DEBUG
		logMsg("init_sscb: SS Name=%s, num States=%d, pSS=0x%x\n",
		 pSS->pSSName, pSS->numStates, pSS, 0,0,0);
#endif	/*DEBUG*/
		/* Create a binary semaphore for synchronizing events in a SS */
		pSS->syncSemId = semBCreate(SEM_Q_FIFO, SEM_FULL);
		if (pSS->syncSemId == NULL)
		{
			logMsg("can't create syncSemId\n", 0,0,0,0,0,0);
			return;
		}

		/* Create a binary semaphore for synchronous pvGet() (-a) */
		if ((pSP->options & OPT_ASYNC) == 0)
		{
			pSS->getSemId =
			 semBCreate(SEM_Q_FIFO, SEM_FULL);
			if (pSS->getSemId == NULL)
			{
				logMsg("can't create getSemId\n", 0,0,0,0,0,0);
				return;
			}

		}

		/* Allocate & fill in state blocks */
		pSS->pStates = pState = (STATE *)calloc(pSS->numStates, sizeof(STATE));

		pSeqState = pSeqSS->pStates;
		for (nstates = 0; nstates < pSeqSS->numStates;
		 nstates++, pState++, pSeqState++)
		{
			pState->pStateName = pSeqState->pStateName;
			pState->actionFunc = (ACTION_FUNC)pSeqState->actionFunc;
			pState->eventFunc = (EVENT_FUNC)pSeqState->eventFunc;
			pState->delayFunc = (DELAY_FUNC)pSeqState->delayFunc;
			pState->pEventMask = pSeqState->pEventMask;
#ifdef	DEBUG
		logMsg("init_sscb: State Name=%s, Event Mask=0x%x\n",
		 pState->pStateName, *pState->pEventMask, 0,0,0,0);
#endif	/*DEBUG*/
		}
	}

#ifdef	DEBUG
	logMsg("init_sscb: numSS=%d\n", pSP->numSS, 0,0,0,0,0);
#endif	/*DEBUG*/
	return;
}

/*
 * init_chan--Build the database channel structures.
 * Note:  Actual PV name is not filled in here. */
LOCAL VOID init_chan(pSeqProg, pSP)
struct seqProgram	*pSeqProg;
SPROG			*pSP;
{
	int		nchan;
	CHAN		*pDB;
	struct seqChan	*pSeqChan;

	/* Allocate space for the CHAN structures */
	pSP->pChan = (CHAN *)calloc(pSP->numChans, sizeof(CHAN));
	pDB = pSP->pChan;

	pSeqChan = pSeqProg->pChan;
	for (nchan = 0; nchan < pSP->numChans; nchan++, pDB++, pSeqChan++)
	{
#ifdef	DEBUG
		logMsg("init_chan: pDB=0x%x\n", pDB, 0,0,0,0,0);
#endif	/*DEBUG*/
		pDB->sprog = pSP;
		pDB->dbAsName = pSeqChan->dbAsName;
		pDB->pVarName = pSeqChan->pVarName;
		pDB->pVarType = pSeqChan->pVarType;
		pDB->pVar = pSeqChan->pVar; /* this is an offset for +r option */
		pDB->count = pSeqChan->count;
		pDB->efId = pSeqChan->efId;
		pDB->monFlag = pSeqChan->monFlag;
		pDB->eventNum = pSeqChan->eventNum;
		pDB->assigned = 0;

		/* Fill in get/put database types, element size, & access offset */
		selectDBtype(pSeqChan->pVarType, &pDB->getType,
		 &pDB->putType, &pDB->size, &pDB->dbOffset);

		/* Reentrant option: Convert offset to address of the user variable. */
		if ((pSP->options & OPT_REENT) != 0)
			pDB->pVar += (int)pSP->pVar;
#ifdef	DEBUG
		logMsg(" Assigned Name=%s, VarName=%s, VarType=%s, count=%d\n",
		 pDB->dbAsName, pDB->pVarName, pDB->pVarType, pDB->count, 0,0);
		logMsg("   size=%d, dbOffset=%d\n", pDB->size, pDB->dbOffset, 0,0,0,0);
		logMsg("   efId=%d, monFlag=%d, eventNum=%d\n",
		 pDB->efId, pDB->monFlag, pDB->eventNum, 0,0,0);
#endif	/*DEBUG*/
	}
}

/* 
 * init_mac - initialize the macro table.
 */
LOCAL VOID init_mac(pSP)
SPROG		*pSP;
{
	int		i;
	MACRO		*pMac;

	pSP->pMacros = pMac = (MACRO *)calloc(MAX_MACROS, sizeof (MACRO));
#ifdef	DEBUG
	logMsg("init_mac: pMac=0x%x\n", pMac, 0,0,0,0,0);
#endif	/*DEBUG*/

	for (i = 0 ; i < MAX_MACROS; i++, pMac++)
	{
		pMac->pName = NULL;
		pMac->pValue = NULL;
	}
}	
/*
 * Evaluate channel names by macro substitution.
 */
#define		MACRO_STR_LEN	(MAX_STRING_SIZE+1)
LOCAL VOID seqChanNameEval(pSP)
SPROG		*pSP;
{
	CHAN		*pDB;
	int		i;

	pDB = pSP->pChan;
	for (i = 0; i < pSP->numChans; i++, pDB++)
	{
		pDB->dbName = calloc(1, MACRO_STR_LEN);
		seqMacEval(pDB->dbAsName, pDB->dbName, MACRO_STR_LEN, pSP->pMacros);
#ifdef	DEBUG
		logMsg("seqChanNameEval: \"%s\" evaluated to \"%s\"\n",
		  pDB->dbAsName, pDB->dbName, 0,0,0,0);
#endif	/*DEBUG*/
	}
}
/*
 * selectDBtype -- returns types for DB put/getm element size, and db access
 * offset based on user variable type.
 * Mapping is determined by the following typeMap[] array.
 * DBR_TIME_* types for gets/monitors returns status and time stamp.
 * Note that type "int" is mapped into DBR_LONG, because DBR_INT is actually
 * 16 bits as defined in the database.  This could cause future problems
 * if the cpu architecture or compiler doesn't make this same assumption!
 */
LOCAL	struct typeMap {
	char	*pTypeStr;
	short	putType;
	short	getType;
	short	size;
	short	offset;
} typeMap[] = {
	{
	"char",   DBR_CHAR,   DBR_TIME_CHAR,
	sizeof (char),   OFFSET(struct dbr_time_char,   value)
	},

	{
	"short",  DBR_SHORT,  DBR_TIME_SHORT,
	sizeof (short),  OFFSET(struct dbr_time_short,  value)
	},

	{
	"int",    DBR_LONG,   DBR_TIME_LONG,
	sizeof (long),   OFFSET(struct dbr_time_long,   value)
	},

	{
	"long",   DBR_LONG,   DBR_TIME_LONG,
	sizeof (long),   OFFSET(struct dbr_time_long,   value)
	},

	{
	"unsigned char",   DBR_CHAR,   DBR_TIME_CHAR,
	sizeof (char),   OFFSET(struct dbr_time_char,   value)
	},

	{
	"unsigned short",  DBR_SHORT,  DBR_TIME_SHORT,
	sizeof (short),  OFFSET(struct dbr_time_short,  value)
	},

	{
	"unsigned int",    DBR_LONG,   DBR_TIME_LONG,
	sizeof (long),   OFFSET(struct dbr_time_long,   value)
	},

	{
	"unsigned long",   DBR_LONG,   DBR_TIME_LONG,
	sizeof (long),   OFFSET(struct dbr_time_long,   value)
	},

	{
	"float",  DBR_FLOAT,  DBR_TIME_FLOAT,
	sizeof (float),  OFFSET(struct dbr_time_float,  value)
	},

	{
	"double", DBR_DOUBLE, DBR_TIME_DOUBLE,
	sizeof (double), OFFSET(struct dbr_time_double, value)
	},

	{
	"string", DBR_STRING, DBR_TIME_STRING,
	MAX_STRING_SIZE, OFFSET(struct dbr_time_string, value[0])
	},

	{
	0, 0, 0, 0, 0
	}
};

LOCAL VOID selectDBtype(pUserType, pGetType, pPutType, pSize, pOffset)
char		*pUserType;
short		*pGetType, *pPutType, *pSize, *pOffset;
{
	struct typeMap	*pMap;

	for (pMap = &typeMap[0]; *pMap->pTypeStr != 0; pMap++)
	{
		if (strcmp(pUserType, pMap->pTypeStr) == 0)
		{
			*pGetType = pMap->getType;
			*pPutType = pMap->putType;
			*pSize = pMap->size;
			*pOffset = pMap->offset;
			return;
		}
	}
	*pGetType = *pPutType = *pSize = *pOffset = 0; /* this shouldn't happen */

	return;
}
/*
 * seq_logInit() - Initialize logging.
 * If "logfile" is not specified, then we log to standard output.
 */
LOCAL VOID seq_logInit(pSP)
SPROG		*pSP;
{
	char		*pValue;
	int		fd;

	/* Create a logging resource locking semaphore */
	pSP->logSemId = semBCreate(SEM_Q_FIFO, SEM_FULL);
	if (pSP->logSemId == NULL)
	{
		logMsg("can't create logSemId\n", 0,0,0,0,0,0);
		return;
	}
	pSP->logFd = ioGlobalStdGet(1); /* default fd is std out */

	/* Check for logfile spec. */
	pValue = seqMacValGet(pSP->pMacros, "logfile");
	if (pValue != NULL && strlen(pValue) > 0)
	{	/* Create & open a new log file for write only */
		remove(pValue); /* remove the file if it exists */
		fd = open(pValue, O_CREAT | O_WRONLY, 0664);
		if (fd != ERROR)
			pSP->logFd = fd;
		printf("logfile=%s, fd=%d\n", pValue, fd);
	}
}
/*
 * seq_log
 * Log a message to the console or a file with task name, date, & time of day.
 * The format looks like "mytask 12/13/93 10:07:43: Hello world!".
 */
#include	"tsDefs.h"
#define	LOG_BFR_SIZE	200

STATUS seq_log(pSP, fmt, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8)
SPROG		*pSP;
char		*fmt;		/* format string */
int		arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8; /* arguments */
{
	int		fd, count, status;
	TS_STAMP	timeStamp;
	char		logBfr[LOG_BFR_SIZE], *pBfr;

	pBfr = logBfr;

	/* Enter taskname */
	sprintf(pBfr, "%s ", taskName(taskIdSelf()) );
	pBfr += strlen(pBfr);

	/* Get time of day */
	tsLocalTime(&timeStamp);	/* time stamp format */

	/* Convert to text format: "mm/dd/yy hh:mm:ss.nano-sec" */
	tsStampToText(&timeStamp, TS_TEXT_MMDDYY, pBfr);
	/* Truncate the ".nano-sec" part */
	if (pBfr[2] == '/') /* valid t-s? */
		pBfr += 17;

	/* Insert ": " */
	*pBfr++ = ':';
	*pBfr++ = ' ';

	/* Append the user's msg to the buffer */
	sprintf(pBfr, fmt, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8);
	pBfr += strlen(pBfr);

	/* Write the msg */
	semTake(pSP->logSemId, WAIT_FOREVER); /* lock it */
	fd = pSP->logFd;
	count = pBfr - logBfr + 1;
	status = write(fd, logBfr, count);

	semGive(pSP->logSemId);
	if (status != count)
	{
		logMsg("Log file error, fd=%d, error no.= %d = \"%s\"\n", 
			fd, errnoGet(), (int) strerror(errnoGet()),0,0,0);
		return ERROR;
	}

	/* If this is an NSF file flush the buffer */
	if (fd != ioGlobalStdGet(1) )
	{
		ioctl(fd, FIOSYNC, 0);
	}
	return OK;
}
/*
 * seq_seqLog() - State program interface to seq_log().
 * Does not require ptr to state program block.
 */
long seq_seqLog(ssId, fmt, arg1,arg2, arg3, arg4, arg5, arg6, arg7, arg8)
SS_ID		ssId;
char		*fmt;		/* format string */
int		arg1,arg2, arg3, arg4, arg5, arg6, arg7, arg8; /* arguments */
{
	SPROG		*pSP;

	pSP = ((SSCB *)ssId)->sprog;
	return seq_log(pSP, fmt, arg1,arg2, arg3, arg4, arg5, arg6, arg7, arg8);
}
