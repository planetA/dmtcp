/****************************************************************************
 *   Copyright (C) 2006-2013 by Jason Ansel, Kapil Arya, and Gene Cooperman *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu              *
 *                                                                          *
 *   This file is part of the dmtcp/src module of DMTCP (DMTCP:dmtcp/src).  *
 *                                                                          *
 *  DMTCP:dmtcp/src is free software: you can redistribute it and/or        *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP:dmtcp/src is distributed in the hope that it will be useful,      *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License along with DMTCP:dmtcp/src.  If not, see                        *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "util.h"
#include "syscallwrappers.h"
#include "uniquepid.h"
#include "processinfo.h"
#include "util.h"
#include "coordinatorapi.h"
#include  "../jalib/jconvert.h"
#include  "../jalib/jfilesystem.h"

static pthread_mutex_t tblLock = PTHREAD_MUTEX_INITIALIZER;

static void _do_lock_tbl()
{
  JASSERT(_real_pthread_mutex_lock(&tblLock) == 0) (JASSERT_ERRNO);
}

static void _do_unlock_tbl()
{
  JASSERT(_real_pthread_mutex_unlock(&tblLock) == 0) (JASSERT_ERRNO);
}

void dmtcp_ProcessInfo_EventHook(DmtcpEvent_t event, DmtcpEventData_t *data)
{
  switch (event) {
    case DMTCP_EVENT_PRE_EXEC:
      {
        jalib::JBinarySerializeWriterRaw wr("", data->serializerInfo.fd);
        dmtcp::ProcessInfo::instance().refresh();
        dmtcp::ProcessInfo::instance().serialize(wr);
      }
      break;

    case DMTCP_EVENT_POST_EXEC:
      {
        jalib::JBinarySerializeReaderRaw rd("", data->serializerInfo.fd);
        dmtcp::ProcessInfo::instance().serialize(rd);
        dmtcp::ProcessInfo::instance().postExec();
      }
      break;

    case DMTCP_EVENT_DRAIN:
      dmtcp::ProcessInfo::instance().refresh();
      break;

    case DMTCP_EVENT_RESTART:
      dmtcp::ProcessInfo::instance().restart();
      break;

    case DMTCP_EVENT_REFILL:
      if (data->refillInfo.isRestart) {
        dmtcp::ProcessInfo::instance().restoreProcessGroupInfo();
      }
      break;


    default:
      break;
  }
}

dmtcp::ProcessInfo::ProcessInfo()
{
  char buf[PATH_MAX];
  _do_lock_tbl();
  _pid = -1;
  _ppid = -1;
  _gid = -1;
  _sid = -1;
  _isRootOfProcessTree = false;
  _noCoordinator = false;
  _childTable.clear();
  _tidVector.clear();
  _pthreadJoinId.clear();
  _procSelfExe = jalib::Filesystem::ResolveSymlink("/proc/self/exe");
  _uppid = UniquePid();
  JASSERT(getcwd(buf, sizeof buf) != NULL);
  _launchCWD = buf;
  _do_unlock_tbl();
}

static dmtcp::ProcessInfo *pInfo = NULL;
dmtcp::ProcessInfo& dmtcp::ProcessInfo::instance()
{
  if (pInfo == NULL) {
    pInfo = new ProcessInfo();
  }
  return *pInfo;
}

void dmtcp::ProcessInfo::restoreProcessGroupInfo()
{
  // Restore group assignment
  if (dmtcp_virtual_to_real_pid && dmtcp_virtual_to_real_pid(_gid) != _gid) {
    pid_t cgid = getpgid(0);
    // Group ID is known inside checkpointed processes
    if (_gid != cgid) {
      JTRACE("Restore Group Assignment")
        (_gid) (_fgid) (cgid) (_pid) (_ppid) (getppid());
      JWARNING(setpgid(0, _gid) == 0) (_gid) (JASSERT_ERRNO)
        .Text("Cannot change group information");
    } else {
      JTRACE("Group is already assigned") (_gid) (cgid);
    }
  } else {
    JTRACE("SKIP Group information, GID unknown");
  }
}

void dmtcp::ProcessInfo::resetOnFork()
{
  pthread_mutex_t newlock = PTHREAD_MUTEX_INITIALIZER;
  tblLock = newlock;
  _ppid = _pid;
  _pid = getpid();
  _isRootOfProcessTree = false;
  _childTable.clear();
  _tidVector.clear();
  _pthreadJoinId.clear();
}

void dmtcp::ProcessInfo::insertChild(pid_t pid, dmtcp::UniquePid uniquePid)
{
  _do_lock_tbl();
  iterator i = _childTable.find( pid );
  JWARNING(i == _childTable.end()) (pid) (uniquePid) (i->second)
    .Text("child pid already exists!");

  _childTable[pid] = uniquePid;
  _do_unlock_tbl();

  JTRACE("Creating new virtualPid -> realPid mapping.") (pid) (uniquePid);
}

void dmtcp::ProcessInfo::restart()
{
  // Try to set the ckptCWD as CWD.
  if (chdir(_ckptCWD.c_str()) != 0) {
    size_t i;
    size_t clen = _ckptCWD.length();
    size_t llen = _launchCWD.length();
    string rpath;
    // If failed, chdir relative to restartCWD.
    if (_launchCWD == _ckptCWD) {
      // _launchCWD = "/A/B"; _ckptCWD = "/A/B" -> rpath = ""
      rpath = "";
    } else if (Util::strStartsWith(_ckptCWD.c_str(), _launchCWD.c_str()) &&
               _ckptCWD[clen] == '/') {
      // _launchCWD = "/A/B"; _ckptCWD = "/A/B/C" -> rpath = "./c"
      rpath = _ckptCWD.substr(clen + 1);
    } else if (Util::strStartsWith(_launchCWD.c_str(), _ckptCWD.c_str()) &&
               _launchCWD[llen] == '/') {
      // _launchCWD = "/A/B"; _ckptCWD = "/A" -> rpath = "../"
      for (i = clen; _launchCWD[i] != '\0'; i++) {
        if (_launchCWD[i] == '/') {
          rpath += "../";
        }
      }
    } else {
      // _launchCWD = "/A/B"; _ckptCWD = "/A/C" -> rpath = "../C"
      size_t lastSlash = 0;
      // find the common prefix
      for (i = 0; _launchCWD[i] == _ckptCWD[i]; i++) {
        if (_launchCWD[i] == '/') {
          lastSlash = i;
        }
      }
      rpath = _ckptCWD.substr(lastSlash + 1);
      for (i = lastSlash + 1; _launchCWD[i] != '\0'; i++) {
        if (_launchCWD[i] == '/') {
          rpath = "../" + rpath;
        }
      }
    }
    char cwd[PATH_MAX];
    JASSERT(getcwd(cwd, sizeof cwd) != NULL);
    JWARNING(chdir(rpath.c_str()) == 0) (_ckptCWD) (_launchCWD) (cwd) (rpath);
  }
}

void dmtcp::ProcessInfo::eraseChild( pid_t virtualPid )
{
  _do_lock_tbl();
  iterator i = _childTable.find ( virtualPid );
  if ( i != _childTable.end() )
    _childTable.erase( virtualPid );
  _do_unlock_tbl();
}

bool dmtcp::ProcessInfo::isChild(const UniquePid& upid)
{
  bool res = false;
  _do_lock_tbl();
  for (iterator i = _childTable.begin(); i != _childTable.end(); i++) {
    if (i->second == upid) {
      res = true;
      break;
    }
  }
  _do_unlock_tbl();
  return res;
}

void dmtcp::ProcessInfo::insertTid( pid_t tid )
{
  eraseTid( tid );
  _do_lock_tbl();
  _tidVector.push_back ( tid );
  _do_unlock_tbl();
  return;
}

void dmtcp::ProcessInfo::eraseTid( pid_t tid )
{
  _do_lock_tbl();
  dmtcp::vector< pid_t >::iterator iter = _tidVector.begin();
  while ( iter != _tidVector.end() ) {
    if ( *iter == tid ) {
      _tidVector.erase( iter );
      break;
    }
    else
      ++iter;
  }
  _do_unlock_tbl();
  return;
}

void dmtcp::ProcessInfo::postExec( )
{
  /// FIXME
  JTRACE("Post-Exec. Emptying tidVector");
  _do_lock_tbl();
  _tidVector.clear();

  _procname   = jalib::Filesystem::GetProgramName();
  _upid       = UniquePid::ThisProcess();
  _uppid      = UniquePid::ParentProcess();
  _do_unlock_tbl();
}

bool dmtcp::ProcessInfo::beginPthreadJoin(pthread_t thread)
{
  bool res = false;
  _do_lock_tbl();
  dmtcp::map<pthread_t, pthread_t>::iterator i = _pthreadJoinId.find(thread);
  if (i == _pthreadJoinId.end()) {
    _pthreadJoinId[thread] = pthread_self();
    res = true;
  }
  _do_unlock_tbl();
  return res;
}

void dmtcp::ProcessInfo::clearPthreadJoinState(pthread_t thread)
{
  _do_lock_tbl();
  if (_pthreadJoinId.find(thread) != _pthreadJoinId.end()) {
    _pthreadJoinId.erase(thread);
  }
  _do_unlock_tbl();
}

void dmtcp::ProcessInfo::endPthreadJoin(pthread_t thread)
{
  _do_lock_tbl();
  if (_pthreadJoinId.find(thread) != _pthreadJoinId.end() &&
      pthread_equal(_pthreadJoinId[thread], pthread_self())) {
    _pthreadJoinId.erase(thread);
  }
  _do_unlock_tbl();
}

void dmtcp::ProcessInfo::refresh()
{
  _pid = getpid();
  _ppid = getppid();
  _gid = getpgid(0);
  _sid = getsid(0);

  _fgid = -1;
  // Try to open the controlling terminal
  int tfd = _real_open("/dev/tty", O_RDWR);
  if (tfd != -1) {
    _fgid = tcgetpgrp(tfd);
    _real_close(tfd);
  }

  if (_ppid == 1) {
    _isRootOfProcessTree = true;
    _uppid = UniquePid();
  } else {
    _uppid = UniquePid::ParentProcess();
  }

  _procname = jalib::Filesystem::GetProgramName();
  _hostname = jalib::Filesystem::GetCurrentHostname();
  _upid = UniquePid::ThisProcess();
  _noCoordinator = dmtcp_no_coordinator();

  char buf[PATH_MAX];
  JASSERT(getcwd(buf, sizeof buf) != NULL);
  _ckptCWD = buf;

  _sessionIds.clear();
  refreshChildTable();
  refreshTidVector();

  JTRACE("CHECK GROUP PID")(_gid)(_fgid)(_ppid)(_pid);
}

void dmtcp::ProcessInfo::refreshTidVector()
{
  dmtcp::vector< pid_t >::iterator iter;
  for (iter = _tidVector.begin(); iter != _tidVector.end(); ) {
    int retVal = syscall(SYS_tgkill, _pid, *iter, 0);
    if (retVal == -1 && errno == ESRCH) {
      iter = _tidVector.erase( iter );
    } else {
      iter++;
    }
  }
  return;
}

void dmtcp::ProcessInfo::refreshChildTable()
{
  iterator i = _childTable.begin();
  while (i != _childTable.end()) {
    pid_t pid = i->first;
    iterator j = i++;
    /* Check to see if the child process is alive*/
    if (kill(pid, 0) == -1 && errno == ESRCH) {
      _childTable.erase(j);
    } else {
      _sessionIds[pid] = getsid(pid);
    }
  }
}

void dmtcp::ProcessInfo::serialize ( jalib::JBinarySerializer& o )
{
  JSERIALIZE_ASSERT_POINT ( "dmtcp::ProcessInfo:" );

  o & _isRootOfProcessTree & _pid & _sid & _ppid & _gid & _fgid;
  o & _procname & _hostname & _launchCWD & _ckptCWD & _upid & _uppid;
  o & _compGroup & _numPeers & _noCoordinator & _argvSize & _envSize;

  JTRACE("Serialized process information")
    (_sid) (_ppid) (_gid) (_fgid)
    (_procname) (_hostname) (_launchCWD) (_ckptCWD) (_upid) (_uppid)
    (_compGroup) (_numPeers) (_noCoordinator) (_argvSize) (_envSize);

  JASSERT(!_noCoordinator || _numPeers == 1) (_noCoordinator) (_numPeers);

  if ( _isRootOfProcessTree ) {
    JTRACE ( "This process is Root of Process Tree" );
  }

  JTRACE ("Serializing ChildPid Table") (_childTable.size()) (o.filename());
  o.serializeMap(_childTable);

  JTRACE ("Serializing tidVector");
  JSERIALIZE_ASSERT_POINT ( "TID Vector:[" );
  o & _tidVector;
  JSERIALIZE_ASSERT_POINT ( "}" );

  JSERIALIZE_ASSERT_POINT( "EOF" );
}
