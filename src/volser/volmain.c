/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>
#include <afs/opr.h>
#ifdef AFS_PTHREAD_ENV
# include <opr/lock.h>
#endif

#ifdef AFS_NT40_ENV
#include <windows.h>
#include <WINNT/afsevent.h>
#endif

#include <rx/rx_queue.h>
#include <afs/afsint.h>
#include <afs/prs_fs.h>
#include <afs/nfs.h>
#include <lwp.h>
#include <lock.h>
#include <afs/afssyscalls.h>
#include <afs/ihandle.h>
#ifdef AFS_NT40_ENV
#include <afs/ntops.h>
#endif
#include <afs/vnode.h>
#include <afs/volume.h>
#include <afs/partition.h>
#include <rx/rx.h>
#include <rx/rxstat.h>
#include <rx/rx_globals.h>
#include <afs/auth.h>
#include <afs/cellconfig.h>
#include <afs/keys.h>
#include <afs/dir.h>
#include <ubik.h>
#include <afs/audit.h>
#include <afs/afsutil.h>
#include <afs/cmd.h>
#include <lwp.h>

#include "volser.h"
#include "volint.h"
#include "volser_internal.h"

#define VolserVersion "2.0"
#define N_SECURITY_OBJECTS 3

extern struct Lock localLock;
char *GlobalNameHack = NULL;
int hackIsIn = 0;
afs_int32 GlobalVolCloneId, GlobalVolParentId;
int GlobalVolType;
int VolumeChanged;		/* XXXX */
static char busyFlags[MAXHELPERS];
struct volser_trans *QI_GlobalWriteTrans = 0;
struct afsconf_dir *tdir;
static afs_int32 runningCalls = 0;
int DoLogging = 0;
int debuglevel = 0;
#define MAXLWP 128
int lwps = 9;
int udpBufSize = 0;		/* UDP buffer size for receive */

int rxBind = 0;
int rxkadDisableDotCheck = 0;
int DoPreserveVolumeStats = 0;
int rxJumbograms = 0;	/* default is to not send and receive jumbograms. */
int rxMaxMTU = -1;
char *auditFileName = NULL;

#define ADDRSPERSITE 16         /* Same global is in rx/rx_user.c */
afs_uint32 SHostAddrs[ADDRSPERSITE];

#define VS_EXIT(code)  {                                          \
                          osi_audit(VS_ExitEvent, code, AUD_END); \
			  exit(code);                             \
		       }

static void
MyBeforeProc(struct rx_call *acall)
{
    VTRANS_LOCK;
    runningCalls++;
    VTRANS_UNLOCK;
    return;
}

static void
MyAfterProc(struct rx_call *acall, afs_int32 code)
{
    VTRANS_LOCK;
    runningCalls--;
    VTRANS_UNLOCK;
    return;
}

/* Called every GCWAKEUP seconds to try to unlock all our partitions,
 * if we're idle and there are no active transactions
 */
static void
TryUnlock(void)
{
    /* if there are no running calls, and there are no active transactions, then
     * it should be safe to release any partition locks we've accumulated */
    VTRANS_LOCK;
    if (runningCalls == 0 && TransList() == (struct volser_trans *)0) {
        VTRANS_UNLOCK;
	VPFullUnlock();		/* in volprocs.c */
    } else
        VTRANS_UNLOCK;
}

/* background daemon for timing out transactions */
static void*
BKGLoop(void *unused)
{
    struct timeval tv;
    int loop = 0;

    afs_pthread_setname_self("vol bkg");
    while (1) {
	tv.tv_sec = GCWAKEUP;
	tv.tv_usec = 0;
#ifdef AFS_PTHREAD_ENV
#ifdef AFS_NT40_ENV
        Sleep(GCWAKEUP * 1000);
#else
        select(0, 0, 0, 0, &tv);
#endif
#else
	(void)IOMGR_Select(0, 0, 0, 0, &tv);
#endif
	GCTrans();
	TryUnlock();
	loop++;
	if (loop == 10) {	/* reopen log every 5 minutes */
	    loop = 0;
	    ReOpenLog(AFSDIR_SERVER_VOLSERLOG_FILEPATH);
	}
    }

    return NULL;
}

/* Background daemon for sleeping so the volserver does not become I/O bound */
afs_int32 TTsleep, TTrun;
#ifndef AFS_PTHREAD_ENV
static void *
BKGSleep(void *unused)
{
    struct volser_trans *tt;

    if (TTsleep) {
	while (1) {
#ifdef AFS_PTHREAD_ENV
	    sleep(TTrun);
#else /* AFS_PTHREAD_ENV */
	    IOMGR_Sleep(TTrun);
#endif
	    VTRANS_LOCK;
	    for (tt = TransList(); tt; tt = tt->next) {
                VTRANS_OBJ_LOCK(tt);
		if ((strcmp(tt->lastProcName, "DeleteVolume") == 0)
		    || (strcmp(tt->lastProcName, "Clone") == 0)
		    || (strcmp(tt->lastProcName, "ReClone") == 0)
		    || (strcmp(tt->lastProcName, "Forward") == 0)
		    || (strcmp(tt->lastProcName, "Restore") == 0)
		    || (strcmp(tt->lastProcName, "ForwardMulti") == 0)) {
                    VTRANS_OBJ_UNLOCK(tt);
		    break;
                }
                VTRANS_OBJ_UNLOCK(tt);
	    }
	    if (tt) {
	        VTRANS_UNLOCK;
		sleep(TTsleep);
	    } else
	        VTRANS_UNLOCK;
	}
    }
    return NULL;
}
#endif

#ifdef AFS_NT40_ENV
/* no volser_syscall */
#elif defined(AFS_SUN511_ENV)
int
volser_syscall(afs_uint32 a3, afs_uint32 a4, void *a5)
{
    int err, code;
    code = ioctl_sun_afs_syscall(28 /* AFSCALL_CALL */, a3, a4, a5, 0, 0, 0,
                                 &err);
    if (code) {
	err = code;
    }
    return err;
}
#else
int
volser_syscall(afs_uint32 a3, afs_uint32 a4, void *a5)
{
    afs_uint32 rcode;
#ifndef AFS_LINUX20_ENV
    void (*old) (int);

    old = signal(SIGSYS, SIG_IGN);
#endif
    rcode =
	syscall(AFS_SYSCALL /* AFS_SYSCALL */ , 28 /* AFSCALL_CALL */ , a3,
		a4, a5);
#ifndef AFS_LINUX20_ENV
    signal(SIGSYS, old);
#endif

    return rcode;
}
#endif


/* check whether caller is authorized to manage RX statistics */
int
vol_rxstat_userok(struct rx_call *call)
{
    return afsconf_SuperUser(tdir, call, NULL);
}

/**
 * Return true if this name is a member of the local realm.
 */
static int
vol_IsLocalRealmMatch(void *rock, char *name, char *inst, char *cell)
{
    struct afsconf_dir *dir = (struct afsconf_dir *)rock;
    afs_int32 islocal = 0;	/* default to no */
    int code;

    code = afsconf_IsLocalRealmMatch(dir, &islocal, name, inst, cell);
    if (code) {
	ViceLog(0,
		("Failed local realm check; code=%d, name=%s, inst=%s, cell=%s\n",
		 code, name, inst, cell));
    }
    return islocal;
}

enum optionsList {
    OPT_log,
    OPT_rxbind,
    OPT_dotted,
    OPT_debug,
    OPT_threads,
    OPT_auditlog,
    OPT_audit_interface,
    OPT_nojumbo,
    OPT_jumbo,
    OPT_rxmaxmtu,
    OPT_sleep,
    OPT_udpsize,
    OPT_peer,
    OPT_process,
    OPT_preserve_vol_stats,
    OPT_sync,
    OPT_syslog
};

static int
ParseArgs(int argc, char **argv) {
    int code;
    int optval;
    char *optstring;
    struct cmd_syndesc *opts;
    char *sleepSpec;
    char *sync_behavior;

    opts = cmd_CreateSyntax(NULL, NULL, NULL, NULL);
    cmd_AddParmAtOffset(opts, OPT_log, "-log", CMD_FLAG, CMD_OPTIONAL,
	   "log vos users");
    cmd_AddParmAtOffset(opts, OPT_rxbind, "-rxbind", CMD_FLAG, CMD_OPTIONAL,
	   "bind only to the primary interface");
    cmd_AddParmAtOffset(opts, OPT_dotted, "-dotted", CMD_FLAG, CMD_OPTIONAL,
	   "permit Kerberos 5 principals with dots");
    cmd_AddParmAtOffset(opts, OPT_debug, "-d", CMD_SINGLE, CMD_OPTIONAL,
	   "debug level");
    cmd_AddParmAtOffset(opts, OPT_threads, "-p", CMD_SINGLE, CMD_OPTIONAL,
	   "number of threads");
    cmd_AddParmAtOffset(opts, OPT_auditlog, "-auditlog", CMD_SINGLE,
	   CMD_OPTIONAL, "location of audit log");
    cmd_AddParmAtOffset(opts, OPT_audit_interface, "-audit-interface",
	   CMD_SINGLE, CMD_OPTIONAL, "interface to use for audit logging");
    cmd_AddParmAtOffset(opts, OPT_nojumbo, "-nojumbo", CMD_FLAG, CMD_OPTIONAL,
	    "disable jumbograms");
    cmd_AddParmAtOffset(opts, OPT_jumbo, "-jumbo", CMD_FLAG, CMD_OPTIONAL,
	    "enable jumbograms");
    cmd_AddParmAtOffset(opts, OPT_rxmaxmtu, "-rxmaxmtu", CMD_SINGLE,
	    CMD_OPTIONAL, "maximum MTU for RX");
    cmd_AddParmAtOffset(opts, OPT_udpsize, "-udpsize", CMD_SINGLE,
	    CMD_OPTIONAL, "size of socket buffer in bytes");
    cmd_AddParmAtOffset(opts, OPT_udpsize, "-sleep", CMD_SINGLE,
	    CMD_OPTIONAL, "make background daemon sleep (LWP only)");
    cmd_AddParmAtOffset(opts, OPT_peer, "-enable_peer_stats", CMD_FLAG,
	    CMD_OPTIONAL, "enable RX transport statistics");
    cmd_AddParmAtOffset(opts, OPT_process, "-enable_process_stats", CMD_FLAG,
	    CMD_OPTIONAL, "enable RX RPC statistics");
    cmd_AddParmAtOffset(opts, OPT_preserve_vol_stats, "-preserve-vol-stats", CMD_FLAG,
	    CMD_OPTIONAL, "preserve volume statistics");
#if !defined(AFS_NT40_ENV)
    cmd_AddParmAtOffset(opts, OPT_syslog, "-syslog", CMD_SINGLE_OR_FLAG,
	    CMD_OPTIONAL, "log to syslog");
#endif
    cmd_AddParmAtOffset(opts, OPT_sync, "-sync",
	    CMD_SINGLE, CMD_OPTIONAL, "always | onclose | never");

    code = cmd_Parse(argc, argv, &opts);

    if (code)
	return 1;

    cmd_OptionAsFlag(opts, OPT_log, &DoLogging);
    cmd_OptionAsFlag(opts, OPT_rxbind, &rxBind);
    cmd_OptionAsFlag(opts, OPT_dotted, &rxkadDisableDotCheck);
    cmd_OptionAsFlag(opts, OPT_preserve_vol_stats, &DoPreserveVolumeStats);
    cmd_OptionAsInt(opts, OPT_debug, &LogLevel);
    if (cmd_OptionPresent(opts, OPT_peer))
	rx_enablePeerRPCStats();
    if (cmd_OptionPresent(opts, OPT_process))
	rx_enableProcessRPCStats();
    if (cmd_OptionPresent(opts, OPT_nojumbo))
	rxJumbograms = 0;
    if (cmd_OptionPresent(opts, OPT_jumbo))
	rxJumbograms = 1;
#ifndef AFS_NT40_ENV
    if (cmd_OptionPresent(opts, OPT_syslog)) {
	serverLogSyslog = 1;
	cmd_OptionAsInt(opts, OPT_syslog, &serverLogSyslogFacility);
    }
#endif
    cmd_OptionAsInt(opts, OPT_rxmaxmtu, &rxMaxMTU);
    if (cmd_OptionAsInt(opts, OPT_udpsize, &optval) == 0) {
	if (optval < rx_GetMinUdpBufSize()) {
	    printf("Warning:udpsize %d is less than minimum %d; ignoring\n",
		    optval, rx_GetMinUdpBufSize());
	} else
	    udpBufSize = optval;
    }
    cmd_OptionAsString(opts, OPT_auditlog, &auditFileName);

    if (cmd_OptionAsString(opts, OPT_audit_interface, &optstring) == 0) {
	if (osi_audit_interface(optstring)) {
	    printf("Invalid audit interface '%s'\n", optstring);
	    return -1;
	}
	free(optstring);
	optstring = NULL;
    }
    if (cmd_OptionAsInt(opts, OPT_threads, &lwps) == 0) {
	if (lwps > MAXLWP) {
	    printf("Warning: '-p %d' is too big; using %d instead\n", lwps, MAXLWP);
	    lwps = MAXLWP;
	}
    }
    if (cmd_OptionAsString(opts, OPT_sleep, &sleepSpec) == 0) {
	sscanf(sleepSpec, "%d/%d", &TTsleep, &TTrun);
	if ((TTsleep < 0) || (TTrun <= 0)) {
	    printf("Warning: '-sleep %d/%d' is incorrect; ignoring\n",
		    TTsleep, TTrun);
	    TTsleep = TTrun = 0;
	}

    }
    if (cmd_OptionAsString(opts, OPT_sync, &sync_behavior) == 0) {
	if (ih_SetSyncBehavior(sync_behavior)) {
	    printf("Invalid -sync value %s\n", sync_behavior);
	    return -1;
	}
    }

    return 0;
}

#include "AFS_component_version_number.c"
int
main(int argc, char **argv)
{
    afs_int32 code;
    struct rx_securityClass **securityClasses;
    afs_int32 numClasses;
    struct rx_service *service;
    int rxpackets = 100;
    afs_uint32 host = ntohl(INADDR_ANY);
    VolumePackageOptions opts;

#ifdef	AFS_AIX32_ENV
    /*
     * The following signal action for AIX is necessary so that in case of a
     * crash (i.e. core is generated) we can include the user's data section
     * in the core dump. Unfortunately, by default, only a partial core is
     * generated which, in many cases, isn't too useful.
     */
    struct sigaction nsa;

    sigemptyset(&nsa.sa_mask);
    nsa.sa_handler = SIG_DFL;
    nsa.sa_flags = SA_FULLDUMP;
    sigaction(SIGABRT, &nsa, NULL);
    sigaction(SIGSEGV, &nsa, NULL);
#endif
    osi_audit_init();
    osi_audit(VS_StartEvent, 0, AUD_END);

    /* Initialize dirpaths */
    if (!(initAFSDirPath() & AFSDIR_SERVER_PATHS_OK)) {
#ifdef AFS_NT40_ENV
	ReportErrorEventAlt(AFSEVT_SVR_NO_INSTALL_DIR, 0, argv[0], 0);
#endif
	fprintf(stderr, "%s: Unable to obtain AFS server directory.\n",
		argv[0]);
	exit(2);
    }

    TTsleep = TTrun = 0;

    ParseArgs(argc, argv);

    if (auditFileName) {
	osi_audit_file(auditFileName);
	osi_audit(VS_StartEvent, 0, AUD_END);
    }
#ifdef AFS_SGI_VNODE_GLUE
    if (afs_init_kernel_config(-1) < 0) {
	printf
	    ("Can't determine NUMA configuration, not starting volserver.\n");
	exit(1);
    }
#endif
    InitErrTabs();

#ifdef AFS_PTHREAD_ENV
    SetLogThreadNumProgram( rx_GetThreadNum );
#endif

#ifdef AFS_NT40_ENV
    if (afs_winsockInit() < 0) {
	ReportErrorEventAlt(AFSEVT_SVR_WINSOCK_INIT_FAILED, 0, argv[0], 0);
	printf("Volume server unable to start winsock, exiting.\n");
	exit(1);
    }
#endif
    /* Open VolserLog and map stdout, stderr into it; VInitVolumePackage2 can
       log, so we need to do this here */
    OpenLog(AFSDIR_SERVER_VOLSERLOG_FILEPATH);

    VOptDefaults(volumeServer, &opts);
    if (VInitVolumePackage2(volumeServer, &opts)) {
	Log("Shutting down: errors encountered initializing volume package\n");
	exit(1);
    }
    /* For nuke() */
    Lock_Init(&localLock);
    DInit(40);
#ifndef AFS_PTHREAD_ENV
    vol_PollProc = IOMGR_Poll;	/* tell vol pkg to poll io system periodically */
#endif
#ifndef AFS_NT40_ENV
    rxi_syscallp = volser_syscall;
#endif
    rx_nPackets = rxpackets;	/* set the max number of packets */
    if (udpBufSize)
	rx_SetUdpBufSize(udpBufSize);	/* set the UDP buffer size for receive */
    if (rxBind) {
	afs_int32 ccode;
        if (AFSDIR_SERVER_NETRESTRICT_FILEPATH ||
            AFSDIR_SERVER_NETINFO_FILEPATH) {
            char reason[1024];
            ccode = afsconf_ParseNetFiles(SHostAddrs, NULL, NULL,
                                          ADDRSPERSITE, reason,
                                          AFSDIR_SERVER_NETINFO_FILEPATH,
                                          AFSDIR_SERVER_NETRESTRICT_FILEPATH);
        } else
	{
            ccode = rx_getAllAddr(SHostAddrs, ADDRSPERSITE);
        }
        if (ccode == 1)
            host = SHostAddrs[0];
    }

    code = rx_InitHost(host, (int)htons(AFSCONF_VOLUMEPORT));
    if (code) {
	fprintf(stderr, "rx init failed on socket AFSCONF_VOLUMEPORT %u\n",
		AFSCONF_VOLUMEPORT);
	VS_EXIT(1);
    }
    if (!rxJumbograms) {
	/* Don't allow 3.4 vos clients to send jumbograms and we don't send. */
	rx_SetNoJumbo();
    }
    if (rxMaxMTU != -1) {
	if (rx_SetMaxMTU(rxMaxMTU) != 0) {
	    fprintf(stderr, "rxMaxMTU %d is invalid\n", rxMaxMTU);
	    VS_EXIT(1);
	}
    }
    rx_GetIFInfo();
    rx_SetRxDeadTime(420);
    memset(busyFlags, 0, sizeof(busyFlags));

    SetupLogSignals();

    {
#ifdef AFS_PTHREAD_ENV
	pthread_t tid;
	pthread_attr_t tattr;
	opr_Verify(pthread_attr_init(&tattr) == 0);
	opr_Verify(pthread_attr_setdetachstate(&tattr,
					       PTHREAD_CREATE_DETACHED) == 0);
	opr_Verify(pthread_create(&tid, &tattr, BKGLoop, NULL) == 0);
#else
	PROCESS pid;
	LWP_CreateProcess(BKGLoop, 16*1024, 3, 0, "vol bkg daemon", &pid);
	LWP_CreateProcess(BKGSleep,16*1024, 3, 0, "vol slp daemon", &pid);
#endif
    }

    /* Create a single security object, in this case the null security object, for unauthenticated connections, which will be used to control security on connections made to this server */

    tdir = afsconf_Open(AFSDIR_SERVER_ETC_DIRPATH);
    if (!tdir) {
	Abort("volser: could not open conf files in %s\n",
	      AFSDIR_SERVER_ETC_DIRPATH);
	VS_EXIT(1);
    }

    /* initialize audit user check */
    osi_audit_set_user_check(tdir, vol_IsLocalRealmMatch);

    afsconf_BuildServerSecurityObjects(tdir, &securityClasses, &numClasses);
    if (securityClasses[0] == NULL)
	Abort("rxnull_NewServerSecurityObject");
    service =
	rx_NewServiceHost(host, 0, VOLSERVICE_ID, "VOLSER", securityClasses,
			  numClasses, AFSVolExecuteRequest);
    if (service == (struct rx_service *)0)
	Abort("rx_NewService");
    rx_SetBeforeProc(service, MyBeforeProc);
    rx_SetAfterProc(service, MyAfterProc);
    rx_SetIdleDeadTime(service, 0);	/* never timeout */
    if (lwps < 4)
	lwps = 4;
    rx_SetMaxProcs(service, lwps);
#if defined(AFS_XBSD_ENV)
    rx_SetStackSize(service, (128 * 1024));
#elif defined(AFS_SGI_ENV)
    rx_SetStackSize(service, (48 * 1024));
#else
    rx_SetStackSize(service, (32 * 1024));
#endif

    if (rxkadDisableDotCheck) {
        rx_SetSecurityConfiguration(service, RXS_CONFIG_FLAGS,
                                    (void *)RXS_CONFIG_FLAGS_DISABLE_DOTCHECK);
    }

    service =
	rx_NewService(0, RX_STATS_SERVICE_ID, "rpcstats", securityClasses,
		      numClasses, RXSTATS_ExecuteRequest);
    if (service == (struct rx_service *)0)
	Abort("rx_NewService");
    rx_SetMinProcs(service, 2);
    rx_SetMaxProcs(service, 4);

    LogCommandLine(argc, argv, "Volserver", VolserVersion, "Starting AFS",
		   Log);
    if (TTsleep) {
	Log("Will sleep %d second%s every %d second%s\n", TTsleep,
	    (TTsleep > 1) ? "s" : "", TTrun + TTsleep,
	    (TTrun + TTsleep > 1) ? "s" : "");
    }

    /* allow super users to manage RX statistics */
    rx_SetRxStatUserOk(vol_rxstat_userok);

    rx_StartServer(1);		/* Donate this process to the server process pool */

    osi_audit(VS_FinishEvent, (-1), AUD_END);
    Abort("StartServer returned?");
    return 0; /* not reached */
}
