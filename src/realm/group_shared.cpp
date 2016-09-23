/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <sstream>
#include <type_traits>

#include <realm/util/features.h>
#include <realm/util/errno.hpp>
#include <realm/util/safe_int_ops.hpp>
#include <realm/util/thread.hpp>
#include <realm/group_writer.hpp>
#include <realm/group_shared.hpp>
#include <realm/group_writer.hpp>
#include <realm/link_view.hpp>
#include <realm/replication.hpp>
#include <realm/impl/simulated_failure.hpp>
#include <realm/disable_sync_to_disk.hpp>

#ifndef _WIN32
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

//#define REALM_ENABLE_LOGFILE


using namespace realm;
using namespace realm::util;
using Durability = SharedGroupOptions::Durability;

namespace {

// Constants controlling the amount of uncommited writes in flight:
#ifdef REALM_ASYNC_DAEMON
const uint16_t max_write_slots = 100;
const uint16_t relaxed_sync_threshold = 50;
#endif

// value   change
// --------------------
// 4       Unknown
// 5       Introduction of SharedInfo::file_format_version and
//         SharedInfo::history_type.
// 6       Using new robust mutex emulation where applicable
// 7       Introducing `commit_in_critical_phase` and `sync_client_present`, and
//         changing `daemon_started` and `daemon_ready` from 1-bit to 8-bit
//         fields.
// 8       Placing the commitlog history inside the Realm file.
const uint_fast16_t g_shared_info_version = 8;

// The following functions are carefully designed for minimal overhead
// in case of contention among read transactions. In case of contention,
// they consume roughly 90% of the cycles used to start and end a read transaction.
//
// Each live version carries a "count" field, which combines a reference count
// of the readers bound to that version, and a single-bit "free" flag, which
// indicates that the entry does not hold valid data.
//
// The usage patterns are as follows:
//
// Read transactions guard their access to the version information by
// increasing the count field for the duration of the transaction.
// A non-zero count field also indicates that the free space associated
// with the version must remain intact. A zero count field indicates that
// no one refers to that version, so it's free lists can be merged into
// older free space and recycled.
//
// Only write transactions allocate and write new version entries. Also,
// Only write transactions scan the ringbuffer for older versions which
// are not used (count is zero) and free them. As write transactions are
// atomic (ensured by mutex), there is no race between freeing entries
// in the ringbuffer and allocating and writing them.
//
// There are no race conditions between read transactions. Read transactions
// never change the versioning information, only increment or decrement the
// count (and do so solely through the use of atomic operations).
//
// There is a race between read transactions incrementing the count field and
// a write transaction setting the free field. These are mutually exclusive:
// if a read sees the free field set, it cannot use the entry. As it has already
// incremented the count field (optimistically, anticipating that the free bit
// was clear), it must immediately decrement it again. Likewise, it is possible
// for one thread to set the free bit (anticipating a count of zero) while another
// thread increments the count (anticipating a clear free bit). In such cases,
// both threads undo their changes and back off.
//
// For all changes to the free field and the count field: It is important that changes
// to the free field takes the count field into account and vice versa, because they
// are changed optimistically but atomically. This is implemented by modifying the
// count field only by atomic add/sub of '2', and modifying the free field only by
// atomic add/sub of '1'.
//
// The following *memory* ordering is required for correctness:
//
// 1 Accesses within a transaction assumes the version info is valid *before*
//   reading it. This is achieved by synchronizing on the count field. Reading
//   the count field is an *acquire*, while clearing the free field is a *release*.
//
// 2 Accesses within a transaction assumes the version *remains* valid, so
//   all memory accesses with a read transaction must happen before
//   the changes to memory (by a write transaction). This is achieved
//   by use of *release* when the count field is decremented, and use of
//   *acquire* when the free field is set (by the write transaction).
//
// 3 Reads of the counter is synchronized by accesses to the put_pos variable
//   in the ringbuffer. Reading put_pos is an acquire and writing put_pos is
//   a release. Put pos is only ever written when a write transaction updates
//   the ring buffer.
//
// Discussion:
//
// - The design forces release/acquire style synchronization on every
//   begin_read/end_read. This feels like a bit too much, because *only* a write
//   transaction ever changes memory contents. Read transactions do not communicate,
//   so with the right scheme, synchronization should only be proportional to the
//   number of write transactions, not all transactions. The original design achieved
//   this by ONLY synchronizing on the put_pos (case 3 above), BUT the following
//   problems forced the addition of further synchronization:
//
//   - during begin_read, after reading put_pos, a thread may be arbitrarily delayed.
//     While delayed, the entry selected by put_pos may be freed and reused, and then
//     we will lack synchronization. Hence case 1 was added.
//
//   - a read transaction must complete all reads of memory before it can be changed
//     by another thread (this is an example of an anti-dependency). This requires
//     the solution described as case 2 above.
//
// - The use of release (in case 2 above) could - in principle - be replaced
//   by a read memory barrier which would be faster on some architectures, but
//   there is no standardized support for it.
//

template <typename T>
bool atomic_double_inc_if_even(std::atomic<T>& counter)
{
    T oldval = counter.fetch_add(2, std::memory_order_acquire);
    if (oldval & 1) {
        // oooops! was odd, adjust
        counter.fetch_sub(2, std::memory_order_relaxed);
        return false;
    }
    return true;
}

template <typename T>
inline void atomic_double_dec(std::atomic<T>& counter)
{
    counter.fetch_sub(2, std::memory_order_release);
}

template <typename T>
bool atomic_one_if_zero(std::atomic<T>& counter)
{
    T old_val = counter.fetch_add(1, std::memory_order_acquire);
    if (old_val != 0) {
        counter.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

template <typename T>
void atomic_dec(std::atomic<T>& counter)
{
    counter.fetch_sub(1, std::memory_order_release);
}

// nonblocking ringbuffer
class Ringbuffer {
public:
    // the ringbuffer is a circular list of ReadCount structures.
    // Entries from old_pos to put_pos are considered live and may
    // have an even value in 'count'. The count indicates the
    // number of referring transactions times 2.
    // Entries from after put_pos up till (not including) old_pos
    // are free entries and must have a count of ONE.
    // Cleanup is performed by starting at old_pos and incrementing
    // (atomically) from 0 to 1 and moving the put_pos. It stops
    // if count is non-zero. This approach requires that only a single thread
    // at a time tries to perform cleanup. This is ensured by doing the cleanup
    // as part of write transactions, where mutual exclusion is assured by the
    // write mutex.
    struct ReadCount {
        uint64_t version;
        uint64_t filesize;
        uint64_t current_top;
        // The count field acts as synchronization point for accesses to the above
        // fields. A succesfull inc implies acquire with regard to memory consistency.
        // Release is triggered by explicitly storing into count whenever a
        // new entry has been initialized.
        mutable std::atomic<uint32_t> count;
        uint32_t next;
    };

    Ringbuffer() noexcept
    {
        entries = init_readers_size;
        for (int i = 0; i < init_readers_size; i++) {
            data[i].version = 1;
            data[i].count.store(1, std::memory_order_relaxed);
            data[i].current_top = 0;
            data[i].filesize = 0;
            data[i].next = i + 1;
        }
        old_pos = 0;
        data[0].count.store(0, std::memory_order_relaxed);
        data[init_readers_size - 1].next = 0;
        put_pos.store(0, std::memory_order_release);
    }

    void dump()
    {
        uint_fast32_t i = old_pos;
        std::cout << "--- " << std::endl;
        while (i != put_pos.load()) {
            std::cout << "  used " << i << " : " << data[i].count.load() << " | " << data[i].version << std::endl;
            i = data[i].next;
        }
        std::cout << "  LAST " << i << " : " << data[i].count.load() << " | " << data[i].version << std::endl;
        i = data[i].next;
        while (i != old_pos) {
            std::cout << "  free " << i << " : " << data[i].count.load() << " | " << data[i].version << std::endl;
            i = data[i].next;
        }
        std::cout << "--- Done" << std::endl;
    }

    void expand_to(uint_fast32_t new_entries) noexcept
    {
        // std::cout << "expanding to " << new_entries << std::endl;
        // dump();
        for (uint_fast32_t i = entries; i < new_entries; i++) {
            data[i].version = 1;
            data[i].count.store(1, std::memory_order_relaxed);
            data[i].current_top = 0;
            data[i].filesize = 0;
            data[i].next = i + 1;
        }
        data[new_entries - 1].next = old_pos;
        data[put_pos.load(std::memory_order_relaxed)].next = entries;
        entries = new_entries;
        // dump();
    }

    static size_t compute_required_space(uint_fast32_t num_entries) noexcept
    {
        // get space required for given number of entries beyond the initial count.
        // NB: this not the size of the ringbuffer, it is the size minus whatever was
        // the initial size.
        return sizeof(ReadCount) * (num_entries - init_readers_size);
    }

    uint_fast32_t get_num_entries() const noexcept
    {
        return entries;
    }

    uint_fast32_t last() const noexcept
    {
        return put_pos.load(std::memory_order_acquire);
    }

    const ReadCount& get(uint_fast32_t idx) const noexcept
    {
        return data[idx];
    }

    const ReadCount& get_last() const noexcept
    {
        return get(last());
    }

    // This method re-initialises the last used ringbuffer entry to hold a new entry.
    // Precondition: This should *only* be done if the caller has established that she
    // is the only thread/process that has access to the ringbuffer. It is currently
    // called from init_versioning(), which is called by SharedGroup::open() under the
    // condition that it is the session initiator and under guard by the control mutex,
    // thus ensuring the precondition.
    // It is most likely not suited for any other use.
    ReadCount& reinit_last() noexcept
    {
        ReadCount& r = data[last()];
        // r.count is an atomic<> due to other usage constraints. Right here, we're
        // operating under mutex protection, so the use of an atomic store is immaterial
        // and just forced on us by the type of r.count.
        // You'll find the full discussion of how r.count is operated and why it must be
        // an atomic earlier in this file.
        r.count.store(0, std::memory_order_relaxed);
        return r;
    }

    const ReadCount& get_oldest() const noexcept
    {
        return get(old_pos.load(std::memory_order_relaxed));
    }

    bool is_full() const noexcept
    {
        uint_fast32_t idx = get(last()).next;
        return idx == old_pos.load(std::memory_order_relaxed);
    }

    uint_fast32_t next() const noexcept
    {
        // do not call this if the buffer is full!
        uint_fast32_t idx = get(last()).next;
        return idx;
    }

    ReadCount& get_next() noexcept
    {
        REALM_ASSERT(!is_full());
        return data[next()];
    }

    void use_next() noexcept
    {
        atomic_dec(get_next().count); // .store_release(0);
        put_pos.store(next(), std::memory_order_release);
    }

    void cleanup() noexcept
    {
        // invariant: entry held by put_pos has count > 1.
        // std::cout << "cleanup: from " << old_pos << " to " << put_pos.load_relaxed();
        // dump();
        while (old_pos.load(std::memory_order_relaxed) != put_pos.load(std::memory_order_relaxed)) {
            const ReadCount& r = get(old_pos.load(std::memory_order_relaxed));
            if (!atomic_one_if_zero(r.count))
                break;
            auto next_ndx = get(old_pos.load(std::memory_order_relaxed)).next;
            old_pos.store(next_ndx, std::memory_order_relaxed);
        }
    }

private:
    // number of entries. Access synchronized through put_pos.
    uint32_t entries;
    std::atomic<uint32_t> put_pos; // only changed under lock, but accessed outside lock
    std::atomic<uint32_t> old_pos; // only changed during write transactions and under lock

    const static int init_readers_size = 32;

    // IMPORTANT: The actual data comprising the linked list MUST BE PLACED LAST in
    // the RingBuffer structure, as the linked list area is extended at run time.
    // Similarly, the RingBuffer must be the final element of the SharedInfo structure.
    // IMPORTANT II:
    // To ensure proper alignment across all platforms, the SharedInfo structure
    // should NOT have a stricter alignment requirement than the ReadCount structure.
    ReadCount data[init_readers_size];
};

} // anonymous namespace


/// The structure of the contents of the per session `.lock` file. Note that
/// this file is transient in that it is recreated/reinitialized at the
/// beginning of every session. A session is any sequence of temporally
/// overlapping openings of a particular Realm file via SharedGroup objects. For
/// example, if there are two SharedGroup objects, A and B, and the file is
/// first opened via A, then opened via B, then closed via A, and finally closed
/// via B, then the session streaches from the opening via A to the closing via
/// B.
///
/// IMPORTANT: Remember to bump `g_shared_info_version` if anything is changed
/// in the memory layout of this class, or if the meaning of any of the stored
/// values change.
///
/// Members `init_complete`, `shared_info_version`, `size_of_mutex`, and
/// `size_of_condvar` may only be modified only while holding an exclusive lock
/// on the file, and may be read only while holding a shared (or exclusive) lock
/// on the file. All other members (except for the Ringbuffer) may be accessed
/// only while holding a lock on `controlmutex`.

/// SharedInfo must be 8-byte aligned. On 32-bit Apple platforms, mutexes store their
/// alignment as part of the mutex state. We're copying the SharedInfo (including
/// embedded but alway unlocked mutexes) and it must retain the same alignment
/// throughout.
struct alignas(8) SharedGroup::SharedInfo {
    // Indicates that initialization of the lock file was completed sucessfully.
    uint8_t init_complete = 0; // Offset 0

    /// The size in bytes of a mutex member of SharedInfo. This allows all
    /// session participants to be in agreement. Obviously, a size match is not
    /// enough to guarantee identical layout internally in the mutex object, but
    /// it is hoped that it will catch some (if not most) of the cases where
    /// there is a layout discrepancy internally in the mutex object.
    uint8_t size_of_mutex; // Offset 1

    /// Like size_of_mutex, but for condition variable members of SharedInfo.
    uint8_t size_of_condvar; // Offset 2

    /// Set during the critical phase of a commit, when the logs, the ringbuffer
    /// and the database may be out of sync with respect to each other. If a
    /// writer crashes during this phase, there is no safe way of continuing
    /// with further write transactions. When beginning a write transaction,
    /// this must be checked and an exception thrown if set.
    /// FIXME: This is a temporary approach until we get the commitlog data
    /// moved into the realm file. After that it should be feasible to either
    /// handle the error condition properly or preclude it by using a non-robust
    /// mutex for the remaining and much smaller critical section.
    uint8_t commit_in_critical_phase = 0; // Offset 3

    /// The target Realm file format version for the current session. This
    /// allows all session participants to be in agreement. It can only differ
    /// from what is returned by Group::get_file_format_version() temporarily,
    /// and only during the Realm file opening process. If it differes, it means
    /// that the file format needs to be upgraded from its current format
    /// (Group::get_file_format_version()), the the format specified by this
    /// member of SharedInfo.
    uint8_t file_format_version; // Offset 4

    /// Stores a value of Replication::HistoryType. Must match across all
    /// session participants.
    int8_t history_type; // Offset 5

    /// The SharedInfo layout version. This allows all session participants to
    /// be in agreement. Must be bumped if the layout of the SharedInfo
    /// structure is changed. Note, however, that only the part that lies beyond
    /// SharedInfoUnchangingLayout can have its layout changed.
    uint16_t shared_info_version = g_shared_info_version; // Offset 6

    uint16_t durability;           // Offset 8
    uint16_t free_write_slots = 0; // Offset 10

    /// Number of participating shared groups
    uint32_t num_participants = 0; // Offset 12

    /// Latest version number. Guarded by the controlmutex (for lock-free
    /// access, use get_version_of_latest_snapshot() instead)
    uint64_t latest_version_number; // Offset 16

    /// Pid of process initiating the session, but only if that process runs
    /// with encryption enabled, zero otherwise. Other processes cannot join a
    /// session wich uses encryption, because interprocess sharing is not
    /// supported by our current encryption mechanisms.
    uint64_t session_initiator_pid = 0; // Offset 24

    uint64_t number_of_versions; // Offset 32

    /// True (1) if there is a sync client present. It is an error to start a
    /// sync client if another one is present. If the sync client crashes and
    /// leaves the flag set, the session will need to be restarted (lock file
    /// reinitialized) before a new sync client can be started.
    uint8_t sync_client_present = 0; // Offset 40

    /// Set when a participant decides to start the daemon, cleared by the
    /// daemon when it decides to exit. Participants check during open() and
    /// start the daemon if running in async mode.
    uint8_t daemon_started = 0; // Offset 41

    /// Set by the daemon when it is ready to handle commits. Participants must
    /// wait during open() on 'daemon_becomes_ready' for this to become true.
    /// Cleared by the daemon when it decides to exit.
    uint8_t daemon_ready = 0; // Offset 42

    uint8_t filler_1;  // Offset 43
    uint32_t filler_2; // Offset 44

    InterprocessMutex::SharedPart shared_writemutex; // Offset 48
#ifdef REALM_ASYNC_DAEMON
    InterprocessMutex::SharedPart shared_balancemutex;
#endif
    InterprocessMutex::SharedPart shared_controlmutex;
#ifndef _WIN32
    // FIXME: windows pthread support for condvar not ready
    InterprocessCondVar::SharedPart room_to_write;
    InterprocessCondVar::SharedPart work_to_do;
    InterprocessCondVar::SharedPart daemon_becomes_ready;
    InterprocessCondVar::SharedPart new_commit_available;
#endif

    // IMPORTANT: The ringbuffer MUST be the last field in SharedInfo - see above.
    Ringbuffer readers;

    SharedInfo(Durability, Replication::HistoryType);
    ~SharedInfo() noexcept
    {
    }

    void init_versioning(ref_type top_ref, size_t file_size, uint64_t initial_version)
    {
        // Create our first versioning entry:
        Ringbuffer::ReadCount& r = readers.reinit_last();
        r.filesize = file_size;
        r.version = initial_version;
        r.current_top = top_ref;
    }

    uint_fast64_t get_current_version_unchecked() const
    {
        return readers.get_last().version;
    }
};


SharedGroup::SharedInfo::SharedInfo(Durability dura, Replication::HistoryType hist_type)
    : size_of_mutex(sizeof(shared_writemutex))
#ifndef _WIN32
    , size_of_condvar(sizeof(room_to_write))
#endif
    , shared_writemutex() // Throws
#ifdef REALM_ASYNC_DAEMON
    , shared_balancemutex() // Throws
#endif
    , shared_controlmutex() // Throws
{
    durability = static_cast<uint16_t>(dura); // durability level is fixed from creation
    REALM_ASSERT(!util::int_cast_has_overflow<decltype(history_type)>(hist_type + 0));
    history_type = hist_type;
#ifndef _WIN32
    InterprocessCondVar::init_shared_part(new_commit_available); // Throws
#ifdef REALM_ASYNC_DAEMON
    InterprocessCondVar::init_shared_part(room_to_write);        // Throws
    InterprocessCondVar::init_shared_part(work_to_do);           // Throws
    InterprocessCondVar::init_shared_part(daemon_becomes_ready); // Throws
#endif
#endif

    // IMPORTANT: The offsets, types (, and meanings) of these members must
    // never change, not even when the SharedInfo layout version is bumped. The
    // eternal constancy of this part of the layout is what ensures that a
    // joining session participant can reliably verify that the actual format is
    // as expected.
    static_assert(offsetof(SharedInfo, init_complete) == 0 && std::is_same<decltype(init_complete), uint8_t>::value &&
                      offsetof(SharedInfo, shared_info_version) == 6 &&
                      std::is_same<decltype(shared_info_version), uint16_t>::value,
                  "Forbidden change in SharedInfo layout");


    // Try to catch some of the memory layout changes that requires bumping of
    // the SharedInfo file format version (shared_info_version).
    static_assert(
        offsetof(SharedInfo, size_of_mutex) == 1 && std::is_same<decltype(size_of_mutex), uint8_t>::value &&
            offsetof(SharedInfo, size_of_condvar) == 2 && std::is_same<decltype(size_of_condvar), uint8_t>::value &&
            offsetof(SharedInfo, commit_in_critical_phase) == 3 &&
            std::is_same<decltype(commit_in_critical_phase), uint8_t>::value &&
            offsetof(SharedInfo, file_format_version) == 4 &&
            std::is_same<decltype(file_format_version), uint8_t>::value && offsetof(SharedInfo, history_type) == 5 &&
            std::is_same<decltype(history_type), int8_t>::value && offsetof(SharedInfo, durability) == 8 &&
            std::is_same<decltype(durability), uint16_t>::value && offsetof(SharedInfo, free_write_slots) == 10 &&
            std::is_same<decltype(free_write_slots), uint16_t>::value &&
            offsetof(SharedInfo, num_participants) == 12 &&
            std::is_same<decltype(num_participants), uint32_t>::value &&
            offsetof(SharedInfo, latest_version_number) == 16 &&
            std::is_same<decltype(latest_version_number), uint64_t>::value &&
            offsetof(SharedInfo, session_initiator_pid) == 24 &&
            std::is_same<decltype(session_initiator_pid), uint64_t>::value &&
            offsetof(SharedInfo, number_of_versions) == 32 &&
            std::is_same<decltype(number_of_versions), uint64_t>::value &&
            offsetof(SharedInfo, sync_client_present) == 40 &&
            std::is_same<decltype(sync_client_present), uint8_t>::value &&
            offsetof(SharedInfo, daemon_started) == 41 && std::is_same<decltype(daemon_started), uint8_t>::value &&
            offsetof(SharedInfo, daemon_ready) == 42 && std::is_same<decltype(daemon_ready), uint8_t>::value &&
            offsetof(SharedInfo, filler_1) == 43 && std::is_same<decltype(filler_1), uint8_t>::value &&
            offsetof(SharedInfo, filler_2) == 44 && std::is_same<decltype(filler_2), uint32_t>::value &&
            offsetof(SharedInfo, shared_writemutex) == 48 &&
            std::is_same<decltype(shared_writemutex), InterprocessMutex::SharedPart>::value,
        "Caught layout change requiring SharedInfo file format bumping");
}


namespace {


#ifdef REALM_ASYNC_DAEMON

void spawn_daemon(const std::string& file)
{
    // determine maximum number of open descriptors
    errno = 0;
    int m = int(sysconf(_SC_OPEN_MAX));
    if (m < 0) {
        if (errno) {
            int err = errno; // Eliminate any risk of clobbering
            throw std::runtime_error(get_errno_msg("'sysconf(_SC_OPEN_MAX)' failed: ", err));
        }
        throw std::runtime_error("'sysconf(_SC_OPEN_MAX)' failed with no reason");
    }

    int pid = fork();
    if (0 == pid) { // child process:

        // close all descriptors:
        int i;
        for (i = m - 1; i >= 0; --i)
            close(i);
        i = ::open("/dev/null", O_RDWR);
#ifdef REALM_ENABLE_LOGFILE
        // FIXME: Do we want to always open the log file? Should it be configurable?
        i = ::open((file + ".log").c_str(), O_RDWR | O_CREAT | O_APPEND | O_SYNC, S_IRWXU);
#else
        i = dup(i);
#endif
        i = dup(i);
        static_cast<void>(i);
#ifdef REALM_ENABLE_LOGFILE
        std::cerr << "Detaching" << std::endl;
#endif
        // detach from current session:
        setsid();

        // start commit daemon executable
        // Note that getenv (which is not thread safe) is called in a
        // single threaded context. This is ensured by the fork above.
        const char* async_daemon = getenv("REALM_ASYNC_DAEMON");
        if (!async_daemon) {
#ifndef REALM_DEBUG
            async_daemon = REALM_INSTALL_LIBEXECDIR "/realmd";
#else
            async_daemon = REALM_INSTALL_LIBEXECDIR "/realmd-dbg";
#endif
        }
        execl(async_daemon, async_daemon, file.c_str(), static_cast<char*>(0));

// if we continue here, exec has failed so return error
// if exec succeeds, we don't come back here.
#if REALM_ANDROID
        _exit(1);
#else
        _Exit(1);
#endif
        // child process ends here
    }
    else if (pid > 0) { // parent process, fork succeeded:

        // use childs exit code to catch and report any errors:
        int status;
        int pid_changed;
        do {
            pid_changed = waitpid(pid, &status, 0);
        } while (pid_changed == -1 && errno == EINTR);
        if (pid_changed != pid) {
            std::cerr << "Waitpid returned pid = " << pid_changed << " and status = " << std::hex << status
                      << std::endl;
            throw std::runtime_error("call to waitpid failed");
        }
        if (!WIFEXITED(status))
            throw std::runtime_error("failed starting async commit (exit)");
        if (WEXITSTATUS(status) == 1) {
            // FIXME: Or `ld` could not find a required shared library
            throw std::runtime_error("async commit daemon not found");
        }
        if (WEXITSTATUS(status) == 2)
            throw std::runtime_error("async commit daemon failed");
        if (WEXITSTATUS(status) == 3)
            throw std::runtime_error("wrong db given to async daemon");
    }
    else { // Parent process, fork failed!

        throw std::runtime_error("Failed to spawn async commit");
    }
}
#endif


} // anonymous namespace

const std::string SharedGroupOptions::sys_tmp_dir = getenv("TMPDIR") ? getenv("TMPDIR") : "";

// NOTES ON CREATION AND DESTRUCTION OF SHARED MUTEXES:
//
// According to the 'process-sharing example' in the POSIX man page
// for pthread_mutexattr_init() other processes may continue to use a
// process-shared mutex after exit of the process that initialized
// it. Also, the example does not contain any call to
// pthread_mutex_destroy(), so apparently a process-shared mutex need
// not be destroyed at all, nor can it be that a process-shared mutex
// is associated with any resources that are local to the initializing
// process, because that would imply a leak.
//
// While it is not explicitely guaranteed in the man page, we shall
// assume that is is valid to initialize a process-shared mutex twice
// without an intervending call to pthread_mutex_destroy(). We need to
// be able to reinitialize a process-shared mutex if the first
// initializing process crashes and leaves the shared memory in an
// undefined state.

void SharedGroup::do_open(const std::string& path, bool no_create_file, bool is_backend,
                          const SharedGroupOptions options)
{
    // Exception safety: Since do_open() is called from constructors, if it
    // throws, it must leave the file closed.

    // FIXME: Assess the exception safety of this function.

    REALM_ASSERT(!is_attached());

#ifndef REALM_ASYNC_DAEMON
    if (options.durability == Durability::Async)
        throw std::runtime_error("Async mode not yet supported on Windows, iOS and watchOS");
#endif

    m_db_path = path;
    m_coordination_dir = path + ".management";
    m_lockfile_path = path + ".lock";
    try_make_dir(m_coordination_dir);
    m_key = options.encryption_key;
    m_lockfile_prefix = m_coordination_dir + "/access_control";
    SlabAlloc& alloc = m_group.m_alloc;

    Replication::HistoryType history_type = Replication::hist_None;
    if (Replication* repl = m_group.get_replication()) {
        history_type = repl->get_history_type();
    }

    int target_file_format_version;

    for (;;) {
        m_file.open(m_lockfile_path, File::access_ReadWrite, File::create_Auto, 0); // Throws
        File::CloseGuard fcg(m_file);

        if (m_file.try_lock_exclusive()) { // Throws
            File::UnlockGuard ulg(m_file);

            // We're alone in the world, and it is Ok to initialize the
            // file. Start by truncating the file, to maximize the chance of a
            // an incorrectly initialized file gets rejected by other session
            // participants that get the shared file lock after the initiator
            // has dies half way through the initialization. Note, however, that
            // this can still happen if the initializing process is dies before
            // the truncation, but after obtaining the exclusive file lock.
            m_file.resize(0);

            // Write an initialized SharedInfo structure to the file, but with
            // init_complete = 0. Need to fill with zeros before constructing
            // due to the bit field members. Otherwise we would write
            // uninitialized bits to the file.
            alignas(SharedInfo) char buffer[sizeof(SharedInfo)] = {0};
            new (buffer) SharedInfo(options.durability, history_type); // Throws
            m_file.write(buffer, sizeof buffer);                       // Throws

            // Mark the file as completely initialized via a memory
            // mapping. Since this is done as a separate final step (involving
            // separate system calls) there is no chance of the individual
            // modifications to get reordered, even in case of a crash at a
            // random position during the initialization (except if it happens
            // before the truncation). This could also have been done by a
            // util::File::write(), but it is more convenient to manipulate the
            // structure via its type.
            m_file_map.map(m_file, File::access_ReadWrite, sizeof(SharedInfo), File::map_NoSync); // Throws
            File::UnmapGuard fug(m_file_map);
            SharedInfo* info_2 = m_file_map.get_addr();
            info_2->init_complete = 1;
        }

        // We hold the shared lock from here until we close the file!
        m_file.lock_shared(); // Throws

        // If the file is not completely initialized at this point in time, the
        // preceeding initialization attempt must have failed. We know that an
        // initialization process was in progress, because this thread (or
        // process) failed to get an exclusive lock on the file. Because this
        // thread (or process) currently has a shared lock on the file, we also
        // know that the initialization process can no longer be in progress, so
        // the initialization must either have completed or failed at this time.

        // The file is taken to be completely initialized if it is large enough
        // to contain the `init_complete` field, and `init_complete` is true. If
        // the file was not completely initialized, this thread must give up its
        // shared lock, and retry to become the initializer. Eventually, one of
        // two things must happen; either this thread, or another thread
        // succeeds in completing the initialization, or this thread becomes the
        // initializer, and fails the initialization. In either case, the retry
        // loop will eventually terminate.

        // An empty file is (and was) never a successfully initialized file.
        size_t info_size = sizeof(SharedInfo);
        {
            auto file_size = m_file.get_size();
            if (util::int_less_than(file_size, info_size)) {
                if (file_size == 0)
                    continue; // Retry
                info_size = size_t(file_size);
            }
        }

        // Map the initial section of the SharedInfo file that corresponds to
        // the SharedInfo struct, or less if the file is smaller. We know that
        // we have at least one byte, and that is enough to read the
        // `init_complete` flag.
        m_file_map.map(m_file, File::access_ReadWrite, info_size, File::map_NoSync);
        File::UnmapGuard fug_1(m_file_map);
        SharedInfo* info = m_file_map.get_addr();
        static_assert(offsetof(SharedInfo, init_complete) + sizeof SharedInfo::init_complete <= 1,
                      "Unexpected position or size of SharedInfo::init_complete");
        if (info->init_complete == 0)
            continue;
        REALM_ASSERT(info->init_complete == 1);

        // At this time, we know that the file was completely initialized, but
        // we still need to verify that is was initialized with the memory
        // layout expected by this session participant. We could find that it is
        // initializaed with a different memory layout if other concurrent
        // session participants use different versions of the core library.
        if (info_size < sizeof(SharedInfo)) {
            std::stringstream ss;
            ss << "Info size doesn't match, " << info_size << " " << sizeof(SharedInfo) << ".";
            throw IncompatibleLockFile(ss.str());
        }
        if (info->shared_info_version != g_shared_info_version) {
            std::stringstream ss;
            ss << "Shared info version doesn't match, " << info->shared_info_version << " " << g_shared_info_version
               << ".";
            throw IncompatibleLockFile(ss.str());
        }
        // Validate compatible sizes of mutex and condvar types. Sizes of all
        // other fields are architecture independent, so if condvar and mutex
        // sizes match, the entire struct matches. The offsets of
        // `size_of_mutex` and `size_of_condvar` are known to be as expected due
        // to the preceeding check in `shared_info_version`.
        if (info->size_of_mutex != sizeof info->shared_controlmutex) {
            std::stringstream ss;
            ss << "Mutex size doesn't match: " << info->size_of_mutex << " " << sizeof(info->shared_controlmutex)
               << ".";
            throw IncompatibleLockFile(ss.str());
        }
#ifndef _WIN32
        if (info->size_of_condvar != sizeof info->room_to_write) {
            std::stringstream ss;
            ss << "Condtion var size doesn't match: " << info->size_of_condvar << " " << sizeof(info->room_to_write)
               << ".";
            throw IncompatibleLockFile(ss.str());
        }
#endif
        // Even though fields match wrt alignment and size, there may still be
        // incompatibilities between implementations, so lets ask one of the
        // mutexes if it thinks it'll work.
        //
        // FIXME: Calling util::RobustMutex::is_valid() on a mutex object of
        // unknown, and possibly invalid state has undefined behaviour, and is
        // therfore dangerous. It should not be done.
        //
        // FIXME: This check tries to lock the mutex, and only unlocks it if the
        // return value is zero. If pthread_mutex_trylock() fails with
        // EOWNERDEAD, this leads to deadlock during the following propper
        // attempt to lock. This cannot be fixed by also unlocking on failure
        // with EOWNERDEAD, because that would mark the mutex as consistent
        // again and prevent us from being notified below.

        m_writemutex.set_shared_part(info->shared_writemutex, m_lockfile_prefix, "write");
#ifdef REALM_ASYNC_DAEMON
        m_balancemutex.set_shared_part(info->shared_balancemutex, m_lockfile_prefix, "balance");
#endif
        m_controlmutex.set_shared_part(info->shared_controlmutex, m_lockfile_prefix, "control");

        // even though fields match wrt alignment and size, there may still be incompatibilities
        // between implementations, so lets ask one of the mutexes if it thinks it'll work.
        if (!m_controlmutex.is_valid()) {
            throw IncompatibleLockFile("Control mutex is invalid.");
        }

        // OK! lock file appears valid. We can now continue operations under the protection
        // of the controlmutex. The controlmutex protects the following activities:
        // - attachment of the database file
        // - start of the async daemon
        // - stop of the async daemon
        // - SharedGroup beginning/ending a session
        // - Waiting for and signalling database changes
        {
            std::lock_guard<InterprocessMutex> lock(m_controlmutex); // Throws
            // we need a thread-local copy of the number of ringbuffer entries in order
            // to later detect concurrent expansion of the ringbuffer.
            m_local_max_entry = info->readers.get_num_entries();

            // We need to map the info file once more for the readers part
            // since that part can be resized and as such remapped which
            // could move our mutexes (which we don't want to risk moving while
            // they are locked)
            size_t reader_info_size = sizeof(SharedInfo) + info->readers.compute_required_space(m_local_max_entry);
            m_reader_map.map(m_file, File::access_ReadWrite, reader_info_size, File::map_NoSync);
            File::UnmapGuard fug_2(m_reader_map);

            // proceed to initialize versioning and other metadata information related to
            // the database. Also create the database if we're beginning a new session
            bool begin_new_session = (info->num_participants == 0);
            SlabAlloc::Config cfg;
            cfg.session_initiator = begin_new_session;
            cfg.is_shared = true;
            cfg.read_only = false;
            cfg.skip_validate = !begin_new_session;

            // only the session initiator is allowed to create the database, all other
            // must assume that it already exists.
            cfg.no_create = begin_new_session ? no_create_file : true;

            // if we're opening a MemOnly file that isn't already opened by
            // someone else then it's a file which should have been deleted on
            // close previously, but wasn't (perhaps due to the process crashing)
            cfg.clear_file = options.durability == Durability::MemOnly && begin_new_session;

            cfg.encryption_key = options.encryption_key;
            ref_type top_ref;
            try {
                top_ref = alloc.attach_file(path, cfg); // Throws
            }
            catch (SlabAlloc::Retry&) {
                continue;
            }
            // If we fail in any way, we must detach the allocator. Failure to do so
            // will retain memory mappings in the mmap cache shared between allocators.
            // This would allow other SharedGroups to reuse the mappings even in
            // situations, where the database has been re-initialised (e.g. through
            // compact()). This could render the mappings (partially) undefined.
            SlabAlloc::DetachGuard alloc_detach_guard(alloc);

            // Determine target file format version for session (upgrade
            // required if greater than file format version of attached file).
            using gf = _impl::GroupFriend;
            int current_file_format_version = gf::get_file_format_version(m_group);
            target_file_format_version =
                gf::get_target_file_format_version_for_session(current_file_format_version, history_type);

            if (begin_new_session) {
                // Determine version (snapshot number) and check history type
                // compatibility
                version_type version = 0;
                int stored_history_type = 0;
                gf::get_version_and_history_type(alloc, top_ref, version, stored_history_type);
                bool good_history_type = false;
                switch (history_type) {
                    case Replication::hist_None:
                    case Replication::hist_OutOfRealm:
                        good_history_type = (stored_history_type == Replication::hist_None);
                        break;
                    case Replication::hist_InRealm:
                        good_history_type = (stored_history_type == Replication::hist_InRealm ||
                                             stored_history_type == Replication::hist_None);
                        break;
                    case Replication::hist_Sync:
                        good_history_type = ((stored_history_type == Replication::hist_Sync) ||
                                             (stored_history_type == Replication::hist_None && top_ref == 0));
                }
                if (!good_history_type)
                    throw InvalidDatabase("Bad or incompatible history type", path);

                if (Replication* repl = gf::get_replication(m_group))
                    repl->initiate_session(version); // Throws

#ifndef _WIN32
                if (options.encryption_key) {
                    static_assert(sizeof(pid_t) <= sizeof(uint64_t), "process identifiers too large");
                    info->session_initiator_pid = uint_fast64_t(getpid());
                }
#endif

                info->file_format_version = uint_fast8_t(target_file_format_version);

                // Initially there is a single version in the file
                info->number_of_versions = 1;

                info->latest_version_number = version;

                SharedInfo* r_info = m_reader_map.get_addr();
                size_t file_size = alloc.get_baseline();
                r_info->init_versioning(top_ref, file_size, version);
            }
            else { // Not the session initiator
                // Durability setting must be consistent across a session. An
                // inconsistency is a logic error, as the user is required to
                // make sure that all possible concurrent session participants
                // use the same durability setting for the same Realm file.
                if (Durability(info->durability) != options.durability)
                    throw LogicError(LogicError::mixed_durability);

                // History type must be consistent across a session. An
                // inconsistency is a logic error, as the user is required to
                // make sure that all possible concurrent session participants
                // use the same history type for the same Realm file.
                if (info->history_type != history_type)
                    throw LogicError(LogicError::mixed_history_type);

#ifndef _WIN32
                if (options.encryption_key && info->session_initiator_pid != uint64_t(getpid())) {
                    std::stringstream ss;
                    ss << path << ": Encrypted interprocess sharing is currently unsupported."
                       << "SharedGroup has been opened by pid: " << info->session_initiator_pid << ". Current pid is "
                       << getpid() << ".";
                    throw std::runtime_error(ss.str());
                }
#endif

                // We need per session agreement among all participants on the
                // target Realm file format. From a technical perspective, the
                // best way to ensure that, would be to require a bumping of the
                // SharedInfo file format version on any change that could lead
                // to a different result from
                // get_target_file_format_for_session() given the same current
                // Realm file format version and the same history type, as that
                // would prevent the outcome of the Realm opening process from
                // depending on race conditions. However, for practical reasons,
                // we shall instead simply check that there is agreement, and
                // throw the same kind of exception, as would have been thrown
                // with a bumped SharedInfo file format version, if there isn't.
                if (info->file_format_version != target_file_format_version) {
                    std::stringstream ss;
                    ss << "File format version deosn't match: " << info->file_format_version << " "
                       << target_file_format_version << ".";
                    throw IncompatibleLockFile(ss.str());
                }
            }

#ifndef _WIN32
            m_new_commit_available.set_shared_part(info->new_commit_available, m_lockfile_prefix, "new_commit",
                                                   options.temp_dir);
#ifdef REALM_ASYNC_DAEMON
            m_daemon_becomes_ready.set_shared_part(info->daemon_becomes_ready, m_lockfile_prefix, "daemon_ready",
                                                   options.temp_dir);
            m_work_to_do.set_shared_part(info->work_to_do, m_lockfile_prefix, "work_ready", options.temp_dir);
            m_room_to_write.set_shared_part(info->room_to_write, m_lockfile_prefix, "allow_write", options.temp_dir);
            // In async mode, we need to make sure the daemon is running and ready:
            if (options.durability == Durability::Async && !is_backend) {
                while (info->daemon_ready == 0) {
                    if (info->daemon_started == 0) {
                        spawn_daemon(path);
                        info->daemon_started = 1;
                    }
                    // FIXME: It might be more robust to sleep a little, then restart the loop
                    // std::cerr << "Waiting for daemon" << std::endl;
                    m_daemon_becomes_ready.wait(m_controlmutex, 0);
                    // std::cerr << " - notified" << std::endl;
                }
            }
// std::cerr << "daemon should be ready" << std::endl;
#endif // REALM_ASYNC_DAEMON
#endif // !defined _WIN32

            // Set initial version so we can track if other instances
            // change the db
            m_read_lock.m_version = get_version_of_latest_snapshot();

            // make our presence noted:
            ++info->num_participants;

            // Initially wait_for_change is enabled
            m_wait_for_change_enabled = true;

            // Keep the mappings and file open:
            alloc_detach_guard.release();
            fug_2.release(); // Do not unmap
            fug_1.release(); // Do not unmap
            fcg.release();   // Do not close
        }
        break;
    }

    m_transact_stage = transact_Ready;
// std::cerr << "open completed" << std::endl;

#ifdef REALM_ASYNC_DAEMON
    if (options.durability == Durability::Async) {
        if (is_backend) {
            do_async_commits();
        }
    }
#else
    static_cast<void>(is_backend);
#endif

    try {
        using gf = _impl::GroupFriend;
        int current_file_format_version = gf::get_file_format_version(m_group);
        if (current_file_format_version == 0) {
            // If the current file format is still undecided, no upgrade is
            // necessary, but we still need to make the chosen file format
            // visible to the rest of the core library by updating that value
            // that will be subsequently returned by
            // Group::get_file_format_version(). For this to work, all session
            // participants must adopt the chosen target Realm file format when
            // the stored file format version is zero regardless of the version
            // of the core library used.
            gf::set_file_format_version(m_group, target_file_format_version);
        }
        else {
            upgrade_file_format(options.allow_file_format_upgrade, target_file_format_version); // Throws
        }
    }
    catch (...) {
        close();
        throw;
    }
}

// WARNING / FIXME: compact() should NOT be exposed publicly on Windows because it's not crash safe! It may
// corrupt your database if something fails
bool SharedGroup::compact()
{
    // FIXME: ExcetionSafety: This function must be rewritten with exception
    // safety in mind.

    // Verify that the database file is attached
    if (is_attached() == false) {
        throw std::runtime_error(m_db_path + ": compact must be done on an open/attached SharedGroup");
    }
    // Verify that preconditions for compacting is met:
    if (m_transact_stage != transact_Ready) {
        throw std::runtime_error(m_db_path + ": compact is not supported whithin a transaction");
    }
    Durability dura;
    std::string tmp_path = m_db_path + ".tmp_compaction_space";
    {
        SharedInfo* info = m_file_map.get_addr();
        std::lock_guard<InterprocessMutex> lock(m_controlmutex); // Throws
        if (info->num_participants > 1)
            return false;

        // group::write() will throw if the file already exists.
        // To prevent this, we have to remove the file (should it exist)
        // before calling group::write().
        File::try_remove(tmp_path);

        // Using begin_read here ensures that we have access to the latest entry
        // in the ringbuffer. We need to have access to that later to update top_ref and file_size.
        // This is also needed to attach the group (get the proper top pointer, etc)
        begin_read(); // Throws

        // Compact by writing a new file holding only live data, then renaming the new file
        // so it becomes the database file, replacing the old one in the process.
        File file;
        file.open(tmp_path, File::access_ReadWrite, File::create_Must, 0);
        m_group.write(file, m_key, info->latest_version_number);
        // Data needs to be flushed to the disk before renaming.
        bool disable_sync = get_disable_sync_to_disk();
        if (!disable_sync)
            file.sync(); // Throws
#ifndef _WIN32
        util::File::move(tmp_path, m_db_path);
#endif
        {
            SharedInfo* r_info = m_reader_map.get_addr();
            Ringbuffer::ReadCount& rc = const_cast<Ringbuffer::ReadCount&>(r_info->readers.get_last());
            REALM_ASSERT_3(rc.version, ==, info->latest_version_number);
            static_cast<void>(rc); // rc unused if ENABLE_ASSERTION is unset
        }
        end_read();
        dura = Durability(info->durability);
        // We need to release any shared mapping *before* releasing the control mutex.
        // When someone attaches to the new database file, they *must* *not* see and
        // reuse any existing memory mapping of the stale file.
        m_group.m_alloc.detach();
    }
    close();
#ifdef _WIN32
    util::File::copy(tmp_path, m_db_path);
#endif

    SharedGroupOptions new_options;
    new_options.durability = dura;
    new_options.encryption_key = m_key;
    new_options.allow_file_format_upgrade = false;
    do_open(m_db_path, true, false, new_options);
    return true;
}

uint_fast64_t SharedGroup::get_number_of_versions()
{
    SharedInfo* info = m_file_map.get_addr();
    std::lock_guard<InterprocessMutex> lock(m_controlmutex); // Throws
    return info->number_of_versions;
}

SharedGroup::~SharedGroup() noexcept
{
    close();
}

void SharedGroup::close() noexcept
{
    if (!is_attached())
        return;

    switch (m_transact_stage) {
        case transact_Ready:
            break;
        case transact_Reading:
            end_read();
            break;
        case transact_Writing:
            rollback();
            break;
    }
    m_group.detach();
    m_transact_stage = transact_Ready;
    SharedInfo* info = m_file_map.get_addr();
    {
        std::lock_guard<InterprocessMutex> lock(m_controlmutex);

        if (m_group.m_alloc.is_attached())
            m_group.m_alloc.detach();

        --info->num_participants;
        bool end_of_session = info->num_participants == 0;
        // std::cerr << "closing" << std::endl;
        if (end_of_session) {

            // If the db file is just backing for a transient data structure,
            // we can delete it when done.
            if (Durability(info->durability) == Durability::MemOnly) {
                try {
                    util::File::remove(m_db_path.c_str());
                }
                catch (...) {
                } // ignored on purpose.
            }
            using gf = _impl::GroupFriend;
            if (Replication* repl = gf::get_replication(m_group))
                repl->terminate_session();
        }
    }
#ifndef _WIN32
#ifdef REALM_ASYNC_DAEMON
    m_room_to_write.close();
    m_work_to_do.close();
    m_daemon_becomes_ready.close();
#endif
    m_new_commit_available.close();
#endif
    // On Windows it is important that we unmap before unlocking, else a SetEndOfFile() call from another thread may
    // interleave which is not permitted on Windows. It is permitted on *nix.
    m_file_map.unmap();
    m_reader_map.unmap();
    m_file.unlock();
    // info->~SharedInfo(); // DO NOT Call destructor
    m_file.close();
}

bool SharedGroup::has_changed()
{
    bool changed = m_read_lock.m_version != get_version_of_latest_snapshot();
    return changed;
}

#ifndef _WIN32
bool SharedGroup::wait_for_change()
{
    SharedInfo* info = m_file_map.get_addr();
    std::lock_guard<InterprocessMutex> lock(m_controlmutex);
    while (m_read_lock.m_version == info->latest_version_number && m_wait_for_change_enabled) {
        m_new_commit_available.wait(m_controlmutex, 0);
    }
    return m_read_lock.m_version != info->latest_version_number;
}


void SharedGroup::wait_for_change_release()
{
    std::lock_guard<InterprocessMutex> lock(m_controlmutex);
    m_wait_for_change_enabled = false;
    m_new_commit_available.notify_all();
}


void SharedGroup::enable_wait_for_change()
{
    std::lock_guard<InterprocessMutex> lock(m_controlmutex);
    m_wait_for_change_enabled = true;
}

#ifdef REALM_ASYNC_DAEMON
void SharedGroup::do_async_commits()
{
    bool shutdown = false;
    SharedInfo* info = m_file_map.get_addr();

    // We always want to keep a read lock on the last version
    // that was commited to disk, to protect it against being
    // overwritten by commits being made to memory by others.
    {
        VersionID version_id = VersionID();      // Latest available snapshot
        grab_read_lock(m_read_lock, version_id); // Throws
    }
    // we must treat version and version_index the same way:
    {
        std::lock_guard<InterprocessMutex> lock(m_controlmutex);
        info->free_write_slots = max_write_slots;
        info->daemon_ready = 1;
        m_daemon_becomes_ready.notify_all();
    }
    using gf = _impl::GroupFriend;
    gf::detach(m_group);

    while (true) {
        if (m_file.is_removed()) { // operator removed the lock file. take a hint!

            shutdown = true;
#ifdef REALM_ENABLE_LOGFILE
            std::cerr << "Lock file removed, initiating shutdown" << std::endl;
#endif
        }

        bool is_same;
        ReadLockInfo next_read_lock = m_read_lock;
        {
            // detect if we're the last "client", and if so, shutdown (must be under lock):
            std::lock_guard<InterprocessMutex> lock2(m_writemutex);
            std::lock_guard<InterprocessMutex> lock(m_controlmutex);
            version_type old_version = next_read_lock.m_version;
            VersionID version_id = VersionID(); // Latest available snapshot
            grab_read_lock(next_read_lock, version_id);
            is_same = (next_read_lock.m_version == old_version);
            if (is_same && (shutdown || info->num_participants == 1)) {
#ifdef REALM_ENABLE_LOGFILE
                std::cerr << "Daemon exiting nicely" << std::endl << std::endl;
#endif
                release_read_lock(next_read_lock);
                release_read_lock(m_read_lock);
                info->daemon_started = 0;
                info->daemon_ready = 0;
                return;
            }
        }

        if (!is_same) {

#ifdef REALM_ENABLE_LOGFILE
            std::cerr << "Syncing from version " << m_read_lock.m_version << " to " << next_read_lock.m_version
                      << std::endl;
#endif
            GroupWriter writer(m_group);
            writer.commit(next_read_lock.m_top_ref);

#ifdef REALM_ENABLE_LOGFILE
            std::cerr << "..and Done" << std::endl;
#endif
        }

        // Now we can release the version that was previously commited
        // to disk and just keep the lock on the latest version.
        release_read_lock(m_read_lock);
        m_read_lock = next_read_lock;

        m_balancemutex.lock();

        // We have caught up with the writers, let them know that there are
        // now free write slots, wakeup any that has been suspended.
        uint16_t free_write_slots = info->free_write_slots;
        info->free_write_slots = max_write_slots;
        if (free_write_slots <= 0) {
            m_room_to_write.notify_all();
        }

        // If we have plenty of write slots available, relax and wait a bit before syncing
        if (free_write_slots > relaxed_sync_threshold) {
            timespec ts;
            timeval tv;
            // clock_gettime(CLOCK_REALTIME, &ts); <- would like to use this, but not there on mac
            gettimeofday(&tv, nullptr);
            ts.tv_sec = tv.tv_sec;
            ts.tv_nsec = tv.tv_usec * 1000;
            ts.tv_nsec += 10000000;         // 10 msec
            if (ts.tv_nsec >= 1000000000) { // overflow
                ts.tv_nsec -= 1000000000;
                ts.tv_sec += 1;
            }

            // no timeout support if the condvars are only emulated, so this will assert
            m_work_to_do.wait(m_balancemutex, &ts);
        }
        m_balancemutex.unlock();
    }
}
#endif // REALM_ASYNC_DAEMON
#endif // _WIN32


void SharedGroup::upgrade_file_format(bool allow_file_format_upgrade, int target_file_format_version)
{
    // In a multithreaded scenario multiple threads may set upgrade = true, but
    // that is ok, because the condition is later rechecked in a fully reliable
    // way inside a transaction.

    // First a non-threadsafe but fast check
    using gf = _impl::GroupFriend;
    int current_file_format_version = gf::get_file_format_version(m_group);
    REALM_ASSERT(current_file_format_version <= target_file_format_version);
    bool maybe_upgrade = (current_file_format_version < target_file_format_version);
    if (maybe_upgrade) {
#ifdef REALM_DEBUG
// This sleep() only exists in order to increase the quality of the
// TEST(Upgrade_Database_2_3_Writes_New_File_Format_new) unit test.
// The unit test creates multiple threads that all call
// upgrade_file_format() simultaneously. This sleep() then acts like
// a simple thread barrier that makes sure the threads meet here, to
// increase the likelyhood of detecting any potential race problems.
// See the unit test for details.
#ifdef _WIN32
        _sleep(200);
#else
        // sleep() takes seconds and usleep() is deprecated, so use nanosleep()
        timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200000000;
        nanosleep(&ts, 0);
#endif
#endif

        WriteTransaction wt(*this);
        int current_file_format_version_2 = gf::get_committed_file_format_version(m_group);
        // The file must either still be using its initial file_format or have
        // been upgraded already to the chosen target file format via a
        // concurrent SharedGroup object.
        REALM_ASSERT(current_file_format_version_2 == current_file_format_version ||
                     current_file_format_version_2 == target_file_format_version);
        bool need_upgrade = (current_file_format_version_2 < target_file_format_version);
        if (need_upgrade) {
            if (!allow_file_format_upgrade)
                throw FileFormatUpgradeRequired();
            gf::upgrade_file_format(m_group, target_file_format_version); // Throws
            // Note: The file format version stored in the Realm file will be
            // updated to the new file format version as part of the following
            // commit operation. This happens in GroupWriter::commit().
            if (m_upgrade_callback) {
                try {
                    m_upgrade_callback(current_file_format_version_2, target_file_format_version); // Throws
                }
                catch (...) {
                    rollback();
                    throw;
                }
            }
            commit(); // Throws
        }
        else {
            // If somebody else has already performed the upgrade, we still need
            // to inform the rest of the core library about the new file format
            // of the attached file.
            gf::set_file_format_version(m_group, target_file_format_version);
        }
    }
}


SharedGroup::VersionID SharedGroup::get_version_of_current_transaction()
{
    return VersionID(m_read_lock.m_version, m_read_lock.m_reader_idx);
}


void SharedGroup::release_read_lock(ReadLockInfo& read_lock) noexcept
{
    // The release may be tried on a version imported from a different thread,
    // hence generated on a different shared group, which may have memory mapped
    // a larger ringbuffer than we - so make sure we've mapped enough of the
    // ringbuffer to access the chosen ringbuffer entry.
    grow_reader_mapping(read_lock.m_reader_idx);
    SharedInfo* r_info = m_reader_map.get_addr();
    const Ringbuffer::ReadCount& r = r_info->readers.get(read_lock.m_reader_idx);
    atomic_double_dec(r.count); // <-- most of the exec time spent here
}


void SharedGroup::grab_read_lock(ReadLockInfo& read_lock, VersionID version_id)
{
    if (version_id.version == std::numeric_limits<version_type>::max()) {
        for (;;) {
            SharedInfo* r_info = m_reader_map.get_addr();
            read_lock.m_reader_idx = r_info->readers.last();
            if (grow_reader_mapping(read_lock.m_reader_idx)) { // Throws
                // remapping takes time, so retry with a fresh entry
                continue;
            }
            r_info = m_reader_map.get_addr();
            const Ringbuffer::ReadCount& r = r_info->readers.get(read_lock.m_reader_idx);
            // if the entry is stale and has been cleared by the cleanup process,
            // we need to start all over again. This is extremely unlikely, but possible.
            if (!atomic_double_inc_if_even(r.count)) // <-- most of the exec time spent here!
                continue;
            read_lock.m_version = r.version;
            read_lock.m_top_ref = to_size_t(r.current_top);
            read_lock.m_file_size = to_size_t(r.filesize);
            return;
        }
    }

    for (;;) {
        SharedInfo* r_info = m_reader_map.get_addr();
        read_lock.m_reader_idx = version_id.index;
        if (grow_reader_mapping(read_lock.m_reader_idx)) { // Throws
            // remapping takes time, so retry with a fresh entry
            continue;
        }
        r_info = m_reader_map.get_addr();
        const Ringbuffer::ReadCount& r = r_info->readers.get(read_lock.m_reader_idx);

        // if the entry is stale and has been cleared by the cleanup process,
        // the requested version is no longer available
        while (!atomic_double_inc_if_even(r.count)) { // <-- most of the exec time spent here!
            // we failed to lock the version. This could be because the version
            // is being cleaned up, but also because the cleanup is probing for access
            // to it. If it's being probed, the tail ptr of the ringbuffer will point
            // to it. If so we retry. If the tail ptr points somewhere else, the entry
            // has been cleaned up.
            if (&r_info->readers.get_oldest() != &r)
                throw BadVersion();
        }
        // we managed to lock an entry in the ringbuffer, but it may be so old that
        // the version doesn't match the specific request. In that case we must release and fail
        if (r.version != version_id.version) {
            atomic_double_dec(r.count); // <-- release
            throw BadVersion();
        }
        read_lock.m_version = r.version;
        read_lock.m_top_ref = to_size_t(r.current_top);
        read_lock.m_file_size = to_size_t(r.filesize);
        return;
    }
}


const Group& SharedGroup::begin_read(VersionID version_id)
{
    if (m_transact_stage != transact_Ready)
        throw LogicError(LogicError::wrong_transact_state);

    bool writable = false;
    do_begin_read(version_id, writable); // Throws

    m_transact_stage = transact_Reading;
    return m_group;
}


void SharedGroup::end_read() noexcept
{
    if (m_transact_stage == transact_Ready)
        return; // Idempotency

    if (m_transact_stage != transact_Reading)
        throw LogicError(LogicError::wrong_transact_state);

    do_end_read();

    m_transact_stage = transact_Ready;
}


Group& SharedGroup::begin_write()
{
    if (m_transact_stage != transact_Ready)
        throw LogicError(LogicError::wrong_transact_state);

    do_begin_write(); // Throws
    try {
        // We can be sure that do_begin_read() will bind to the latest snapshot,
        // since no other write transaction can be initated while we hold the
        // write mutex.
        VersionID version_id = VersionID(); // Latest available snapshot
        bool writable = true;
        do_begin_read(version_id, writable); // Throws

        if (Replication* repl = m_group.get_replication()) {
            version_type current_version = m_read_lock.m_version;
            bool history_updated = false;
            repl->initiate_transact(current_version, history_updated); // Throws
        }
    }
    catch (...) {
        do_end_write();
        throw;
    }

    m_transact_stage = transact_Writing;
    return m_group;
}


SharedGroup::version_type SharedGroup::commit()
{
    if (m_transact_stage != transact_Writing)
        throw LogicError(LogicError::wrong_transact_state);

    REALM_ASSERT(m_group.is_attached());

    version_type new_version = do_commit(); // Throws
    do_end_write();
    do_end_read();

    m_transact_stage = transact_Ready;
    return new_version;
}


void SharedGroup::rollback() noexcept
{
    if (m_transact_stage == transact_Ready)
        return; // Idempotency

    if (m_transact_stage != transact_Writing)
        throw LogicError(LogicError::wrong_transact_state);

    do_end_write();
    do_end_read();

    if (Replication* repl = m_group.get_replication())
        repl->abort_transact();

    m_transact_stage = transact_Ready;
}

SharedGroup::VersionID SharedGroup::pin_version()
{
    REALM_ASSERT(m_transact_stage != transact_Ready);

    // Get current version
    VersionID version_id(m_read_lock.m_version, m_read_lock.m_reader_idx);

    ReadLockInfo read_lock;
    grab_read_lock(read_lock, version_id); // Throws

    return version_id;
}

void SharedGroup::unpin_version(VersionID token)
{
    ReadLockInfo read_lock;
    read_lock.m_reader_idx = token.index;

    release_read_lock(read_lock);
}


void SharedGroup::do_begin_read(VersionID version_id, bool writable)
{
    // FIXME: BadVersion must be thrown in every case where the specified
    // version is not tethered in accordance with the documentation of
    // begin_read().

    grab_read_lock(m_read_lock, version_id); // Throws

    ReadLockUnlockGuard g(*this, m_read_lock);

    using gf = _impl::GroupFriend;
    gf::attach_shared(m_group, m_read_lock.m_top_ref, m_read_lock.m_file_size, writable); // Throws

    g.release();
}


void SharedGroup::do_end_read() noexcept
{
    REALM_ASSERT(m_read_lock.m_version != std::numeric_limits<version_type>::max());
    release_read_lock(m_read_lock);
    using gf = _impl::GroupFriend;
    gf::detach(m_group);
}


void SharedGroup::do_begin_write()
{
    SharedInfo* info = m_file_map.get_addr();
    // Get write lock
    // Note that this will not get released until we call
    // commit() or rollback()
    m_writemutex.lock(); // Throws

    if (info->commit_in_critical_phase) {
        m_writemutex.unlock();
        throw std::runtime_error("Crash of other process detected, session restart required");
    }

#ifdef REALM_ASYNC_DAEMON
    if (info->durability == static_cast<uint16_t>(Durability::Async)) {

        m_balancemutex.lock(); // Throws

        // if we are running low on write slots, kick the sync daemon
        if (info->free_write_slots < relaxed_sync_threshold)
            m_work_to_do.notify();
        // if we are out of write slots, wait for the sync daemon to catch up
        while (info->free_write_slots <= 0) {
            m_room_to_write.wait(m_balancemutex, 0);
        }

        info->free_write_slots--;
        m_balancemutex.unlock();
    }
#endif // _WIN32
}


void SharedGroup::do_end_write() noexcept
{
    m_writemutex.unlock();
}


Replication::version_type SharedGroup::do_commit()
{
    REALM_ASSERT(m_transact_stage == transact_Writing);

    SharedInfo* r_info = m_reader_map.get_addr();

    version_type current_version = r_info->get_current_version_unchecked();
    version_type new_version = current_version + 1;
    if (Replication* repl = m_group.get_replication()) {
        // If Replication::prepare_commit() fails, then the entire transaction
        // fails. The application then has the option of terminating the
        // transaction with a call to SharedGroup::rollback(), which in turn
        // must call Replication::abort_transact().
        new_version = repl->prepare_commit(current_version); // Throws
        try {
            low_level_commit(new_version); // Throws
        }
        catch (...) {
            repl->abort_transact();
            throw;
        }
        repl->finalize_commit();
    }
    else {
        low_level_commit(new_version); // Throws
    }
    return new_version;
}


SharedGroup::version_type SharedGroup::commit_and_continue_as_read()
{
    if (m_transact_stage != transact_Writing)
        throw LogicError(LogicError::wrong_transact_state);

    version_type version = do_commit(); // Throws

    // advance read lock but dont update accessors:
    // As this is done under lock, along with the addition above of the newest commit,
    // we know for certain that the read lock we will grab WILL refer to our own newly
    // completed commit.
    release_read_lock(m_read_lock);

    VersionID version_id = VersionID();      // Latest available snapshot
    grab_read_lock(m_read_lock, version_id); // Throws

    do_end_write();

    // Free memory that was allocated during the write transaction.
    using gf = _impl::GroupFriend;
    gf::reset_free_space_tracking(m_group); // Throws

    // Remap file if it has grown, and update refs in underlying node structure
    gf::remap_and_update_refs(m_group, m_read_lock.m_top_ref, m_read_lock.m_file_size); // Throws

    m_transact_stage = transact_Reading;

    return version;
}


bool SharedGroup::grow_reader_mapping(uint_fast32_t index)
{
    using _impl::SimulatedFailure;
    SimulatedFailure::trigger(SimulatedFailure::shared_group__grow_reader_mapping); // Throws

    if (index >= m_local_max_entry) {
        // handle mapping expansion if required
        SharedInfo* r_info = m_reader_map.get_addr();
        m_local_max_entry = r_info->readers.get_num_entries();
        size_t info_size = sizeof(SharedInfo) + r_info->readers.compute_required_space(m_local_max_entry);
        // std::cout << "Growing reader mapping to " << infosize << std::endl;
        m_reader_map.remap(m_file, util::File::access_ReadWrite, info_size); // Throws
        return true;
    }
    return false;
}


SharedGroup::version_type SharedGroup::get_version_of_latest_snapshot()
{
    // As get_version_of_latest_snapshot() may be called outside of the write
    // mutex, another thread may be performing changes to the ringbuffer
    // concurrently. It may even cleanup and recycle the current entry from
    // under our feet, so we need to protect the entry by temporarily
    // incrementing the reader ref count until we've got a safe reading of the
    // version number.
    while (1) {
        uint_fast32_t index;
        SharedInfo* r_info;
        do {
            // make sure that the index we are about to dereference falls within
            // the portion of the ringbuffer that we have mapped - if not, extend
            // the mapping to fit.
            r_info = m_reader_map.get_addr();
            index = r_info->readers.last();
        } while (grow_reader_mapping(index)); // throws

        // now (double) increment the read count so that no-one cleans up the entry
        // while we read it.
        const Ringbuffer::ReadCount& r = r_info->readers.get(index);
        if (!atomic_double_inc_if_even(r.count)) {

            continue;
        }
        version_type version = r.version;
        // release the entry again:
        atomic_double_dec(r.count);
        return version;
    }
}


void SharedGroup::low_level_commit(uint_fast64_t new_version)
{
    SharedInfo* info = m_file_map.get_addr();

    // Version of oldest snapshot currently (or recently) bound in a transaction
    // of the current session.
    uint_fast64_t oldest_version;
    {
        SharedInfo* r_info = m_reader_map.get_addr();

        // the cleanup process may access the entire ring buffer, so make sure it is mapped.
        // this is not ensured as part of begin_read, which only makes sure that the current
        // last entry in the buffer is available.
        if (grow_reader_mapping(r_info->readers.get_num_entries())) { // throws
            r_info = m_reader_map.get_addr();
        }
        r_info->readers.cleanup();
        const Ringbuffer::ReadCount& rc = r_info->readers.get_oldest();
        oldest_version = rc.version;

        // Allow for trimming of the history. Some types of histories do not
        // need store changesets prior to the oldest bound snapshot.
        if (_impl::History* hist = get_history())
            hist->set_oldest_bound_version(oldest_version); // Throws
    }

    // Do the actual commit
    REALM_ASSERT(m_group.m_top.is_attached());
    REALM_ASSERT(oldest_version <= new_version);
    // info->readers.dump();
    GroupWriter out(m_group); // Throws
    out.set_versions(new_version, oldest_version);
    // Recursively write all changed arrays to end of file
    ref_type new_top_ref = out.write_group(); // Throws
    // std::cout << "Writing version " << new_version << ", Topptr " << new_top_ref
    //     << " Read lock at version " << oldest_version << std::endl;
    switch (Durability(info->durability)) {
        case Durability::Full:
            out.commit(new_top_ref); // Throws
            break;
        case Durability::MemOnly:
        case Durability::Async:
            // In Durability::MemOnly mode, we just use the file as backing for
            // the shared memory. So we never actually flush the data to disk
            // (the OS may do so opportinisticly, or when swapping). So in this
            // mode the file on disk may very likely be in an invalid state.
            break;
    }
    size_t new_file_size = out.get_file_size();
    // Update reader info. If this fails in any way, the ringbuffer may be corrupted.
    // This can lead to other readers seing invalid data which is likely to cause them
    // to crash. Other writers *must* be prevented from writing any further updates
    // to the database. The flag "commit_in_critical_phase" is used to prevent such updates.
    info->commit_in_critical_phase = 1;
    {
        SharedInfo* r_info = m_reader_map.get_addr();
        if (r_info->readers.is_full()) {
            // buffer expansion
            uint_fast32_t entries = r_info->readers.get_num_entries();
            entries = entries + 32;
            size_t new_info_size = sizeof(SharedInfo) + r_info->readers.compute_required_space(entries);
            // std::cout << "resizing: " << entries << " = " << new_info_size << std::endl;
            m_file.prealloc(0, new_info_size);                                       // Throws
            m_reader_map.remap(m_file, util::File::access_ReadWrite, new_info_size); // Throws
            r_info = m_reader_map.get_addr();
            m_local_max_entry = entries;
            r_info->readers.expand_to(entries);
        }
        Ringbuffer::ReadCount& r = r_info->readers.get_next();
        r.current_top = new_top_ref;
        r.filesize = new_file_size;
        r.version = new_version;
        r_info->readers.use_next();
    }
    // At this point, the ringbuffer has been succesfully updated, and the next writer
    // can safely proceed once the writemutex has been lifted.
    info->commit_in_critical_phase = 0;
    {
        std::lock_guard<InterprocessMutex> lock(m_controlmutex);
        info->number_of_versions = new_version - oldest_version + 1;
        info->latest_version_number = new_version;
#ifndef _WIN32
        m_new_commit_available.notify_all();
#endif
    }
}


void SharedGroup::reserve(size_t size)
{
    REALM_ASSERT(is_attached());
    // FIXME: There is currently no synchronization between this and
    // concurrent commits in progress. This is so because it is
    // believed that the OS guarantees race free behavior when
    // util::File::prealloc_if_supported() (posix_fallocate() on
    // Linux) runs concurrently with modfications via a memory map of
    // the file. This assumption must be verified though.
    m_group.m_alloc.reserve_disk_space(size); // Throws
}


std::unique_ptr<SharedGroup::Handover<LinkView>>
SharedGroup::export_linkview_for_handover(const LinkViewRef& accessor)
{
    if (m_transact_stage != transact_Reading) {
        throw LogicError(LogicError::wrong_transact_state);
    }
    std::unique_ptr<Handover<LinkView>> result(new Handover<LinkView>());
    LinkView::generate_patch(accessor, result->patch);
    result->clone = 0; // not used for LinkView - maybe specialize Handover<LinkView> ?
    result->version = get_version_of_current_transaction();
    return result;
}


LinkViewRef SharedGroup::import_linkview_from_handover(std::unique_ptr<Handover<LinkView>> handover)
{
    if (handover->version != get_version_of_current_transaction()) {
        throw BadVersion();
    }
    // move data
    LinkViewRef result = LinkView::create_from_and_consume_patch(handover->patch, m_group);
    return result;
}


std::unique_ptr<SharedGroup::Handover<Table>> SharedGroup::export_table_for_handover(const TableRef& accessor)
{
    if (m_transact_stage != transact_Reading) {
        throw LogicError(LogicError::wrong_transact_state);
    }
    std::unique_ptr<Handover<Table>> result(new Handover<Table>());
    Table::generate_patch(accessor.get(), result->patch);
    result->clone = 0;
    result->version = get_version_of_current_transaction();
    return result;
}


TableRef SharedGroup::import_table_from_handover(std::unique_ptr<Handover<Table>> handover)
{
    if (handover->version != get_version_of_current_transaction()) {
        throw BadVersion();
    }
    TableRef result = Table::create_from_and_consume_patch(handover->patch, m_group);
    return result;
}
