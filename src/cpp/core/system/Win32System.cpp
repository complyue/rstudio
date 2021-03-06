/*
 * Win32System.cpp
 *
 * Copyright (C) 2009-17 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include <core/system/System.hpp>

#include <stdio.h>
#include <stdlib.h>
#include <io.h>

#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>

#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <boost/foreach.hpp>
#include <boost/bind.hpp>
#include <boost/system/windows_error.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/range.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <core/Log.hpp>
#include <core/LogWriter.hpp>
#include <core/Error.hpp>
#include <core/FileLogWriter.hpp>
#include <core/StderrLogWriter.hpp>
#include <core/FilePath.hpp>
#include <core/FileInfo.hpp>
#include <core/DateTime.hpp>
#include <core/StringUtils.hpp>
#include <core/SafeConvert.hpp>
#include <core/system/Environment.hpp>

#ifndef JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x2000
#endif
#ifndef JOB_OBJECT_LIMIT_BREAKAWAY_OK
#define JOB_OBJECT_LIMIT_BREAKAWAY_OK 0x00000800
#endif

namespace rstudio {
namespace core {
namespace system {

namespace {
// main log writer
LogWriter* s_pLogWriter = NULL;

// additional log writers
std::vector<boost::shared_ptr<LogWriter> > s_logWriters;

Error initJobObject(bool* detachFromJob)
{
   /*
    * Create a Job object and assign this process to it. This will
    * cause all child processes to be assigned to the same job.
    * With JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE set, all the child
    * processes will be killed when this process terminates (since
    * it is the only one holding a handle to the job). With
    * JOB_OBJECT_LIMIT_BREAKAWAY_OK set it is possible to pass
    * CREATE_BREAKAWAY_FROM_JOB to CreateProcess (this is required
    * by Chrome for creating its sub-processes)
    */

   // If detachFromJob is true, it means we need to relaunch this
   // executable with CREATE_BREAKAWAY_FROM_JOB
   *detachFromJob = false;

   HANDLE hJob = ::CreateJobObject(NULL, NULL);
   if (!hJob)
      return systemError(::GetLastError(), ERROR_LOCATION);

   JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
   jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                           JOB_OBJECT_LIMIT_BREAKAWAY_OK;
   ::SetInformationJobObject(hJob,
                             JobObjectExtendedLimitInformation,
                             &jeli,
                             sizeof(jeli));

   if (::AssignProcessToJobObject(hJob, ::GetCurrentProcess()))
   {
      DWORD error = ::GetLastError();
      if (error == ERROR_ACCESS_DENIED)
      {
         // Use an environment variable to prevent us from somehow
         // getting into an infinite loop of detaching (which would
         // otherwise occur if ERROR_ACCESS_DENIED is being returned
         // for some reason other than an existing job object being
         // attached). This works because environment variables are
         // inherited by our job-detached child process.
         if (getenv("_RSTUDIO_LEVEL").empty())
         {
            setenv("_RSTUDIO_LEVEL", "1");
            *detachFromJob = true;
         }
      }
      return systemError(error, ERROR_LOCATION);
   }

   return Success();
}

bool isHiddenFile(const std::string& path)
{
   DWORD attribs = ::GetFileAttributesA(path.c_str());
   if (attribs == INVALID_FILE_ATTRIBUTES)
      return false;
   else if (attribs & FILE_ATTRIBUTE_HIDDEN)
      return true;
   else
      return false;
}

} // anonymous namespace

void initHook()
{
   // Logging will NOT work in this function!!

   bool detachFromJob;
   Error error = initJobObject(&detachFromJob);
   if (!detachFromJob)
      return;

   TCHAR path[MAX_PATH];
   if (!::GetModuleFileName(NULL, path, MAX_PATH))
      return;  // Couldn't get the path of the current .exe

   STARTUPINFO startupInfo;
   memset(&startupInfo, 0, sizeof(startupInfo));
   startupInfo.cb = sizeof(startupInfo);
   PROCESS_INFORMATION procInfo;
   memset(&procInfo, 0, sizeof(procInfo));

   if (!::CreateProcess(NULL,
                        ::GetCommandLine(),
                        NULL,
                        NULL,
                        TRUE,
                        CREATE_BREAKAWAY_FROM_JOB | ::GetPriorityClass(::GetCurrentProcess()),
                        NULL,
                        NULL,
                        &startupInfo,
                        &procInfo))
   {
      return;  // Couldn't execute
   }

   ::AllowSetForegroundWindow(procInfo.dwProcessId);
   ::WaitForSingleObject(procInfo.hProcess, INFINITE);

   DWORD exitCode;
   if (!::GetExitCodeProcess(procInfo.hProcess, &exitCode))
      exitCode = ::GetLastError();

   ::CloseHandle(procInfo.hProcess);
   ::CloseHandle(procInfo.hThread);

   ::ExitProcess(exitCode);
}

void initializeSystemLog(const std::string& programIdentity, int logLevel)
{
}

void initializeStderrLog(const std::string& programIdentity, int logLevel)
{
   if (s_pLogWriter)
      delete s_pLogWriter;

   s_pLogWriter = new StderrLogWriter(programIdentity, logLevel);
}


void initializeLog(const std::string& programIdentity, int logLevel, const FilePath& settingsDir)
{
   if (s_pLogWriter)
      delete s_pLogWriter;

   s_pLogWriter = new FileLogWriter(programIdentity, logLevel, settingsDir);
}

void setLogToStderr(bool logToStderr)
{
   if (s_pLogWriter)
      s_pLogWriter->setLogToStderr(logToStderr);
}

void addLogWriter(boost::shared_ptr<core::LogWriter> pLogWriter)
{
   s_logWriters.push_back(pLogWriter);
}

void log(LogLevel logLevel, const std::string& message)
{
   if (s_pLogWriter)
      s_pLogWriter->log(logLevel, message);

   std::for_each(s_logWriters.begin(),
                 s_logWriters.end(),
                 boost::bind(&LogWriter::log, _1, logLevel, message));
}

bool isWin64()
{
   return !getenv("PROCESSOR_ARCHITEW6432").empty()
      || getenv("PROCESSOR_ARCHITECTURE") == "AMD64";
}

bool isCurrentProcessWin64()
{
   return getenv("PROCESSOR_ARCHITECTURE") == "AMD64";
}

bool isVistaOrLater()
{
   OSVERSIONINFOA osVersion;
   ZeroMemory(&osVersion, sizeof(OSVERSIONINFOA));
   osVersion.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);

   if (::GetVersionExA(&osVersion))
   {
      return osVersion.dwMajorVersion >= 6;
   }
   else
   {
      LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));
      return false;
   }
}

bool isWin7OrLater()
{
   OSVERSIONINFOA osVersion;
   ZeroMemory(&osVersion, sizeof(OSVERSIONINFOA));
   osVersion.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);

   if (::GetVersionExA(&osVersion))
   {
      // 6.0 Vista, 6.1 Win7, 6.2 Win8, 6.3 Win8.1, >6 is Win10+
      return osVersion.dwMajorVersion > 6 ||
             osVersion.dwMajorVersion == 6 && osVersion.dwMinorVersion > 0;
   }
   else
   {
      LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));
      return false;
   }
}

std::string username()
{
   return system::getenv("USERNAME");
}

unsigned int effectiveUserId()
{
   return 0; // no concept of this on Win32
}

// home path strategies
namespace {

FilePath environmentHomePath(std::string envVariables)
{
   using namespace boost::algorithm;

   // use environment override if specified
   if (!envVariables.empty())
   {
      for (split_iterator<std::string::iterator> it =
           make_split_iterator(envVariables, first_finder("|", is_iequal()));
           it != split_iterator<std::string::iterator>();
           ++it)
      {
         std::string envHomePath =
                  system::getenv(boost::copy_range<std::string>(*it));
         if (!envHomePath.empty())
         {
            FilePath userHomePath(envHomePath);
            if (userHomePath.exists())
               return userHomePath;
         }
      }
   }

   // no override
   return FilePath();
}

FilePath currentCSIDLPersonalHomePath()
{
   // query for My Documents directory
   const DWORD SHGFP_TYPE_CURRENT = 0;
   wchar_t homePath[MAX_PATH];
   HRESULT hr = ::SHGetFolderPathW(NULL,
                                   CSIDL_PERSONAL,
                                   NULL,
                                   SHGFP_TYPE_CURRENT,
                                   homePath);
   if (SUCCEEDED(hr))
   {
      return FilePath(homePath);
   }
   else
   {
      LOG_WARNING_MESSAGE("Unable to retreive user home path. HRESULT:  " +
                          safe_convert::numberToString(hr));
      return FilePath();
   }
}

FilePath defaultCSIDLPersonalHomePath()
{
   // query for default and force creation (works around situations
   // where redirected path is not available)
   const DWORD SHGFP_TYPE_DEFAULT = 1;
   wchar_t homePath[MAX_PATH];
   HRESULT hr = ::SHGetFolderPathW(NULL,
                                   CSIDL_PERSONAL|CSIDL_FLAG_CREATE,
                                   NULL,
                                   SHGFP_TYPE_DEFAULT,
                                   homePath);
   if (SUCCEEDED(hr))
   {
      return FilePath(homePath);
   }
   else
   {
      LOG_WARNING_MESSAGE("Unable to retreive user home path. HRESULT:  " +
                          safe_convert::numberToString(hr));
      return FilePath();
   }
}

FilePath homepathHomePath()
{
   std::string homeDrive = core::system::getenv("HOMEDRIVE");
   std::string homePath = core::system::getenv("HOMEPATH");
   if (!homeDrive.empty() && !homePath.empty())
      return FilePath(homeDrive + homePath);
   else
      return FilePath();
}

FilePath homedriveHomePath()
{
   std::string homeDrive = core::system::getenv("HOMEDRIVE");
   if (homeDrive.empty())
      homeDrive = "C:";
   return FilePath(homeDrive);
}

typedef std::pair<std::string,boost::function<FilePath()> > HomePathSource;

} // anonymous namespace

FilePath userHomePath(std::string envOverride)
{
   using boost::bind;
   std::vector<HomePathSource> sources;
   sources.push_back(std::make_pair("R_USER|HOME",
                                    bind(environmentHomePath, envOverride)));
   sources.push_back(std::make_pair("SHGFP_TYPE_CURRENT",
                                    currentCSIDLPersonalHomePath));
   sources.push_back(std::make_pair("SHGFP_TYPE_DEFAULT",
                                    defaultCSIDLPersonalHomePath));
   std::string envFallback = "USERPROFILE";
   sources.push_back(std::make_pair(envFallback,
                                    bind(environmentHomePath, envFallback)));
   sources.push_back(std::make_pair("HOMEPATH",
                                    homepathHomePath));
   sources.push_back(std::make_pair("HOMEDRIVE",
                                    homedriveHomePath));

   BOOST_FOREACH(const HomePathSource& source, sources)
   {
      FilePath homePath = source.second();
      if (!homePath.empty())
      {
         // return if we found one that exists
         if (homePath.exists())
         {
            std::string path = homePath.absolutePath();

            // standardize drive letter capitalization if in X:/y/z format
            if (path.length() > 1 && path[1] == ':')
            {
               path[0] = toupper(path[0]);
               homePath = FilePath(path);
            }

            return homePath;
         }

         // otherwise warn that we got a value that didn't exist
         LOG_WARNING_MESSAGE("Home path returned by " + source.first + " (" +
                             homePath.absolutePath() + ") does not exist.");
      }
   }

   // no luck!
   LOG_ERROR_MESSAGE("No valid home path found for user");
   return FilePath();
}

FilePath userSettingsPath(const FilePath& userHomeDirectory,
                          const std::string& appName)
{
   wchar_t path[MAX_PATH + 1];
   std::wstring appNameWide(appName.begin(), appName.end());
   HRESULT hr = ::SHGetFolderPathAndSubDirW(
         NULL,
         CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
         NULL,
         SHGFP_TYPE_CURRENT,
         appNameWide.c_str(),
         path);

   if (hr != S_OK)
   {
      LOG_ERROR_MESSAGE("Unable to retreive user home path. HRESULT:  " +
                        safe_convert::numberToString(hr));
      return FilePath();
   }

   return FilePath(std::wstring(path));
}

bool currentUserIsPrivilleged(unsigned int minimumUserId)
{
   return false;
}



Error captureCommand(const std::string& command, std::string* pOutput)
{
   // WIN32 popen docs:
   // http://msdn.microsoft.com/en-us/library/96ayss4b(VS.80).aspx

   // NOTE: note that popen only works from win32 console applications!

   // start process
   FILE* fp = ::_popen(command.c_str(), "r");
   if (fp == NULL)
      return systemError(errno, ERROR_LOCATION);

   // collect output
   const int kBuffSize = 1024;
   char buffer[kBuffSize];
   while (::fgets(buffer, kBuffSize, fp) != NULL)
      *pOutput += buffer;

   // check if an error terminated our output
   Error error ;
   if (::ferror(fp))
      error = systemError(boost::system::errc::io_error, ERROR_LOCATION);

   // close file
   if (::_pclose(fp) == -1)
   {
      // log existing error before overwriting it
      if (error)
         LOG_ERROR(error);

      error = systemError(errno, ERROR_LOCATION);
   }

   // return status
   return error;
}

Error realPath(const FilePath& filePath, FilePath* pRealPath)
{
   std::wstring wPath = filePath.absolutePathW();
   std::vector<wchar_t> buffer(512);
   DWORD res = ::GetFullPathNameW(wPath.c_str(),
                                  buffer.size(),
                                  &(buffer[0]),
                                  NULL);
   if (res == 0)
   {
      Error error = systemError(::GetLastError(), ERROR_LOCATION);
      error.addProperty("path", filePath);
      return error;
   }
   else if (res > buffer.size())
   {
      buffer.resize(res);
      res = ::GetFullPathNameW(wPath.c_str(),
                               buffer.size(),
                               &(buffer[0]),
                               NULL);
      if (res == 0)
         return systemError(::GetLastError(), ERROR_LOCATION);
      else if (res > buffer.size())
         return systemError(boost::system::windows_error::bad_length,
                            ERROR_LOCATION);
   }

   wPath = std::wstring(&(buffer[0]), res);
   *pRealPath = FilePath(wPath);
   return Success();
}

Error realPath(const std::string& path, FilePath* pRealPath)
{
   return realPath(FilePath(path), pRealPath);
}

bool isHiddenFile(const FilePath& filePath)
{
   return isHiddenFile(filePath.absolutePath());
}

bool isHiddenFile(const FileInfo& fileInfo)
{
   return isHiddenFile(fileInfo.absolutePath());
}

bool isReadOnly(const FilePath& filePath)
{
   // TODO: readonly detection for windows
   return false;
}

Error makeFileHidden(const FilePath& path)
{
   std::wstring filePath = path.absolutePathW();
   LPCWSTR lpszPath = filePath.c_str();

   DWORD attribs = ::GetFileAttributesW(lpszPath);
   if (attribs == INVALID_FILE_ATTRIBUTES)
      return systemError(GetLastError(), ERROR_LOCATION);

   if (!::SetFileAttributesW(lpszPath, attribs | FILE_ATTRIBUTE_HIDDEN))
      return systemError(GetLastError(), ERROR_LOCATION);

   return Success();
}




bool stderrIsTerminal()
{
   return _isatty(_fileno(stderr));
}

bool stdoutIsTerminal()
{
   return _isatty(_fileno(stdout));
}

// uuid
std::string generateUuid(bool includeDashes)
{
   // create the uuid
   UUID uuid = {0};
   ::UuidCreate(&uuid);
   PUCHAR pChar = NULL;
   ::UuidToStringA(&uuid, &pChar);
   std::string uuidStr((char*)pChar);
   ::RpcStringFreeA(&pChar);

   // remove dashes if requested
   if (!includeDashes)
      boost::algorithm::replace_all(uuidStr, "-", "");

   // return
   return uuidStr;
}

PidType currentProcessId()
{
   return ::GetCurrentProcessId();
}

Error executablePath(const char * argv0,
                     FilePath* pExecutablePath)
{
   *pExecutablePath = FilePath(_pgmptr);
   return Success();
}

// installation path
Error installPath(const std::string& relativeToExecutable,
                  const char * argv0,
                  FilePath* pInstallationPath)
{
   // get full executable path
   FilePath exePath;
   Error error = executablePath(argv0, &exePath);
   if (error)
      return error;

   // resolve to install path using given relative path
   if (relativeToExecutable == "..") // common case
     *pInstallationPath = exePath.parent().parent();
   else
     *pInstallationPath = exePath.parent().complete(relativeToExecutable);

   return Success();
}

void fixupExecutablePath(FilePath* pExePath)
{
   if (pExePath->extension().empty())
     *pExePath = pExePath->parent().complete(pExePath->filename() + ".exe");
}

void abort()
{
   ::exit(1);
}

 
////////////////////////////////////////////////////////////////////////////
//
//  No signals on Win32 so all of these are no-ops
//
//


Error ignoreTerminalSignals()
{
   return Success();
}
      
Error ignoreChildExits()
{
   return Success();
}
     
Error reapChildren()
{
   return Success();
}
   
struct SignalBlocker::Impl
{
};
   
SignalBlocker::SignalBlocker()
   : pImpl_(new Impl())
{
}
   
   
Error SignalBlocker::block(SignalType signal)
{
   return Success();
}

Error SignalBlocker::blockAll()
{
   return Success();
}
      
SignalBlocker::~SignalBlocker()
{
   try
   {
   }
   catch(...)
   {
   }
}
   
Error clearSignalMask()
{
   return Success();
}
   
Error handleSignal(SignalType signal, void (*handler)(int))
{
  return Success();
}
   
core::Error ignoreSignal(SignalType signal)
{
   return Success();
}   


Error useDefaultSignalHandler(SignalType signal)
{
   return Success();
}

void sendSignalToSelf(SignalType signal)
{
}

class ClipboardScope : boost::noncopyable
{
public:
   ClipboardScope() : opened_(false) {}

   Error open()
   {
      if (!::OpenClipboard(NULL))
      {
         return systemError(::GetLastError(), ERROR_LOCATION);
      }
      else
      {
         opened_ = true;
         return Success();
      }
   }

   ~ClipboardScope()
   {
      try
      {
         if (opened_)
         {
            if (!::CloseClipboard())
               LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));
         }
      }
      catch(...)
      {
      }
   }

private:
   bool opened_;
};

class EnhMetaFile : boost::noncopyable
{
public:
   EnhMetaFile() : hMF_(NULL) {}

   Error open(const FilePath& path)
   {
      hMF_ = ::GetEnhMetaFileW(path.absolutePathW().c_str());
      if (hMF_ == NULL)
         return systemError(::GetLastError(), ERROR_LOCATION);
      else
         return Success();
   }

   ~EnhMetaFile()
   {
      try
      {
         if (hMF_ != NULL)
         {
            if (!::DeleteEnhMetaFile(hMF_))
               LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));
         }
      }
      catch(...)
      {
      }
   }

   HENHMETAFILE handle() const { return hMF_; }

   void release()
   {
      hMF_ = NULL;
   }

private:
  HENHMETAFILE hMF_;
};


Error copyMetafileToClipboard(const FilePath& path)
{
   // open metafile
   EnhMetaFile enhMetaFile;
   Error error = enhMetaFile.open(path);
   if (error)
      return error;

   // open the clipboard
   ClipboardScope clipboardScope;
   error = clipboardScope.open();
   if (error)
      return error;

   // emtpy the clipboard
   if (!::EmptyClipboard())
      return systemError(::GetLastError(), ERROR_LOCATION);

   // set the clipboard data
   if (!::SetClipboardData(CF_ENHMETAFILE, enhMetaFile.handle()))
      return systemError(::GetLastError(), ERROR_LOCATION);

   // release the handle (because the clipboard now owns it)
   enhMetaFile.release();

   // return success
   return Success();
}

void ensureLongPath(FilePath* pFilePath)
{
   const std::size_t kBuffSize = (MAX_PATH*2) + 1;
   char buffer[kBuffSize];
   std::string path = string_utils::utf8ToSystem(pFilePath->absolutePath());
   if (::GetLongPathName(path.c_str(),
                         buffer,
                         kBuffSize) > 0)
   {
      *pFilePath = FilePath(string_utils::systemToUtf8(buffer));
   }
}
Error expandEnvironmentVariables(std::string value, std::string* pResult)
{
   if (value.empty())
   {
      *pResult = value;
      return Success();
   }

   DWORD sizeRequired = ::ExpandEnvironmentStrings(value.c_str(), NULL, 0);
   if (!sizeRequired)
      return systemError(::GetLastError(), ERROR_LOCATION);

   std::vector<char> buffer;
   buffer.reserve(sizeRequired);
   int result = ::ExpandEnvironmentStrings(value.c_str(),
                                           &buffer[0],
                                           buffer.capacity());

   if (!result)
      return systemError(GetLastError(), ERROR_LOCATION);
   else if (result > buffer.capacity())
      return systemError(ERROR_MORE_DATA, ERROR_LOCATION); // not expected

   *pResult = std::string(&buffer[0]);
   return Success();
}

FilePath expandComSpec()
{
   std::string result;
   Error err = expandEnvironmentVariables("%COMSPEC%", &result);
   if (err)
      return FilePath();
   return FilePath(result);
}

Error terminateProcess(PidType pid)
{
   HANDLE hProc = ::OpenProcess(PROCESS_TERMINATE, false, pid);
   if (!hProc)
      return systemError(::GetLastError(), ERROR_LOCATION);
   if (!::TerminateProcess(hProc, 1))
      return systemError(::GetLastError(), ERROR_LOCATION);
   return Success();
}

std::vector<SubprocInfo> getSubprocesses(PidType pid)
{
   std::vector<SubprocInfo> subprocs;

   HANDLE hSnapShot;
   CloseHandleOnExitScope closeSnapShot(&hSnapShot, ERROR_LOCATION);

   hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
   if (hSnapShot == INVALID_HANDLE_VALUE)
   {
      // err on the side of assuming child processes, so we don't kill
      // a job unintentionally
      LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));
      return subprocs;
   }

   PROCESSENTRY32 pe32;
   pe32.dwSize = sizeof(pe32);
   if (!Process32First(hSnapShot, &pe32))
   {
      LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));
      return subprocs;
   }

   do
   {
      if (pe32.th32ParentProcessID == pid)
      {
         // Found a child process
         SubprocInfo info;
         info.pid = pe32.th32ProcessID;
         info.exe = pe32.szExeFile;

         subprocs.push_back(info);
      }
   } while (Process32Next(hSnapShot, &pe32));

   return subprocs;
}

FilePath currentWorkingDir(PidType pid)
{
   // NYI for Win32; commonly accepted technique for this is to use
   // CreateRemoteThread to inject code to run GetCurrentDirectory in the
   // context of the target program. That is ugly and we aren't
   // likely to ever do it.
   return FilePath();
}

Error closeHandle(HANDLE* pHandle, const ErrorLocation& location)
{
   if (*pHandle != NULL)
   {
      BOOL result = ::CloseHandle(*pHandle);
      *pHandle = NULL;

      if (!result)
         return systemError(::GetLastError(), location);
      else
         return Success();
   }
   else
   {
      return Success();
   }
}

CloseHandleOnExitScope::~CloseHandleOnExitScope()
{
   try
   {
      // A "null" handle can contain INVALID_HANDLE or NULL, depending
      // on the context. This is a painful inconsistency in Windows, see:
      // https://blogs.msdn.microsoft.com/oldnewthing/20040302-00/?p=40443
      if (!pHandle_ || *pHandle_ == INVALID_HANDLE_VALUE || *pHandle_ == NULL)
         return;

      Error error = closeHandle(pHandle_, location_);
      if (error)
         LOG_ERROR(error);
   }
   catch(...)
   {
   }
}

struct ProcessInfo
{
   DWORD processId;
   DWORD parentProcessId;
};

// simple cass to encapsulate parent-child
// relationship of processes
struct ProcessTreeNode
{
   boost::shared_ptr<ProcessInfo> data;
   std::vector<boost::shared_ptr<ProcessTreeNode> > children;
};

// process tree, indexed by pid
typedef std::map<DWORD, boost::shared_ptr<ProcessTreeNode> > ProcessTreeT;

void createProcessTree(const std::vector<ProcessInfo>& processes,
                       ProcessTreeT *pOutTree)
{
   // first pass, create the nodes in the tree
   BOOST_FOREACH(const ProcessInfo& process, processes)
   {
      ProcessTreeT::iterator iter = pOutTree->find(process.processId);
      if (iter == pOutTree->end())
      {
         // process not found, so create a new entry for it
         boost::shared_ptr<ProcessTreeNode> nodePtr = boost::shared_ptr<ProcessTreeNode>(
                                                         new ProcessTreeNode());

         nodePtr->data = boost::shared_ptr<ProcessInfo>(new ProcessInfo(process));

         (*pOutTree)[process.processId] = nodePtr;
      }
   }

   // second pass, link the nodes together
   BOOST_FOREACH(ProcessTreeT::value_type& element, *pOutTree)
   {
      DWORD parent = element.second->data->parentProcessId;
      ProcessTreeT::iterator iter = pOutTree->find(parent);

      // if we cannot find the parent in the tree, move on
      if (iter == pOutTree->end())
         continue;

      // add this node to its parent's children
      iter->second->children.push_back(element.second);
   }
}

void getChildren(const boost::shared_ptr<ProcessTreeNode>& node,
                 std::vector<ProcessInfo> *pOutChildren)
{
   BOOST_FOREACH(const boost::shared_ptr<ProcessTreeNode>& child, node->children)
   {
      pOutChildren->push_back(*child->data.get());
      getChildren(child, pOutChildren);
   }
}

Error getProcesses(std::vector<ProcessInfo> *pOutProcesses)
{
   PROCESSENTRY32 processEntry;
   memset(&processEntry, 0, sizeof(PROCESSENTRY32));
   processEntry.dwSize = sizeof(PROCESSENTRY32);

   HANDLE hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
   if (hSnap == INVALID_HANDLE_VALUE)
   {
      return systemError(::GetLastError(), ERROR_LOCATION);
   }

   if (Process32First(hSnap, &processEntry))
   {
      BOOL moreProcesses = TRUE;

      while (moreProcesses)
      {
         ProcessInfo process;
         process.processId = processEntry.th32ProcessID;
         process.parentProcessId = processEntry.th32ParentProcessID;
         pOutProcesses->push_back(process);

         moreProcesses = ::Process32Next(hSnap, &processEntry);
      }
   }

   return Success();
}

Error getChildProcesses(std::vector<ProcessInfo> *pOutProcesses)
{
   if (!pOutProcesses)
      return systemError(EINVAL, ERROR_LOCATION);

   // get all processes
   std::vector<ProcessInfo> processes;
   Error error = getProcesses(&processes);
   if (error) return error;

   // build a process tree of the processes
   ProcessTreeT tree;
   createProcessTree(processes, &tree);

   // return just the children of this process
   ProcessTreeT::const_iterator iter = tree.find(::GetCurrentProcessId());
   if (iter == tree.end())
      return Success();

   const boost::shared_ptr<ProcessTreeNode>& rootNode = iter->second;
   getChildren(rootNode, pOutProcesses);

   return Success();
}

Error terminateChildProcesses()
{
   std::vector<ProcessInfo> childProcesses;
   Error error = getChildProcesses(&childProcesses);
   if (error)
      return error;

   BOOST_FOREACH(const ProcessInfo& process, childProcesses)
   {
      HANDLE hChildProc = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, process.processId);
      if (hChildProc)
      {
         if (!::TerminateProcess(hChildProc, 1))
         {
            LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));
         }

         if (!::CloseHandle(hChildProc))
         {
            LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));
         }
      }
      else
      {
         LOG_ERROR(systemError(::GetLastError(), ERROR_LOCATION));
      }
   }

   // the actual kill is best effort
   // so return success regardless
   return Success();
}

void setHomeToUserProfile(core::system::Options* pChildEnv)
{
   std::string userProfile = core::system::getenv(*pChildEnv, "USERPROFILE");
   core::system::setenv(pChildEnv, "HOME", userProfile);
}

} // namespace system
} // namespace core
} // namespace rstudio

