/* Dynamic thread pool group
(C) 2020 Niall Douglas <http://www.nedproductions.biz/> (9 commits)
File Created: Dec 2020


Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License in the accompanying file
Licence.txt or at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.


Distributed under the Boost Software License, Version 1.0.
    (See accompanying file Licence.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

#ifndef LLFIO_DYNAMIC_THREAD_POOL_GROUP_H
#define LLFIO_DYNAMIC_THREAD_POOL_GROUP_H

#include "deadline.h"

#include <memory>  // for unique_ptr and shared_ptr

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)  // dll interface
#endif

LLFIO_V2_NAMESPACE_EXPORT_BEGIN

class dynamic_thread_pool_group_impl;

namespace detail
{
  struct global_dynamic_thread_pool_impl;
  LLFIO_HEADERS_ONLY_FUNC_SPEC global_dynamic_thread_pool_impl &global_dynamic_thread_pool() noexcept;
}  // namespace detail

/*! \class dynamic_thread_pool_group
\brief Work group within the global dynamic thread pool.

Some operating systems provide a per-process global kernel thread pool capable of
dynamically adjusting its kernel thread count to how many of the threads in
the pool are currently blocked. The platform will choose the exact strategy used,
but as an example of a strategy, one might keep creating new kernel threads
so long as the total threads currently running and not blocked on page faults,
i/o or syscalls, is below the hardware concurrency. Similarly, if more threads
are running and not blocked than hardware concurrency, one might remove kernel
threads from executing work. Such a strategy would dynamically increase
concurrency until all CPUs are busy, but reduce concurrency if more work is
being done than CPUs available.

Such dynamic kernel thread pools are excellent for CPU bound processing, you
simply fire and forget work into them. However, for i/o bound processing, you
must be careful as there are gotchas. For non-seekable i/o, it is very possible
that there could be 100k handles upon which we do i/o. Doing i/o on
100k handles using a dynamic thread pool would in theory cause the creation
of 100k kernel threads, which would not be wise. A much better solution is
to use an `io_multiplexer` to await changes in large sets of i/o handles.

For seekable i/o, the same problem applies, but worse again: an i/o bound problem
would cause a rapid increase in the number of kernel threads, which by
definition makes i/o even more congested. Basically the system runs off
into pathological performance loss. You must therefore never naively do
i/o bound work (e.g. with memory mapped files) from within a dynamic thread
pool without employing some mechanism to force concurrency downwards if
the backing storage is congested.

## Work groups

Instances of this class contain zero or more work items. Each work item
is asked for its next item of work, and if an item of work is available,
that item of work is executed by the global kernel thread pool at a time
of its choosing. It is NEVER possible that any one work item is concurrently
executed at a time, each work item is always sequentially executed with
respect to itself. The only concurrency possible is *across* work items.
Therefore, if you want to execute the same piece of code concurrently,
you need to submit a separate work item for each possible amount of
concurrency (e.g. `std::thread::hardware_concurrency()`).

You can have as many or as few items of work as you like. You can
dynamically submit additional work items at any time. The group of work items can
be waited upon to complete, after which the work group becomes reset as
if back to freshly constructed. You can also stop executing all the work
items in the group, even if they have not fully completed. If any work
item returns a failure, this equals a `stop()`, and the next `wait()` will
return that error.

Work items may create sub work groups as part of their
operation. If they do so, the work items from such nested work groups are
scheduled preferentially. This ensures good forward progress, so if you
have 100 work items each of which do another 100 work items, you don't get
10,000 slowly progressing work. Rather, the work items in the first set
progress slowly, whereas the work items in the second set progress quickly.

`work_item::next()` may optionally set a deadline to delay when that work
item ought to be processed again. Deadlines can be relative or absolute.

## C++ 23 Executors

As with elsewhere in LLFIO, as a low level facility, we don't implement
https://wg21.link/P0443 Executors, but it is trivially easy to implement
a dynamic equivalent to `std::static_thread_pool` using this class.

## Implementation notes

### Microsoft Windows

On Microsoft Windows, the Win32 thread pool API is used (https://docs.microsoft.com/en-us/windows/win32/procthread/thread-pool-api).
This is an IOCP-aware thread pool which will dynamically increase the number
of kernel threads until none are blocked. If more kernel threads
are running than twice the number of CPUs in the system, the number of kernel
threads is dynamically reduced. The maximum number of kernel threads which
will run simultaneously is 500. Note that the Win32 thread pool is shared
across the process by multiple Windows facilities.

Note that the Win32 thread pool has built in support for IOCP, so if you
have a custom i/o multiplexer, you can use the global Win32 thread pool
to execute i/o completions handling. See `CreateThreadpoolIo()` for more.

No dynamic memory allocation is performed by this implementation outside
of the initial `make_dynamic_thread_pool_group()`. The Win32 thread pool
API may perform dynamic memory allocation internally, but that is outside
our control.

### Linux

On Linux, a similar strategy to Microsoft Windows' approach is used. We
dynamically increase the number of kernel threads until none are sleeping
awaiting i/o. If more kernel threads are running than 1.5x the number of
CPUs in the system, the number of kernel threads is dynamically reduced.
For portability, we also gate the maximum number of kernel threads to 500.
Note that **all** the kernel threads for the current process are considered,
not just the kernel threads created by this thread pool implementation.
Therefore, if you have alternative thread pool implementations (e.g. OpenMP,
`std::async`), those are also included in the dynamic adjustment.

As this is wholly implemented by this library, dynamic memory allocation
occurs in the initial `make_dynamic_thread_pool_group()`, but otherwise
the implementation does not perform dynamic memory allocations.
*/
class LLFIO_DECL dynamic_thread_pool_group
{
public:
  //! An individual item of work within the work group.
  class work_item
  {
    friend struct detail::global_dynamic_thread_pool_impl;
    friend class dynamic_thread_pool_group_impl;
    dynamic_thread_pool_group_impl *_parent{nullptr};
    void *_internalworkh{nullptr}, *_internaltimerh{nullptr};
    work_item *_prev{nullptr}, *_next{nullptr};
    intptr_t _nextwork{-1};
    std::chrono::steady_clock::time_point _timepoint1;
    std::chrono::system_clock::time_point _timepoint2;
    int _internalworkh_inuse{0};

  protected:
    work_item() = default;
    work_item(const work_item &o) = delete;
    work_item(work_item &&o) noexcept
        : _parent(o._parent)
        , _internalworkh(o._internalworkh)
        , _internaltimerh(o._internaltimerh)
        , _prev(o._prev)
        , _next(o._next)
        , _nextwork(o._nextwork)
        , _timepoint1(o._timepoint1)
        , _timepoint2(o._timepoint2)
    {
      assert(o._parent == nullptr);
      assert(o._internalworkh == nullptr);
      assert(o._internaltimerh == nullptr);
      if(o._parent != nullptr || o._internalworkh != nullptr)
      {
        LLFIO_LOG_FATAL(this, "FATAL: dynamic_thread_pool_group::work_item was relocated in memory during use!");
        abort();
      }
      o._prev = o._next = nullptr;
      o._nextwork = -1;
    }
    work_item &operator=(const work_item &) = delete;
    work_item &operator=(work_item &&) = delete;

  public:
    virtual ~work_item()
    {
      assert(_nextwork == -1);
      if(_nextwork != -1)
      {
        LLFIO_LOG_FATAL(this, "FATAL: dynamic_thread_pool_group::work_item destroyed before all work was done!");
        abort();
      }
      assert(_internalworkh == nullptr);
      assert(_internaltimerh == nullptr);
      assert(_parent == nullptr);
      if(_internalworkh != nullptr || _parent != nullptr)
      {
        LLFIO_LOG_FATAL(this, "FATAL: dynamic_thread_pool_group::work_item destroyed before group_complete() was executed!");
        abort();
      }
    }

    //! Returns the parent work group between successful submission and just before `group_complete()`.
    dynamic_thread_pool_group *parent() const noexcept { return reinterpret_cast<dynamic_thread_pool_group *>(_parent); }

    /*! Invoked by the i/o thread pool to determine if this work item
    has more work to do.

    \return If there is no work _currently_ available to do, but there
    might be some later, you should return zero. You will be called again
    later after other work has been done. If you return -1, you are
    saying that no further work will be done, and the group need never
    call you again. If you have more work you want to do, return any
    other value.
    \param d Optional delay before the next item of work ought to be
    executed (return != 0), or `next()` ought to be called again to
    determine the next item (return == 0). On entry `d` is set to no
    delay, so if you don't modify it, the next item of work occurs
    as soon as possible.

    Note that this function is called from multiple kernel threads.
    You must NOT do any significant work in this function.
    In particular do NOT call any dynamic thread pool group function,
    as you will experience deadlock.

    `dynamic_thread_pool_group::current_work_item()` may have any
    value during this call.
    */
    virtual intptr_t next(deadline &d) noexcept = 0;

    /*! Invoked by the i/o thread pool to perform the next item of work.

    \return Any failure causes all remaining work in this group to
    be cancelled as soon as possible.
    \param work The value returned by `next()`.

    Note that this function is called from multiple kernel threads,
    and may not be the kernel thread from which `next()`
    was called.

    `dynamic_thread_pool_group::current_work_item()` will always be
    `this` during this call.
    */
    virtual result<void> operator()(intptr_t work) noexcept = 0;

    /*! Invoked by the i/o thread pool when all work in this thread
    pool group is complete.

    `cancelled` indicates if this is an abnormal completion. If its
    error compares equal to `errc::operation_cancelled`, then `stop()`
    was called.

    Just before this is called for all work items submitted, the group
    becomes reset to fresh, and `parent()` becomes null. You can resubmit
    this work item, but do not submit other work items until their
    `group_complete()` has been invoked.

    Note that this function is called from multiple kernel threads.

    `dynamic_thread_pool_group::current_work_item()` may have any
    value during this call.
    */
    virtual void group_complete(const result<void> &cancelled) noexcept { (void) cancelled; }
  };

  virtual ~dynamic_thread_pool_group() {}

  /*! \brief Threadsafe. Submit one or more work items for execution. Note that you can submit more later.

  Note that if the group is currently stopping, you cannot submit more
  work until the group has stopped. An error code comparing equal to
  `errc::operation_canceled` is returned if you try.
  */
  virtual result<void> submit(span<work_item *> work) noexcept = 0;
  //! \overload
  result<void> submit(work_item *wi) noexcept { return submit(span<work_item *>(&wi, 1)); }
  //! \overload
  LLFIO_TEMPLATE(class T)
  LLFIO_TREQUIRES(LLFIO_TPRED(!std::is_pointer<T>::value), LLFIO_TPRED(std::is_base_of<work_item, T>::value))
  result<void> submit(span<T> wi) noexcept
  {
    auto *wis = (T **) alloca(sizeof(T *) * wi.size());
    for(size_t n = 0; n < wi.size(); n++)
    {
      wis[n] = &wi[n];
    }
    return submit(span<work_item *>((work_item **) wis, wi.size()));
  }

  //! Threadsafe. Cancel any remaining work previously submitted, but without blocking (use `wait()` to block).
  virtual result<void> stop() noexcept = 0;

  /*! \brief Threadsafe. True if a work item reported an error, or
  `stop()` was called, but work items are still running.
  */
  virtual bool stopping() const noexcept = 0;

  //! Threadsafe. True if all the work previously submitted is complete.
  virtual bool stopped() const noexcept = 0;

  //! Threadsafe. Wait for work previously submitted to complete, returning any failures by any work item.
  virtual result<void> wait(deadline d = {}) const noexcept = 0;
  //! \overload
  template <class Rep, class Period> result<bool> wait_for(const std::chrono::duration<Rep, Period> &duration) const noexcept
  {
    auto r = wait(duration);
    if(!r && r.error() == errc::timed_out)
    {
      return false;
    }
    OUTCOME_TRY(std::move(r));
    return true;
  }
  //! \overload
  template <class Clock, class Duration> result<bool> wait_until(const std::chrono::time_point<Clock, Duration> &timeout) const noexcept
  {
    auto r = wait(timeout);
    if(!r && r.error() == errc::timed_out)
    {
      return false;
    }
    OUTCOME_TRY(std::move(r));
    return true;
  }

  //! Returns the work item nesting level which would be used if a new dynamic thread pool group were created within the current work item.
  static LLFIO_HEADERS_ONLY_MEMFUNC_SPEC size_t current_nesting_level() noexcept;
  //! Returns the work item the calling thread is running within, if any.
  static LLFIO_HEADERS_ONLY_MEMFUNC_SPEC work_item *current_work_item() noexcept;
};
//! A unique ptr to a work group within the global dynamic thread pool.
using dynamic_thread_pool_group_ptr = std::unique_ptr<dynamic_thread_pool_group>;

//! Creates a new work group within the global dynamic thread pool.
LLFIO_HEADERS_ONLY_FUNC_SPEC result<dynamic_thread_pool_group_ptr> make_dynamic_thread_pool_group() noexcept;

// BEGIN make_free_functions.py
// END make_free_functions.py

LLFIO_V2_NAMESPACE_END

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#if LLFIO_HEADERS_ONLY == 1 && !defined(DOXYGEN_SHOULD_SKIP_THIS)
#define LLFIO_INCLUDED_BY_HEADER 1
#include "detail/impl/dynamic_thread_pool_group.ipp"
#undef LLFIO_INCLUDED_BY_HEADER
#endif

#endif
