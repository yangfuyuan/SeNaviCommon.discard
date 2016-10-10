
#include "CallbackQueue.h"
#include <assert.h>
#include <boost/scope_exit.hpp>
#include <boost/make_shared.hpp>

namespace NS_NaviCommon
{

CallbackQueue::CallbackQueue(bool enabled)
: calling_(0)
, enabled_(enabled)
{
}

CallbackQueue::~CallbackQueue()
{
  disable();
}

void CallbackQueue::enable()
{
  boost::mutex::scoped_lock lock(mutex_);
  enabled_ = true;

  condition_.notify_all();
}

void CallbackQueue::disable()
{
  boost::mutex::scoped_lock lock(mutex_);
  enabled_ = false;

  condition_.notify_all();
}

void CallbackQueue::clear()
{
  boost::mutex::scoped_lock lock(mutex_);

  callbacks_.clear();
}

bool CallbackQueue::isEmpty()
{
  boost::mutex::scoped_lock lock(mutex_);

  return callbacks_.empty() && calling_ == 0;
}

bool CallbackQueue::isEnabled()
{
  boost::mutex::scoped_lock lock(mutex_);

  return enabled_;
}

void CallbackQueue::setupTLS()
{
  if (!tls_.get())
  {
    tls_.reset(new TLS);
  }
}

void CallbackQueue::addCallback(const CallbackInterfacePtr& callback, unsigned long removal_id)
{
  CallbackInfo info;
  info.callback = callback;
  info.removal_id = removal_id;

  {
    boost::mutex::scoped_lock lock(mutex_);

    if (!enabled_)
    {
      return;
    }

    callbacks_.push_back(info);
  }

  {
    boost::mutex::scoped_lock lock(id_info_mutex_);

    M_IDInfo::iterator it = id_info_.find(removal_id);
    if (it == id_info_.end())
    {
      IDInfoPtr id_info(boost::make_shared<IDInfo>());
      id_info->id = removal_id;
      id_info_.insert(std::make_pair(removal_id, id_info));
    }
  }

  condition_.notify_one();
}

CallbackQueue::IDInfoPtr CallbackQueue::getIDInfo(unsigned long id)
{
  boost::mutex::scoped_lock lock(id_info_mutex_);
  M_IDInfo::iterator it = id_info_.find(id);
  if (it != id_info_.end())
  {
    return it->second;
  }

  return IDInfoPtr();
}

void CallbackQueue::removeByID(unsigned long removal_id)
{
  setupTLS();

  {
    IDInfoPtr id_info;
    {
      boost::mutex::scoped_lock lock(id_info_mutex_);
      M_IDInfo::iterator it = id_info_.find(removal_id);
      if (it != id_info_.end())
      {
        id_info = it->second;
      }
      else
      {
        return;
      }
    }

    // If we're being called from within a callback from our queue, we must unlock the shared lock we already own
    // here so that we can take a unique lock.  We'll re-lock it later.
    if (tls_->calling_in_this_thread == id_info->id)
    {
      id_info->calling_rw_mutex.unlock_shared();
    }

    {
      boost::unique_lock<boost::shared_mutex> rw_lock(id_info->calling_rw_mutex);
      boost::mutex::scoped_lock lock(mutex_);
      D_CallbackInfo::iterator it = callbacks_.begin();
      for (; it != callbacks_.end();)
      {
        CallbackInfo& info = *it;
        if (info.removal_id == removal_id)
        {
          it = callbacks_.erase(it);
        }
        else
        {
          ++it;
        }
      }
    }

    if (tls_->calling_in_this_thread == id_info->id)
    {
      id_info->calling_rw_mutex.lock_shared();
    }
  }

  // If we're being called from within a callback, we need to remove the callbacks that match the id that have already been
  // popped off the queue
  {
    D_CallbackInfo::iterator it = tls_->callbacks.begin();
    D_CallbackInfo::iterator end = tls_->callbacks.end();
    for (; it != end; ++it)
    {
      CallbackInfo& info = *it;
      if (info.removal_id == removal_id)
      {
        info.marked_for_removal = true;
      }
    }
  }

  {
    boost::mutex::scoped_lock lock(id_info_mutex_);
    id_info_.erase(removal_id);
  }
}

CallbackQueue::CallOneResult CallbackQueue::callOne(Duration timeout)
{
  setupTLS();
  TLS* tls = tls_.get();

  CallbackInfo cb_info;

  {
    boost::mutex::scoped_lock lock(mutex_);

    if (!enabled_)
    {
      return Disabled;
    }

    if (callbacks_.empty())
    {
      if (!timeout.isZero())
      {
        condition_.timed_wait(lock, boost::posix_time::microseconds(timeout.toSec() * 1000000.0f));
      }

      if (callbacks_.empty())
      {
        return Empty;
      }

      if (!enabled_)
      {
        return Disabled;
      }
    }

    D_CallbackInfo::iterator it = callbacks_.begin();
    for (; it != callbacks_.end();)
    {
      CallbackInfo& info = *it;

      if (info.marked_for_removal)
      {
        it = callbacks_.erase(it);
        continue;
      }

      if (info.callback->ready())
      {
        cb_info = info;
        it = callbacks_.erase(it);
        break;
      }

      ++it;
    }

    if (!cb_info.callback)
    {
      return TryAgain;
    }

    ++calling_;
  }

  bool was_empty = tls->callbacks.empty();
  tls->callbacks.push_back(cb_info);
  if (was_empty)
  {
    tls->cb_it = tls->callbacks.begin();
  }

  CallOneResult res = callOneCB(tls);
  if (res != Empty)
  {
    boost::mutex::scoped_lock lock(mutex_);
    --calling_;
  }
  return res;
}

void CallbackQueue::callAvailable(Duration timeout)
{
  setupTLS();
  TLS* tls = tls_.get();

  {
    boost::mutex::scoped_lock lock(mutex_);

    if (!enabled_)
    {
      return;
    }

    if (callbacks_.empty())
    {
      if (!timeout.isZero())
      {
        condition_.timed_wait(lock, boost::posix_time::microseconds(timeout.toSec() * 1000000.0f));
      }

      if (callbacks_.empty() || !enabled_)
      {
        return;
      }
    }

    bool was_empty = tls->callbacks.empty();

    tls->callbacks.insert(tls->callbacks.end(), callbacks_.begin(), callbacks_.end());
    callbacks_.clear();

    calling_ += tls->callbacks.size();

    if (was_empty)
    {
      tls->cb_it = tls->callbacks.begin();
    }
  }

  size_t called = 0;

  while (!tls->callbacks.empty())
  {
    if (callOneCB(tls) != Empty)
    {
      ++called;
    }
  }

  {
    boost::mutex::scoped_lock lock(mutex_);
    calling_ -= called;
  }
}

CallbackQueue::CallOneResult CallbackQueue::callOneCB(TLS* tls)
{
  // Check for a recursive call.  If recursive, increment the current iterator.  Otherwise
  // set the iterator it the beginning of the thread-local callbacks
  if (tls->calling_in_this_thread == 0xffffffffffffffffULL)
  {
    tls->cb_it = tls->callbacks.begin();
  }

  if (tls->cb_it == tls->callbacks.end())
  {
    return Empty;
  }

  assert(!tls->callbacks.empty());
  assert(tls->cb_it != tls->callbacks.end());

  CallbackInfo info = *tls->cb_it;
  CallbackInterfacePtr& cb = info.callback;

  IDInfoPtr id_info = getIDInfo(info.removal_id);
  if (id_info)
  {
    boost::shared_lock<boost::shared_mutex> rw_lock(id_info->calling_rw_mutex);

    unsigned long last_calling = tls->calling_in_this_thread;
    tls->calling_in_this_thread = id_info->id;

    CallbackInterface::CallResult result = CallbackInterface::Invalid;

    {
      // Ensure that thread id gets restored, even if callback throws.
      // This is done with RAII rather than try-catch so that the source
      // of the original exception is not masked in a crash report.
      BOOST_SCOPE_EXIT(&tls, &last_calling)
      {
        tls->calling_in_this_thread = last_calling;
      }
      BOOST_SCOPE_EXIT_END

      if (info.marked_for_removal)
      {
        tls->cb_it = tls->callbacks.erase(tls->cb_it);
      }
      else
      {
        tls->cb_it = tls->callbacks.erase(tls->cb_it);
        result = cb->call();
      }
    }

    // Push TryAgain callbacks to the back of the shared queue
    if (result == CallbackInterface::TryAgain && !info.marked_for_removal)
    {
      boost::mutex::scoped_lock lock(mutex_);
      callbacks_.push_back(info);

      return TryAgain;
    }

    return Called;
  }
  else
  {
    tls->cb_it = tls->callbacks.erase(tls->cb_it);
  }

  return Called;
}

}
