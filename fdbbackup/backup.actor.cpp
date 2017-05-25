/*
 * backup.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flow/actorcompiler.h"
#include "flow/FastAlloc.h"
#include "flow/serialize.h"
#include "flow/IRandom.h"
#include "flow/genericactors.actor.h"

#include "fdbclient/FDBTypes.h"
#include "fdbclient/BackupAgent.h"
#include "fdbclient/Status.h"
#include "fdbclient/BackupContainer.h"

#include "fdbclient/RunTransaction.actor.h"
#include "fdbrpc/Platform.h"
#include "fdbclient/json_spirit/json_spirit_writer_template.h"

#include <stdarg.h>
#include <stdio.h>
#include <algorithm>	// std::transform
#include <string>
#include <iostream>
using std::cout;
using std::endl;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef min
#undef max
#endif
#include <time.h>

#define BOOST_DATE_TIME_NO_LIB
#include <boost/interprocess/managed_shared_memory.hpp>

#ifdef  __linux__
#include <execinfo.h>
#ifdef ALLOC_INSTRUMENTATION
#include <cxxabi.h>
#endif
#endif

#ifndef WIN32
#include "versions.h"
#endif

#include "flow/SimpleOpt.h"


// Type of program being executed
enum enumProgramExe {
	EXE_AGENT, EXE_BACKUP, EXE_RESTORE, EXE_DR_AGENT, EXE_DB_BACKUP, EXE_BLOBMANAGER, EXE_UNDEFINED
};

enum enumBackupType {
	BACKUP_UNDEFINED=0, BACKUP_START, BACKUP_STATUS, BACKUP_ABORT, BACKUP_WAIT, BACKUP_DISCONTINUE
};

enum enumDBType {
	DB_UNDEFINED=0, DB_START, DB_STATUS, DB_SWITCH, DB_ABORT
};

enum enumRestoreType {
	RESTORE_UNKNOWN, RESTORE_START, RESTORE_STATUS, RESTORE_ABORT, RESTORE_WAIT
};

//
enum {
	// Backup constants
	OPT_DESTCONTAINER, OPT_ERRORLIMIT, OPT_NOSTOPWHENDONE,

	// Backup and Restore constants
	OPT_TAGNAME, OPT_BACKUPKEYS, OPT_WAITFORDONE,

	// Restore constants
	OPT_RESTORECONTAINER, OPT_DBVERSION, OPT_PREFIX_ADD, OPT_PREFIX_REMOVE,

	// Shared constants
	OPT_CLUSTERFILE, OPT_QUIET, OPT_DRYRUN, OPT_FORCE,
	OPT_HELP, OPT_DEVHELP, OPT_VERSION, OPT_PARENTPID, OPT_CRASHONERROR,
	OPT_NOBUFSTDOUT, OPT_BUFSTDOUTERR, OPT_TRACE, OPT_TRACE_DIR,
	OPT_KNOB, OPT_TRACE_LOG_GROUP,

	//DB constants
	OPT_SOURCE_CLUSTER,
	OPT_DEST_CLUSTER,
	OPT_CLEANUP
};

CSimpleOpt::SOption g_rgAgentOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_CLUSTERFILE,	   "-C",               SO_REQ_SEP },
	{ OPT_CLUSTERFILE,     "--cluster_file",   SO_REQ_SEP },
	{ OPT_TRACE_LOG_GROUP, "--loggroup",       SO_REQ_SEP },
	{ OPT_KNOB,            "--knob_",          SO_REQ_SEP },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgBackupStartOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_CLUSTERFILE,	   "-C",               SO_REQ_SEP },
	{ OPT_CLUSTERFILE,     "--cluster_file",   SO_REQ_SEP },
	{ OPT_WAITFORDONE,      "-w",              SO_NONE },
	{ OPT_WAITFORDONE,      "--waitfordone",   SO_NONE },
	{ OPT_NOSTOPWHENDONE,   "-z",               SO_NONE },
	{ OPT_NOSTOPWHENDONE,   "--no-stop-when-done",SO_NONE },
	{ OPT_DESTCONTAINER,    "-d",               SO_REQ_SEP },
	{ OPT_DESTCONTAINER,    "--destcontainer",  SO_REQ_SEP },
	{ OPT_TAGNAME,         "-t",               SO_REQ_SEP },
	{ OPT_TAGNAME,         "--tagname",        SO_REQ_SEP },
	{ OPT_BACKUPKEYS,      "-k",               SO_REQ_SEP },
	{ OPT_BACKUPKEYS,      "--keys",           SO_REQ_SEP },
	{ OPT_DRYRUN,          "-n",               SO_NONE },
	{ OPT_DRYRUN,          "--dryrun",         SO_NONE },
	{ OPT_FORCE,           "-f",               SO_NONE },
	{ OPT_FORCE,           "--force",          SO_NONE },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },
	{ OPT_KNOB,            "--knob_",          SO_REQ_SEP },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgBackupStatusOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_CLUSTERFILE,	   "-C",               SO_REQ_SEP },
	{ OPT_CLUSTERFILE,     "--cluster_file",   SO_REQ_SEP },
	{ OPT_ERRORLIMIT,      "-e",               SO_REQ_SEP },
	{ OPT_ERRORLIMIT,      "--errorlimit",     SO_REQ_SEP },
	{ OPT_TAGNAME,         "-t",               SO_REQ_SEP },
	{ OPT_TAGNAME,         "--tagname",        SO_REQ_SEP },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgBackupAbortOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_CLUSTERFILE,	   "-C",               SO_REQ_SEP },
	{ OPT_CLUSTERFILE,     "--cluster_file",   SO_REQ_SEP },
	{ OPT_TAGNAME,         "-t",               SO_REQ_SEP },
	{ OPT_TAGNAME,         "--tagname",        SO_REQ_SEP },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgBackupDiscontinueOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_CLUSTERFILE,	   "-C",               SO_REQ_SEP },
	{ OPT_CLUSTERFILE,     "--cluster_file",   SO_REQ_SEP },
	{ OPT_TAGNAME,         "-t",               SO_REQ_SEP },
	{ OPT_TAGNAME,         "--tagname",        SO_REQ_SEP },
	{ OPT_WAITFORDONE,      "-w",              SO_NONE },
	{ OPT_WAITFORDONE,      "--waitfordone",   SO_NONE },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgBackupWaitOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_CLUSTERFILE,	   "-C",               SO_REQ_SEP },
	{ OPT_CLUSTERFILE,     "--cluster_file",   SO_REQ_SEP },
	{ OPT_TAGNAME,         "-t",               SO_REQ_SEP },
	{ OPT_TAGNAME,         "--tagname",        SO_REQ_SEP },
	{ OPT_NOSTOPWHENDONE,   "-z",               SO_NONE },
	{ OPT_NOSTOPWHENDONE,   "--no-stop-when-done",SO_NONE },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgRestoreOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_CLUSTERFILE,	   "-C",               SO_REQ_SEP },
	{ OPT_KNOB,            "--knob_",          SO_REQ_SEP },
	{ OPT_RESTORECONTAINER,"-r",               SO_REQ_SEP },
	{ OPT_PREFIX_ADD,      "-add_prefix",      SO_REQ_SEP },
	{ OPT_PREFIX_REMOVE,   "-remove_prefix",   SO_REQ_SEP },
	{ OPT_TAGNAME,         "-t",               SO_REQ_SEP },
	{ OPT_TAGNAME,         "--tagname",        SO_REQ_SEP },
	{ OPT_BACKUPKEYS,      "-k",               SO_REQ_SEP },
	{ OPT_BACKUPKEYS,      "--keys",           SO_REQ_SEP },
	{ OPT_WAITFORDONE,      "-w",              SO_NONE },
	{ OPT_WAITFORDONE,      "--waitfordone",   SO_NONE },
	{ OPT_CLUSTERFILE,     "--cluster_file",   SO_REQ_SEP },
	{ OPT_DBVERSION,       "--version",        SO_REQ_SEP },
	{ OPT_DBVERSION,       "-v",               SO_REQ_SEP },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_DRYRUN,          "-n",               SO_NONE },
	{ OPT_DRYRUN,          "--dryrun",         SO_NONE },
	{ OPT_FORCE,           "-f",               SO_NONE },
	{ OPT_FORCE,           "--force",          SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgDBAgentOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_TRACE_LOG_GROUP, "--loggroup",       SO_REQ_SEP },
	{ OPT_SOURCE_CLUSTER,  "-s",               SO_REQ_SEP },
	{ OPT_SOURCE_CLUSTER,  "--source",         SO_REQ_SEP },
	{ OPT_DEST_CLUSTER,    "-d",               SO_REQ_SEP },
	{ OPT_DEST_CLUSTER,    "--destination",    SO_REQ_SEP },
	{ OPT_KNOB,            "--knob_",          SO_REQ_SEP },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgDBStartOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_SOURCE_CLUSTER,  "-s",               SO_REQ_SEP },
	{ OPT_SOURCE_CLUSTER,  "--source",         SO_REQ_SEP },
	{ OPT_DEST_CLUSTER,    "-d",               SO_REQ_SEP },
	{ OPT_DEST_CLUSTER,    "--destination",    SO_REQ_SEP },
	{ OPT_TAGNAME,         "-t",               SO_REQ_SEP },
	{ OPT_TAGNAME,         "--tagname",        SO_REQ_SEP },
	{ OPT_BACKUPKEYS,      "-k",               SO_REQ_SEP },
	{ OPT_BACKUPKEYS,      "--keys",           SO_REQ_SEP },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgDBStatusOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_SOURCE_CLUSTER,  "-s",               SO_REQ_SEP },
	{ OPT_SOURCE_CLUSTER,  "--source",         SO_REQ_SEP },
	{ OPT_DEST_CLUSTER,    "-d",               SO_REQ_SEP },
	{ OPT_DEST_CLUSTER,    "--destination",    SO_REQ_SEP },
	{ OPT_ERRORLIMIT,      "-e",               SO_REQ_SEP },
	{ OPT_ERRORLIMIT,      "--errorlimit",     SO_REQ_SEP },
	{ OPT_TAGNAME,         "-t",               SO_REQ_SEP },
	{ OPT_TAGNAME,         "--tagname",        SO_REQ_SEP },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgDBSwitchOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_SOURCE_CLUSTER,  "-s",               SO_REQ_SEP },
	{ OPT_SOURCE_CLUSTER,  "--source",         SO_REQ_SEP },
	{ OPT_DEST_CLUSTER,    "-d",               SO_REQ_SEP },
	{ OPT_DEST_CLUSTER,    "--destination",    SO_REQ_SEP },
	{ OPT_TAGNAME,         "-t",               SO_REQ_SEP },
	{ OPT_TAGNAME,         "--tagname",        SO_REQ_SEP },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgDBAbortOptions[] = {
#ifdef _WIN32
	{ OPT_PARENTPID,      "--parentpid",       SO_REQ_SEP },
#endif
	{ OPT_SOURCE_CLUSTER,  "-s",               SO_REQ_SEP },
	{ OPT_SOURCE_CLUSTER,  "--source",         SO_REQ_SEP },
	{ OPT_DEST_CLUSTER,    "-d",               SO_REQ_SEP },
	{ OPT_DEST_CLUSTER,    "--destination",    SO_REQ_SEP },
	{ OPT_CLEANUP,         "--cleanup",        SO_NONE },
	{ OPT_TAGNAME,         "-t",               SO_REQ_SEP },
	{ OPT_TAGNAME,         "--tagname",        SO_REQ_SEP },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },
	{ OPT_QUIET,           "-q",               SO_NONE },
	{ OPT_QUIET,           "--quiet",          SO_NONE },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_DEVHELP,         "--dev-help",       SO_NONE },

	SO_END_OF_OPTIONS
};

CSimpleOpt::SOption g_rgBlobOptions[] = {
	{ OPT_KNOB,            "--knob_",          SO_REQ_SEP },
	{ OPT_VERSION,         "--version",        SO_NONE },
	{ OPT_VERSION,         "-v",               SO_NONE },
	{ OPT_CRASHONERROR,    "--crash",          SO_NONE },
	{ OPT_HELP,            "-?",               SO_NONE },
	{ OPT_HELP,            "-h",               SO_NONE },
	{ OPT_HELP,            "--help",           SO_NONE },
	{ OPT_TRACE,           "--log",            SO_NONE },
	{ OPT_TRACE_DIR,       "--logdir",         SO_REQ_SEP },

	SO_END_OF_OPTIONS
};

const KeyRef exeAgent = LiteralStringRef("backup_agent");
const KeyRef exeBackup = LiteralStringRef("fdbbackup");
const KeyRef exeRestore = LiteralStringRef("fdbrestore");
const KeyRef exeDatabaseAgent = LiteralStringRef("dr_agent");
const KeyRef exeDatabaseBackup = LiteralStringRef("fdbdr");
const KeyRef exeBlobManager = LiteralStringRef("fdbblob");

extern void flushTraceFileVoid();
extern const char* getHGVersion();

#ifdef _WIN32
void parentWatcher(void *parentHandle) {
	HANDLE parent = (HANDLE)parentHandle;
	int signal = WaitForSingleObject(parent, INFINITE);
	CloseHandle(parentHandle);
	if (signal == WAIT_OBJECT_0)
		criticalError(FDB_EXIT_SUCCESS, "ParentProcessExited", "Parent process exited");
	TraceEvent(SevError, "ParentProcessWaitFailed").detail("RetCode", signal).GetLastError();
}

#endif

static void printVersion() {
	printf("FoundationDB " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n");
	printf("source version %s\n", getHGVersion());
	printf("protocol %llx\n", (long long) currentProtocolVersion);
}

static void printHelpTeaser( const char *name ) {
	fprintf(stderr, "Try `%s --help' for more information.\n", name);
}

static void printAgentUsage(bool devhelp) {
	printf("FoundationDB " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n");
	printf("Usage: %s [OPTIONS]\n\n", exeAgent.toString().c_str());
	printf("  -C CONNFILE    The path of a file containing the connection string for the\n"
		   "                 FoundationDB cluster. The default is first the value of the\n"
		   "                 FDB_CLUSTER_FILE environment variable, then `./fdb.cluster',\n"
		   "                 then `%s'.\n", platform::getDefaultClusterFilePath().c_str());
	printf("  --log          Enables trace file logging for the CLI session.\n"
		   "  --logdir PATH  Specifes the output directory for trace files. If\n"
		   "                 unspecified, defaults to the current directory. Has\n"
		   "                 no effect unless --log is specified.\n");
	printf("  -v, --version  Print version information and exit.\n");
	printf("  -h, --help     Display this help and exit.\n");
	if (devhelp) {
#ifdef _WIN32
		printf("  -n             Create a new console.\n");
		printf("  -q             Disable error dialog on crash.\n");
		printf("  --parentpid PID\n");
		printf("                 Specify a process after whose termination to exit.\n");
#endif
	}

	return;
}

void printBlobStoreParameterInfo(const char *pad) {
	printf("%sValid Blob Store parameters:\n\n", pad);
	for(auto &f : BlobStoreEndpoint::BlobKnobs::getKnobDescriptions())
		printf("%s   %s\n", pad, f.c_str());
}

void printBackupContainerInfo() {
	printf("                 Backup URL forms:\n\n");
	std::vector<std::string> formats = IBackupContainer::getURLFormats();
	for(auto &f : formats)
		printf("                     %s\n", f.c_str());
	printf("\n");
	printBlobStoreParameterInfo("                     ");
}

static void printBackupUsage(bool devhelp) {
	printf("FoundationDB " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n");
	printf("Usage: %s (start | status | abort | wait | discontinue) [OPTIONS]\n\n", exeBackup.toString().c_str());
	printf("  -C CONNFILE    The path of a file containing the connection string for the\n"
		   "                 FoundationDB cluster. The default is first the value of the\n"
		   "                 FDB_CLUSTER_FILE environment variable, then `./fdb.cluster',\n"
		   "                 then `%s'.\n", platform::getDefaultClusterFilePath().c_str());
	printf("  -d, --destcontainer URL\n"
	       "                 The Backup URL for the destination of this backup.\n");
	printBackupContainerInfo();
	printf("  -e ERRORLIMIT  The maximum number of errors printed by status (default is 10).\n");
	printf("  -k KEYS        List of key ranges to backup.\n"
		   "                 If not specified, the entire database will be backed up.\n");
	printf("  -n, --dry-run  Perform a trial run with no changes made.\n");
	printf("  -v, --version  Print version information and exit.\n");
	printf("  -w, --wait     Wait for the backup to complete (allowed with `start' and `discontinue').\n");
	printf("  -z, --no-stop-when-done\n"
		   "                 Do not stop backup when restorable.\n");
	printf("  -h, --help     Display this help and exit.\n");
	printf("\n"
		   "  KEYS FORMAT:   \"<BEGINKEY> <ENDKEY>\" [...]\n");

	if (devhelp) {
#ifdef _WIN32
		printf("  -n             Create a new console.\n");
		printf("  -q             Disable error dialog on crash.\n");
		printf("  --parentpid PID\n");
		printf("                 Specify a process after whose termination to exit.\n");
#endif
	}

	return;
}

static void printRestoreUsage(bool devhelp ) {
	printf("FoundationDB " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n");
	printf("Usage: %s (start | status | abort | wait) [OPTIONS]\n\n", exeRestore.toString().c_str());
	//printf("  FOLDERS        Paths to folders containing the backup files.\n");
	printf("Options for all commands:\n\n");
	printf("  -C CONNFILE    The path of a file containing the connection string for the\n"
		   "                 FoundationDB cluster. The default is first the value of the\n"
		   "                 FDB_CLUSTER_FILE environment variable, then `./fdb.cluster',\n"
		   "                 then `%s'.\n", platform::getDefaultClusterFilePath().c_str());
	printf("  -t TAGNAME     The restore tag to act on.  Default is 'default'\n");
	printf("    --tagname TAGNAME\n\n");
	printf(" Options for start:\n\n");
	printf("  -r URL         The Backup URL for the restore to read from.\n");
	printBackupContainerInfo();
	printf("  -w             Wait for the restore to complete before exiting.  Prints progress updates.\n");
	printf("    --waitfordone\n");
	printf("  -k KEYS        List of key ranges from the backup to restore\n");
	printf("  --remove_prefix PREFIX   prefix to remove from the restored keys\n");
	printf("  --add_prefix PREFIX      prefix to add to the restored keys\n");
	printf("  -n, --dry-run  Perform a trial run with no changes made.\n");
	printf("  -v DBVERSION   The version at which the database will be restored.\n");
	printf("  -h, --help     Display this help and exit.\n");

	if( devhelp ) {
#ifdef _WIN32
		printf("  -q             Disable error dialog on crash.\n");
		printf("  --parentpid PID\n");
		printf("                 Specify a process after whose termination to exit.\n");
#endif
	}

	return;
}

static void printDBAgentUsage(bool devhelp) {
	printf("FoundationDB " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n");
	printf("Usage: %s [OPTIONS]\n\n", exeDatabaseAgent.toString().c_str());
	printf("  -d CONNFILE    The path of a file containing the connection string for the\n"
		   "                 destination FoundationDB cluster.\n");
	printf("  -s CONNFILE    The path of a file containing the connection string for the\n"
		   "                 source FoundationDB cluster.\n");
	printf("  --log          Enables trace file logging for the CLI session.\n"
		   "  --logdir PATH  Specifes the output directory for trace files. If\n"
		   "                 unspecified, defaults to the current directory. Has\n"
		   "                 no effect unless --log is specified.\n");
	printf("  -v, --version  Print version information and exit.\n");
	printf("  -h, --help     Display this help and exit.\n");
	if (devhelp) {
#ifdef _WIN32
		printf("  -n             Create a new console.\n");
		printf("  -q             Disable error dialog on crash.\n");
		printf("  --parentpid PID\n");
		printf("                 Specify a process after whose termination to exit.\n");
#endif
	}

	return;
}

static void printDBBackupUsage(bool devhelp) {
	printf("FoundationDB " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n");
	printf("Usage: %s (start | status | switch | abort) [OPTIONS]\n\n", exeDatabaseBackup.toString().c_str());
	printf("  -d, --destination CONNFILE\n"
	       "                 The path of a file containing the connection string for the\n");
	printf("                 destination FoundationDB cluster.\n");
	printf("  -s, --source CONNFILE\n"
	       "                 The path of a file containing the connection string for the\n"
		   "                 source FoundationDB cluster.\n");
	printf("  -e ERRORLIMIT  The maximum number of errors printed by status (default is 10).\n");
	printf("  -k KEYS        List of key ranges to backup.\n"
		   "                 If not specified, the entire database will be backed up.\n");
	printf("  --cleanup      Abort will attempt to stop mutation logging on the source cluster.\n");
	printf("  -v, --version  Print version information and exit.\n");
	printf("  -h, --help     Display this help and exit.\n");
	printf("\n"
		   "  KEYS FORMAT:   \"<BEGINKEY> <ENDKEY>\" [...]\n");

	if (devhelp) {
#ifdef _WIN32
		printf("  -n             Create a new console.\n");
		printf("  -q             Disable error dialog on crash.\n");
		printf("  --parentpid PID\n");
		printf("                 Specify a process after whose termination to exit.\n");
#endif
	}

	return;
}

static void printBlobManagerUsage() {
	printf("FoundationDB " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n");
	printf("Usage: %s [options] <command> <arg>)\n\n", exeBlobManager.toString().c_str());
	printf(" Commands:\n");
	printf("  list <url>            Lists the backups found at the given blob store URL.  URL format is\n");
	printf("                            %s\n", BlobStoreEndpoint::getURLFormat().c_str());
	printf("  listinfo <url>        Same as list but shows 'info' output for each backup.\n");
	printf("  info <url>            Scans the given blob store Backup URL and outputs size and object count.  URL format is\n");
	printf("                            %s\n", BackupContainerBlobStore::getURLFormat().c_str());
	printf("  dump <url>            Same as list but also lists all objects and their sizes.\n");
	printf("  delete <url>          Deletes the backup specified by the blob store Backup URL.  URL format is\n");
	printf("                            %s\n", BackupContainerBlobStore::getURLFormat().c_str());
	printf("\n");
	printBlobStoreParameterInfo("                            ");
	printf("  -v, --version         Print version information and exit.\n");
	printf("  -h, --help            Display this help and exit.\n");
	return;
}

static void printUsage(enumProgramExe programExe, bool devhelp)
{

	switch (programExe)
	{
	case EXE_AGENT:
		printAgentUsage(devhelp);
		break;
	case EXE_BACKUP:
		printBackupUsage(devhelp);
		break;
	case EXE_RESTORE:
		printRestoreUsage(devhelp);
		break;
	case EXE_DR_AGENT:
		printDBAgentUsage(devhelp);
		break;
	case EXE_DB_BACKUP:
		printDBBackupUsage(devhelp);
		break;
	case EXE_BLOBMANAGER:
		printBlobManagerUsage();
		break;
	case EXE_UNDEFINED:
	default:
		break;
	}

	return;
}

extern bool g_crashOnError;

// Return the type of program executable based on the name of executable file
enumProgramExe	getProgramType(std::string programExe)
{
	enumProgramExe	enProgramExe = EXE_UNDEFINED;

	// lowercase the string
	std::transform(programExe.begin(), programExe.end(), programExe.begin(), ::tolower);

	// Remove the extension, if Windows
#ifdef _WIN32
	size_t lastDot = programExe.find_last_of(".");
	if (lastDot != std::string::npos) {
		size_t lastSlash = programExe.find_last_of("\\");

		// Ensure last dot is after last slash, if present
		if ((lastSlash == std::string::npos)||
			(lastSlash < lastDot)			)
		{
			programExe = programExe.substr(0, lastDot);
		}
	}
#endif

	// Check if backup agent
	if ((programExe.length() >= exeAgent.size())																		&&
		(programExe.compare(programExe.length()-exeAgent.size(), exeAgent.size(), (const char*) exeAgent.begin()) == 0)	)
	{
		enProgramExe = EXE_AGENT;
	}

	// Check if backup
	else if ((programExe.length() >= exeBackup.size())																	&&
		(programExe.compare(programExe.length() - exeBackup.size(), exeBackup.size(), (const char*)exeBackup.begin()) == 0))
	{
		enProgramExe = EXE_BACKUP;
	}

	// Check if restore
	else if ((programExe.length() >= exeRestore.size())																		&&
		(programExe.compare(programExe.length() - exeRestore.size(), exeRestore.size(), (const char*)exeRestore.begin()) == 0))
	{
		enProgramExe = EXE_RESTORE;
	}

	// Check if db agent
	else if ((programExe.length() >= exeDatabaseAgent.size())																		&&
		(programExe.compare(programExe.length() - exeDatabaseAgent.size(), exeDatabaseAgent.size(), (const char*)exeDatabaseAgent.begin()) == 0))
	{
		enProgramExe = EXE_DR_AGENT;
	}

	// Check if db backup
	else if ((programExe.length() >= exeDatabaseBackup.size())																		&&
		(programExe.compare(programExe.length() - exeDatabaseBackup.size(), exeDatabaseBackup.size(), (const char*)exeDatabaseBackup.begin()) == 0))
	{
		enProgramExe = EXE_DB_BACKUP;
	}

	// Check if blob manager
	else if ((programExe.length() >= exeBlobManager.size())																		&&
		(programExe.compare(programExe.length() - exeBlobManager.size(), exeBlobManager.size(), (const char*)exeBlobManager.begin()) == 0))
	{
		enProgramExe = EXE_BLOBMANAGER;
	}

	return enProgramExe;
}

enumBackupType	getBackupType(std::string backupType)
{
	enumBackupType	enBackupType = BACKUP_UNDEFINED;

	// lowercase the string
	std::transform(backupType.begin(), backupType.end(), backupType.begin(), ::tolower);

	static std::map<std::string, enumBackupType> values;
	if(values.empty()) {
		values["start"] = BACKUP_START;
		values["status"] = BACKUP_STATUS;
		values["abort"] = BACKUP_ABORT;
		values["wait"] = BACKUP_WAIT;
		values["discontinue"] = BACKUP_DISCONTINUE;
	}

	auto i = values.find(backupType);
	if(i != values.end())
		enBackupType = i->second;

	return enBackupType;
}

enumRestoreType getRestoreType(std::string name) {
	if(name == "start") return RESTORE_START;
	if(name == "abort") return RESTORE_ABORT;
	if(name == "status") return RESTORE_STATUS;
	if(name == "wait") return RESTORE_WAIT;
	return RESTORE_UNKNOWN;
}

enumDBType getDBType(std::string dbType)
{
	enumDBType enBackupType = DB_UNDEFINED;

	// lowercase the string
	std::transform(dbType.begin(), dbType.end(), dbType.begin(), ::tolower);

	static std::map<std::string, enumDBType> values;
	if(values.empty()) {
		values["start"] = DB_START;
		values["status"] = DB_STATUS;
		values["switch"] = DB_SWITCH;
		values["abort"] = DB_ABORT;
	}

	auto i = values.find(dbType);
	if(i != values.end())
		enBackupType = i->second;

	return enBackupType;
}

ACTOR Future<std::string> getLayerStatus(Reference<ReadYourWritesTransaction> tr, std::string name, std::string id, enumProgramExe exe, Database dest) {
	// This process will write a document that looks like this:
	// { backup : { $expires : {<subdoc>}, version: <version from approximately 30 seconds from now> }
	// so that the value under 'backup' will eventually expire to null and thus be ignored by
	// readers of status.  This is because if all agents die then they can no longer clean up old
	// status docs from other dead agents.

	state Version readVer = wait(tr->getReadVersion());

	state json_spirit::mValue layersRootValue;         // Will contain stuff that goes into the doc at the layers status root
	JSONDoc layersRoot(layersRootValue);               // Convenient mutator / accessor for the layers root
	JSONDoc op = layersRoot.subDoc(name);              // Operator object for the $expires operation
	// Create the $expires key which is where the rest of the status output will go

	state JSONDoc layerRoot = op.subDoc("$expires");
	// Set the version argument in the $expires operator object.
	op.create("version") = readVer + 120 * CLIENT_KNOBS->CORE_VERSIONSPERSECOND;

	layerRoot.create("instances_running.$sum") = 1;
	layerRoot.create("total_workers.$sum") = CLIENT_KNOBS->BACKUP_TASKS_PER_AGENT;
	layerRoot.create("last_updated.$max") = now();

	state JSONDoc o = layerRoot.subDoc("instances." + id);

	o.create("version") = FDB_VT_VERSION;
	o.create("id")      = id;
	o.create("last_updated") = now();
	o.create("memory_usage")  = (int64_t)getMemoryUsage();
	o.create("resident_size") = (int64_t)getResidentMemoryUsage();
	o.create("main_thread_cpu_seconds") = getProcessorTimeThread();
	o.create("process_cpu_seconds")     = getProcessorTimeProcess();
	o.create("workers") = CLIENT_KNOBS->BACKUP_TASKS_PER_AGENT;

	if(exe == EXE_AGENT) {
		static BlobStoreEndpoint::Stats last_stats;
		static double last_ts = 0;
		BlobStoreEndpoint::Stats current_stats = BlobStoreEndpoint::s_stats;
		JSONDoc blobstats = o.create("blob_stats");
		blobstats.create("total") = current_stats.getJSON();
		BlobStoreEndpoint::Stats diff = current_stats - last_stats;
		json_spirit::mObject diffObj = diff.getJSON();
		if(last_ts > 0)
			diffObj["bytes_per_second"] = double(current_stats.bytes_sent - last_stats.bytes_sent) / (now() - last_ts);
		blobstats.create("recent") = diffObj;
		last_stats = current_stats;
		last_ts = now();

		JSONDoc totalBlobStats = layerRoot.subDoc("blob_recent_io");
		for(auto &p : diffObj)
			totalBlobStats.create(p.first + ".$sum") = p.second;

		state FileBackupAgent fba;
		state Standalone<RangeResultRef> backupTagNames = wait( tr->getRange(fba.tagNames.range(), 10000));
		state std::vector<Future<Version>> tagLastRestorableVersions;
		state std::vector<Future<int>> tagStates;
		state std::vector<Future<std::string>> tagContainers;
		state std::vector<Future<int64_t>> tagRangeBytes;
		state std::vector<Future<int64_t>> tagLogBytes;
		state int i = 0;

		for(i = 0; i < backupTagNames.size(); i++) {
			Standalone<KeyRef> tagName = fba.tagNames.unpack(backupTagNames[i].key).getString(0);
			UID tagUID = BinaryReader::fromStringRef<UID>(backupTagNames[i].value, Unversioned());
			tagLastRestorableVersions.push_back(fba.getLastRestorable(tr, tagName));
			tagStates.push_back(fba.getStateValue(tr, tagUID));
			tagContainers.push_back(fba.getLastBackupContainer(tr, tagUID));
			tagRangeBytes.push_back(fba.getRangeBytesWritten(tr, tagUID));
			tagLogBytes.push_back(fba.getLogBytesWritten(tr, tagUID));
		}

		Void _ = wait( waitForAll(tagLastRestorableVersions) && waitForAll(tagStates) && waitForAll(tagContainers) && waitForAll(tagRangeBytes) && waitForAll(tagLogBytes));

		JSONDoc tagsRoot = layerRoot.subDoc("tags.$latest");
		layerRoot.create("tags.timestamp") = now();

		for (int j = 0; j < backupTagNames.size(); j++) {
			std::string tagName = fba.tagNames.unpack(backupTagNames[j].key).getString(0).toString();

			Version last_restorable_version = tagLastRestorableVersions[j].get();
			double last_restorable_seconds_behind = ((double)readVer - last_restorable_version) / CLIENT_KNOBS->CORE_VERSIONSPERSECOND;
			BackupAgentBase::enumState status = (BackupAgentBase::enumState)tagStates[j].get();
			const char *statusText = fba.getStateText(status);

			// The object for this backup tag inside this instance's subdocument
			JSONDoc tagRoot = tagsRoot.subDoc(tagName);
			tagRoot.create("current_container") = tagContainers[j].get();
			tagRoot.create("current_status") = statusText;
			tagRoot.create("last_restorable_version") = tagLastRestorableVersions[j].get();
			tagRoot.create("last_restorable_seconds_behind") = last_restorable_seconds_behind;
			tagRoot.create("running_backup") = (status == BackupAgentBase::STATE_DIFFERENTIAL || status == BackupAgentBase::STATE_BACKUP);
			tagRoot.create("running_backup_is_restorable") = (status == BackupAgentBase::STATE_DIFFERENTIAL);
			tagRoot.create("range_bytes_written") = tagRangeBytes[j].get();
			tagRoot.create("mutation_log_bytes_written") = tagLogBytes[j].get();
		}
	}
	else if(exe == EXE_DR_AGENT) {
		state DatabaseBackupAgent dba;
		state Reference<ReadYourWritesTransaction> tr2(new ReadYourWritesTransaction(dest));
		tr2->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		tr2->setOption(FDBTransactionOptions::LOCK_AWARE);
		state Standalone<RangeResultRef> tagNames = wait(tr2->getRange(dba.tagNames.range(), 10000));
		state std::vector<Future<Optional<Key>>> backupVersion;
		state std::vector<Future<int>> backupStatus;
		state std::vector<Future<int64_t>> tagRangeBytesDR;
		state std::vector<Future<int64_t>> tagLogBytesDR;

		for(int i = 0; i < tagNames.size(); i++) {
			backupVersion.push_back(tr2->get(tagNames[i].value.withPrefix(applyMutationsBeginRange.begin)));
			UID tagUID = BinaryReader::fromStringRef<UID>(tagNames[i].value, Unversioned());
			backupStatus.push_back(dba.getStateValue(tr2, tagUID));
			tagRangeBytesDR.push_back(dba.getRangeBytesWritten(tr2, tagUID));
			tagLogBytesDR.push_back(dba.getLogBytesWritten(tr2, tagUID));
		}

		Void _ = wait(waitForAll(backupStatus) && waitForAll(backupVersion) && waitForAll(tagRangeBytesDR) && waitForAll(tagLogBytesDR));

		JSONDoc tagsRoot = layerRoot.subDoc("tags.$latest");
		layerRoot.create("tags.timestamp") = now();

		for (int i = 0; i < tagNames.size(); i++) {
			std::string tagName = dba.sourceTagNames.unpack(tagNames[i].key).getString(0).toString();

			BackupAgentBase::enumState status = (BackupAgentBase::enumState)backupStatus[i].get();

			JSONDoc tagRoot = tagsRoot.create(tagName);
			tagRoot.create("running_backup") = (status == BackupAgentBase::STATE_DIFFERENTIAL || status == BackupAgentBase::STATE_BACKUP);
			tagRoot.create("running_backup_is_restorable") = (status == BackupAgentBase::STATE_DIFFERENTIAL);
			tagRoot.create("range_bytes_written") = tagRangeBytesDR[i].get();
			tagRoot.create("mutation_log_bytes_written") = tagLogBytesDR[i].get();

			if (backupVersion[i].get().present()) {
				double seconds_behind = ((double)readVer - BinaryReader::fromStringRef<Version>(backupVersion[i].get().get(), Unversioned())) / CLIENT_KNOBS->CORE_VERSIONSPERSECOND;
				tagRoot.create("seconds_behind") = seconds_behind;
				//TraceEvent("BackupMetrics").detail("secondsBehind", seconds_behind);
			}

			tagRoot.create("backup_state") = BackupAgentBase::getStateText(status);
		}
	}

	std::string json = json_spirit::write_string(layersRootValue);
	return json;
}

// Check for unparseable or expired statuses and delete them.
// First checks the first doc in the key range, and if it is valid, alive and not "me" then
// returns.  Otherwise, checks the rest of the range as well.
ACTOR Future<Void> cleanupStatus(Reference<ReadYourWritesTransaction> tr, std::string rootKey, std::string name, std::string id, int limit = 1) {
	state Standalone<RangeResultRef> docs = wait(tr->getRange(KeyRangeRef(rootKey, strinc(rootKey)), limit, true));
	state bool readMore = false;
	state int i;
	for(i = 0; i < docs.size(); ++i) {
		json_spirit::mValue docValue;
		try {
			json_spirit::read_string(docs[i].value.toString(), docValue);
			JSONDoc doc(docValue);
			// Update the reference version for $expires
			JSONDoc::expires_reference_version = tr->getReadVersion().get();
			// Evaluate the operators in the document, which will reduce to nothing if it is expired.
			doc.cleanOps();
			if(!doc.has(name + ".last_updated"))
				throw Error();

			// Alive and valid.
			// If limit == 1 and id is present then read more
			if(limit == 1 && doc.has(name + ".instances." + id))
				readMore = true;
		} catch(Error &e) {
			// If doc can't be parsed or isn't alive, delete it.
			TraceEvent(SevWarn, "RemovedDeadBackupLayerStatus").detail("Key", printable(docs[i].key));
			tr->clear(docs[i].key);
			// If limit is 1 then read more.
			if(limit == 1)
				readMore = true;
		}
		if(readMore) {
			limit = 10000;
			Standalone<RangeResultRef> docs2 = wait(tr->getRange(KeyRangeRef(rootKey, strinc(rootKey)), limit, true));
			docs = std::move(docs2);
			readMore = false;
		}
	}

	return Void();
}

// Get layer status document for just this layer
ACTOR Future<json_spirit::mObject> getLayerStatus(Database src, std::string rootKey) {
	state Transaction tr(src);

	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			state Standalone<RangeResultRef> kvPairs = wait(tr.getRange(KeyRangeRef(rootKey, strinc(rootKey)), CLIENT_KNOBS->ROW_LIMIT_UNLIMITED));
			json_spirit::mObject statusDoc;
			JSONDoc modifier(statusDoc);
			for(auto &kv : kvPairs) {
				json_spirit::mValue docValue;
				json_spirit::read_string(kv.value.toString(), docValue);
				modifier.absorb(docValue);
			}
			JSONDoc::expires_reference_version = (uint64_t)tr.getReadVersion().get();
			modifier.cleanOps();
			return statusDoc;
		}
		catch (Error& e) {
			Void _ = wait(tr.onError(e));
		}
	}
}

// Read layer status for this layer and get the total count of agent processes (instances) then adjust the poll delay based on that and BACKUP_AGGREGATE_POLL_RATE
ACTOR Future<Void> updateAgentPollRate(Database src, std::string rootKey, std::string name, double *pollDelay) {
	loop {
		try {
			json_spirit::mObject status = wait(getLayerStatus(src, rootKey));
			int64_t processes = 0;
			// If instances count is present and greater than 0 then update pollDelay
			if(JSONDoc(status).tryGet<int64_t>(name + ".instances_running", processes) && processes > 0) {
				// The aggregate poll rate is the target poll rate for all agent processes in the cluster
				// The poll rate (polls/sec) for a single processes is aggregate poll rate / processes, and pollDelay is the inverse of that
				*pollDelay = (double)processes / CLIENT_KNOBS->BACKUP_AGGREGATE_POLL_RATE;
			}
		} catch(Error &e) {
			TraceEvent(SevWarn, "BackupAgentPollRateUpdateError").error(e);
		}
		Void _ = wait(delay(CLIENT_KNOBS->BACKUP_AGGREGATE_POLL_RATE_UPDATE_INTERVAL));
	}
}

ACTOR Future<Void> statusUpdateActor(Database statusUpdateDest, std::string name, enumProgramExe exe, double *pollDelay, Database taskDest = Database() ) {
	state std::string id = g_nondeterministic_random->randomUniqueID().toString();
	state std::string metaKey = layerStatusMetaPrefixRange.begin.toString() + "json/" + name;
	state std::string rootKey = backupStatusPrefixRange.begin.toString() + name + "/json";
	state std::string instanceKey = rootKey + "/" + "agent-" + id;
	state Reference<ReadYourWritesTransaction> tr(new ReadYourWritesTransaction(statusUpdateDest));
	state Future<Void> pollRateUpdater;

	// Register the existence of this layer in the meta key space
	loop {
		try {
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);
			tr->set(metaKey, rootKey);
			Void _ = wait(tr->commit());
			break;
		}
		catch (Error& e) {
			Void _ = wait(tr->onError(e));
		}
	}

	// Write status periodically
	loop {
		tr->reset();
		try {
			loop{
				try {
					tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					tr->setOption(FDBTransactionOptions::LOCK_AWARE);
					state Future<std::string> futureStatusDoc = getLayerStatus(tr, name, id, exe, taskDest);
					Void _ = wait(cleanupStatus(tr, rootKey, name, id));
					std::string statusdoc = wait(futureStatusDoc);
					tr->set(instanceKey, statusdoc);
					Void _ = wait(tr->commit());
					break;
				}
				catch (Error& e) {
					Void _ = wait(tr->onError(e));
				}
			}

			Void _ = wait(delay(CLIENT_KNOBS->BACKUP_STATUS_DELAY * ( ( 1.0 - CLIENT_KNOBS->BACKUP_STATUS_JITTER ) + 2 * g_random->random01() * CLIENT_KNOBS->BACKUP_STATUS_JITTER )));

			// Now that status was written at least once by this process (and hopefully others), start the poll rate control updater if it wasn't started yet
			if(!pollRateUpdater.isValid() && pollDelay != nullptr)
				pollRateUpdater = updateAgentPollRate(statusUpdateDest, rootKey, name, pollDelay);
		}
		catch (Error& e) {
			TraceEvent(SevWarnAlways, "UnableToWriteStatus").error(e);
			Void _ = wait(delay(10.0));
		}
	}
}

ACTOR Future<Void> runDBAgent(Database src, Database dest) {
	state double pollDelay = 1.0 / CLIENT_KNOBS->BACKUP_AGGREGATE_POLL_RATE;
	state Future<Void> status = statusUpdateActor(src, "dr_backup", EXE_DR_AGENT, &pollDelay, dest);
	state Future<Void> status_other = statusUpdateActor(dest, "dr_backup_dest", EXE_DR_AGENT, &pollDelay, dest);

	state DatabaseBackupAgent backupAgent(src);

	loop {
		try {
			state Void run = wait(backupAgent.run(dest, &pollDelay, CLIENT_KNOBS->BACKUP_TASKS_PER_AGENT));
			break;
		}
		catch (Error& e) {
			if (e.code() == error_code_operation_cancelled)
				throw;

			TraceEvent(SevError, "DA_runAgent").error(e);
			fprintf(stderr, "ERROR: DR agent encountered fatal error `%s'\n", e.what());

			Void _ = wait( delay(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY) );
		}
	}

	return Void();
}

ACTOR Future<Void> runAgent(Database db) {
	state double pollDelay = 1.0 / CLIENT_KNOBS->BACKUP_AGGREGATE_POLL_RATE;
	state Future<Void> status = statusUpdateActor(db, "backup", EXE_AGENT, &pollDelay);

	state FileBackupAgent backupAgent;

	loop {
		try {
			state Void run = wait(backupAgent.run(db, &pollDelay, CLIENT_KNOBS->BACKUP_TASKS_PER_AGENT));
			break;
		}
		catch (Error& e) {
			if (e.code() == error_code_operation_cancelled)
				throw;

			TraceEvent(SevError, "BA_runAgent").error(e);
			fprintf(stderr, "ERROR: backup agent encountered fatal error `%s'\n", e.what());

			Void _ = wait( delay(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY) );
		}
	}

	return Void();
}

ACTOR Future<Void> submitDBBackup(Database src, Database dest, Standalone<VectorRef<KeyRangeRef>> backupRanges, std::string tagName) {
	try
	{
		state DatabaseBackupAgent backupAgent(src);

		// Backup everything, if no ranges were specified
		if (backupRanges.size() == 0) {
			backupRanges.push_back_deep(backupRanges.arena(), normalKeys);
		}


		Void _ = wait(backupAgent.submitBackup(dest, KeyRef(tagName), backupRanges, false, StringRef(), StringRef(), true));

		// Check if a backup agent is running
		bool agentRunning = wait(backupAgent.checkActive(dest));

		if (!agentRunning) {
			printf("The DR on tag `%s' was successfully submitted but no DR agents are responding.\n", printable(StringRef(tagName)).c_str());

			// Throw an error that will not display any additional information
			throw actor_cancelled();
		}
		else {
			printf("The DR on tag `%s' was successfully submitted.\n", printable(StringRef(tagName)).c_str());
		}
	}

	catch (Error& e) {
		if(e.code() == error_code_actor_cancelled)
			throw;
		switch (e.code())
		{
			case error_code_backup_error:
				fprintf(stderr, "ERROR: An error was encountered during submission\n");
			break;
			case error_code_backup_duplicate:
				fprintf(stderr, "ERROR: A DR is already running on tag `%s'\n", printable(StringRef(tagName)).c_str());
			break;
			default:
				fprintf(stderr, "ERROR: %s\n", e.what());
			break;
		}

		throw backup_error();
	}

	return Void();
}

ACTOR Future<Void> submitBackup(Database db, std::string destinationDir, Standalone<VectorRef<KeyRangeRef>> backupRanges, std::string tagName, bool dryRun, bool waitForCompletion, bool stopWhenDone) {
	try
	{
		state FileBackupAgent backupAgent;

		// Backup everything, if no ranges were specified
		if (backupRanges.size() == 0) {
			backupRanges.push_back_deep(backupRanges.arena(), normalKeys);
		}

		if (dryRun) {
			state UID logUid = wait(backupAgent.getLogUid(db, StringRef(tagName)));
			state int backupStatus = wait(backupAgent.getStateValue(db, logUid));

			// Throw error if a backup is currently running until we support parallel backups
			if (BackupAgentBase::isRunnable((BackupAgentBase::enumState)backupStatus)) {
				throw backup_duplicate();
			}

			if (waitForCompletion) {
				printf("Submitted and now waiting for the backup on tag `%s' to complete. (DRY RUN)\n", printable(StringRef(tagName)).c_str());
			}

			else {
				// Check if a backup agent is running
				bool agentRunning = wait(backupAgent.checkActive(db));

				if (!agentRunning) {
					printf("The backup on tag `%s' was successfully submitted but no backup agents are responding. (DRY RUN)\n", printable(StringRef(tagName)).c_str());

					// Throw an error that will not display any additional information
					throw actor_cancelled();
				}
				else {
					printf("The backup on tag `%s' was successfully submitted. (DRY RUN)\n", printable(StringRef(tagName)).c_str());
				}
			}
		}

		else {
			Void _ = wait(backupAgent.submitBackup(db, KeyRef(destinationDir), KeyRef(tagName), backupRanges, stopWhenDone));

			// Wait for the backup to complete, if requested
			if (waitForCompletion) {
				printf("Submitted and now waiting for the backup on tag `%s' to complete.\n", printable(StringRef(tagName)).c_str());
				int _ = wait(backupAgent.waitBackup(db, StringRef(tagName)));
			}
			else {
				// Check if a backup agent is running
				bool agentRunning = wait(backupAgent.checkActive(db));

				if (!agentRunning) {
					printf("The backup on tag `%s' was successfully submitted but no backup agents are responding.\n", printable(StringRef(tagName)).c_str());

					// Throw an error that will not display any additional information
					throw actor_cancelled();
				}
				else {
					printf("The backup on tag `%s' was successfully submitted.\n", printable(StringRef(tagName)).c_str());
				}
			}
		}
	}
	catch (Error& e) {
		if(e.code() == error_code_actor_cancelled)
			throw;
		switch (e.code())
		{
			case error_code_backup_error:
				fprintf(stderr, "ERROR: An error was encountered during submission\n");
			break;
			case error_code_backup_duplicate:
				fprintf(stderr, "ERROR: A backup is already running on tag `%s'\n", printable(StringRef(tagName)).c_str());
			break;
			default:
				fprintf(stderr, "ERROR: %s\n", e.what());
			break;
		}

		throw backup_error();
	}

	return Void();
}

ACTOR Future<Void> switchDBBackup(Database src, Database dest, Standalone<VectorRef<KeyRangeRef>> backupRanges, std::string tagName) {
	try
	{
		state DatabaseBackupAgent backupAgent(src);

		// Backup everything, if no ranges were specified
		if (backupRanges.size() == 0) {
			backupRanges.push_back_deep(backupRanges.arena(), normalKeys);
		}


		Void _ = wait(backupAgent.atomicSwitchover(dest, KeyRef(tagName), backupRanges, StringRef(), StringRef()));
		printf("The DR on tag `%s' was successfully switched.\n", printable(StringRef(tagName)).c_str());
	}

	catch (Error& e) {
		if(e.code() == error_code_actor_cancelled)
			throw;
		switch (e.code())
		{
			case error_code_backup_error:
				fprintf(stderr, "ERROR: An error was encountered during submission\n");
			break;
			case error_code_backup_duplicate:
				fprintf(stderr, "ERROR: A DR is already running on tag `%s'\n", printable(StringRef(tagName)).c_str());
			break;
			default:
				fprintf(stderr, "ERROR: %s\n", e.what());
			break;
		}

		throw backup_error();
	}

	return Void();
}

ACTOR Future<Void> statusDBBackup(Database src, Database dest, std::string tagName, int errorLimit) {
	try
	{
		state DatabaseBackupAgent backupAgent(src);

		std::string	statusText = wait(backupAgent.getStatus(dest, errorLimit, StringRef(tagName)));
		printf("%s\n", statusText.c_str());
	}
	catch (Error& e) {
		if(e.code() == error_code_actor_cancelled)
			throw;
		fprintf(stderr, "ERROR: %s\n", e.what());
		throw;
	}

	return Void();
}

ACTOR Future<Void> statusBackup(Database db, std::string tagName, int errorLimit) {
	try
	{
		state FileBackupAgent backupAgent;

		std::string	statusText = wait(backupAgent.getStatus(db, errorLimit, StringRef(tagName)));
		printf("%s\n", statusText.c_str());
	}
	catch (Error& e) {
		if(e.code() == error_code_actor_cancelled)
			throw;
		fprintf(stderr, "ERROR: %s\n", e.what());
		throw;
	}

	return Void();
}

ACTOR Future<Void> abortDBBackup(Database src, Database dest, std::string tagName, bool partial) {
	try
	{
		state DatabaseBackupAgent backupAgent(src);

		Void _ = wait(backupAgent.abortBackup(dest, Key(tagName), partial));
		Void _ = wait(backupAgent.unlockBackup(dest, Key(tagName)));

		printf("The DR on tag `%s' was successfully aborted.\n", printable(StringRef(tagName)).c_str());
	}
	catch (Error& e) {
		if(e.code() == error_code_actor_cancelled)
			throw;
		switch (e.code())
		{
			case error_code_backup_error:
				fprintf(stderr, "ERROR: An error was encountered during submission\n");
			break;
			case error_code_backup_unneeded:
				fprintf(stderr, "ERROR: A DR was not running on tag `%s'\n", printable(StringRef(tagName)).c_str());
			break;
			default:
				fprintf(stderr, "ERROR: %s\n", e.what());
			break;
		}
		throw;
	}

	return Void();
}

ACTOR Future<Void> abortBackup(Database db, std::string tagName) {
	try
	{
		state FileBackupAgent backupAgent;

		Void _ = wait(backupAgent.abortBackup(db, Key(tagName)));

		printf("The backup on tag `%s' was successfully aborted.\n", printable(StringRef(tagName)).c_str());
	}
	catch (Error& e) {
		if(e.code() == error_code_actor_cancelled)
			throw;
		switch (e.code())
		{
			case error_code_backup_error:
				fprintf(stderr, "ERROR: An error was encountered during submission\n");
			break;
			case error_code_backup_unneeded:
				fprintf(stderr, "ERROR: A backup was not running on tag `%s'\n", printable(StringRef(tagName)).c_str());
			break;
			default:
				fprintf(stderr, "ERROR: %s\n", e.what());
			break;
		}
		throw;
	}

	return Void();
}

ACTOR Future<Void> waitBackup(Database db, std::string tagName, bool stopWhenDone) {
	try
	{
		state FileBackupAgent backupAgent;

		int status = wait(backupAgent.waitBackup(db, StringRef(tagName), stopWhenDone));

		printf("The backup on tag `%s' %s.\n", printable(StringRef(tagName)).c_str(),
			BackupAgentBase::getStateText((BackupAgentBase::enumState) status));
	}
	catch (Error& e) {
		if(e.code() == error_code_actor_cancelled)
			throw;
		fprintf(stderr, "ERROR: %s\n", e.what());
		throw;
	}

	return Void();
}

ACTOR Future<Void> discontinueBackup(Database db, std::string tagName, bool waitForCompletion) {
	try
	{
		state FileBackupAgent backupAgent;

		Void _ = wait(backupAgent.discontinueBackup(db, StringRef(tagName)));

		// Wait for the backup to complete, if requested
		if (waitForCompletion) {
			printf("Discontinued and now waiting for the backup on tag `%s' to complete.\n", printable(StringRef(tagName)).c_str());
			int _ = wait(backupAgent.waitBackup(db, StringRef(tagName)));
		}
		else {
			printf("The backup on tag `%s' was successfully discontinued.\n", printable(StringRef(tagName)).c_str());
		}

	}
	catch (Error& e) {
		if(e.code() == error_code_actor_cancelled)
			throw;
		switch (e.code())
		{
			case error_code_backup_error:
				fprintf(stderr, "ERROR: An encounter was error during submission\n");
			break;
			case error_code_backup_unneeded:
				fprintf(stderr, "ERROR: A backup in not running on tag `%s'\n", printable(StringRef(tagName)).c_str());
			break;
			case error_code_backup_duplicate:
				fprintf(stderr, "ERROR: The backup on tag `%s' is already discontinued\n", printable(StringRef(tagName)).c_str());
			break;
			default:
				fprintf(stderr, "ERROR: %s\n", e.what());
			break;
		}
		throw;
	}

	return Void();
}

ACTOR Future<Void> runRestore(Database db, std::string tagName, std::string container, Standalone<VectorRef<KeyRangeRef>> ranges, Version dbVersion, bool performRestore, bool verbose, bool waitForDone, std::string addPrefix, std::string removePrefix) {
	try
	{
		state FileBackupAgent backupAgent;
		state int64_t restoreVersion = -1;

		if(ranges.size() > 1) {
			fprintf(stderr, "Currently only a single restore range is supported!\n");
			throw restore_error();
		}

		KeyRange range = (ranges.size() == 0) ? normalKeys : ranges.front();

		if (performRestore) {
			Version _restoreVersion = wait(backupAgent.restore(db, KeyRef(tagName), KeyRef(container), waitForDone, dbVersion, verbose, range, KeyRef(addPrefix), KeyRef(removePrefix)));
			restoreVersion = _restoreVersion;
		}
		else {
			state Version defaultRestoreVersion = -1;

			// Get the folder information
			std::string info = wait(FileBackupAgent::getBackupInfo(container, &defaultRestoreVersion));

			restoreVersion = (int64_t) (dbVersion > 0) ? dbVersion : defaultRestoreVersion;

			// Display the restore information, if requested
			if (verbose) {
				printf("[DRY RUN] Restoring backup to version: %lld\n", (long long) restoreVersion);
				printf("%s\n", info.c_str());
			}
		}

		if(waitForDone && verbose) {
			// If restore completed then report version restored
			printf("Restored to version %lld%s\n", (long long) restoreVersion, (performRestore) ? "" : " (DRY RUN)");
		}
	}
	catch (Error& e) {
		if(e.code() == error_code_actor_cancelled)
			throw;
		fprintf(stderr, "ERROR: %s\n", e.what());
		throw;
	}

	return Void();
}

ACTOR Future<int> doBlobDelete(std::string url) {
	state std::string error;
	try {
		state Reference<IBackupContainer> c = IBackupContainer::openContainer(url, &error);
	} catch(Error &e) {
		fprintf(stderr, "ERROR:  Invalid blobstore URL: %s (%s) Format is: %s\n", url.c_str(), error.c_str(), BackupContainerBlobStore::getURLFormat().c_str());
		return FDB_EXIT_ERROR;
	}

	state int pNumDeleted = 0;
	state Future<Void> f = ((BackupContainerBlobStore *)c.getPtr())->deleteContainer(&pNumDeleted);
	loop {
		choose {
			when(Void _ = wait(f)) {
				break;
			}
			when(Void _ = wait(delay(3.0))) {
				printf("%d objects deleted so far...\n", pNumDeleted);
			}
		}
	}
	printf("Done. %d objects deleted.\n", pNumDeleted);
	return FDB_EXIT_SUCCESS;
}

ACTOR Future<int> doBlobInfo(std::string url, bool showObjects = false) {
	state std::string error;
	try {
		state Reference<IBackupContainer> c = IBackupContainer::openContainer(url, &error);
	} catch(Error &e) {
		fprintf(stderr, "ERROR:  Invalid blobstore URL: %s (%s) Format is: %s\n", url.c_str(), error.c_str(), BackupContainerBlobStore::getURLFormat().c_str());
		return FDB_EXIT_ERROR;
	}

	state BackupContainerBlobStore *bc = (BackupContainerBlobStore *)c.getPtr();
	state PromiseStream<BlobStoreEndpoint::ObjectInfo> resultsStream;
	state Future<Void> done = bc->listFilesStream(resultsStream);
	state int64_t total_bytes = 0;
	state int64_t total_objects = 0;

	try {
		loop {
			choose {
				when(Void _ = wait(done)) {
					break;
				}
				when(BlobStoreEndpoint::ObjectInfo info = waitNext(resultsStream.getFuture())) {
					++total_objects;
					total_bytes += info.size;
					if(showObjects)
						printf("\t%lld\t%s/%s\n", info.size, info.bucket.c_str(), info.name.c_str());
				}
			}
		}
	} catch(Error &e) {
		printf("ERROR (%s) on %s\n", e.what(), url.c_str());
		return FDB_EXIT_ERROR;
	}

	printf("%lld\t%lld\t%s\n", total_bytes, total_objects, url.c_str());

	return FDB_EXIT_SUCCESS;
}

ACTOR Future<int> doBlobList(std::string url, bool deep = false) {
	state Reference<BlobStoreEndpoint> bse;
	state std::string error;
	try {
		bse = BlobStoreEndpoint::fromString(url, NULL, &error);
	} catch(Error &e) {
		fprintf(stderr, "ERROR:  Invalid blobstore endpoint: %s (%s).  Must look like this: %s\n", url.c_str(), error.c_str(), BlobStoreEndpoint::getURLFormat().c_str());
		return FDB_EXIT_ERROR;
	}

	state std::vector<std::string> results = wait(BackupContainerBlobStore::listBackupContainers(bse));
	state std::vector<std::string>::iterator i;
	state int status = FDB_EXIT_SUCCESS;
	for(i = results.begin(); i != results.end(); ++i) {
		std::string url = bse->getResourceURL(*i);
		if(!deep)
			printf("%s\n", url.c_str());
		else {
			int r = wait(doBlobInfo(url));
			if(status == FDB_EXIT_SUCCESS)
				status = r;
		}
	}

	return status;
}

ACTOR Future<int> doBlobCommand(std::vector<std::string> args) {
	if(args.size() < 2) {
		printBlobManagerUsage();
		return FDB_EXIT_ERROR;
	}

	state std::string cmd = args[0];

	if(cmd == "-h" || cmd == "--help") {
		printBlobManagerUsage();
		return FDB_EXIT_ERROR;
	}

	try {
		if(cmd == "list") {
			int r = wait(doBlobList(args[1]));
			return r;
		}
		if(cmd == "listinfo") {
			printf("BYTES\tOBJECTS\tURL\n");
			int r = wait(doBlobList(args[1], true));
			return r;
		}
		else if(cmd == "delete") {
			int r = wait(doBlobDelete(args[1]));
			return r;
		}
		else if(cmd == "info") {
			printf("BYTES\tOBJECTS\tURL\n");
			int r = wait(doBlobInfo(args[1]));
			return r;
		}
		else if(cmd == "dump") {
			int r = wait(doBlobInfo(args[1], true));
			return r;
		}
		else {
			printf("ERROR:  Unknown command: '%s'\n", cmd.c_str());
			printBlobManagerUsage();
			return FDB_EXIT_ERROR;
		}
	} catch(Error &e) {
		fprintf(stderr, "ERROR:  Blob command '%s' failed:  %s\n", cmd.c_str(), e.what());
		throw;
	}
}

static std::vector<std::vector<StringRef>> parseLine(std::string &line, bool& err, bool& partial)
{
	err = false;
	partial = false;

	bool quoted = false;
	std::vector<StringRef> buf;
	std::vector<std::vector<StringRef>> ret;

	size_t i = line.find_first_not_of(' ');
	size_t offset = i;

	bool forcetoken = false;

	while (i <= line.length()) {
		switch (line[i]) {
		case ';':
			if (!quoted) {
				if (i > offset)
					buf.push_back(StringRef((uint8_t*)(line.data() + offset), i - offset));
				ret.push_back(std::move(buf));
				offset = i = line.find_first_not_of(' ', i + 1);
			}
			else
				i++;
			break;
		case '"':
			quoted = !quoted;
			line.erase(i, 1);
			if (quoted)
				forcetoken = true;
			break;
		case ' ':
			if (!quoted) {
				buf.push_back(StringRef((uint8_t *)(line.data() + offset),
					i - offset));
				offset = i = line.find_first_not_of(' ', i);
				forcetoken = false;
			}
			else
				i++;
			break;
		case '\\':
			if (i + 2 > line.length()) {
				err = true;
				ret.push_back(std::move(buf));
				return ret;
			}
			switch (line[i + 1]) {
				char ent, save;
			case '"':
			case '\\':
			case ' ':
			case ';':
				line.erase(i, 1);
				break;
			case 'x':
				if (i + 4 > line.length()) {
					err = true;
					ret.push_back(std::move(buf));
					return ret;
				}
				char *pEnd;
				save = line[i + 4];
				line[i + 4] = 0;
				ent = char(strtoul(line.data() + i + 2, &pEnd, 16));
				if (*pEnd) {
					err = true;
					ret.push_back(std::move(buf));
					return ret;
				}
				line[i + 4] = save;
				line.replace(i, 4, 1, ent);
				break;
			default:
				err = true;
				ret.push_back(std::move(buf));
				return ret;
			}
		default:
			i++;
		}
	}

	i -= 1;
	if (i > offset || forcetoken)
		buf.push_back(StringRef((uint8_t*)(line.data() + offset), i - offset));

	ret.push_back(std::move(buf));

	if (quoted)
		partial = true;

	return ret;
}

static void addKeyRange(std::string optionValue, Standalone<VectorRef<KeyRangeRef>>& keyRanges)
{
	bool	err = false, partial = false;
	int	tokenArray = 0, tokenIndex = 0;

	auto parsed = parseLine(optionValue, err, partial);

	for (auto tokens : parsed)
	{
		tokenArray++;
		tokenIndex = 0;

		/*
		for (auto token : tokens)
		{
			tokenIndex++;

			printf("%4d token #%2d: %s\n", tokenArray, tokenIndex, printable(token).c_str());
		}
		*/

		// Process the keys
		// <begin> [end]
		switch (tokens.size())
		{
			// empty
		case 0:
			break;

			// single key range
		case 1:
				keyRanges.push_back_deep(keyRanges.arena(), KeyRangeRef(tokens.at(0), strinc(tokens.at(0))));
			break;

			// full key range
		case 2:
			try {
				keyRanges.push_back_deep(keyRanges.arena(), KeyRangeRef(tokens.at(0), tokens.at(1)));
			}
			catch (Error& e) {
				fprintf(stderr, "ERROR: Invalid key range `%s %s' reported error %s\n",
					tokens.at(0).toString().c_str(), tokens.at(1).toString().c_str(), e.what());
				throw invalid_option_value();
			}
			break;

			// Too many keys
		default:
			fprintf(stderr, "ERROR: Invalid key range identified with %ld keys", tokens.size());
			throw invalid_option_value();
			break;
		}
	}

	return;
}

#ifdef ALLOC_INSTRUMENTATION
extern uint8_t *g_extra_memory;
#endif

int main(int argc, char* argv[]) {
	platformInit();

	int	status = FDB_EXIT_SUCCESS;

	try {
#ifdef ALLOC_INSTRUMENTATION
		g_extra_memory = new uint8_t[1000000];
#endif
		registerCrashHandler();

		// Set default of line buffering standard out and error
		setvbuf(stdout, NULL, _IONBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);

		enumProgramExe programExe = getProgramType(argv[0]);
		enumBackupType backupType = BACKUP_UNDEFINED;
		enumRestoreType restoreType = RESTORE_UNKNOWN;
		enumDBType dbType = DB_UNDEFINED;

		CSimpleOpt* args = NULL;

		switch (programExe)
		{
		case EXE_AGENT:
			args = new CSimpleOpt(argc, argv, g_rgAgentOptions, SO_O_EXACT);
			break;
		case EXE_DR_AGENT:
			args = new CSimpleOpt(argc, argv, g_rgDBAgentOptions, SO_O_EXACT);
			break;
		case EXE_BACKUP:
			// Display backup help, if no arguments
			if (argc < 2) {
				printBackupUsage(false);
				return FDB_EXIT_ERROR;
			}
			else {
				// Get the backup type
				backupType = getBackupType(argv[1]);

				// Create the appropriate simple opt
				switch (backupType)
				{
				case BACKUP_START:
					args = new CSimpleOpt(argc-1, &argv[1], g_rgBackupStartOptions, SO_O_EXACT);
					break;
				case BACKUP_STATUS:
					args = new CSimpleOpt(argc - 1, &argv[1], g_rgBackupStatusOptions, SO_O_EXACT);
					break;
				case BACKUP_ABORT:
					args = new CSimpleOpt(argc - 1, &argv[1], g_rgBackupAbortOptions, SO_O_EXACT);
					break;
				case BACKUP_WAIT:
					args = new CSimpleOpt(argc - 1, &argv[1], g_rgBackupWaitOptions, SO_O_EXACT);
					break;
				case BACKUP_DISCONTINUE:
					args = new CSimpleOpt(argc - 1, &argv[1], g_rgBackupDiscontinueOptions, SO_O_EXACT);
					break;
				case BACKUP_UNDEFINED:
				default:
					// Display help, if requested
					if ((strcmp(argv[1], "-h") == 0)		||
						(strcmp(argv[1], "--help") == 0)	)
					{
						printBackupUsage(false);
						return FDB_EXIT_ERROR;
					}
					else {
						fprintf(stderr, "ERROR: Unsupported backup action %s\n", argv[1]);
						printHelpTeaser(argv[0]);
						return FDB_EXIT_ERROR;
					}
					break;
				}
			}
			break;
		case EXE_DB_BACKUP:
			// Display backup help, if no arguments
			if (argc < 2) {
				printDBBackupUsage(false);
				return FDB_EXIT_ERROR;
			}
			else {
				// Get the backup type
				dbType = getDBType(argv[1]);

				// Create the appropriate simple opt
				switch (dbType)
				{
				case DB_START:
					args = new CSimpleOpt(argc-1, &argv[1], g_rgDBStartOptions, SO_O_EXACT);
					break;
				case DB_STATUS:
					args = new CSimpleOpt(argc - 1, &argv[1], g_rgDBStatusOptions, SO_O_EXACT);
					break;
				case DB_SWITCH:
					args = new CSimpleOpt(argc - 1, &argv[1], g_rgDBSwitchOptions, SO_O_EXACT);
					break;
				case DB_ABORT:
					args = new CSimpleOpt(argc - 1, &argv[1], g_rgDBAbortOptions, SO_O_EXACT);
					break;
				case DB_UNDEFINED:
				default:
					// Display help, if requested
					if ((strcmp(argv[1], "-h") == 0)		||
						(strcmp(argv[1], "--help") == 0)	)
					{
						printDBBackupUsage(false);
						return FDB_EXIT_ERROR;
					}
					else {
						fprintf(stderr, "ERROR: Unsupported dr action %s %d\n", argv[1], dbType);
						printHelpTeaser(argv[0]);
						return FDB_EXIT_ERROR;
					}
					break;
				}
			}
			break;
		case EXE_BLOBMANAGER:
			args = new CSimpleOpt(argc, argv, g_rgBlobOptions, SO_O_NOERR);
			break;
		case EXE_RESTORE:
			if (argc < 2) {
				printRestoreUsage(false);
				return FDB_EXIT_ERROR;
			}
			// Get the restore operation type
			restoreType = getRestoreType(argv[1]);
			if(restoreType == RESTORE_UNKNOWN) {
				// Display help, if requested
				if ((strcmp(argv[1], "-h") == 0)		||
					(strcmp(argv[1], "--help") == 0)	)
				{
					printRestoreUsage(false);
					return FDB_EXIT_ERROR;
				}
				else {
					fprintf(stderr, "ERROR: Unsupported restore command: '%s'\n", argv[1]);
					printHelpTeaser(argv[0]);
					return FDB_EXIT_ERROR;
				}
			}
			args = new CSimpleOpt(argc - 1, argv + 1, g_rgRestoreOptions, SO_O_EXACT);
			break;
		case EXE_UNDEFINED:
		default:
			fprintf(stderr, "FoundationDB " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n");
			fprintf(stderr, "ERROR: Unable to determine program type based on executable `%s'\n", argv[0]);
			return FDB_EXIT_ERROR;
			break;
		}

		std::string destinationContainer;
		std::string clusterFile;
		std::string sourceClusterFile;
		std::vector<std::pair<std::string, std::string>> knobs;
		std::string tagName = BackupAgentBase::getDefaultTag().toString();
		bool tagProvided = false;
		std::string restoreContainer;
		std::string addPrefix;
		std::string removePrefix;
		Standalone<VectorRef<KeyRangeRef>> backupKeys;
		int maxErrors = 20;
		Version dbVersion = 0;
		bool waitForDone = false;
		bool stopWhenDone = true;
		bool forceAction = false;
		bool trace = false;
		bool quietDisplay = false;
		bool dryRun = false;
		std::string traceDir = "";
		std::string traceLogGroup;
		ESOError	lastError;
		bool partial = true;

		std::vector<std::string> blobArgs;

		if( argc == 1 ) {
			printUsage(programExe, false);
			return FDB_EXIT_ERROR; 
		}

	#ifdef _WIN32
		// Windows needs a gentle nudge to format floats correctly
		//_set_output_format(_TWO_DIGIT_EXPONENT);
	#endif

		while (args->Next()) {
			lastError = args->LastError();

			switch (lastError)
			{
			case SO_SUCCESS:
				break;

			case SO_ARG_INVALID_DATA:
				fprintf(stderr, "ERROR: invalid argument to option `%s'\n", args->OptionText());
				printHelpTeaser(argv[0]);
				return FDB_EXIT_ERROR;
				break;

			case SO_ARG_INVALID:
				fprintf(stderr, "ERROR: argument given for option `%s'\n", args->OptionText());
				printHelpTeaser(argv[0]);
				return FDB_EXIT_ERROR;
				break;

			case SO_ARG_MISSING:
				fprintf(stderr, "ERROR: missing argument for option `%s'\n", args->OptionText());
				printHelpTeaser(argv[0]);
				return FDB_EXIT_ERROR;

			case SO_OPT_INVALID:
				fprintf(stderr, "ERROR: unknown option `%s'\n", args->OptionText());
				printHelpTeaser(argv[0]);
				return FDB_EXIT_ERROR;
				break;

			default:
				fprintf(stderr, "ERROR: argument given for option `%s'\n", args->OptionText());
				printHelpTeaser(argv[0]);
				return FDB_EXIT_ERROR;
				break;
			}

			switch (args->OptionId()) {
				case OPT_HELP:
					printUsage(programExe, false);
					return FDB_EXIT_SUCCESS;
					break;
				case OPT_DEVHELP:
					printUsage(programExe, true);
					return FDB_EXIT_SUCCESS;
					break;
				case OPT_VERSION:
					printVersion();
					return FDB_EXIT_SUCCESS;
					break;
				case OPT_NOBUFSTDOUT:
					setvbuf(stdout, NULL, _IONBF, 0);
					setvbuf(stderr, NULL, _IONBF, 0);
					break;
				case OPT_BUFSTDOUTERR:
					setvbuf(stdout, NULL, _IOFBF, BUFSIZ);
					setvbuf(stderr, NULL, _IOFBF, BUFSIZ);
					break;
				case OPT_QUIET:
					quietDisplay = true;
					break;
				case OPT_DRYRUN:
					dryRun = true;
					break;
				case OPT_FORCE:
					forceAction = true;
					break;
				case OPT_TRACE:
					trace = true;
					break;
				case OPT_TRACE_DIR:
					trace = true;
					traceDir = args->OptionArg();
					break;
				case OPT_TRACE_LOG_GROUP:
					traceLogGroup = args->OptionArg();
					break;
				case OPT_CLUSTERFILE:
					clusterFile = args->OptionArg();
					break;
				case OPT_DEST_CLUSTER:
					clusterFile = args->OptionArg();
					break;
				case OPT_SOURCE_CLUSTER:
					sourceClusterFile = args->OptionArg();
					break;
				case OPT_CLEANUP:
					partial = false;
					break;
				case OPT_KNOB: {
					std::string syn = args->OptionSyntax();
					if (!StringRef(syn).startsWith(LiteralStringRef("--knob_"))) {
						fprintf(stderr, "ERROR: unable to parse knob option '%s'\n", syn.c_str());
						return FDB_EXIT_ERROR;
					}
					syn = syn.substr(7);
					knobs.push_back( std::make_pair( syn, args->OptionArg() ) );
					break;
					}
				case OPT_BACKUPKEYS:
					try {
						addKeyRange(args->OptionArg(), backupKeys);
					}
					catch (Error &) {
						printHelpTeaser(argv[0]);
						return FDB_EXIT_ERROR;
					}
					break;
				case OPT_DESTCONTAINER:
					destinationContainer = args->OptionArg();
					// If the url starts with '/' then prepend "file://" for backwards compatibility
					if(StringRef(destinationContainer).startsWith(LiteralStringRef("/")))
						destinationContainer = std::string("file://") + destinationContainer;
					break;
				case OPT_WAITFORDONE:
					waitForDone = true;
					break;
				case OPT_NOSTOPWHENDONE:
					stopWhenDone = false;
					break;
				case OPT_RESTORECONTAINER:
					restoreContainer = args->OptionArg();
					// If the url starts with '/' then prepend "file://" for backwards compatibility
					if(StringRef(restoreContainer).startsWith(LiteralStringRef("/")))
						restoreContainer = std::string("file://") + restoreContainer;
					break;
				case OPT_PREFIX_ADD:
					addPrefix = args->OptionArg();
					break;
				case OPT_PREFIX_REMOVE:
					removePrefix = args->OptionArg();
					break;
				case OPT_ERRORLIMIT: {
					const char* a = args->OptionArg();
					if (!sscanf(a, "%d", &maxErrors)) {
						fprintf(stderr, "ERROR: Could not parse max number of errors `%s'\n", a);
						printHelpTeaser(argv[0]);
						return FDB_EXIT_ERROR;
					}
					break;
				}
				case OPT_DBVERSION: {
					const char* a = args->OptionArg();
					long long dbVersionValue = 0;
					if (!sscanf(a, "%lld", &dbVersionValue)) {
						fprintf(stderr, "ERROR: Could not parse database version `%s'\n", a);
						printHelpTeaser(argv[0]);
						return FDB_EXIT_ERROR;
					}
					dbVersion = dbVersionValue;
					break;
				}
	#ifdef _WIN32
				case OPT_PARENTPID: {
					auto pid_str = args->OptionArg();
					int parent_pid = atoi(pid_str);
					auto pHandle = OpenProcess( SYNCHRONIZE, FALSE, parent_pid );
					if( !pHandle ) {
						TraceEvent("ParentProcessOpenError").GetLastError();
						fprintf(stderr, "Could not open parent process at pid %d (error %d)", parent_pid, GetLastError());
						throw platform_error();
					}
					startThread(&parentWatcher, pHandle);
					break;
				}
	#endif
				case OPT_TAGNAME:
					tagName = args->OptionArg();
					tagProvided = true;
					break;
				case OPT_CRASHONERROR:
					g_crashOnError = true;
					break;
			}
		}

		// Process the extra arguments
		for (int argLoop = 0; argLoop < args->FileCount(); argLoop++)
		{
			switch (programExe)
			{
			case EXE_AGENT:
				fprintf(stderr, "ERROR: Backup Agent does not support argument value `%s'\n", args->File(argLoop));
				printHelpTeaser(argv[0]);
				return FDB_EXIT_ERROR;
				break;

				// Add the backup key range
			case EXE_BACKUP:
				// Error, if the keys option was not specified
				if (backupKeys.size() == 0) {
					fprintf(stderr, "ERROR: Unknown backup option value `%s'\n", args->File(argLoop));
					printHelpTeaser(argv[0]);
					return FDB_EXIT_ERROR;
				}
				// Otherwise, assume the item is a key range
				else {
					try {
						addKeyRange(args->File(argLoop), backupKeys);
					}
					catch (Error& e) {
						printHelpTeaser(argv[0]);
						return FDB_EXIT_ERROR;
					}
				}
				break;

			case EXE_RESTORE:
				fprintf(stderr, "ERROR: FDB Restore does not support argument value `%s'\n", args->File(argLoop));
				printHelpTeaser(argv[0]);
				return FDB_EXIT_ERROR;
				break;

			case EXE_DR_AGENT:
				fprintf(stderr, "ERROR: DR Agent does not support argument value `%s'\n", args->File(argLoop));
				printHelpTeaser(argv[0]);
				return FDB_EXIT_ERROR;
				break;

			case EXE_DB_BACKUP:
				// Error, if the keys option was not specified
				if (backupKeys.size() == 0) {
					fprintf(stderr, "ERROR: Unknown DR option value `%s'\n", args->File(argLoop));
					printHelpTeaser(argv[0]);
					return FDB_EXIT_ERROR;
				}
				// Otherwise, assume the item is a key range
				else {
					try {
						addKeyRange(args->File(argLoop), backupKeys);
					}
					catch (Error& e) {
						printHelpTeaser(argv[0]);
						return FDB_EXIT_ERROR;
					}
				}
				break;

			case EXE_BLOBMANAGER:
				blobArgs.push_back(args->File(argLoop));
				break;

			case EXE_UNDEFINED:
			default:
				return FDB_EXIT_ERROR;
			}
		}

		// Delete the simple option object, if defined
		if (args)
		{
			delete args;
			args = NULL;
		}

		std::string commandLine;
		for(int a=0; a<argc; a++) {
			if (a) commandLine += ' ';
			commandLine += argv[a];
		}

		delete CLIENT_KNOBS;
		ClientKnobs* clientKnobs = new ClientKnobs(true);
		CLIENT_KNOBS = clientKnobs;

		for(auto k=knobs.begin(); k!=knobs.end(); ++k) {
			try {
				if (!clientKnobs->setKnob( k->first, k->second )) {
					fprintf(stderr, "Unrecognized knob option '%s'\n", k->first.c_str());
					return FDB_EXIT_ERROR;
				}
			} catch (Error& e) {
				if (e.code() == error_code_invalid_option_value) {
					fprintf(stderr, "Invalid value '%s' for option '%s'\n", k->second.c_str(), k->first.c_str());
					return FDB_EXIT_ERROR;
				}
				throw;
			}
		}

		if (trace) {
			if(!traceLogGroup.empty())
				setNetworkOption(FDBNetworkOptions::TRACE_LOG_GROUP, StringRef(traceLogGroup));

			if (traceDir.empty())
				setNetworkOption(FDBNetworkOptions::TRACE_ENABLE);
			else
				setNetworkOption(FDBNetworkOptions::TRACE_ENABLE, StringRef(traceDir));

			setNetworkOption(FDBNetworkOptions::ENABLE_SLOW_TASK_PROFILING);
		}
		setNetworkOption(FDBNetworkOptions::DISABLE_CLIENT_STATISTICS_LOGGING);
		Error::init();
		std::set_new_handler( &platform::outOfMemory );

		int total = 0;
		for(auto i = Error::errorCounts().begin(); i != Error::errorCounts().end(); ++i)
			total += i->second;
		if (total)
			printf("%d errors:\n", total);
		for(auto i = Error::errorCounts().begin(); i != Error::errorCounts().end(); ++i)
			if (i->second > 0)
				printf("  %d: %d %s\n", i->second, i->first, Error::fromCode(i->first).what());


		Reference<Cluster> cluster;
		Reference<ClusterConnectionFile> ccf;
		Database db;
		Reference<Cluster> source_cluster;
		Reference<ClusterConnectionFile> source_ccf;
		Database source_db;
		const KeyRef databaseKey = LiteralStringRef("DB");
		FileBackupAgent ba;
		Key tag;
		Future<Optional<Void>> f;
		Future<Optional<int>> fstatus;

		try {
			setupNetwork(0, true);
		}
		catch (Error& e) {
			fprintf(stderr, "ERROR: %s\n", e.what());
			return 1;
		}

		// Ordinarily, this is done when the network is run. However, network thread should be set before TraceEvents are logged. This thread will eventually run the network, so call it now.
		TraceEvent::setNetworkThread(); 

		// Blob Manager mode does not require connecting to any cluster
		if(programExe != EXE_BLOBMANAGER) {
			auto resolvedClusterFile = ClusterConnectionFile::lookupClusterFileName(clusterFile);
			try {
				ccf = Reference<ClusterConnectionFile>(new ClusterConnectionFile(resolvedClusterFile.first));
			}
			catch (Error& e) {
				fprintf(stderr, "%s\n", ClusterConnectionFile::getErrorString(resolvedClusterFile, e).c_str());
				return 1;
			}

			try {
				cluster = Cluster::createCluster(ccf, -1);
			}
			catch (Error& e) {
				fprintf(stderr, "ERROR: %s\n", e.what());
				fprintf(stderr, "ERROR: Unable to connect to cluster from `%s'\n", ccf->getFilename().c_str());
				return 1;
			}

			TraceEvent("ProgramStart")
				.detail("SourceVersion", getHGVersion())
				.detail("Version", FDB_VT_VERSION )
				.detail("PackageName", FDB_VT_PACKAGE_NAME)
				.detailf("ActualTime", "%lld", DEBUG_DETERMINISM ? 0 : time(NULL))
				.detail("CommandLine", commandLine)
				.trackLatest("ProgramStart");

			db = cluster->createDatabase(databaseKey).get();
			
			if(sourceClusterFile.size()) {
				auto resolvedSourceClusterFile = ClusterConnectionFile::lookupClusterFileName(sourceClusterFile);
				try {
					source_ccf = Reference<ClusterConnectionFile>(new ClusterConnectionFile(resolvedSourceClusterFile.first));
				}
				catch (Error& e) {
					fprintf(stderr, "%s\n", ClusterConnectionFile::getErrorString(resolvedSourceClusterFile, e).c_str());
					return 1;
				}

				try {
					source_cluster = Cluster::createCluster(source_ccf, -1);
				}
				catch (Error& e) {
					fprintf(stderr, "ERROR: %s\n", e.what());
					fprintf(stderr, "ERROR: Unable to connect to cluster from `%s'\n", source_ccf->getFilename().c_str());
					return 1;
				}

				source_db = source_cluster->createDatabase(databaseKey).get();
			}
		}

		switch (programExe)
		{
		case EXE_AGENT:
			f = stopAfter(runAgent(db));
			break;
		case EXE_BACKUP:
			switch (backupType)
			{
			case BACKUP_START:
			{
				// Error, if no dest container was specified
				if (destinationContainer.empty()) {
					fprintf(stderr, "ERROR: No backup destination was specified.\n");
					printHelpTeaser(argv[0]);
					return FDB_EXIT_ERROR;
				}

				// Test out the backup url to make sure it parses.  Doesn't test to make sure it's actually writeable.
				std::string error;
				try {
					Reference<IBackupContainer> c = IBackupContainer::openContainer(destinationContainer, &error);
				}
				catch (Error& e) {
					if(!error.empty())
						error = std::string("[") + error + "]";
					fprintf(stderr, "ERROR (%s) on %s %s\n", e.what(), destinationContainer.c_str(), error.c_str());
					printHelpTeaser(argv[0]);
					return FDB_EXIT_ERROR;
				}

				f = stopAfter( submitBackup(db, destinationContainer, backupKeys, tagName, dryRun, waitForDone, stopWhenDone) );
				break;
			}

			case BACKUP_STATUS:
				f = stopAfter( statusBackup(db, tagName, maxErrors) );
				break;

			case BACKUP_ABORT:
				f = stopAfter( abortBackup(db, tagName) );
				break;

			case BACKUP_WAIT:
				f = stopAfter( waitBackup(db, tagName, stopWhenDone) );
				break;

			case BACKUP_DISCONTINUE:
				f = stopAfter( discontinueBackup(db, tagName, waitForDone) );
				break;

			case BACKUP_UNDEFINED:
			default:
				fprintf(stderr, "ERROR: Unsupported backup action %s\n", argv[1]);
				printHelpTeaser(argv[0]);
				return FDB_EXIT_ERROR;
				break;
			}

			break;
		case EXE_RESTORE:
			switch(restoreType) {
				case RESTORE_START:
					f = stopAfter( runRestore(db, tagName, restoreContainer, backupKeys, dbVersion, !dryRun, !quietDisplay, waitForDone, addPrefix, removePrefix) );
					break;
				case RESTORE_WAIT:
					f = stopAfter( success(ba.waitRestore(db, KeyRef(tagName), true)) );
					break;
				case RESTORE_ABORT:
					f = stopAfter( map(ba.abortRestore(db, KeyRef(tagName)), [tagName](FileBackupAgent::ERestoreState s) -> Void {
						printf("Tag: %s  State: %s\n", tagName.c_str(), FileBackupAgent::restoreStateText(s).toString().c_str());
						return Void();
					}) );
					break;
				case RESTORE_STATUS:
					
					// If no tag is specifically provided then print all tag status, don't just use "default"
					if(tagProvided)
						tag = tagName;
					f = stopAfter( map(ba.restoreStatus(db, KeyRef(tag)), [](std::string s) -> Void {
						printf("%s\n", s.c_str());
						return Void();
					}) );
					break;
				default:
					throw restore_error();
			}
			break;
		case EXE_DR_AGENT:
			f = stopAfter( runDBAgent(source_db, db) );
			break;
		case EXE_DB_BACKUP: //DB_START, DB_STATUS, DB_SWITCH, DB_ABORT, DB_CLEANUP
			switch (dbType)
			{
			case DB_START:
				f = stopAfter( submitDBBackup(source_db, db, backupKeys, tagName) );
				break;
			case DB_STATUS:
				f = stopAfter( statusDBBackup(source_db, db, tagName, maxErrors) );
				break;
			case DB_SWITCH:
				f = stopAfter( switchDBBackup(source_db, db, backupKeys, tagName) );
				break;
			case DB_ABORT:
				f = stopAfter( abortDBBackup(source_db, db, tagName, partial) );
				break;
			case DB_UNDEFINED:
			default:
				fprintf(stderr, "ERROR: Unsupported DR action %s\n", argv[1]);
				printHelpTeaser(argv[0]);
				return FDB_EXIT_ERROR;
				break;
			}
			break;
		case EXE_BLOBMANAGER:
			fstatus = stopAfter( doBlobCommand(blobArgs) );
			break;
		case EXE_UNDEFINED:
		default:
			return FDB_EXIT_ERROR;
		}

		runNetwork();

		if(f.isValid() && f.isReady() && !f.isError() && !f.get().present()) {
			status = FDB_EXIT_ERROR;
		}

		if(fstatus.isValid() && fstatus.isReady() && !fstatus.isError() && fstatus.get().present()) {
			status = fstatus.get().get();
		}

		#ifdef ALLOC_INSTRUMENTATION
		{
			cout << "Page Counts: "
				<< FastAllocator<16>::pageCount << " "
				<< FastAllocator<32>::pageCount << " "
				<< FastAllocator<64>::pageCount << " "
				<< FastAllocator<128>::pageCount << " "
				<< FastAllocator<256>::pageCount << " "
				<< FastAllocator<512>::pageCount << " "
				<< FastAllocator<1024>::pageCount << " "
				<< FastAllocator<2048>::pageCount << " "
				<< FastAllocator<4096>::pageCount << endl;

			vector< std::pair<std::string, const char*> > typeNames;
			for( auto i = allocInstr.begin(); i != allocInstr.end(); ++i ) {
				std::string s;

#ifdef __linux__
				char *demangled = abi::__cxa_demangle(i->first, NULL, NULL, NULL);
				if (demangled) {
					s = demangled;
					if (StringRef(s).startsWith(LiteralStringRef("(anonymous namespace)::")))
						s = s.substr(LiteralStringRef("(anonymous namespace)::").size());
					free(demangled);
				} else
					s = i->first;
#else
				s = i->first;
				if (StringRef(s).startsWith(LiteralStringRef("class `anonymous namespace'::")))
					s = s.substr(LiteralStringRef("class `anonymous namespace'::").size());
				else if (StringRef(s).startsWith(LiteralStringRef("class ")))
					s = s.substr(LiteralStringRef("class ").size());
				else if (StringRef(s).startsWith(LiteralStringRef("struct ")))
					s = s.substr(LiteralStringRef("struct ").size());
#endif

				typeNames.push_back( std::make_pair(s, i->first) );
			}
			std::sort(typeNames.begin(), typeNames.end());
			for(int i=0; i<typeNames.size(); i++) {
				const char* n = typeNames[i].second;
				auto& f = allocInstr[n];
				printf("%+d\t%+d\t%d\t%d\t%s\n", f.allocCount, -f.deallocCount, f.allocCount-f.deallocCount, f.maxAllocated, typeNames[i].first.c_str());
			}

			// We're about to exit and clean up data structures, this will wreak havoc on allocation recording
			memSample_entered = true;
		}
		#endif
	} catch (Error& e) {
		TraceEvent(SevError, "MainError").error(e);
		status = FDB_EXIT_MAIN_ERROR;
	} catch (std::exception& e) {
		TraceEvent(SevError, "MainError").error(unknown_error()).detail("std::exception", e.what());
		status = FDB_EXIT_MAIN_EXCEPTION;
	}

	return status;
}