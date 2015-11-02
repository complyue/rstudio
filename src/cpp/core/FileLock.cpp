/*
 * FileLock.cpp
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
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

#include <core/FileLock.hpp>

#include <core/Macros.hpp>
#include <core/Error.hpp>
#include <core/Log.hpp>

namespace rstudio {
namespace core {

// default values for static members
FileLock::LockType FileLock::type_(FileLock::LOCKTYPE_ADVISORY);
boost::posix_time::seconds FileLock::timeoutInterval_(30);
boost::posix_time::seconds FileLock::refreshRate_(20);

boost::shared_ptr<FileLock> FileLock::create()
{
   switch (FileLock::getLockType())
   {
   case LOCKTYPE_ADVISORY:  return boost::shared_ptr<FileLock>(new AdvisoryFileLock());
   case LOCKTYPE_LINKBASED: return boost::shared_ptr<FileLock>(new LinkBasedFileLock());
   }
}

void FileLock::refresh()
{
   AdvisoryFileLock::refresh();
   LinkBasedFileLock::refresh();
}

namespace {

void schedulePeriodicExecution(
      boost::asio::deadline_timer* pTimer,
      boost::posix_time::seconds interval,
      boost::function<void()> callback)
{
   try
   {
      // execute callback
      callback();

      // reschedule
      boost::system::error_code errc;
      pTimer->expires_at(pTimer->expires_at() + interval, errc);
      if (errc)
      {
         LOG_ERROR(Error(errc, ERROR_LOCATION));
         return;
      }
      
      pTimer->async_wait(boost::bind(
                            schedulePeriodicExecution,
                            pTimer,
                            interval,
                            callback));
   }
   catch (...)
   {
      // swallow errors
   }
}

} // end anonymous namespace

void FileLock::refreshPeriodically(boost::asio::io_service& service,
                                   boost::posix_time::seconds interval)
{
   // protect against re-entrancy
   static bool s_isRefreshing = false;
   if (s_isRefreshing)
      return;
   s_isRefreshing = true;
   
   static boost::asio::deadline_timer timer(service, interval);
   schedulePeriodicExecution(&timer, interval, FileLock::refresh);
}

} // end namespace core
} // end namespace rstudio
