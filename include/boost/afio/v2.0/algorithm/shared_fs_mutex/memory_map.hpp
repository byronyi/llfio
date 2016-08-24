/* memory_map.hpp
Efficient large actor read-write lock
(C) 2016 Niall Douglas http://www.nedprod.com/
File Created: Aug 2016


Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/

#ifndef BOOST_AFIO_SHARED_FS_MUTEX_MEMORY_MAP_HPP
#define BOOST_AFIO_SHARED_FS_MUTEX_MEMORY_MAP_HPP

#include "../../map_handle.hpp"
#include "base.hpp"

#include "../boost-lite/include/algorithm/hash.hpp"
#include "../boost-lite/include/spinlock.hpp"

//! \file memory_map.hpp Provides algorithm::shared_fs_mutex::memory_map

BOOST_AFIO_V2_NAMESPACE_BEGIN

namespace algorithm
{
  namespace shared_fs_mutex
  {
    /*! \class memory_map
    \brief Many entity memory mapped shared/exclusive file system based lock

    This is the highest performing filing system mutex in AFIO, but it comes with a long list of potential
    gotchas. It works by creating a random temporary file somewhere on the system and placing its path
    in a file in the lock file location. The random temporary file is mapped into memory by all processes
    using the lock where an open addressed hash table is kept. Each entity is hashed into somewhere in the
    hash table and its individual spin lock is used to implement the exclusion. As with `byte_ranges`, each
    entity is locked individually in sequence but if a particular lock fails, all are unlocked and the
    list is randomised before trying again. Because this locking
    implementation is entirely implemented in userspace using shared memory without any kernel syscalls,
    performance is probably as fast as any many-arbitrary-entity shared locking system could be.

    Performance ought to be excellent so long as no lock user attempts to use the lock from across a
    networked filing system. As soon as a locking entity fails to find the temporary file given in the
    lock file location, it will *permanently* degrade the memory mapped lock into a `byte_ranges` lock.
    This means that a single once off networked filing system user will permanently reduce performance
    to that of `byte_ranges`.

    - Compatible with networked file systems, though with a substantial performance degrade as described above.
    - Linear complexity to number of concurrent users up until hash table starts to get full or hashed
    entries collide.
    - Sudden power loss during use is recovered from.
    - Safe for multithreaded usage of the same instance.
    - In the lightly contended case, an order of magnitude faster than any other `shared_fs_mutex` algorithm.

    Caveats:
    - No ability to sleep until a lock becomes free, so CPUs are spun at 100%.
    - Sudden process exit with locks held will deadlock all other users.
    - Exponential complexity to number of entities being concurrently locked.
    - Hyperbolic i.e. pathological complexity to contention. Most SMP and especially
    NUMA systems have a finite bandwidth for atomic compare and swap operations, and every attempt to
    lock or unlock an entity under this implementation is several of those operations. Under heavy contention,
    whole system performance very noticeably nose dives from excessive atomic operations, things like audio and the
    mouse pointer will stutter.
    - Sometimes different entities hash to the same offset and collide with one another, causing poor performance.
    - Byte range locks need to work properly on your system. Misconfiguring NFS or Samba
    to cause byte range locks to not work right will produce bad outcomes.
    - Memory mapped files need to be cache unified with normal i/o in your OS kernel. Known OSs which
    don't use a unified cache for memory mapped and normal i/o are QNX, OpenBSD. Furthermore, doing
    normal i/o and memory mapped i/o to the same file needs to not corrupt the file. In the past,
    there have been editions of the Linux kernel and the OS X kernel which did this.
    - If your OS doesn't have sane byte range locks (OS X, BSD, older Linuxes) and multiple
    objects in your process use the same lock file, misoperation will occur.
    */
    class memory_map : public shared_fs_mutex
    {
    public:
      //! The type of an entity id
      using entity_type = shared_fs_mutex::entity_type;
      //! The type of a sequence of entities
      using entities_type = shared_fs_mutex::entities_type;

    private:
      using _spinlock_type = boost_lite::configurable_spinlock::shared_spinlock<>;
      static constexpr size_t HashIndexSize = 4096;
      using Hasher = boost_lite::algorithm::hash::fnv1a_hash<entity_type::value_type>;
      static constexpr size_t _container_entries = HashIndexSize / sizeof(_spinlock_type);
      using _hash_index_type = std::array<_spinlock_type, _container_entries>;

      file_handle _h, _temph;
      file_handle::extent_guard _hlockinuse;  // shared lock of last byte of _h marking if lock is in use
      file_handle::extent_guard _hmapinuse;   // shared lock of second last byte of _h marking if mmap is in use
      map_handle _hmap, _temphmap;

      _hash_index_type &_index() const
      {
        _hash_index_type *ret = (_hash_index_type *) _temphmap.address();
        return *ret;
      }

      memory_map(file_handle &&h, file_handle &&temph, file_handle::extent_guard &&hlockinuse, file_handle::extent_guard &&hmapinuse, map_handle &&hmap, map_handle &&temphmap)
          : _h(std::move(h))
          , _temph(std::move(temph))
          , _hlockinuse(std::move(hlockinuse))
          , _hmapinuse(std::move(hmapinuse))
          , _hmap(std::move(hmap))
          , _temphmap(std::move(temphmap))
      {
      }
      memory_map(const memory_map &) = delete;
      memory_map &operator=(const memory_map &) = delete;

    public:
      //! Move constructor
      memory_map(memory_map &&o) noexcept : _h(std::move(o._h)), _temph(std::move(o._temph)), _hlockinuse(std::move(o._hlockinuse)), _hmapinuse(std::move(o._hmapinuse)), _hmap(std::move(o._hmap)), _temphmap(std::move(o._temphmap)) {}
      //! Move assign
      memory_map &operator=(memory_map &&o) noexcept
      {
        this->~memory_map();
        new(this) memory_map(std::move(o));
        return *this;
      }
      ~memory_map()
      {
        // Release my shared locks and try locking entire file exclusively
        _hmapinuse.unlock();
        _hlockinuse.unlock();
        auto lockresult = _h.try_lock(0, (handle::extent_type) -1, true);
        if(lockresult)
        {
          // This means I am the last user, so zop the file contents as temp file is about to go away
          char buffer[4096];
          memset(buffer, 0, sizeof(buffer));
          (void) _h.write(0, buffer, sizeof(buffer));
          // You might wonder why I am now truncating to zero? It's to ensure any
          // memory maps definitely get written with zeros before truncation, some
          // OSs don't reflect zeros into memory maps upon truncation for quite a
          // long time
          _h.truncate(0);
#ifndef _WIN32
          // On POSIX we also need to delete the temp file
          _temph.unlink();
#endif
        }
      }

      //! Initialises a shared filing system mutex using the file at \em lockfile
      //[[bindlib::make_free]]
      static result<memory_map> fs_mutex_map(file_handle::path_type lockfile) noexcept
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(0);
        try
        {
          BOOST_OUTCOME_FILTER_ERROR(ret, file_handle::file(std::move(lockfile), file_handle::mode::write, file_handle::creation::if_needed, file_handle::caching::temporary, file_handle::flag::win_delete_on_last_close));
          file_handle temph;
          // Am I the first person to this file? Lock the entire file exclusively
          auto lockinuse = ret.try_lock(0, (handle::extent_type) -1, true);
          file_handle::extent_guard mapinuse;
          if(lockinuse.has_error())
          {
            if(lockinuse.get_error().value() != ETIMEDOUT)
              return lockinuse.get_error();
            // Somebody else is also using this file, so try to read the hash index file I ought to use
            lockinuse = ret.lock((handle::extent_type) -1, 1, false);  // last byte shared access
            char buffer[65536];
            memset(buffer, 0, sizeof(buffer));
            {
              BOOST_OUTCOME_FILTER_ERROR(_, ret.read(0, buffer, 65535));
              (void) _;
            }
            fixme_path::value_type *temphpath = (fixme_path::value_type *) buffer;
            result<file_handle> _temph;
            // If path is zeroed, fall back onto backup lock
            if(!buffer[1])
              goto use_fall_back_lock;
            else
              _temph = file_handle::file(temphpath, file_handle::mode::write, file_handle::creation::open_existing, file_handle::caching::temporary, file_handle::flag::win_delete_on_last_close);
            // If temp file doesn't exist, I am on a different machine
            if(!_temph)
            {
              // Zop the path so any new entrants into this lock will go to the fallback lock
              char buffer[4096];
              memset(buffer, 0, sizeof(buffer));
              (void) ret.write(0, buffer, sizeof(buffer));
            use_fall_back_lock:
              // I am guaranteed that all mmap users have locked the second last byte
              // and will unlock it once everyone has stopped using the mmap, so make
              // absolutely sure the mmap is not in use by anyone by taking an exclusive
              // lock on the second final byte
              BOOST_OUTCOME_FILTER_ERROR(mapinuse2, ret.lock((handle::extent_type) -2, 1, true));
              // TODO, awaiting a template parameter to specify the backup lock
              abort();
            }
            else
            {
              // Mark the map as being in use by me too
              BOOST_OUTCOME_FILTER_ERROR(mapinuse2, ret.lock((handle::extent_type) -2, 1, false));
              mapinuse = std::move(mapinuse2);
              temph = std::move(_temph.get());
            }
          }
          else
          {
            // I am the first person to be using this (stale?) file, so create a new hash index file and write its path
            ret.truncate(0);
            BOOST_OUTCOME_FILTER_ERROR(_temph, file_handle::temp_file());
            temph = std::move(_temph);
            auto temppath(temph.path());
            temph.truncate(HashIndexSize);
            // Write the path of my new hash index file and convert my lock to a shared one
            {
              BOOST_OUTCOME_FILTER_ERROR(_, ret.write(0, (const char *) temppath.c_str(), temppath.native().size() * sizeof(*temppath.c_str())));
              (void) _;
            }
            // Convert exclusive whole file lock into lock in use
            BOOST_OUTCOME_FILTER_ERROR(lockinuse2, ret.lock((handle::extent_type) -1, 1, false));
            BOOST_OUTCOME_FILTER_ERROR(mapinuse2, ret.lock((handle::extent_type) -2, 1, false));
            mapinuse = std::move(mapinuse2);
            lockinuse = std::move(lockinuse2);
          }
          // Map the files into memory, being very careful that ret is only ever mapped read only
          BOOST_OUTCOME_FILTER_ERROR(hsection, section_handle::section(ret, 0, section_handle::flag::read));
          BOOST_OUTCOME_FILTER_ERROR(temphsection, section_handle::section(temph, HashIndexSize));
          BOOST_OUTCOME_FILTER_ERROR(hmap, map_handle::map(hsection, 0, 0, section_handle::flag::read));
          BOOST_OUTCOME_FILTER_ERROR(temphmap, map_handle::map(temphsection, HashIndexSize));
          return memory_map(std::move(ret), std::move(temph), std::move(lockinuse.get()), std::move(mapinuse), std::move(hmap), std::move(temphmap));
        }
        BOOST_OUTCOME_CATCH_EXCEPTION_TO_RESULT(memory_map)
      }

      //! Return the handle to file being used for this lock
      const file_handle &handle() const noexcept { return _h; }

    protected:
      struct _entity_idx
      {
        unsigned value : 31;
        unsigned exclusive : 1;
      };
      // Create a cache of entities to their indices, eliding collisions where necessary
      static span<_entity_idx> _hash_entities(_entity_idx *entity_to_idx, entities_type &entities)
      {
        _entity_idx *ep = entity_to_idx;
        for(size_t n = 0; n < entities.size(); n++)
        {
          ep->value = Hasher()(entities[n].value) % _container_entries;
          ep->exclusive = entities[n].exclusive;
          bool skip = false;
          for(size_t m = 0; m < n; m++)
          {
            if(entity_to_idx[m].value == ep->value)
            {
              if(ep->exclusive && !entity_to_idx[m].exclusive)
                entity_to_idx[m].exclusive = true;
              skip = true;
            }
          }
          if(!skip)
            ++ep;
        }
        return span<_entity_idx>(entity_to_idx, ep - entity_to_idx);
      }
      virtual result<void> _lock(entities_guard &out, deadline d, bool spin_not_sleep) noexcept override final
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(this);
        if(_hmap.address()[1] != 0)
        {
          // TODO: Fall back onto backup locking system
          abort();
        }
        stl11::chrono::steady_clock::time_point began_steady;
        stl11::chrono::system_clock::time_point end_utc;
        if(d)
        {
          if((d).steady)
            began_steady = stl11::chrono::steady_clock::now();
          else
            end_utc = (d).to_time_point();
        }
        span<_entity_idx> entity_to_idx(_hash_entities((_entity_idx *) alloca(sizeof(_entity_idx) * out.entities.size()), out.entities));
        _hash_index_type &index = _index();
        // Fire this if an error occurs
        auto disableunlock = detail::Undoer([&] { out.release(); });
        size_t n;
        for(;;)
        {
          size_t was_contended = (size_t) -1;
          {
            auto undo = detail::Undoer([&] {
              // 0 to (n-1) need to be closed
              if(n > 0)
              {
                --n;
                // Now 0 to n needs to be closed
                for(; n > 0; n--)
                  entity_to_idx[n].exclusive ? index[entity_to_idx[n].value].unlock() : index[entity_to_idx[n].value].unlock_shared();
                entity_to_idx[0].exclusive ? index[entity_to_idx[0].value].unlock() : index[entity_to_idx[0].value].unlock_shared();
              }
            });
            for(n = 0; n < entity_to_idx.size(); n++)
            {
              if(!(entity_to_idx[n].exclusive ? index[entity_to_idx[n].value].try_lock() : index[entity_to_idx[n].value].try_lock_shared()))
              {
                was_contended = n;
                goto failed;
              }
            }
            // Everything is locked, exit
            undo.dismiss();
            disableunlock.dismiss();
            return make_result<void>();
          }
        failed:
          if(d)
          {
            if((d).steady)
            {
              if(stl11::chrono::steady_clock::now() >= (began_steady + stl11::chrono::nanoseconds((d).nsecs)))
                return make_errored_result<void>(ETIMEDOUT);
            }
            else
            {
              if(stl11::chrono::system_clock::now() >= end_utc)
                return make_errored_result<void>(ETIMEDOUT);
            }
          }
          // Move was_contended to front and randomise rest of out.entities
          std::swap(entity_to_idx[was_contended], entity_to_idx[0]);
          auto front = entity_to_idx.begin();
          ++front;
          std::random_shuffle(front, entity_to_idx.end());
          if(!spin_not_sleep)
            std::this_thread::yield();
        }
        // return make_result<void>();
      }

    public:
      virtual void unlock(entities_type entities, unsigned long long) noexcept override final
      {
        BOOST_AFIO_LOG_FUNCTION_CALL(this);
        span<_entity_idx> entity_to_idx(_hash_entities((_entity_idx *) alloca(sizeof(_entity_idx) * entities.size()), entities));
        _hash_index_type &index = _index();
        for(const auto &i : entity_to_idx)
        {
          i.exclusive ? index[i.value].unlock() : index[i.value].unlock_shared();
        }
      }
    };

  }  // namespace
}  // namespace

BOOST_AFIO_V2_NAMESPACE_END


#endif