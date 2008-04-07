/*****************************************************************************
 * $Source$
 * $Author$
 * $Date$
 * $Revision$
 *****************************************************************************/

/** @defgroup Converse
 * \brief Converse--a parallel portability layer.

 * Converse is the lowest level inside the Charm++ hierarchy. It stands on top
 * of the machine layer, and it provides all the common functionality across
 * platforms.

 * One converse program is running on every processor (or node in the smp
 * version). it manages the message transmission, and the memory allocation.
 * Charm++, which is on top of Converse, uses its functionality for
 * interprocess *communication.

 * In order to maintain multiple independent objects inside a single user space
 * program, it uses a personalized version of threads, which can be executed,
 * suspended, and migrated across processors.

 * It provides a scheduler for message delivery: methods can be registered to
 * the scheduler, and then messages allocated through CmiAlloc can be sent to
 * the correspondent method in a remote processor. This is done through the
 * converse header (which has few common fields, but is architecture dependent).
*/

/** @file
 * converse main core
 * @ingroup Converse
 */

/**
 * @addtogroup Converse
 * @{
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include "converse.h"
#include "conv-trace.h"
#include "sockRoutines.h"
#include "queueing.h"
#include "conv-ccs.h"
#include "ccs-server.h"
#include "memory-isomalloc.h"
#include "converseEvents.h"             /* projector */
#include "traceCoreCommon.h"    /* projector */
#include "machineEvents.h"     /* projector */

#if CMK_OUT_OF_CORE
#include "conv-ooc.h"
#endif

#if CONVERSE_POOL
#include "cmipool.h"
#endif

#if CMK_CONDS_USE_SPECIAL_CODE
CmiSwitchToPEFnPtr CmiSwitchToPE;
#endif

CpvExtern(int, _traceCoreOn);   /* projector */
extern void CcdModuleInit(char **);
extern void CmiMemoryInit(char **);
extern void CldModuleInit(char **);
extern void CmiInitCPUAffinity(char **);

#if CMK_WHEN_PROCESSOR_IDLE_USLEEP
#include <sys/types.h>
#include <sys/time.h>
#endif

#if CMK_TIMER_USE_TIMES
#include <sys/times.h>
#include <limits.h>
#include <unistd.h>
#endif

#if CMK_TIMER_USE_GETRUSAGE
#include <sys/time.h>
#include <sys/resource.h>
#endif

#if CMK_TIMER_USE_RDTSC
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/resource.h>
#endif

#ifdef CMK_TIMER_USE_WIN32API
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/timeb.h>
#endif

#include "quiescence.h"

int cur_restart_phase = 1;      /* checkpointing/restarting phase counter */

static int CsdLocalMax = CSD_LOCAL_MAX_DEFAULT;

/*****************************************************************************
 *
 * Unix Stub Functions
 *
 ****************************************************************************/

#ifdef MEMMONITOR
typedef unsigned long mmulong;
CpvDeclare(mmulong,MemoryUsage);
CpvDeclare(mmulong,HiWaterMark);
CpvDeclare(mmulong,ReportedHiWaterMark);
CpvDeclare(int,AllocCount);
CpvDeclare(int,BlocksAllocated);
#endif

#define MAX_HANDLERS 512

#if ! CMK_CMIPRINTF_IS_A_BUILTIN
CpvDeclare(int,expIOFlushFlag);
#if CMI_IO_BUFFER_EXPLICIT
/* 250k not too large depending on how slow terminal IO is */
#define DEFAULT_IO_BUFFER_SIZE 250000
CpvDeclare(char*,explicitIOBuffer);
CpvDeclare(int,expIOBufferSize);
#endif
#endif

#if CMK_NODE_QUEUE_AVAILABLE
void  *CmiGetNonLocalNodeQ();
#endif

CpvDeclare(void*, CsdSchedQueue);

#if CMK_OUT_OF_CORE
/* The Queue where the Prefetch Thread puts the messages from CsdSchedQueue  */
CpvDeclare(void*, CsdPrefetchQueue);
pthread_mutex_t prefetchLock;
#endif

#if CMK_NODE_QUEUE_AVAILABLE
CsvDeclare(void*, CsdNodeQueue);
CsvDeclare(CmiNodeLock, CsdNodeQueueLock);
#endif
CpvDeclare(int,   CsdStopFlag);
CpvDeclare(int,   CsdLocalCounter);

CmiNodeLock smp_mutex;               /* for smp */

#if CONVERSE_VERSION_VMI
void *CMI_VMI_CmiAlloc (int size);
void CMI_VMI_CmiFree (void *ptr);
#endif

#if CONVERSE_VERSION_ELAN
void* elan_CmiAlloc(int size);
#endif

#if CMK_USE_IBVERBS
void *infi_CmiAlloc(int size);
void infi_CmiFree(void *ptr);
#endif


#if CMK_GRID_QUEUE_AVAILABLE
CpvDeclare(void *, CkGridObject);
CpvDeclare(void *, CsdGridQueue);
#endif


/*****************************************************************************
 *
 * Command-Line Argument (CLA) parsing routines.
 *
 *****************************************************************************/

static int usageChecked=0; /* set when argv has been searched for a usage request */
static int printUsage=0; /* if set, print command-line usage information */
static const char *CLAformatString="%20s %10s %s\n";

/* This little list of CLA's holds the argument descriptions until it's
   safe to print them--it's needed because the net- versions don't have 
   printf until they're pretty well started.
 */
typedef struct {
	const char *arg; /* Flag name, like "-foo"*/
	const char *param; /* Argument's parameter type, like "integer" or "none"*/
	const char *desc; /* Human-readable description of what it does */
} CLA;
static int CLAlistLen=0;
static int CLAlistMax=0;
static CLA *CLAlist=NULL;

/* Add this CLA */
static void CmiAddCLA(const char *arg,const char *param,const char *desc) {
	int i;
	if (CmiMyPe()!=0) return; /*Don't bother if we're not PE 0*/
	if (desc==NULL) return; /*It's an internal argument*/
	if (usageChecked) { /* Printf should work now */
		if (printUsage)
			CmiPrintf(CLAformatString,arg,param,desc);
	}
	else { /* Printf doesn't work yet-- just add to the list.
		This assumes the const char *'s are static references,
		which is probably reasonable. */
		i=CLAlistLen++;
		if (CLAlistLen>CLAlistMax) { /*Grow the CLA list */
			CLAlistMax=16+2*CLAlistLen;
			CLAlist=realloc(CLAlist,sizeof(CLA)*CLAlistMax);\
		}
		CLAlist[i].arg=arg;
		CLAlist[i].param=param;
		CLAlist[i].desc=desc;
	}
}

/* Print out the stored list of CLA's */
static void CmiPrintCLAs(void) {
	int i;
	if (CmiMyPe()!=0) return; /*Don't bother if we're not PE 0*/
	CmiPrintf("Converse Machine Command-line Parameters:\n ");
	CmiPrintf(CLAformatString,"Option:","Parameter:","Description:");
	for (i=0;i<CLAlistLen;i++) {
		CLA *c=&CLAlist[i];
		CmiPrintf(CLAformatString,c->arg,c->param,c->desc);
	}
}

/**
 * Determines if command-line usage information should be printed--
 * that is, if a "-?", "-h", or "--help" flag is present.
 * Must be called after printf is setup.
 */
void CmiArgInit(char **argv) {
	int i;
	for (i=0;argv[i]!=NULL;i++)
	{
		if (0==strcmp(argv[i],"-?") ||
		    0==strcmp(argv[i],"-h") ||
		    0==strcmp(argv[i],"--help")) 
		{
			printUsage=1;
			/* Don't delete arg:  CmiDeleteArgs(&argv[i],1);
			  Leave it there for user program to see... */
			CmiPrintCLAs();
		}
	}
	if (CmiMyPe()==0) { /* Throw away list of stored CLA's */
		CLAlistLen=CLAlistMax=0;
		free(CLAlist); CLAlist=NULL;
	}
	usageChecked=1;
}

/* Return 1 if we're currently printing command-line usage information. */
int CmiArgGivingUsage(void) {
	return (CmiMyPe()==0) && printUsage;
}

/* Identifies the module that accepts the following command-line parameters */
void CmiArgGroup(const char *parentName,const char *groupName) {
	if (CmiArgGivingUsage()) {
		if (groupName==NULL) groupName=parentName; /* Start of a new group */
		CmiPrintf("\n%s Command-line Parameters:\n",groupName);
	}
}

/*Count the number of non-NULL arguments in list*/
int CmiGetArgc(char **argv)
{
	int i=0,argc=0;
	while (argv[i++]!=NULL)
		argc++;
	return argc;
}

/*Return a new, heap-allocated copy of the argv array*/
char **CmiCopyArgs(char **argv)
{
	int argc=CmiGetArgc(argv);
	char **ret=(char **)malloc(sizeof(char *)*(argc+1));
	int i;
	for (i=0;i<=argc;i++)
		ret[i]=argv[i];
	return ret;
}

/*Delete the first k argument from the given list, shifting
all other arguments down by k spaces.
e.g., argv=={"a","b","c","d",NULL}, k==3 modifies
argv={"d",NULL,"c","d",NULL}
*/
void CmiDeleteArgs(char **argv,int k)
{
	int i=0;
	while ((argv[i]=argv[i+k])!=NULL)
		i++;
}

/*Find the given argment and string option in argv.
If the argument is present, set the string option and
delete both from argv.  If not present, return NULL.
e.g., arg=="-name" returns "bob" from
argv=={"a.out","foo","-name","bob","bar"},
and sets argv={"a.out","foo","bar"};
*/
int CmiGetArgStringDesc(char **argv,const char *arg,char **optDest,const char *desc)
{
	int i;
	CmiAddCLA(arg,"string",desc);
	for (i=0;argv[i]!=NULL;i++)
		if (0==strcmp(argv[i],arg))
		{/*We found the argument*/
			if (argv[i+1]==NULL) CmiAbort("Argument not complete!");
			*optDest=argv[i+1];
			CmiDeleteArgs(&argv[i],2);
			return 1;
		}
	return 0;/*Didn't find the argument*/
}
int CmiGetArgString(char **argv,const char *arg,char **optDest) {
	return CmiGetArgStringDesc(argv,arg,optDest,"");
}

/*Find the given argument and floating-point option in argv.
Remove it and return 1; or return 0.
*/
int CmiGetArgDoubleDesc(char **argv,const char *arg,double *optDest,const char *desc) {
	char *number=NULL;
	CmiAddCLA(arg,"number",desc);
	if (!CmiGetArgStringDesc(argv,arg,&number,NULL)) return 0;
	if (1!=sscanf(number,"%lg",optDest)) return 0;
	return 1;
}
int CmiGetArgDouble(char **argv,const char *arg,double *optDest) {
	return CmiGetArgDoubleDesc(argv,arg,optDest,"");
}

/*Find the given argument and integer option in argv.
If the argument is present, parse and set the numeric option,
delete both from argv, and return 1. If not present, return 0.
e.g., arg=="-pack" matches argv=={...,"-pack","27",...},
argv=={...,"-pack0xf8",...}, and argv=={...,"-pack=0777",...};
but not argv=={...,"-packsize",...}.
*/
int CmiGetArgIntDesc(char **argv,const char *arg,int *optDest,const char *desc)
{
	int i;
	int argLen=strlen(arg);
	CmiAddCLA(arg,"integer",desc);
	for (i=0;argv[i]!=NULL;i++)
		if (0==strncmp(argv[i],arg,argLen))
		{/*We *may* have found the argument*/
			const char *opt=NULL;
			int nDel=0;
			switch(argv[i][argLen]) {
			case 0: /* like "-p","27" */
				opt=argv[i+1]; nDel=2; break;
			case '=': /* like "-p=27" */
				opt=&argv[i][argLen+1]; nDel=1; break;
			case '-':case '+':
			case '0':case '1':case '2':case '3':case '4':
			case '5':case '6':case '7':case '8':case '9':
				/* like "-p27" */
				opt=&argv[i][argLen]; nDel=1; break;
			default:
				continue; /*False alarm-- skip it*/
			}
			if (opt==NULL) continue; /*False alarm*/
			if (sscanf(opt,"%i",optDest)<1) {
			/*Bad command line argument-- die*/
				fprintf(stderr,"Cannot parse %s option '%s' "
					"as an integer.\n",arg,opt);
				CmiAbort("Bad command-line argument\n");
			}
			CmiDeleteArgs(&argv[i],nDel);
			return 1;
		}
	return 0;/*Didn't find the argument-- dest is unchanged*/	
}
int CmiGetArgInt(char **argv,const char *arg,int *optDest) {
	return CmiGetArgIntDesc(argv,arg,optDest,"");
}

/*Find the given argument in argv.  If present, delete
it and return 1; if not present, return 0.
e.g., arg=="-foo" matches argv=={...,"-foo",...} but not
argv={...,"-foobar",...}.
*/
int CmiGetArgFlagDesc(char **argv,const char *arg,const char *desc)
{
	int i;
	CmiAddCLA(arg,"",desc);
	for (i=0;argv[i]!=NULL;i++)
		if (0==strcmp(argv[i],arg))
		{/*We found the argument*/
			CmiDeleteArgs(&argv[i],1);
			return 1;
		}
	return 0;/*Didn't find the argument*/
}
int CmiGetArgFlag(char **argv,const char *arg) {
	return CmiGetArgFlagDesc(argv,arg,"");
}


/*****************************************************************************
 *
 * Stack tracing routines.
 *
 *****************************************************************************/
#include "cmibacktrace.c"

/*
Convert "X(Y) Z" to "Y Z"-- remove text prior to first '(', and supress
the next parenthesis.  Operates in-place on the character data.
*/
static char *_implTrimParenthesis(char *str) {
  char *lParen=str, *ret=NULL, *rParen=NULL;
  while (*lParen!='(') {
    if (*lParen==0) return str; /* No left parenthesis at all. */
    lParen++;
  }
  /* now *lParen=='(', so trim it*/
  ret=lParen+1;
  rParen=ret;
  while (*rParen!=')') {
    if (*rParen==0) return ret; /* No right parenthesis at all. */
    rParen++;
  }
  /* now *rParen==')', so trim it*/
  *rParen=' ';
  return ret;  
}

/*
Return the text description of this trimmed routine name, if 
it's a system-generated routine where we should stop printing. 
This is probably overkill, but improves the appearance of callbacks.
*/
static const char* _implGetBacktraceSys(const char *name) {
  if (0==strncmp(name,"_call",5)) 
  { /*it might be something we're interested in*/
    if (0==strncmp(name,"_call_",6)) return "Call Entry Method";
    if (0==strncmp(name,"_callthr_",9)) return "Call Threaded Entry Method";
  }
  if (0==strncmp(name,"CthResume",9)) return "Resumed thread";
  if (0==strncmp(name,"qt_args",7)) return "Converse thread";
  
  return 0; /*ordinary user routine-- just print normally*/
}

/** Print out the names of these function pointers. */
void CmiBacktracePrint(void **retPtrs,int nLevels) {
  if (nLevels>0) {
    int i;
    char **names=CmiBacktraceLookup(retPtrs,nLevels);
    if (names==NULL) return;
    CmiPrintf("Stack Traceback:\n");
    for (i=0;i<nLevels;i++) {
      const char *trimmed=_implTrimParenthesis(names[i]);
      const char *print=trimmed;
      const char *sys=_implGetBacktraceSys(print);
      if (sys) {
          CmiPrintf("  [%d] Charm++ Runtime: %s (%s)\n",i,sys,print);
          break; /*Stop when we hit Charm++ runtime.*/
      } else {
          CmiPrintf("  [%d] %s\n",i,print);
      }
    }
    free(names);
  }
}

/* Print (to stdout) the names of the functions that have been 
   called up to this point. nSkip is the number of routines on the
   top of the stack to *not* print out. */
void CmiPrintStackTrace(int nSkip) {
#if CMK_USE_BACKTRACE
	int nLevels=max_stack;
	void *stackPtrs[max_stack];
	CmiBacktraceRecord(stackPtrs,1+nSkip,&nLevels);
	CmiBacktracePrint(stackPtrs,nLevels);
#endif
}

/*****************************************************************************
 *
 * Statistics: currently, the following statistics are not updated by converse.
 *
 *****************************************************************************/

CpvDeclare(int, CstatsMaxChareQueueLength);
CpvDeclare(int, CstatsMaxForChareQueueLength);
CpvDeclare(int, CstatsMaxFixedChareQueueLength);
CpvStaticDeclare(int, CstatPrintQueueStatsFlag);
CpvStaticDeclare(int, CstatPrintMemStatsFlag);

void CstatsInit(argv)
char **argv;
{

#ifdef MEMMONITOR
  CpvInitialize(mmulong,MemoryUsage);
  CpvAccess(MemoryUsage) = 0;
  CpvInitialize(mmulong,HiWaterMark);
  CpvAccess(HiWaterMark) = 0;
  CpvInitialize(mmulong,ReportedHiWaterMark);
  CpvAccess(ReportedHiWaterMark) = 0;
  CpvInitialize(int,AllocCount);
  CpvAccess(AllocCount) = 0;
  CpvInitialize(int,BlocksAllocated);
  CpvAccess(BlocksAllocated) = 0;
#endif

  CpvInitialize(int, CstatsMaxChareQueueLength);
  CpvInitialize(int, CstatsMaxForChareQueueLength);
  CpvInitialize(int, CstatsMaxFixedChareQueueLength);
  CpvInitialize(int, CstatPrintQueueStatsFlag);
  CpvInitialize(int, CstatPrintMemStatsFlag);

  CpvAccess(CstatsMaxChareQueueLength) = 0;
  CpvAccess(CstatsMaxForChareQueueLength) = 0;
  CpvAccess(CstatsMaxFixedChareQueueLength) = 0;
  CpvAccess(CstatPrintQueueStatsFlag) = 0;
  CpvAccess(CstatPrintMemStatsFlag) = 0;

#if 0
  if (CmiGetArgFlagDesc(argv,"+mems", "Print memory statistics at shutdown"))
    CpvAccess(CstatPrintMemStatsFlag)=1;
  if (CmiGetArgFlagDesc(argv,"+qs", "Print queue statistics at shutdown"))
    CpvAccess(CstatPrintQueueStatsFlag)=1;
#endif
}

int CstatMemory(i)
int i;
{
  return 0;
}

int CstatPrintQueueStats()
{
  return CpvAccess(CstatPrintQueueStatsFlag);
}

int CstatPrintMemStats()
{
  return CpvAccess(CstatPrintMemStatsFlag);
}

/*****************************************************************************
 *
 * Cmi handler registration
 *
 *****************************************************************************/

CpvDeclare(CmiHandlerInfo*, CmiHandlerTable);
CpvStaticDeclare(int  , CmiHandlerCount);
CpvStaticDeclare(int  , CmiHandlerLocal);
CpvStaticDeclare(int  , CmiHandlerGlobal);
CpvDeclare(int,         CmiHandlerMax);

static void CmiExtendHandlerTable(int atLeastLen) {
    int max = CpvAccess(CmiHandlerMax);
    int newmax = (atLeastLen+(atLeastLen>>2)+32);
    int bytes = max*sizeof(CmiHandlerInfo);
    int newbytes = newmax*sizeof(CmiHandlerInfo);
    CmiHandlerInfo *nu = (CmiHandlerInfo*)malloc(newbytes);
    CmiHandlerInfo *tab = CpvAccess(CmiHandlerTable);
    _MEMCHECK(nu);
    memcpy(nu, tab, bytes);
    memset(((char *)nu)+bytes, 0, (newbytes-bytes));
    free(tab); tab=nu;
    CpvAccess(CmiHandlerTable) = tab;
    CpvAccess(CmiHandlerMax) = newmax;
}

void CmiNumberHandler(int n, CmiHandler h)
{
  CmiHandlerInfo *tab;
  if (n >= CpvAccess(CmiHandlerMax)) CmiExtendHandlerTable(n);
  tab = CpvAccess(CmiHandlerTable);
  tab[n].hdlr = (CmiHandlerEx)h; /* LIE!  This assumes extra pointer will be ignored!*/
  tab[n].userPtr = 0;
}
void CmiNumberHandlerEx(int n, CmiHandlerEx h,void *userPtr) {
  CmiHandlerInfo *tab;
  if (n >= CpvAccess(CmiHandlerMax)) CmiExtendHandlerTable(n);
  tab = CpvAccess(CmiHandlerTable);
  tab[n].hdlr = h;
  tab[n].userPtr=userPtr;
}

#if CMI_LOCAL_GLOBAL_AVAILABLE /*Leave room for local and global handlers*/
#  define DIST_BETWEEN_HANDLERS 3
#else /*No local or global handlers; ordinary handlers are back-to-back*/
#  define DIST_BETWEEN_HANDLERS 1
#endif

int CmiRegisterHandler(CmiHandler h)
{
  int Count = CpvAccess(CmiHandlerCount);
  CmiNumberHandler(Count, h);
  CpvAccess(CmiHandlerCount) = Count+DIST_BETWEEN_HANDLERS;
  return Count;
}
int CmiRegisterHandlerEx(CmiHandlerEx h,void *userPtr)
{
  int Count = CpvAccess(CmiHandlerCount);
  CmiNumberHandlerEx(Count, h, userPtr);
  CpvAccess(CmiHandlerCount) = Count+DIST_BETWEEN_HANDLERS;
  return Count;
}

#if CMI_LOCAL_GLOBAL_AVAILABLE
int CmiRegisterHandlerLocal(h)
CmiHandler h;
{
  int Local = CpvAccess(CmiHandlerLocal);
  CmiNumberHandler(Local, h);
  CpvAccess(CmiHandlerLocal) = Local+3;
  return Local;
}

int CmiRegisterHandlerGlobal(h)
CmiHandler h;
{
  int Global = CpvAccess(CmiHandlerGlobal);
  if (CmiMyPe()!=0) 
    CmiError("CmiRegisterHandlerGlobal must only be called on PE 0.\n");
  CmiNumberHandler(Global, h);
  CpvAccess(CmiHandlerGlobal) = Global+3;
  return Global;
}
#endif

static void _cmiZeroHandler(void *msg) {
	CmiAbort("Converse zero handler executed-- was a message corrupted?\n");
}

static void CmiHandlerInit()
{
  CpvInitialize(CmiHandlerInfo *, CmiHandlerTable);
  CpvInitialize(int         , CmiHandlerCount);
  CpvInitialize(int         , CmiHandlerLocal);
  CpvInitialize(int         , CmiHandlerGlobal);
  CpvInitialize(int         , CmiHandlerMax);
  CpvAccess(CmiHandlerCount)  = 0;
  CpvAccess(CmiHandlerLocal)  = 1;
  CpvAccess(CmiHandlerGlobal) = 2;
  CpvAccess(CmiHandlerMax) = 0; /* Table will be extended on the first registration*/
  CpvAccess(CmiHandlerTable) = NULL;
  CmiRegisterHandler((CmiHandler)_cmiZeroHandler);
}


/******************************************************************************
 *
 * CmiTimer
 *
 * Here are two possible implementations of CmiTimer.  Some machines don't
 * select either, and define the timer in machine.c instead.
 *
 *****************************************************************************/

#if CMK_TIMER_USE_TIMES

CpvStaticDeclare(double, clocktick);
CpvStaticDeclare(int,inittime_wallclock);
CpvStaticDeclare(int,inittime_virtual);

int CmiTimerIsSynchronized()
{
  return 0;
}

void CmiTimerInit()
{
  struct tms temp;
  CpvInitialize(double, clocktick);
  CpvInitialize(int, inittime_wallclock);
  CpvInitialize(int, inittime_virtual);
  CpvAccess(inittime_wallclock) = times(&temp);
  CpvAccess(inittime_virtual) = temp.tms_utime + temp.tms_stime;
  CpvAccess(clocktick) = 1.0 / (sysconf(_SC_CLK_TCK));
}

double CmiWallTimer()
{
  struct tms temp;
  double currenttime;
  int now;

  now = times(&temp);
  currenttime = (now - CpvAccess(inittime_wallclock)) * CpvAccess(clocktick);
  return (currenttime);
}

double CmiCpuTimer()
{
  struct tms temp;
  double currenttime;
  int now;

  times(&temp);
  now = temp.tms_stime + temp.tms_utime;
  currenttime = (now - CpvAccess(inittime_virtual)) * CpvAccess(clocktick);
  return (currenttime);
}

double CmiTimer()
{
  return CmiCpuTimer();
}

#endif

#if CMK_TIMER_USE_GETRUSAGE

static double inittime_wallclock;
CpvStaticDeclare(double, inittime_virtual);

int CmiTimerIsSynchronized()
{
  return 0;
}

void CmiTimerInit()
{
  struct timeval tv;
  struct rusage ru;
  CpvInitialize(double, inittime_virtual);

  /* try to synchronize calling barrier */
  CmiBarrier();
  CmiBarrier();
  CmiBarrier();

  gettimeofday(&tv,0);
  inittime_wallclock = (tv.tv_sec * 1.0) + (tv.tv_usec*0.000001);
  getrusage(0, &ru); 
  CpvAccess(inittime_virtual) =
    (ru.ru_utime.tv_sec * 1.0)+(ru.ru_utime.tv_usec * 0.000001) +
    (ru.ru_stime.tv_sec * 1.0)+(ru.ru_stime.tv_usec * 0.000001);

  CmiBarrierZero();
}

double CmiCpuTimer()
{
  struct rusage ru;
  double currenttime;

  getrusage(0, &ru);
  currenttime =
    (ru.ru_utime.tv_sec * 1.0)+(ru.ru_utime.tv_usec * 0.000001) +
    (ru.ru_stime.tv_sec * 1.0)+(ru.ru_stime.tv_usec * 0.000001);
  return currenttime - CpvAccess(inittime_virtual);
}

static double lastT = -1.0;

double CmiWallTimer()
{
  struct timeval tv;
  double currenttime;

  gettimeofday(&tv,0);
  currenttime = (tv.tv_sec * 1.0) + (tv.tv_usec * 0.000001);
#ifndef CMK_OPTIMIZE
  if (lastT > 0.0 && currenttime < lastT) {
    currenttime = lastT;
  }
  lastT = currenttime;
#endif
  return currenttime - inittime_wallclock;
}

double CmiTimer()
{
  return CmiCpuTimer();
}

#endif

#if CMK_TIMER_USE_RDTSC

static double readMHz(void)
{
  double x;
  char str[1000];
  char buf[100];
  FILE *fp;
  CmiLock(smp_mutex);
  fp = fopen("/proc/cpuinfo", "r");
  if (fp != NULL)
  while(fgets(str, 1000, fp)!=0) {
    if(sscanf(str, "cpu MHz%[^:]",buf)==1)
    {
      char *s = strchr(str, ':'); s=s+1;
      sscanf(s, "%lf", &x);
      fclose(fp);
      CmiUnlock(smp_mutex);
      return x;
    }
  }
  CmiUnlock(smp_mutex);
  CmiAbort("Cannot read CPU MHz from /proc/cpuinfo file.");
  return 0.0;
}

double _cpu_speed_factor;
CpvStaticDeclare(double, inittime_virtual);
CpvStaticDeclare(double, inittime_walltime);

double  CmiStartTimer(void)
{
  return CpvAccess(inittime_walltime);
}

void CmiTimerInit()
{
  struct rusage ru;

  CmiBarrier();
  CmiBarrier();

  _cpu_speed_factor = 1.0/(readMHz()*1.0e6); 
  rdtsc(); rdtsc(); rdtsc(); rdtsc(); rdtsc();
  CpvInitialize(double, inittime_walltime);
  CpvAccess(inittime_walltime) = CmiWallTimer();
  CpvInitialize(double, inittime_virtual);
  getrusage(0, &ru); 
  CpvAccess(inittime_virtual) =
    (ru.ru_utime.tv_sec * 1.0)+(ru.ru_utime.tv_usec * 0.000001) +
    (ru.ru_stime.tv_sec * 1.0)+(ru.ru_stime.tv_usec * 0.000001);

  CmiBarrierZero();
}

double CmiCpuTimer()
{
  struct rusage ru;
  double currenttime;

  getrusage(0, &ru);
  currenttime =
    (ru.ru_utime.tv_sec * 1.0)+(ru.ru_utime.tv_usec * 0.000001) +
    (ru.ru_stime.tv_sec * 1.0)+(ru.ru_stime.tv_usec * 0.000001);
  return currenttime - CpvAccess(inittime_virtual);
}

#endif

#if CMK_VERSION_BLUEGENE || CMK_BLUEGENEP
#include "dcopy.h"
#endif

#if CMK_TIMER_USE_BLUEGENEL

#include "rts.h"

#if 0 
#define SPRN_TBRL 0x10C  /* Time Base Read Lower Register (user & sup R/O) */
#define SPRN_TBRU 0x10D  /* Time Base Read Upper Register (user & sup R/O) */
#define SPRN_PIR  0x11E  /* CPU id */

static inline unsigned long long BGLTimebase(void)
{
  unsigned volatile u1, u2, lo;
  union
  {
    struct { unsigned hi, lo; } w;
    unsigned long long d;
  } result;
                                                                                
  do {
    asm volatile ("mfspr %0,%1" : "=r" (u1) : "i" (SPRN_TBRU));
    asm volatile ("mfspr %0,%1" : "=r" (lo) : "i" (SPRN_TBRL));
    asm volatile ("mfspr %0,%1" : "=r" (u2) : "i" (SPRN_TBRU));
  } while (u1!=u2);
                                                                                
  result.w.lo = lo;
  result.w.hi = u2;
  return result.d;
}
#endif

static unsigned long long inittime_wallclock = 0;
CpvStaticDeclare(double, clocktick);

int CmiTimerIsSynchronized()
{
  return 0;
}

void CmiTimerInit()
{
  BGLPersonality dst;
  CpvInitialize(double, clocktick);
  int size = sizeof(BGLPersonality);
  rts_get_personality(&dst, size);
  CpvAccess(clocktick) = 1.0 / dst.clockHz;

  /* try to synchronize calling barrier */
  CmiBarrier();
  CmiBarrier();
  CmiBarrier();

  /* inittime_wallclock = rts_get_timebase(); */
  inittime_wallclock = 0.0;    /* use bgl absolute time */
}

double CmiWallTimer()
{
  unsigned long long currenttime;
  currenttime = rts_get_timebase();
  return CpvAccess(clocktick)*(currenttime-inittime_wallclock);
}

double CmiCpuTimer()
{
  return CmiWallTimer();
}

double CmiTimer()
{
  return CmiWallTimer();
}

#endif

#if CMK_TIMER_USE_BLUEGENEP  /* This module just compiles with GCC charm. */

void CmiTimerInit() {}

#if 0
#include "common/bgp_personality.h"
#include <spi/bgp_SPI.h>

#define SPRN_TBRL 0x10C  /* Time Base Read Lower Register (user & sup R/O) */
#define SPRN_TBRU 0x10D  /* Time Base Read Upper Register (user & sup R/O) */
#define SPRN_PIR  0x11E  /* CPU id */

static inline unsigned long long BGPTimebase(void)
{
  unsigned volatile u1, u2, lo;
  union
  {
    struct { unsigned hi, lo; } w;
    unsigned long long d;
  } result;
                                                                         
  do {
    asm volatile ("mfspr %0,%1" : "=r" (u1) : "i" (SPRN_TBRU));
    asm volatile ("mfspr %0,%1" : "=r" (lo) : "i" (SPRN_TBRL));
    asm volatile ("mfspr %0,%1" : "=r" (u2) : "i" (SPRN_TBRU));
  } while (u1!=u2);
                                                                         
  result.w.lo = lo;
  result.w.hi = u2;
  return result.d;
}

static unsigned long long inittime_wallclock = 0;
CpvStaticDeclare(double, clocktick);

int CmiTimerIsSynchronized()
{
  return 0;
}

void CmiTimerInit()
{
  _BGP_Personality_t dst;
  CpvInitialize(double, clocktick);
  int size = sizeof(_BGP_Personality_t);
  rts_get_personality(&dst, size);

  CpvAccess(clocktick) = 1.0 / (dst.Kernel_Config.FreqMHz * 1e6);

  /* try to synchronize calling barrier */
  CmiBarrier();
  CmiBarrier();
  CmiBarrier();

  inittime_wallclock = BGPTimebase (); 
}

double CmiWallTimer()
{
  unsigned long long currenttime;
  currenttime = BGPTimebase();
  return CpvAccess(clocktick)*(currenttime-inittime_wallclock);
}
#endif

#include "dcmf.h"

double CmiWallTimer () {
  return DCMF_Timer();
}

double CmiCpuTimer()
{
  return CmiWallTimer();
}

double CmiTimer()
{
  return CmiWallTimer();
}

#endif


#if CMK_TIMER_USE_WIN32API

CpvStaticDeclare(double, inittime_wallclock);
CpvStaticDeclare(double, inittime_virtual);

void CmiTimerInit()
{
#ifdef __CYGWIN__
	struct timeb tv;
#else
	struct _timeb tv;
#endif
	clock_t       ru;

	CpvInitialize(double, inittime_wallclock);
	CpvInitialize(double, inittime_virtual);
	_ftime(&tv);
	CpvAccess(inittime_wallclock) = tv.time*1.0 + tv.millitm*0.001;
	ru = clock();
	CpvAccess(inittime_virtual) = ((double) ru)/CLOCKS_PER_SEC;
}

double CmiCpuTimer()
{
	clock_t ru;
	double currenttime;

	ru = clock();
	currenttime = (double) ru/CLOCKS_PER_SEC;

	return currenttime - CpvAccess(inittime_virtual);
}

double CmiWallTimer()
{
#ifdef __CYGWIN__
	struct timeb tv;
#else
	struct _timeb tv;
#endif
	double currenttime;

	_ftime(&tv);
	currenttime = tv.time*1.0 + tv.millitm*0.001;

	return currenttime - CpvAccess(inittime_wallclock);
}
	

double CmiTimer()
{
	return CmiCpuTimer();
}

#endif

#if CMK_TIMER_USE_RTC

#if __crayx1
 /* For _rtc() on Cray X1 */
#include <intrinsics.h>
#endif

static double clocktick;
CpvStaticDeclare(long long, inittime_wallclock);

void CmiTimerInit()
{
  CpvInitialize(long long, inittime_wallclock);
  CpvAccess(inittime_wallclock) = _rtc();
  clocktick = 1.0 / (double)(sysconf(_SC_SV2_USER_TIME_RATE));
}

double CmiWallTimer()
{
  long long now;

  now = _rtc();
  return (clocktick * (now - CpvAccess(inittime_wallclock)));
}

double CmiCpuTimer()
{
  return CmiWallTimer();
}

double CmiTimer()
{
  return CmiCpuTimer();
}

#endif

#ifndef CMK_USE_SPECIAL_MESSAGE_QUEUE_CHECK
/** Return 1 if our outgoing message queue 
   for this node is longer than this many bytes. */
int CmiLongSendQueue(int forNode,int longerThanBytes) {
  return 0;
}
#endif

#if CMK_SIGNAL_USE_SIGACTION
#include <signal.h>
void CmiSignal(sig1, sig2, sig3, handler)
int sig1, sig2, sig3;
void (*handler)();
{
  struct sigaction in, out ;
  in.sa_handler = handler;
  sigemptyset(&in.sa_mask);
  if (sig1) sigaddset(&in.sa_mask, sig1);
  if (sig2) sigaddset(&in.sa_mask, sig2);
  if (sig3) sigaddset(&in.sa_mask, sig3);
  in.sa_flags = 0;
  if (sig1) if (sigaction(sig1, &in, &out)<0) exit(1);
  if (sig2) if (sigaction(sig2, &in, &out)<0) exit(1);
  if (sig3) if (sigaction(sig3, &in, &out)<0) exit(1);
}
#endif

#if CMK_SIGNAL_USE_SIGACTION_WITH_RESTART
#include <signal.h>
void CmiSignal(sig1, sig2, sig3, handler)
int sig1, sig2, sig3;
void (*handler)();
{
  struct sigaction in, out ;
  in.sa_handler = handler;
  sigemptyset(&in.sa_mask);
  if (sig1) sigaddset(&in.sa_mask, sig1);
  if (sig2) sigaddset(&in.sa_mask, sig2);
  if (sig3) sigaddset(&in.sa_mask, sig3);
  in.sa_flags = SA_RESTART;
  if (sig1) if (sigaction(sig1, &in, &out)<0) exit(1);
  if (sig2) if (sigaction(sig2, &in, &out)<0) exit(1);
  if (sig3) if (sigaction(sig3, &in, &out)<0) exit(1);
}
#endif

/*****************************************************************************
 *
 * The following is the CsdScheduler function.  A common
 * implementation is provided below.  The machine layer can provide an
 * alternate implementation if it so desires.
 *
 * void CmiDeliversInit()
 *
 *      - CmiInit promises to call this before calling CmiDeliverMsgs
 *        or any of the other functions in this section.
 *
 * int CmiDeliverMsgs(int maxmsgs)
 *
 *      - CmiDeliverMsgs will retrieve up to maxmsgs that were transmitted
 *        with the Cmi, and will invoke their handlers.  It does not wait
 *        if no message is unavailable.  Instead, it returns the quantity
 *        (maxmsgs-delivered), where delivered is the number of messages it
 *        delivered.
 *
 * void CmiDeliverSpecificMsg(int handlerno)
 *
 *      - Waits for a message with the specified handler to show up, then
 *        invokes the message's handler.  Note that unlike CmiDeliverMsgs,
 *        This function _does_ wait.
 *
 * For this common implementation to work, the machine layer must provide the
 * following:
 *
 * void *CmiGetNonLocal()
 *
 *      - returns a message just retrieved from some other PE, not from
 *        local.  If no such message exists, returns 0.
 *
 * CpvExtern(CdsFifo, CmiLocalQueue);
 *
 *      - a FIFO queue containing all messages from the local processor.
 *
 *****************************************************************************/

void CsdBeginIdle(void)
{
  CcdCallBacks();
  _LOG_E_PROC_IDLE(); 	/* projector */
  CcdRaiseCondition(CcdPROCESSOR_BEGIN_IDLE) ;
}

void CsdStillIdle(void)
{
  CcdRaiseCondition(CcdPROCESSOR_STILL_IDLE);
}

void CsdEndIdle(void)
{
  _LOG_E_PROC_BUSY(); 	/* projector */
  CcdRaiseCondition(CcdPROCESSOR_BEGIN_BUSY) ;
}

#if CMK_MEM_CHECKPOINT
#define MESSAGE_PHASE_CHECK	\
	{	\
          int phase = CmiGetRestartPhase(msg);	\
	  if (phase < cur_restart_phase) {	\
            /*CmiPrintf("[%d] discard message of phase %d cur_restart_phase:%d handler:%d. \n", CmiMyPe(), phase, cur_restart_phase, handler);*/	\
            CmiFree(msg);	\
	    return;	\
          }	\
	}
#else
#define MESSAGE_PHASE_CHECK
#endif

extern int _exitHandlerIdx;

void CmiHandleMessage(void *msg)
{
/* this is wrong because it counts the Charm++ messages in sched queue
 	CpvAccess(cQdState)->mProcessed++;
*/
	CmiHandlerInfo *h;
#ifndef CMK_OPTIMIZE
	CmiUInt2 handler=CmiGetHandler(msg); /* Save handler for use after msg is gone */
	_LOG_E_HANDLER_BEGIN(handler); /* projector */
	setMemoryStatus(1)  /* charmdebug */
#endif

/*
	FAULT_EVAC
*/
/*	if((!CpvAccess(_validProcessors)[CmiMyPe()]) && handler != _exitHandlerIdx){
		return;
	}*/
	
        MESSAGE_PHASE_CHECK

	h=&CmiGetHandlerInfo(msg);
	(h->hdlr)(msg,h->userPtr);
#ifndef CMK_OPTIMIZE
	setMemoryStatus(0)  /* charmdebug */
	_LOG_E_HANDLER_END(handler); 	/* projector */
#endif
}

#if CMK_CMIDELIVERS_USE_COMMON_CODE

void CmiDeliversInit()
{
}

int CmiDeliverMsgs(int maxmsgs)
{
  return CsdScheduler(maxmsgs);
}

#if CMK_OBJECT_QUEUE_AVAILABLE
CpvDeclare(void *, CsdObjQueue);
#endif

void CsdSchedulerState_new(CsdSchedulerState_t *s)
{
#if CMK_OBJECT_QUEUE_AVAILABLE
	s->objQ=CpvAccess(CsdObjQueue);
#endif
	s->localQ=CpvAccess(CmiLocalQueue);
	s->schedQ=CpvAccess(CsdSchedQueue);
	s->localCounter=&(CpvAccess(CsdLocalCounter));
#if CMK_NODE_QUEUE_AVAILABLE
	s->nodeQ=CsvAccess(CsdNodeQueue);
	s->nodeLock=CsvAccess(CsdNodeQueueLock);
#endif
#if CMK_GRID_QUEUE_AVAILABLE
	s->gridQ=CpvAccess(CsdGridQueue);
#endif
}

void *CsdNextMessage(CsdSchedulerState_t *s) {
	void *msg;
	if((*(s->localCounter))-- >0)
	  {
              /* This avoids a race condition with migration detected by megatest*/
              msg=CdsFifo_Dequeue(s->localQ);
              if (msg!=NULL)
		{
		  CpvAccess(cQdState)->mProcessed++;
		  return msg;	    
		}
              CqsDequeue(s->schedQ,(void **)&msg);
              if (msg!=NULL) return msg;
	  }
	
	*(s->localCounter)=CsdLocalMax;
	if ( NULL!=(msg=CmiGetNonLocal()) || 
	     NULL!=(msg=CdsFifo_Dequeue(s->localQ)) ) {
            CpvAccess(cQdState)->mProcessed++;
            return msg;
        }
#if CMK_GRID_QUEUE_AVAILABLE
	CqsDequeue (s->gridQ, (void **) &msg);
	if (msg != NULL) {
	  return (msg);
	}
#endif
#if CMK_NODE_QUEUE_AVAILABLE
	if (NULL!=(msg=CmiGetNonLocalNodeQ())) return msg;
	if (!CqsEmpty(s->nodeQ)
	 && !CqsPrioGT(CqsGetPriority(s->nodeQ),
		       CqsGetPriority(s->schedQ))) {
	  CmiLock(s->nodeLock);
	  CqsDequeue(s->nodeQ,(void **)&msg);
	  CmiUnlock(s->nodeLock);
	  if (msg!=NULL) return msg;
	}
#endif
#if CMK_OBJECT_QUEUE_AVAILABLE
	if (NULL!=(msg=CdsFifo_Dequeue(s->objQ))) {
          return msg;
        }
#endif
        if(!CsdLocalMax) {
	  CqsDequeue(s->schedQ,(void **)&msg);
            if (msg!=NULL) return msg;	    
        }

	return NULL;
}

int CsdScheduler(int maxmsgs)
{
	if (maxmsgs<0) CsdScheduleForever();	
	else if (maxmsgs==0)
		CsdSchedulePoll();
	else /*(maxmsgs>0)*/ 
		return CsdScheduleCount(maxmsgs);
	return 0;
}

/*Declare the standard scheduler housekeeping*/
#define SCHEDULE_TOP \
      void *msg;\
			int rank = CmiMyRank();\
      int cycle = Cpv_CsdStopFlag_[rank]; \
      CsdSchedulerState_t state;\
      CsdSchedulerState_new(&state);\

/*A message is available-- process it*/
#define SCHEDULE_MESSAGE \
      CmiHandleMessage(msg);\
      if (Cpv_CsdStopFlag_[rank] != cycle) break;\

/*No message available-- go (or remain) idle*/
#define SCHEDULE_IDLE \
      if (!isIdle) {isIdle=1;CsdBeginIdle();}\
      else CsdStillIdle();\
      if (Cpv_CsdStopFlag_[rank] != cycle) {\
	CsdEndIdle();\
	break;\
      }\
/*
	EVAC
*/
extern void CkClearAllArrayElements();


extern void machine_OffloadAPIProgress();

void CsdScheduleForever(void)
{
  #if CMK_CELL
    #define CMK_CELL_PROGRESS_FREQ  96  /* (MSG-Q Entries x1.5) */
    int progressCount = CMK_CELL_PROGRESS_FREQ;
  #endif

  int isIdle=0;
  SCHEDULE_TOP
  while (1) {
    msg = CsdNextMessage(&state);
    if (msg) { /*A message is available-- process it*/
      if (isIdle) {isIdle=0;CsdEndIdle();}
      SCHEDULE_MESSAGE

      #if CMK_CELL
        if (progressCount <= 0) {
          //OffloadAPIProgress();
          machine_OffloadAPIProgress();
          progressCount = CMK_CELL_PROGRESS_FREQ;
	}
        progressCount--;
      #endif

    } else { /*No message available-- go (or remain) idle*/
      SCHEDULE_IDLE

      #if CMK_CELL
        //OffloadAPIProgress();
        machine_OffloadAPIProgress();
        progressCount = CMK_CELL_PROGRESS_FREQ;
      #endif

    }
    CsdPeriodic();
  }
}
int CsdScheduleCount(int maxmsgs)
{
  int isIdle=0;
  SCHEDULE_TOP
  while (1) {
    msg = CsdNextMessage(&state);
    if (msg) { /*A message is available-- process it*/
      if (isIdle) {isIdle=0;CsdEndIdle();}
      maxmsgs--; 
      SCHEDULE_MESSAGE
      if (maxmsgs==0) break;
    } else { /*No message available-- go (or remain) idle*/
      SCHEDULE_IDLE
    }
    CsdPeriodic();
  }
  return maxmsgs;
}

void CsdSchedulePoll(void)
{
  SCHEDULE_TOP
  while (1)
  {
	CsdPeriodic();
        /*CmiMachineProgressImpl(); ??? */
	if (NULL!=(msg = CsdNextMessage(&state)))
	{
	     SCHEDULE_MESSAGE 
     	}
	else break;
  }
}

void CmiDeliverSpecificMsg(handler)
int handler;
{
  int *msg; int side;
  void *localqueue = CpvAccess(CmiLocalQueue);
 
  side = 0;
  while (1) {
    CsdPeriodic();
    side ^= 1;
    if (side) msg = CmiGetNonLocal();
    else      msg = CdsFifo_Dequeue(localqueue);
    if (msg) {
      if (CmiGetHandler(msg)==handler) {
	CpvAccess(cQdState)->mProcessed++;
	CmiHandleMessage(msg);
	return;
      } else {
	CdsFifo_Enqueue(localqueue, msg);
      }
    }
  }
}
 
#endif /* CMK_CMIDELIVERS_USE_COMMON_CODE */

/***************************************************************************
 *
 * Standin Schedulers.
 *
 * We use the following strategy to make sure somebody's always running
 * the scheduler (CsdScheduler).  Initially, we assume the main thread
 * is responsible for this.  If the main thread blocks, we create a
 * "standin scheduler" thread to replace it.  If the standin scheduler
 * blocks, we create another standin scheduler to replace that one,
 * ad infinitum.  Collectively, the main thread and all the standin
 * schedulers are called "scheduling threads".
 *
 * Suppose the main thread is blocked waiting for data, and a standin
 * scheduler is running instead.  Suppose, then, that the data shows
 * up and the main thread is CthAwakened.  This causes a token to be
 * pushed into the queue.  When the standin pulls the token from the
 * queue and handles it, the standin goes to sleep, and control shifts
 * back to the main thread.  In this way, unnecessary standins are put
 * back to sleep.  These sleeping standins are stored on the
 * CthSleepingStandins list.
 *
 ***************************************************************************/

CpvStaticDeclare(CthThread, CthMainThread);
CpvStaticDeclare(CthThread, CthSchedulingThread);
CpvStaticDeclare(CthThread, CthSleepingStandins);
CpvStaticDeclare(int      , CthResumeNormalThreadIdx);
CpvStaticDeclare(int      , CthResumeSchedulingThreadIdx);


void CthStandinCode()
{
  while (1) CsdScheduler(0);
}

/* this fix the function pointer for thread migration and pup */
static CthThread CthSuspendNormalThread()
{
  return CpvAccess(CthSchedulingThread);
}

void CthEnqueueSchedulingThread(CthThreadToken *token, int, int, unsigned int*);
CthThread CthSuspendSchedulingThread();

CthThread CthSuspendSchedulingThread()
{
  CthThread succ = CpvAccess(CthSleepingStandins);

  if (succ) {
    CpvAccess(CthSleepingStandins) = CthGetNext(succ);
  } else {
    succ = CthCreate(CthStandinCode, 0, 256000);
    CthSetStrategy(succ,
		   CthEnqueueSchedulingThread,
		   CthSuspendSchedulingThread);
  }
  
  CpvAccess(CthSchedulingThread) = succ;
  return succ;
}

void CthResumeNormalThread(CthThreadToken* token)
{
  CthThread t = token->thread;
  if(t == NULL){
    free(token);
    return;
  }
#ifndef CMK_OPTIMIZE
#if ! CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn))
    CthTraceResume(t);
/*    if(CpvAccess(_traceCoreOn)) 
	        resumeTraceCore();*/
#endif
#endif
    
  CthResume(t);
}

void CthResumeSchedulingThread(CthThreadToken  *token)
{
  CthThread t = token->thread;
  CthThread me = CthSelf();
  if (me == CpvAccess(CthMainThread)) {
    CthEnqueueSchedulingThread(CthGetToken(me),CQS_QUEUEING_FIFO, 0, 0);
  } else {
    CthSetNext(me, CpvAccess(CthSleepingStandins));
    CpvAccess(CthSleepingStandins) = me;
  }
  CpvAccess(CthSchedulingThread) = t;
#ifndef CMK_OPTIMIZE
#if ! CMK_TRACE_IN_CHARM
  if(CpvAccess(traceOn))
    CthTraceResume(t);
/*    if(CpvAccess(_traceCoreOn)) 
	        resumeTraceCore();*/
#endif
#endif
  CthResume(t);
}

void CthEnqueueNormalThread(CthThreadToken* token, int s, 
				   int pb,unsigned int *prio)
{
  CmiSetHandler(token, CpvAccess(CthResumeNormalThreadIdx));
  CsdEnqueueGeneral(token, s, pb, prio);
}

void CthEnqueueSchedulingThread(CthThreadToken* token, int s, 
				       int pb,unsigned int *prio)
{
  CmiSetHandler(token, CpvAccess(CthResumeSchedulingThreadIdx));
  CsdEnqueueGeneral(token, s, pb, prio);
}

void CthSetStrategyDefault(CthThread t)
{
  CthSetStrategy(t,
		 CthEnqueueNormalThread,
		 CthSuspendNormalThread);
}

void CthSchedInit()
{
  CpvInitialize(CthThread, CthMainThread);
  CpvInitialize(CthThread, CthSchedulingThread);
  CpvInitialize(CthThread, CthSleepingStandins);
  CpvInitialize(int      , CthResumeNormalThreadIdx);
  CpvInitialize(int      , CthResumeSchedulingThreadIdx);

  CpvAccess(CthMainThread) = CthSelf();
  CpvAccess(CthSchedulingThread) = CthSelf();
  CpvAccess(CthSleepingStandins) = 0;
  CpvAccess(CthResumeNormalThreadIdx) =
    CmiRegisterHandler((CmiHandler)CthResumeNormalThread);
  CpvAccess(CthResumeSchedulingThreadIdx) =
    CmiRegisterHandler((CmiHandler)CthResumeSchedulingThread);
  CthSetStrategy(CthSelf(),
		 CthEnqueueSchedulingThread,
		 CthSuspendSchedulingThread);
}

void CsdInit(argv)
  char **argv;
{
  CpvInitialize(void *, CsdSchedQueue);
  CpvInitialize(int,   CsdStopFlag);
  CpvInitialize(int,   CsdLocalCounter);
  if(!CmiGetArgIntDesc(argv,"+csdLocalMax",&CsdLocalMax,"Set the max number of local messages to process before forcing a check for remote messages."))
    {
      CsdLocalMax= CSD_LOCAL_MAX_DEFAULT;
    }
  CpvAccess(CsdLocalCounter) = CsdLocalMax;
  CpvAccess(CsdSchedQueue) = (void *)CqsCreate();

#if CMK_OBJECT_QUEUE_AVAILABLE
  CpvInitialize(void *,CsdObjQueue);
  CpvAccess(CsdObjQueue) = CdsFifo_Create();
#endif

#if CMK_NODE_QUEUE_AVAILABLE
  CsvInitialize(CmiLock, CsdNodeQueueLock);
  CsvInitialize(void *, CsdNodeQueue);
  if (CmiMyRank() ==0) {
	CsvAccess(CsdNodeQueueLock) = CmiCreateLock();
	CsvAccess(CsdNodeQueue) = (void *)CqsCreate();
  }
  CmiNodeAllBarrier();
#endif

#if CMK_GRID_QUEUE_AVAILABLE
  CsvInitialize(void *, CsdGridQueue);
  CpvAccess(CsdGridQueue) = (void *)CqsCreate();
#endif

  CpvAccess(CsdStopFlag)  = 0;
}


/*****************************************************************************
 *
 * Vector Send
 *
 * The last parameter "system" is by default at zero, in which case the normal
 * messages are sent. If it is set to 1, the CmiChunkHeader prepended to every
 * CmiAllocced message will also be sent (except for the first one). Useful for
 * AllToAll communication, and other system features. If system is 1, also all
 * the messages will be padded to 8 bytes. Thus, the caller must be aware of
 * that.
 *
 ****************************************************************************/

#if CMK_VECTOR_SEND_USES_COMMON_CODE

void CmiSyncVectorSend(int destPE, int n, int *sizes, char **msgs) {
  int total;
  char *mesg;
  VECTOR_COMPACT(total, mesg, n, sizes, msgs);
  CmiSyncSendAndFree(destPE, total, mesg);
}

CmiCommHandle CmiASyncVectorSend(int destPE, int n, int *sizes, char **msgs) {
  CmiSyncVectorSend(destPE, n, sizes, msgs);
  return NULL;
}

void CmiSyncVectorSendAndFree(int destPE, int n, int *sizes, char **msgs) {
  int i;
  CmiSyncVectorSend(destPE, n, sizes, msgs);
  for(i=0;i<n;i++) CmiFree(msgs[i]);
  CmiFree(sizes);
  CmiFree(msgs);
}

#endif

/*****************************************************************************
 *
 * Reduction management
 *
 * Only one reduction can be active at a single time in the program.
 * Moreover, since every call is supposed to pass in the same arguments,
 * having some static variables is not a problem for multithreading.
 * 
 * Except for "data" and "size", all the other parameters (which are all function
 * pointers) MUST be the same in every processor. Having different processors
 * pass in different function pointers results in an undefined behaviour.
 * 
 * The data passed in to CmiReduce and CmiNodeReduce is deleted by the system,
 * and MUST be allocated with CmiAlloc. The data passed in to the "Struct"
 * functions is deleted with the provided function, or it is left intact if no
 * function is specified.
 * 
 * The destination handler for the the first form MUST be embedded into the
 * message's header.
 * 
 * The pup function is used to pup the input data structure into a message to
 * be sent to the parent processor. This pup routine is currently used only
 * for sizing and packing, NOT unpacking. It MUST be non-null.
 * 
 * The merge function receives as first parameter the input "data", being it
 * a message or a complex data structure (it is up to the user to interpret it
 * correctly), and a list of incoming (packed) messages from the children.
 * The merge function is responsible to delete "data" if this is no longer needed.
 * The system will be in charge of deleting the messages passed in as the second
 * argument, and the return value of the function (using the provided deleteFn in
 * the second version, or CmiFree in the first). The merge function can return
 * data if the merge can be performed in-place. It MUST be non-null.
 * 
 * At the destination, on processor zero, the final data returned by the last
 * merge call will not be deleted by the system, and the CmiHandler function
 * will be in charge of its deletion.
 * 
 * CmiReduce/CmiReduceStruct MUST be called once by every processor,
 * CmiNodeReduce/CmiNodeReduceStruct MUST be called once by every node, and in
 * particular by the rank zero in each node.
 ****************************************************************************/

CpvStaticDeclare(int, CmiReductionMessageHandler);
CpvStaticDeclare(int, _reduce_num_children);
CpvStaticDeclare(int, _reduce_parent);
CpvStaticDeclare(int, _reduce_received);
CpvStaticDeclare(char**, _reduce_msg_list);
CpvStaticDeclare(void*, _reduce_data);
CpvStaticDeclare(int, _reduce_data_size);
static CmiHandler _reduce_destination;
static void * (*_reduce_mergeFn)(void*,void**,int);
static void (*_reduce_pupFn)(void*,void*);
static void (*_reduce_deleteFn)(void*);

CpvStaticDeclare(CmiUInt2, _reduce_seqID);

int CmiGetReductionHandler() { return CpvAccess(CmiReductionMessageHandler); }
CmiHandler CmiGetReductionDestination() { return _reduce_destination; }

CmiReductionsInit() {
  CpvInitialize(int, CmiReductionMessageHandler);
  CpvAccess(CmiReductionMessageHandler) = CmiRegisterHandler((CmiHandler)CmiHandleReductionMessage);
  CpvInitialize(int, _reduce_num_children);
  CpvInitialize(int, _reduce_parent);
  CpvInitialize(int, _reduce_received);
  CpvInitialize(char**, _reduce_msg_list);
  CpvInitialize(void*, _reduce_data);
  CpvInitialize(int, _reduce_data_size);
  CpvAccess(_reduce_num_children) = 0;
  CpvAccess(_reduce_received) = 0;
  CpvAccess(_reduce_msg_list) = (char**)malloc(CmiNumSpanTreeChildren(CmiMyPe())*sizeof(void*));

  CpvInitialize(CmiUInt2, _reduce_seqID);
  CpvAccess(_reduce_seqID) = 0x8000;
}

int CmiReduceNextID() {
  if (CpvAccess(_reduce_seqID) == 0xffff) CpvAccess(_reduce_seqID) = 0x8000;
  return ++CpvAccess(_reduce_seqID);
}

void CmiSendReduce() {
  void *mergedData = CpvAccess(_reduce_data);
  void *msg;
  int msg_size;
  if (CpvAccess(_reduce_num_children) > 0) {
    int i, offset=0;
    if (_reduce_pupFn != NULL) {
      offset = CmiMsgHeaderSizeBytes;
      for (i=0; i<CpvAccess(_reduce_num_children); ++i) CpvAccess(_reduce_msg_list)[i] += offset;
    }
    mergedData = _reduce_mergeFn(CpvAccess(_reduce_data), (void **)CpvAccess(_reduce_msg_list), CpvAccess(_reduce_num_children));
    for (i=0; i<CpvAccess(_reduce_num_children); ++i) CmiFree(CpvAccess(_reduce_msg_list)[i] - offset);
  }
  CpvAccess(_reduce_num_children) = 0;
  CpvAccess(_reduce_received) = 0;
  msg = mergedData;
  msg_size = CpvAccess(_reduce_data_size);
  if (CmiMyPe() != 0) {
    if (_reduce_pupFn != NULL) {
      pup_er p = pup_new_sizer();
      _reduce_pupFn(p, mergedData);
      msg_size = pup_size(p) + CmiMsgHeaderSizeBytes;
      pup_destroy(p);
      msg = CmiAlloc(msg_size);
      p = pup_new_toMem((void*)(((char*)msg)+CmiMsgHeaderSizeBytes));
      _reduce_pupFn(p, mergedData);
      pup_destroy(p);
      if (_reduce_deleteFn != NULL) _reduce_deleteFn(CpvAccess(_reduce_data));
    }
    CmiSetHandler(msg, CpvAccess(CmiReductionMessageHandler));
    /*CmiPrintf("CmiSendReduce(%d): sending %d bytes to %d\n",CmiMyPe(),msg_size,CpvAccess(_reduce_parent));*/
    CmiSyncSendAndFree(CpvAccess(_reduce_parent), msg_size, msg);
  } else {
    _reduce_destination(msg);
  }
}

void CmiReduce(void *data, int size, void * (*mergeFn)(void*,void**,int)) {
  CpvAccess(_reduce_data) = data;
  CpvAccess(_reduce_data_size) = size;
  CpvAccess(_reduce_parent) = CmiSpanTreeParent(CmiMyPe());
  _reduce_destination = (CmiHandler)CmiGetHandlerFunction(data);
  _reduce_pupFn = NULL;
  _reduce_mergeFn = mergeFn;
  CpvAccess(_reduce_num_children) = CmiNumSpanTreeChildren(CmiMyPe());
  if (CpvAccess(_reduce_received) == CpvAccess(_reduce_num_children)) CmiSendReduce();
}

void CmiReduceStruct(void *data, void (*pupFn)(void*,void*),
                     void * (*mergeFn)(void*,void**,int), CmiHandler dest,
                     void (*deleteFn)(void*)) {
  CpvAccess(_reduce_data) = data;
  CpvAccess(_reduce_parent) = CmiSpanTreeParent(CmiMyPe());
  _reduce_destination = dest;
  _reduce_pupFn = pupFn;
  _reduce_mergeFn = mergeFn;
  _reduce_deleteFn = deleteFn;
  CpvAccess(_reduce_num_children) = CmiNumSpanTreeChildren(CmiMyPe());
  if (CpvAccess(_reduce_received) == CpvAccess(_reduce_num_children)) CmiSendReduce();
  /*else CmiPrintf("CmiReduceStruct(%d): %d - %d\n",CmiMyPe(),CpvAccess(_reduce_received),CpvAccess(_reduce_num_children));*/
}

void CmiNodeReduce(void *data, int size, void * (*mergeFn)(void*,void**,int), int redID, int numChildren, int parent) {
  CmiAssert(CmiRankOf(CmiMyPe()) == 0);
  CpvAccess(_reduce_data) = data;
  CpvAccess(_reduce_data_size) = size;
  CpvAccess(_reduce_parent) = CmiNodeFirst(CmiNodeSpanTreeParent(CmiMyNode()));
  _reduce_destination = (CmiHandler)CmiGetHandlerFunction(data);
  _reduce_pupFn = NULL;
  _reduce_mergeFn = mergeFn;
  CpvAccess(_reduce_num_children) = CmiNumNodeSpanTreeChildren(CmiMyNode());
  if (CpvAccess(_reduce_received) == CpvAccess(_reduce_num_children)) CmiSendReduce();
}
/*
//void CmiNodeReduce(void *data, int size, void * (*mergeFn)(void*,void**,int), int redID) {
//  CmiNodeReduce(data, size, mergeFn, redID, CmiNumNodeSpanTreeChildren(CmiMyNode()),
//      CmiNodeFirst(CmiNodeSpanTreeParent(CmiMyNode())));
//}
//void CmiNodeReduce(void *data, int size, void * (*mergeFn)(void*,void**,int), int numChildren, int parent) {
//  CmiNodeReduce(data, size, mergeFn, CmiReduceNextID(), numChildren, parent);
//}
//void CmiNodeReduce(void *data, int size, void * (*mergeFn)(void*,void**,int)) {
//  CmiNodeReduce(data, size, mergeFn, CmiReduceNextID(), CmiNumNodeSpanTreeChildren(CmiMyNode()),
//      CmiNodeFirst(CmiNodeSpanTreeParent(CmiMyNode())));
//}
*/

void CmiNodeReduceStruct(void *data, void (*pupFn)(void*,void*),
                         void * (*mergeFn)(void*,void**,int), CmiHandler dest,
                         void (*deleteFn)(void*)) {
  CmiAssert(CmiRankOf(CmiMyPe()) == 0);
  CpvAccess(_reduce_data) = data;
  CpvAccess(_reduce_parent) = CmiNodeFirst(CmiNodeSpanTreeParent(CmiMyNode()));
  _reduce_destination = dest;
  _reduce_pupFn = pupFn;
  _reduce_mergeFn = mergeFn;
  _reduce_deleteFn = deleteFn;
  CpvAccess(_reduce_num_children) = CmiNumNodeSpanTreeChildren(CmiMyNode());
  if (CpvAccess(_reduce_received) == CpvAccess(_reduce_num_children)) CmiSendReduce();
}

void CmiHandleReductionMessage(void *msg) {
  CpvAccess(_reduce_msg_list)[CpvAccess(_reduce_received)++] = msg;
  if (CpvAccess(_reduce_received) == CpvAccess(_reduce_num_children)) CmiSendReduce();
  /*else CmiPrintf("CmiHandleReductionMessage(%d): %d - %d\n",CmiMyPe(),CpvAccess(_reduce_received),CpvAccess(_reduce_num_children));*/
}

/*****************************************************************************
 *
 * Multicast groups
 *
 ****************************************************************************/

#if CMK_MULTICAST_DEF_USE_COMMON_CODE

typedef struct GroupDef
{
  union {
    char core[CmiMsgHeaderSizeBytes];
    struct GroupDef *next;
  } core;
  CmiGroup group;
  int npes;
  int pes[1];
}
*GroupDef;

#define GROUPTAB_SIZE 101

CpvStaticDeclare(int, CmiGroupHandlerIndex);
CpvStaticDeclare(int, CmiGroupCounter);
CpvStaticDeclare(GroupDef *, CmiGroupTable);

void CmiGroupHandler(GroupDef def)
{
  /* receive group definition, insert into group table */
  GroupDef *table = CpvAccess(CmiGroupTable);
  unsigned int hashval, bucket;
  hashval = (def->group.id ^ def->group.pe);
  bucket = hashval % GROUPTAB_SIZE;
  def->core.next = table[bucket];
  table[bucket] = def;
}

CmiGroup CmiEstablishGroup(int npes, int *pes)
{
  /* build new group definition, broadcast it */
  CmiGroup grp; GroupDef def; int len, i;
  grp.id = CpvAccess(CmiGroupCounter)++;
  grp.pe = CmiMyPe();
  len = sizeof(struct GroupDef)+(npes*sizeof(int));
  def = (GroupDef)CmiAlloc(len);
  def->group = grp;
  def->npes = npes;
  for (i=0; i<npes; i++)
    def->pes[i] = pes[i];
  CmiSetHandler(def, CpvAccess(CmiGroupHandlerIndex));
  CmiSyncBroadcastAllAndFree(len, def);
  return grp;
}

void CmiLookupGroup(CmiGroup grp, int *npes, int **pes)
{
  unsigned int hashval, bucket;  GroupDef def;
  GroupDef *table = CpvAccess(CmiGroupTable);
  hashval = (grp.id ^ grp.pe);
  bucket = hashval % GROUPTAB_SIZE;
  for (def=table[bucket]; def; def=def->core.next) {
    if ((def->group.id == grp.id)&&(def->group.pe == grp.pe)) {
      *npes = def->npes;
      *pes = def->pes;
      return;
    }
  }
  *npes = 0; *pes = 0;
}

void CmiGroupInit()
{
  CpvInitialize(int, CmiGroupHandlerIndex);
  CpvInitialize(int, CmiGroupCounter);
  CpvInitialize(GroupDef *, CmiGroupTable);
  CpvAccess(CmiGroupHandlerIndex) = CmiRegisterHandler((CmiHandler)CmiGroupHandler);
  CpvAccess(CmiGroupCounter) = 0;
  CpvAccess(CmiGroupTable) =
    (GroupDef*)calloc(GROUPTAB_SIZE, sizeof(GroupDef));
  if (CpvAccess(CmiGroupTable) == 0)
    CmiAbort("Memory Allocation Error");
}

#endif

/*****************************************************************************
 *
 * Common List-Cast and Multicast Code
 *
 ****************************************************************************/

#if CMK_MULTICAST_LIST_USE_COMMON_CODE

void CmiSyncListSendFn(int npes, int *pes, int len, char *msg)
{
  int i;
  for(i=0;i<npes;i++) {
    CmiSyncSend(pes[i], len, msg);
  }
}

CmiCommHandle CmiAsyncListSendFn(int npes, int *pes, int len, char *msg)
{
  /* A better asynchronous implementation may be wanted, but at least it works */
  CmiSyncListSendFn(npes, pes, len, msg);
  return (CmiCommHandle) 0;
}

void CmiFreeListSendFn(int npes, int *pes, int len, char *msg)
{
  int i;
  for(i=0;i<npes-1;i++) {
    CmiSyncSend(pes[i], len, msg);
  }
  if (npes)
    CmiSyncSendAndFree(pes[npes-1], len, msg);
  else 
    CmiFree(msg);
}

#endif

#if CMK_MULTICAST_GROUP_USE_COMMON_CODE

typedef struct MultiMsg
{
  char core[CmiMsgHeaderSizeBytes];
  CmiGroup group;
  int pos;
  int origlen;
}
*MultiMsg;

CpvDeclare(int, CmiMulticastHandlerIndex);

void CmiMulticastDeliver(MultiMsg msg)
{
  int npes, *pes; int olen, nlen, pos, child1, child2;
  olen = msg->origlen;
  nlen = olen + sizeof(struct MultiMsg);
  CmiLookupGroup(msg->group, &npes, &pes);
  if (pes==0) {
    CmiSyncSendAndFree(CmiMyPe(), nlen, msg);
    return;
  }
  if (npes==0) {
    CmiFree(msg);
    return;
  }
  if (msg->pos == -1) {
    msg->pos=0;
    CmiSyncSendAndFree(pes[0], nlen, msg);
    return;
  }
  pos = msg->pos;
  child1 = ((pos+1)<<1);
  child2 = child1-1;
  if (child1 < npes) {
    msg->pos = child1;
    CmiSyncSend(pes[child1], nlen, msg);
  }
  if (child2 < npes) {
    msg->pos = child2;
    CmiSyncSend(pes[child2], nlen, msg);
  }
  if(olen < sizeof(struct MultiMsg)) {
    memcpy(msg, msg+1, olen);
  } else {
    memcpy(msg, (((char*)msg)+olen), sizeof(struct MultiMsg));
  }
  CmiSyncSendAndFree(CmiMyPe(), olen, msg);
}

void CmiMulticastHandler(MultiMsg msg)
{
  CmiMulticastDeliver(msg);
}

void CmiSyncMulticastFn(CmiGroup grp, int len, char *msg)
{
  int newlen; MultiMsg newmsg;
  newlen = len + sizeof(struct MultiMsg);
  newmsg = (MultiMsg)CmiAlloc(newlen);
  if(len < sizeof(struct MultiMsg)) {
    memcpy(newmsg+1, msg, len);
  } else {
    memcpy(newmsg+1, msg+sizeof(struct MultiMsg), len-sizeof(struct MultiMsg));
    memcpy(((char *)newmsg+len), msg, sizeof(struct MultiMsg));
  }
  newmsg->group = grp;
  newmsg->origlen = len;
  newmsg->pos = -1;
  CmiSetHandler(newmsg, CpvAccess(CmiMulticastHandlerIndex));
  CmiMulticastDeliver(newmsg);
}

void CmiFreeMulticastFn(CmiGroup grp, int len, char *msg)
{
  CmiSyncMulticastFn(grp, len, msg);
  CmiFree(msg);
}

CmiCommHandle CmiAsyncMulticastFn(CmiGroup grp, int len, char *msg)
{
  CmiError("Async Multicast not implemented.");
  return (CmiCommHandle) 0;
}

void CmiMulticastInit()
{
  CpvInitialize(int, CmiMulticastHandlerIndex);
  CpvAccess(CmiMulticastHandlerIndex) =
    CmiRegisterHandler((CmiHandler)CmiMulticastHandler);
}

#endif

/***************************************************************************
 *
 * Memory Allocation routines 
 *
 * A block of memory can consist of multiple chunks.  Each chunk has
 * a sizefield and a refcount.  The first chunk's refcount is a reference
 * count.  That's how many CmiFrees it takes to free the message.
 * Subsequent chunks have a refcount which is less than zero.  This is
 * the offset back to the start of the first chunk.
 *
 * Each chunk has a CmiChunkHeader before the user data, with the fields:
 *
 *  size: The user-allocated size of the chunk, in bytes.
 *
 *  ref: A magic reference count object. Ordinary blocks start with
 *     reference count 1.  When the reference count reaches zero,
 *     the block is deleted.  To support nested buffers, the 
 *     reference count can also be negative, which means it is 
 *     a byte offset to the enclosing buffer's reference count.
 *
 ***************************************************************************/


void *CmiAlloc(int size)
{
  char *res;

#if CONVERSE_VERSION_ELAN
  res = (char *) elan_CmiAlloc(size+sizeof(CmiChunkHeader));
#elif CONVERSE_VERSION_VMI
  res = (char *) CMI_VMI_CmiAlloc(size+sizeof(CmiChunkHeader));
#elif CMK_USE_IBVERBS
	res = (char *) infi_CmiAlloc(size+sizeof(CmiChunkHeader));
#elif CONVERSE_POOL
  res =(char *) CmiPoolAlloc(size+sizeof(CmiChunkHeader));
#else
  res =(char *) malloc_nomigrate(size+sizeof(CmiChunkHeader));
#endif

  _MEMCHECK(res);

#ifdef MEMMONITOR
  CpvAccess(MemoryUsage) += size+sizeof(CmiChunkHeader);
  CpvAccess(AllocCount)++;
  CpvAccess(BlocksAllocated)++;
  if (CpvAccess(MemoryUsage) > CpvAccess(HiWaterMark)) {
    CpvAccess(HiWaterMark) = CpvAccess(MemoryUsage);
  }
  if (CpvAccess(MemoryUsage) > 1.1 * CpvAccess(ReportedHiWaterMark)) {
    CmiPrintf("HIMEM STAT PE%d: %d Allocs, %d blocks, %lu K, Max %lu K\n",
	    CmiMyPe(), CpvAccess(AllocCount), CpvAccess(BlocksAllocated),
            CpvAccess(MemoryUsage)/1024, CpvAccess(HiWaterMark)/1024);
    CpvAccess(ReportedHiWaterMark) = CpvAccess(MemoryUsage);
  }
  if ((CpvAccess(AllocCount) % 1000) == 0) {
    CmiPrintf("MEM STAT PE%d: %d Allocs, %d blocks, %lu K, Max %lu K\n",
	    CmiMyPe(), CpvAccess(AllocCount), CpvAccess(BlocksAllocated),
            CpvAccess(MemoryUsage)/1024, CpvAccess(HiWaterMark)/1024);
  }
#endif

  res+=sizeof(CmiChunkHeader);
  SIZEFIELD(res)=size;
  REFFIELD(res)=1;
  return (void *)res;
}

/** Follow the header links out to the most enclosing block */
static void *CmiAllocFindEnclosing(void *blk) {
  int refCount = REFFIELD(blk);
  while (refCount < 0) {
    blk = (void *)((char*)blk+refCount); /* Jump to enclosing block */
    refCount = REFFIELD(blk);
  }
  return blk;
}

/** Increment the reference count for this block's owner.
    This call must be matched by an equivalent CmiFree. */
void CmiReference(void *blk)
{
  REFFIELD(CmiAllocFindEnclosing(blk))++;
}

/** Return the size of the user portion of this block. */
int CmiSize(void *blk)
{
  return SIZEFIELD(blk);
}

/** Decrement the reference count for this block. */
void CmiFree(void *blk)
{
  void *parentBlk=CmiAllocFindEnclosing(blk);
  int refCount=REFFIELD(parentBlk);
#ifndef CMK_OPTIMIZE
  if(refCount==0) /* Logic error: reference count shouldn't already have been zero */
    CmiAbort("CmiFree reference count was zero-- is this a duplicate free?");
#endif
  refCount--;
  REFFIELD(parentBlk) = refCount;
  if(refCount==0) { /* This was the last reference to the block-- free it */
#ifdef MEMMONITOR
    int size=SIZEFIELD(parentBlk);
    if (size > 1000000000) /* Absurdly large size field-- warning */
      CmiPrintf("MEMSTAT Uh-oh -- SIZEFIELD=%d\n",size);
    CpvAccess(MemoryUsage) -= (size + sizeof(CmiChunkHeader);
    CpvAccess(BlocksAllocated)--;
#endif

#if CONVERSE_VERSION_ELAN
    elan_CmiFree(BLKSTART(parentBlk));
#elif CONVERSE_VERSION_VMI
    CMI_VMI_CmiFree(BLKSTART(parentBlk));
#elif CMK_USE_IBVERBS
		infi_CmiFree(BLKSTART(parentBlk));
#elif CONVERSE_POOL
    CmiPoolFree(BLKSTART(parentBlk));
#else
    free_nomigrate(BLKSTART(parentBlk));
#endif
  }
}


/***************************************************************************
 *
 * Temporary-memory Allocation routines 
 *
 *  This buffer augments the storage available on the regular machine stack
 * for fairly large temporary buffers, which allows us to use smaller machine
 * stacks.
 *
 ***************************************************************************/

#define CMI_TMP_BUF_MAX 128*1024 /* Allow this much temporary storage. */

typedef struct {
  char *buf; /* Start of temporary buffer */
  int cur; /* First unused location in temporary buffer */
  int max; /* Length of temporary buffer */
} CmiTmpBuf_t;
CpvDeclare(CmiTmpBuf_t,CmiTmpBuf); /* One temporary buffer per PE */

static void CmiTmpSetup(CmiTmpBuf_t *b) {
  b->buf=malloc(CMI_TMP_BUF_MAX);
  b->cur=0;
  b->max=CMI_TMP_BUF_MAX;
}

void *CmiTmpAlloc(int size) {
  if (!CpvInitialized(CmiTmpBuf)) {
    return malloc(size);
  }
  else { /* regular case */
    CmiTmpBuf_t *b=&CpvAccess(CmiTmpBuf);
    void *t;
    if (b->cur+size>b->max) {
      if (b->max==0) /* We're just uninitialized */
        CmiTmpSetup(b);
      else /* We're really out of space! */
        CmiAbort("CmiTmpAlloc: asked for too much temporary buffer space");
    }
    t=b->buf+b->cur;
    b->cur+=size;
    return t;
  }
}
void CmiTmpFree(void *t) {
  if (!CpvInitialized(CmiTmpBuf)) {
    free(t);
  }
  else { /* regular case */
    CmiTmpBuf_t *b=&CpvAccess(CmiTmpBuf);
    /* t should point into our temporary buffer: figure out where */
    int cur=((const char *)t)-b->buf;
#ifndef CMK_OPTIMIZE
    if (cur<0 || cur>b->max)
      CmiAbort("CmiTmpFree: called with an invalid pointer");
#endif
    b->cur=cur;
  }
}

void CmiTmpInit(char **argv) {
  CpvInitialize(CmiTmpBuf_t,CmiTmpBuf);
  /* Set up this processor's temporary buffer */
  CmiTmpSetup(&CpvAccess(CmiTmpBuf));
}

/******************************************************************************

  Cross-platform directory creation

  ****************************************************************************/
#ifdef _MSC_VER
/* Windows directory creation: */
#include <windows.h>

void CmiMkdir(const char *dirName) {
	CreateDirectory(dirName,NULL);
}

#else /* !_MSC_VER */
/* UNIX directory creation */
#include <unistd.h> 
#include <sys/stat.h> /* from "mkdir" man page */
#include <sys/types.h>

void CmiMkdir(const char *dirName) {
	mkdir(dirName,0777);
}

#endif


/******************************************************************************

  Multiple Send function                               

  ****************************************************************************/

CpvDeclare(int, CmiMainHandlerIDP); /* Main handler that is run on every node */

/****************************************************************************
* DESCRIPTION : This function call allows the user to send multiple messages
*               from one processor to another, all intended for differnet 
*	        handlers.
*
*	        Parameters :
*
*	        destPE, len, int sizes[0..len-1], char *messages[0..len-1]
*
****************************************************************************/
/* Round up message size to the message granularity. 
   Does this by adding, then truncating.
*/
static int roundUpSize(unsigned int s) {
  return (int)((s+sizeof(double)-1)&~(sizeof(double)-1));
}
/* Return the amount of message padding required for a message
   with this many user bytes. 
 */
static int paddingSize(unsigned int s) {
  return roundUpSize(s)-s;
}

/* Message header for a bundle of multiple-sent messages */
typedef struct {
  char convHeader[CmiMsgHeaderSizeBytes];
  int nMessages; /* Number of distinct messages bundled below. */
  double pad; /* To align the first message, which follows this header */
} CmiMultipleSendHeader;

static void _CmiMultipleSend(unsigned int destPE, int len, int sizes[], char *msgComps[], int immed)
{
  CmiMultipleSendHeader header;
  int m; /* Outgoing message */
  CmiChunkHeader *msgHdr; /* Chunk headers for each message */
  double pad = 0; /* padding required */
  int vecLen; /* Number of pieces in outgoing message vector */
  int *vecSizes; /* Sizes of each piece we're sending out. */
  char **vecPtrs; /* Pointers to each piece we're sending out. */
  int vec; /* Entry we're currently filling out in above array */

  msgHdr = (CmiChunkHeader *)CmiTmpAlloc(len * sizeof(CmiChunkHeader));
  /* Allocate memory for the outgoing vector*/
  vecLen=1+3*len; /* Header and 3 parts per message */
  vecSizes = (int *)CmiTmpAlloc(vecLen * sizeof(int));
  vecPtrs = (char **)CmiTmpAlloc(vecLen * sizeof(char *));
  vec=0;
  
  /* Build the header */
  header.nMessages=len;
  CmiSetHandler(&header, CpvAccess(CmiMainHandlerIDP));
#if CMK_IMMEDIATE_MSG
  if (immed) CmiBecomeImmediate(&header);
#endif
  vecSizes[vec]=sizeof(header); vecPtrs[vec]=(char *)&header;
  vec++;

  /* Build an entry for each message: 
         | CmiChunkHeader | Message data | Message padding | ...next message entry ...
  */
  for (m=0;m<len;m++) {
    msgHdr[m].size=roundUpSize(sizes[m]); /* Size of message and padding */
    msgHdr[m].ref=0; /* Reference count will be filled out on receive side */
    
    /* First send the message's CmiChunkHeader (for use on receive side) */
    vecSizes[vec]=sizeof(CmiChunkHeader); vecPtrs[vec]=(char *)&msgHdr[m];
    vec++;
    
    /* Now send the actual message data */
    vecSizes[vec]=sizes[m]; vecPtrs[vec]=msgComps[m];
    vec++;
    
    /* Now send padding to align the next message on a double-boundary */
    vecSizes[vec]=paddingSize(sizes[m]); vecPtrs[vec]=(char *)&pad;
    vec++;
  }
  CmiAssert(vec==vecLen);
  
  CmiSyncVectorSend(destPE, vecLen, vecSizes, vecPtrs);
  
  CmiTmpFree(vecPtrs); /* CmiTmp: Be sure to throw away in opposite order of allocation */
  CmiTmpFree(vecSizes);
  CmiTmpFree(msgHdr);
}

void CmiMultipleSend(unsigned int destPE, int len, int sizes[], char *msgComps[])
{
  _CmiMultipleSend(destPE, len, sizes, msgComps, 0);
}

void CmiMultipleIsend(unsigned int destPE, int len, int sizes[], char *msgComps[])
{
  _CmiMultipleSend(destPE, len, sizes, msgComps, 1);
}

/****************************************************************************
* DESCRIPTION : This function initializes the main handler required for the
*               CmiMultipleSend() function to work. 
*	        
*               This function should be called once in any Converse program
*	        that uses CmiMultipleSend()
*
****************************************************************************/

static void CmiMultiMsgHandler(char *msgWhole);

void CmiInitMultipleSend(void)
{
  CpvInitialize(int,CmiMainHandlerIDP); 
  CpvAccess(CmiMainHandlerIDP) =
    CmiRegisterHandler((CmiHandler)CmiMultiMsgHandler);
}

/****************************************************************************
* DESCRIPTION : This function is the main handler that splits up the messages
*               CmiMultipleSend() pastes together. 
*
****************************************************************************/

static void CmiMultiMsgHandler(char *msgWhole)
{
  int len=((CmiMultipleSendHeader *)msgWhole)->nMessages;
  int offset=sizeof(CmiMultipleSendHeader);
  int m;
  for (m=0;m<len;m++) {
    CmiChunkHeader *ch=(CmiChunkHeader *)(msgWhole+offset);
    char *msg=(msgWhole+offset+sizeof(CmiChunkHeader));
    int msgSize=ch->size; /* Size of user portion of message (plus padding at end) */
    /* Link new message to owner via a negative ref pointer */
    ch->ref=msgWhole-msg; 
    CmiReference(msg); /* Follows link & increases reference count of *msgWhole* */
    CmiSyncSendAndFree(CmiMyPe(), msgSize, msg);
    offset+= sizeof(CmiChunkHeader) + msgSize;
  }
  /* Release our reference to the whole message.  The message will
     only actually be deleted once all its sub-messages are free'd as well. */
  CmiFree(msgWhole);
}

/****************************************************************************
* Hypercube broadcast message passing.
****************************************************************************/

int HypercubeGetBcastDestinations(int mype, int total_pes, int k, int *dest_pes) {
  int num_pes = 0;
  for ( ; k>=0; --k) {
    /* add the processor destination at level k if it exist */
    dest_pes[num_pes] = mype ^ (1<<k);
    if (dest_pes[num_pes] >= total_pes) {
      /* find the first proc in the other part of the current dimention */
      dest_pes[num_pes] &= (-1)<<k;
      /* if the first proc there is over CmiNumPes() then there is no other
      	 dimension, otherwise if it is valid compute my correspondent in such
      	 a way to minimize the load for every processor */
      if (total_pes>dest_pes[num_pes]) dest_pes[num_pes] += (mype - (mype & ((-1)<<k))) % (total_pes - dest_pes[num_pes]);
      }
    if (dest_pes[num_pes] < total_pes) {
      /* if the destination is in the acceptable range increment num_pes */
      ++num_pes;
    }
  }
  return num_pes;
}


/****************************************************************************
* DESCRIPTION : This function initializes the main handler required for the
*               Immediate message
*	        
*               This function should be called once in any Converse program
*
****************************************************************************/

int _immediateLock = 0; /* if locked, all immediate message handling will be delayed. */
int _immediateFlag = 0; /* if set, there is delayed immediate message. */

CpvDeclare(int, CmiImmediateMsgHandlerIdx); /* Main handler that is run on every node */

/* xdl is the real handler */
static void CmiImmediateMsgHandler(char *msg)
{
  CmiSetHandler(msg, CmiGetXHandler(msg));
  CmiHandleMessage(msg);
}

void CmiInitImmediateMsg(void)
{
  CpvInitialize(int,CmiImmediateMsgHandlerIdx); 
  CpvAccess(CmiImmediateMsgHandlerIdx) =
    CmiRegisterHandler((CmiHandler)CmiImmediateMsgHandler);
}

/*#if !CMK_IMMEDIATE_MSG
#if !CMK_MACHINE_PROGRESS_DEFINED
void CmiProbeImmediateMsg()
{
}
#endif
#endif*/

/******** Idle timeout module (+idletimeout=30) *********/

typedef struct {
  int idle_timeout;/*Milliseconds to wait idle before aborting*/
  int is_idle;/*Boolean currently-idle flag*/
  int call_count;/*Number of timeout calls currently in flight*/
} cmi_cpu_idlerec;

static void on_timeout(cmi_cpu_idlerec *rec,double curWallTime)
{
  rec->call_count--;
  if(rec->call_count==0 && rec->is_idle==1) {
    CmiError("Idle time on PE %d exceeded specified timeout.\n", CmiMyPe());
    CmiAbort("Exiting.\n");
  }
}
static void on_idle(cmi_cpu_idlerec *rec,double curWallTime)
{
  CcdCallFnAfter((CcdVoidFn)on_timeout, rec, rec->idle_timeout);
  rec->call_count++; /*Keeps track of overlapping timeout calls.*/  
  rec->is_idle = 1;
}
static void on_busy(cmi_cpu_idlerec *rec,double curWallTime)
{
  rec->is_idle = 0;
}
static void CIdleTimeoutInit(char **argv)
{
  int idle_timeout=0; /*Seconds to wait*/
  CmiGetArgIntDesc(argv,"+idle-timeout",&idle_timeout,"Abort if idle for this many seconds");
  if(idle_timeout != 0) {
    cmi_cpu_idlerec *rec=(cmi_cpu_idlerec *)malloc(sizeof(cmi_cpu_idlerec));
    _MEMCHECK(rec);
    rec->idle_timeout=idle_timeout*1000;
    rec->is_idle=0;
    rec->call_count=0;
    CcdCallOnCondition(CcdPROCESSOR_BEGIN_IDLE, (CcdVoidFn)on_idle, rec);
    CcdCallOnCondition(CcdPROCESSOR_BEGIN_BUSY, (CcdVoidFn)on_busy, rec);
  }
}

/*****************************************************************************
 *
 * Converse Initialization
 *
 *****************************************************************************/

extern void CrnInit(void);
extern void CmiIsomallocInit(char **argv);
#if ! CMK_CMIPRINTF_IS_A_BUILTIN
void CmiIOInit(char **argv);
#endif

static void CmiProcessPriority(char **argv)
{
  int dummy, nicelevel=-100;      /* process priority */
  CmiGetArgIntDesc(argv,"+nice",&nicelevel,"Set the process priority level");
  /* ignore others */
  while (CmiGetArgIntDesc(argv,"+nice",&dummy,"Set the process priority level"));
  /* call setpriority once on each process to set process's priority */
  if (CmiMyRank() == 0 && nicelevel != -100)  {
#ifndef _WIN32
    if (0!=setpriority(PRIO_PROCESS, 0, nicelevel))  {
      CmiPrintf("[%d] setpriority failed with value %d. \n", CmiMyPe(), nicelevel);
      perror("setpriority");
      CmiAbort("setpriority failed.");
    }
    else
      CmiPrintf("[%d] Charm++: setpriority %d\n", CmiMyPe(), nicelevel);
#else
    HANDLE hProcess = GetCurrentProcess();
    DWORD dwPriorityClass = NORMAL_PRIORITY_CLASS;
    char *prio_str = "NORMAL_PRIORITY_CLASS";
    BOOL status;
    /*
       <-20:      real time
       -20--10:   high 
       -10-0:     above normal
       0:         normal
       0-10:      below normal
       10-:       idle
    */
    if (0) ;
#ifdef BELOW_NORMAL_PRIORITY_CLASS
    else if (nicelevel<10 && nicelevel>0) {
      dwPriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
      prio_str = "BELOW_NORMAL_PRIORITY_CLASS";
    }
#endif
    else if (nicelevel>0) {
      dwPriorityClass = IDLE_PRIORITY_CLASS;
      prio_str = "IDLE_PRIORITY_CLASS";
    }
    else if (nicelevel<=-20) {
      dwPriorityClass = REALTIME_PRIORITY_CLASS;
      prio_str = "REALTIME_PRIORITY_CLASS";
    }
#ifdef ABOVE_NORMAL_PRIORITY_CLASS
    else if (nicelevel>-10 && nicelevel<0) {
      dwPriorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
      prio_str = "ABOVE_NORMAL_PRIORITY_CLASS";
    }
#endif
    else if (nicelevel<0) {
      dwPriorityClass = HIGH_PRIORITY_CLASS;
      prio_str = "HIGH_PRIORITY_CLASS";
    }
    status = SetPriorityClass(hProcess, dwPriorityClass);
    if (!status)  {
        int err=GetLastError();
        CmiPrintf("SetPriorityClass failed errno=%d, WSAerr=%d\n",errno, err);
        CmiAbort("SetPriorityClass failed.");
    }
    else
      CmiPrintf("[%d] Charm++: setpriority %s\n", CmiMyPe(), prio_str);
#endif
  }
}

void CommunicationServerInit()
{
#if CMK_IMMEDIATE_MSG
  CQdCpvInit();
  CpvInitialize(int,CmiImmediateMsgHandlerIdx); 
#endif
}

/**
  Main Converse initialization routine.  This routine is 
  called by the machine file (machine.c) to set up Converse.
  It's "Common" because it's shared by all the machine.c files. 
  
  The main task of this routine is to set up all the Cpv's
  (message queues, handler tables, etc.) used during main execution.
  
  On SMP versions, this initialization routine is called by 
  *all* processors of a node simultaniously.  It's *also* called
  by the communication thread, which is rather strange but needed
  for immediate messages.  Each call to this routine expects a 
  different copy of the argv arguments, so use CmiCopyArgs(argv).
  
  Requires:
    - A working network layer.
    - Working Cpv's and CmiNodeBarrier.
    - CthInit to already have been called.  CthInit is called
      from the machine layer directly, because some machine layers
      (like uth) use Converse threads internally.

  Initialization is somewhat subtle, in that various modules
  won't work properly until they're initialized.  For example,
  nobody can register handlers before calling CmiHandlerInit.
*/
void ConverseCommonInit(char **argv)
{
  CmiArgInit(argv);
  CmiMemoryInit(argv);
#if ! CMK_CMIPRINTF_IS_A_BUILTIN
  CmiIOInit(argv);
#endif
#if CONVERSE_POOL
  CmiPoolAllocInit(30);  
#endif
  CmiTmpInit(argv);
  CmiTimerInit();
  CstatsInit(argv);

  CcdModuleInit(argv);
  CmiHandlerInit();
  CmiReductionsInit();
  CIdleTimeoutInit(argv);
  
#if CMK_SHARED_VARS_POSIX_THREADS_SMP /*Used by the net-*-smp versions*/
	if(CmiMyRank() == 0){
		_Cmi_noprocforcommthread=0;
		if(CmiGetArgFlagDesc(argv,"+CmiNoProcForComThread","Is there an extra processor for the communication thread on each node(only for net-smp-*) ?")){
			_Cmi_noprocforcommthread=1;
		}
	}
#endif
	
#ifndef CMK_OPTIMIZE
  traceInit(argv);
/*initTraceCore(argv);*/ /* projector */
#endif
  CmiProcessPriority(argv);

#if CMK_CCS_AVAILABLE
  CcsInit(argv);
#endif
  CmiPersistentInit();
  CmiIsomallocInit(argv);
  CpdInit();
  CmiDeliversInit();
  CsdInit(argv);
  CthSchedInit();
  CmiGroupInit();
  CmiMulticastInit();
  CmiInitMultipleSend();
  CQdInit();

  CrnInit();
  CmiInitImmediateMsg();
  CldModuleInit(argv);
  
#if CMK_CELL
  void CmiInitCell();
  CmiInitCell();
#endif
  /* main thread is suspendable */
/*
  CthSetSuspendable(CthSelf(), 0);
*/

  CmiInitCPUAffinity(argv);

#if CMK_BLUEGENE_CHARM
   /* have to initialize QD here instead of _initCharm */
  extern void initQd();
  initQd();
#endif
}

void ConverseCommonExit(void)
{
  CcsImpl_kill();

#ifndef CMK_OPTIMIZE
  traceClose();
/*closeTraceCore();*/ /* projector */
#endif

#if CMI_IO_BUFFER_EXPLICIT
  CmiFlush(stdout);  /* end of program, always flush */
#endif

#if CMK_CELL
  CloseOffloadAPI();
#endif
}


#if CMK_CELL

void CmiInitCell()
{
  // Create a unique string for each PPE to use for the timing
  //   data file's name
  char fileNameBuf[128];
  sprintf(fileNameBuf, "speTiming.%d", CmiMyPe());

  InitOffloadAPI(offloadCallback, NULL, NULL, fileNameBuf);
  //CcdCallOnConditionKeep(CcdPERIODIC, 
  //      (CcdVoidFn) OffloadAPIProgress, NULL);
  CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE,
      (CcdVoidFn) OffloadAPIProgress, NULL);
}

#include "cell-api.c"

#endif

/****
 * CW Lee - 9/14/2005
 * Added a mechanism to allow some control over machines with extremely
 * inefficient terminal IO mechanisms. Case in point: the XT3 has a
 * 20ms flush overhead along with about 25MB/s bandwidth for IO. This,
 * coupled with a default setup using unbuffered stdout introduced
 * severe overheads (and hence limiting scaling) for applications like 
 * NAMD.
 */
#if ! CMK_CMIPRINTF_IS_A_BUILTIN
void CmiIOInit(char **argv) {
  CpvInitialize(int, expIOFlushFlag);
#if CMI_IO_BUFFER_EXPLICIT
  /* 
     Support for an explicit buffer only makes sense if the machine
     layer does not wish to make its own implementation.

     Placing this after CmiMemoryInit() means that CmiMemoryInit()
     MUST NOT make use of stdout if an explicit buffer is requested.

     The setvbuf function may only be used after opening a stream and
     before any other operations have been performed on it
  */
  CpvInitialize(char*, explicitIOBuffer);
  CpvInitialize(int, expIOBufferSize);
  if (!CmiGetArgIntDesc(argv,"+io_buffer_size", &CpvAccess(expIOBufferSize),
			"Explicit IO Buffer Size")) {
    CpvAccess(expIOBufferSize) = DEFAULT_IO_BUFFER_SIZE;
  }
  if (CpvAccess(expIOBufferSize) <= 0) {
    CpvAccess(expIOBufferSize) = DEFAULT_IO_BUFFER_SIZE;
  }
  CpvAccess(explicitIOBuffer) = (char*)CmiAlloc(CpvAccess(expIOBufferSize)*
						sizeof(char));
  if (setvbuf(stdout, CpvAccess(explicitIOBuffer), _IOFBF, 
	      CpvAccess(expIOBufferSize))) {
    CmiAbort("Explicit IO Buffering failed\n");
  }
#endif
#if CMI_IO_FLUSH_USER
  /* system default to have user control flushing of IO */
  /* Now look for user override */
  CpvAccess(expIOFlushFlag) = !CmiGetArgFlagDesc(argv,"+io_flush_system",
						 "System Controls IO Flush");
#else
  /* system default to have system handle IO flushing */
  /* Now look for user override */
  CpvAccess(expIOFlushFlag) = CmiGetArgFlagDesc(argv,"+io_flush_user",
						"User Controls IO Flush");
#endif
}
#endif

#if ! CMK_CMIPRINTF_IS_A_BUILTIN

void CmiPrintf(const char *format, ...)
{
  va_list args;
  va_start(args,format);
  vfprintf(stdout,format, args);
  if (CpvInitialized(expIOFlushFlag) && !CpvAccess(expIOFlushFlag)) {
    CmiFlush(stdout);
  }
  va_end(args);
}

void CmiError(const char *format, ...)
{
  va_list args;
  va_start(args,format);
  vfprintf(stderr,format, args);
  CmiFlush(stderr);  /* stderr is always flushed */
  va_end(args);
}

#endif

void __cmi_assert(const char *expr, const char *file, int line)
{
  CmiError("[%d] Assertion \"%s\" failed in file %s line %d.\n",
      CmiMyPe(), expr, file, line);
  CmiAbort("");
}

char *CmiCopyMsg(char *msg, int len)
{
  char *copy = (char *)CmiAlloc(len);
  _MEMCHECK(copy);
  memcpy(copy, msg, len);
  return copy;
}

unsigned char computeCheckSum(unsigned char *data, int len)
{
  int i;
  unsigned char ret = 0;
  for (i=0; i<len; i++) ret ^= (unsigned char)data[i];
  return ret;
}

/*@}*/
