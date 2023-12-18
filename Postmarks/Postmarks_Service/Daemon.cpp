#include <stdio.h>
#include <signal.h>
#include <syslog.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "Logging/Log.h"
#include "Postmarks/Postmarks.h"
#include "Task/lock.h"

#define DAEMON_NAME "postmarksd"

namespace Logging
{
	const uint32_t LC_Service = 0x0010;
	template <> const char* getLCStr<LC_Service      >() { return "Service      "; }
}

void daemonShutdown();
void signal_handler(int sig);
void daemonize(char *rundir, char *pidfile);
void usage();
bool parseCmdLine(int argc, char *argv[]);

std::string g_psubaddr("127.0.0.1");
std::string g_version = "1.2.5";
std::string g_cfgfile("./SystemConfig.xml");
std::string g_diffpath(".");
bool g_exe(false);

int pidFilehandle;
std::string logfilen = DAEMON_NAME ".log";
Logging::LogFile logfile, *plogfile(&logfile);
VEvent stopEvent;

void signal_handler(int sig)
{
	switch(sig)
	{
	case SIGHUP:
		syslog(LOG_WARNING, "Received SIGHUP signal.");
		break;
	case SIGINT:
	case SIGTERM:
		syslog(LOG_INFO, "Daemon exiting");
		daemonShutdown();
		exit(EXIT_SUCCESS);
		break;
	default:
		syslog(LOG_WARNING, "Unhandled signal %s", strsignal(sig));
		break;
	}
}

void daemonShutdown()
{
	stopEvent.set();
	close(pidFilehandle);
}

void daemonize(const char *rundir, const char *pidfile)
{
	int pid, sid, i;
	char str[10];
	struct sigaction newSigAction;
	sigset_t newSigSet;

	/* Check if parent process id is set */
	if(getppid() == 1)
	{
		/* PPID exists, therefore we are already a daemon */
		return;
	}

	/* Set signal mask - signals we want to block */
	sigemptyset(&newSigSet);
	sigaddset(&newSigSet, SIGCHLD);  /* ignore child - i.e. we don't need to wait for it */
	sigaddset(&newSigSet, SIGTSTP);  /* ignore Tty stop signals */
	sigaddset(&newSigSet, SIGTTOU);  /* ignore Tty background writes */
	sigaddset(&newSigSet, SIGTTIN);  /* ignore Tty background reads */
	sigprocmask(SIG_BLOCK, &newSigSet, NULL);   /* Block the above specified signals */

	/* Set up a signal handler */
	newSigAction.sa_handler = signal_handler;
	sigemptyset(&newSigAction.sa_mask);
	newSigAction.sa_flags = 0;

	/* Signals to handle */
	sigaction(SIGHUP, &newSigAction, NULL);     /* catch hangup signal */
	sigaction(SIGTERM, &newSigAction, NULL);    /* catch term signal */
	sigaction(SIGINT, &newSigAction, NULL);     /* catch interrupt signal */


	/* Fork*/
	pid = fork();

	if(pid < 0)
	{
		/* Could not fork */
		exit(EXIT_FAILURE);
	}

	if(pid > 0)
	{
		/* Child created ok, so exit parent process */
		printf("Child process created: %d\n", pid);
		exit(EXIT_SUCCESS);
	}

	/* Child continues */

	umask(027); /* Set file permissions 750 */

	/* Get a new process group */
	sid = setsid();

	if(sid < 0)
		exit(EXIT_FAILURE);

	/* close all descriptors */
	for(i = getdtablesize(); i >= 0; --i)
		close(i);

	/* Route I/O connections */
	i = open("/dev/null", O_RDWR);
	dup2 (i, STDIN_FILENO);
	dup2 (i, STDOUT_FILENO);
	dup2 (i, STDERR_FILENO);
	close(i);

	if (chdir(rundir) == -1)
	{
		syslog(LOG_INFO, "Could not change running directory to %s", rundir);
		exit(EXIT_FAILURE);
	}

	/* Ensure only one copy */
	pidFilehandle = open(pidfile, O_RDWR | O_CREAT, 0600);

	if(pidFilehandle == -1)
	{
		/* Couldn't open lock file */
		syslog(LOG_INFO, "Could not open PID lock file %s, exiting", pidfile);
		exit(EXIT_FAILURE);
	}

	/* Try to lock file */
	if(lockf(pidFilehandle, F_TLOCK, 0) == -1)
	{
		/* Couldn't get lock on lock file */
		syslog(LOG_INFO, "Could not lock PID lock file %s, exiting", pidfile);
		exit(EXIT_FAILURE);
	}

	/* Get and format PID */
	sprintf(str, "%d\n", getpid());

	/* write pid to lockfile */
	if (write(pidFilehandle, str, strlen(str)) == -1)
	{
		/* Couldn't get lock on lock file */
		syslog(LOG_INFO, "Could not write to PID lock file %s, exiting", pidfile);
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char* argv[])
{
	if (!parseCmdLine(argc, argv))
		return -1;

	if (!g_exe)
	{
		/* Debug logging
		setlogmask(LOG_UPTO(LOG_DEBUG));
		openlog(DAEMON_NAME, LOG_CONS, LOG_USER);
		*/

		/* Logging */
		setlogmask(LOG_UPTO(LOG_INFO));
		openlog(DAEMON_NAME, LOG_CONS | LOG_PERROR, LOG_USER);

		syslog(LOG_INFO, "Daemon starting up");

		/* Deamonize */
		daemonize("/tmp/", "/tmp/postmarksd.pid");

		syslog(LOG_INFO, "Daemon running");
	}

	if (!logfilen.empty())
		logfile.open(logfilen);

	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "************************************ STARTUP ****************************************");
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "***** Executable: " << argv[0]);
	for (int x = 1; x < argc; ++x)
		LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "***** Command line parameter: " << argv[x]);
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "***** Version: " << g_version);
	LOGTO(plogfile, Logging::LL_Info, Logging::LC_Service, "*************************************************************************************");

	Postmarks disp(logfile, g_psubaddr);
	disp.start();

	while(!stopEvent.timedwait(1000))
	{
		//syslog(LOG_INFO, "configd says hello");
		//boost::this_thread::sleep_for(1s);
	}

	disp.stop();
}

bool parseCmdLine(int argc, char *argv[])
{
	bool ret(true);

	plogfile->setLogLevel(Logging::LLSet_Info);
	plogfile->setMaxFiles(5);
	plogfile->setSizeLimit(0x00A00000); // 10 Mb
	for (int x = 1; x < argc; ++x)
	{
		if (argv[x][0] == '-')
		{
			// an option
			int optlen = strlen(argv[x]);
			for (int y = 1; y < optlen; ++y)
			{
				switch (argv[x][y])
				{
				case 'h':
					usage();
					return false;
				case 'd':
					plogfile->setLogLevel(Logging::LLSet_Debug);
					break;
				case 'm':
					plogfile->setLogLevel(Logging::LLSet_Dump);
					break;
				case 't':
					plogfile->setLogLevel(Logging::LLSet_Trace);
					break;
				case 'T':
					plogfile->setLogLevel(Logging::LLSet_Test);
					break;
				case 'e': // run as exe
					g_exe = true;
					break;
//				case 'c': // specify config file
//					if (y == optlen - 1 && ++x < argc && argv[x][0] != '-')
//						g_cfgfile = argv[x];
//					else
//					{
//						std::cout << "Invalid command line parameters" << std::endl;
//						usage();
//						return false;
//					}
//					break;
				case 'l': // specify log file
					if (y == optlen - 1 && ++x < argc && argv[x][0] != '-')
						logfilen = argv[x];
					else
					{
						std::cout << "Invalid command line parameters" << std::endl;
						usage();
						return false;
					}
					break;
				default:
					std::cout << "Invalid command line parameters" << std::endl;
					usage();
					return false;
				}
			}
		}
	}

	return ret;
}

void usage()
{
	using namespace std;
	cout << "postmarksd - pSub Postmark Allocation service" << endl;
	cout << "Usage: postmarksd [OPTIONS]" << endl;
	cout << "Options:" << endl;
	cout << "\t-h - help. Print this message and exit" << endl;
	cout << "\t     If this option is used any subsequent options are ignored" << endl;
	cout << "\t-d - debug. Sets logging level to DEBUG." << endl;
	cout << "\t-t - trace. Sets logging level to TRACE." << endl;
	cout << "\t-m - dump. Sets logging level to DUMP." << endl;
	cout << "\t-e - exe. Runs as executable rather than a daemon." << endl;
	cout << "\t-b <ip address> - bus address. Specifies the address of the publish/subscribe server to connect to" << endl;
	cout << "\t     If this option is not used the default will be the local host 127.0.0.1" << endl;
//	cout << "\t-c <config file> - config. Specifies the configuration file to load." << endl;
//	cout << "\t     Defaults to ./SystemConfig.xml if ommitted." << endl;
	cout << "\t-l <log file> - log. Specifies the log file to produce." << endl;
	cout << endl;
	cout << "Multiple options can be grouped together e.g. -de sets logging level to debug and runs as an executable" << endl;
	cout << "Options that require a value (-c, -l) must be at the end of an option group" << endl;
	cout << "\te.g.  -el postmarks.log  will work but" << endl;
	cout << "\t      -le postmarks.log  will fail" << endl;
	cout << endl;
	cout << "Multiple option groups can be listed e.g.  -d -el postmarksd.log" << endl;
	cout << endl;
	cout << "Note: When setting logging levels (-d, -t) the last option listed will" << endl;
	cout << "override any previously set logging levels" << endl;
	cout << "\te.g.  -dt ignores the 'd' and sets the logging level to TRACE" << endl;
	cout << "\t      -td ignores the 't' and sets the logging level to DEBUG" << endl;
}
