# Design notes

Why TaskHandler is built the way it is. The README documents what the library
does; this is for anyone changing how it does it.

- [Scope](#scope)
- [Shape](#shape)
- [State and invariants](#state-and-invariants)
- [Locking](#locking)
- [Submission](#submission)
- [Scheduling](#scheduling)
- [Shutdown](#shutdown)
- [Backpressure](#backpressure)
- [Shared handlers](#shared-handlers)
- [Two consumption modes](#two-consumption-modes)
- [Versioning and ABI](#versioning-and-abi)
- [Testing](#testing)
- [Performance](#performance)
- [Alternatives considered](#alternatives-considered)

## Scope

TaskHandler is a *serial* executor: one thread per handler, one task at a time,
in a defined order. Everything else follows from that. It is the shape you want
for an event handler, because state owned by a handler needs no locking when
only its worker touches it.

It is deliberately not a thread pool, not a work-stealing scheduler, and not a
coroutine runtime. A pool would break the property the library exists for. Those
belong in a different type, and a program that needs both can have both.

The library also has no dependencies beyond the standard library and a thread
library. That is a feature for the sort of project that vendors a header, and it
is the reason GoogleTest sits behind a vcpkg feature that consumers never build.

## Shape

```
add_callable()                          worker thread
      |                                       |
      v                                       |
 ensure_accepting()  --- refuse --->  TaskHandlerStopped
      |                               TaskHandlerQueueFull
      v
   timed_  --- deadline reached --->  ready_  --->  task->run()
 (deadline,                        (priority,
  sequence)                         sequence)
```

Two ordered containers, one worker, one mutex over both. A submission inserts
into one of the maps and notifies; the worker takes from the front of `ready_`,
runs the task with the lock released, and goes round again.

`Blocked` and `Future` are not separate machinery. They queue an ordinary task
that fulfils a `std::promise` or drives a `std::packaged_task`, so there is one
queue and one execution path to reason about.

## State and invariants

Everything below `mutex_` is one lock's worth of state:

| Member | Meaning |
| --- | --- |
| `ready_` | Runnable tasks, ordered by `(priority desc, sequence asc)` |
| `timed_` | Not yet due, ordered by `(deadline asc, sequence asc)` |
| `sequence_` | Monotonic counter, the tiebreak that makes equal priorities FIFO |
| `stop_requested_` | Set once; the worker exits when it next finds `ready_` empty |
| `busy_` | A task is running right now, so `flush()` must keep waiting |
| `worker_id_` | The worker's thread id, or default when no worker is running |

The invariants a change has to preserve:

- A task is in exactly one of `ready_`, `timed_`, or the worker's hands.
- A `sequence_` value is never reused, which is what makes a `TaskId` safe to
  cancel with: it can never name a later task.
- Promotion from `timed_` to `ready_` keeps the original sequence number, so a
  delayed task stays cancellable across the move.
- Nothing accepted into `ready_` is dropped. `stop()` drains it, because a
  `Blocked` caller is waiting on a promise that only the task can fulfil.
- User code -- a task body, a task destructor, the exception hook -- never runs
  with `mutex_` held. It can call back into the handler, and one that does must
  not deadlock.

## Locking

Two mutexes, in this order and never the other way round:

1. `lifecycle_mutex_` guards `thread_`, so `start()` and `stop()` cannot both
   be handling the same `std::thread` object.
2. `mutex_` guards the queues and the flags above.

`work_cv_` wakes the worker; `idle_cv_` wakes `flush()` and the `start()`
handshake. The handshake matters: `start()` does not return until the worker has
published its id, because otherwise a task could already be running while
`is_current_thread()` still answered `false`.

## Submission

`add_callable` dispatches on a policy tag rather than offering three names, so
the callable and priority arguments stay in one place:

| Policy | Returns | Ownership of the callable |
| --- | --- | --- |
| `Queued` | `TaskId` | Owned by the task; may outlive the caller |
| `Blocked` | nothing | Borrowed; the call does not return until it has run |
| `Future` | `std::future<R>` | Owned, via `std::packaged_task` |

`Blocked` is the one case that captures the caller's callable by reference, and
it is sound only because the function does not return until the task has run.
`Future` must own it: the future outlives the call.

Tasks are stored as `std::unique_ptr<detail::Task>`, a small move-only
type-erased base, rather than `std::function`. `std::function` requires its
target to be copy-constructible, which would reject a lambda that captured a
`std::unique_ptr` -- exactly the shape a queued task usually has.

`Blocked` and `Future` check `is_current_thread()` and run the task inline when
they are already on the worker. Queueing there would wait on a task that cannot
start until the current one returns, which is a self-deadlock rather than
recursion.

## Scheduling

`ready_` is keyed on `(priority, sequence)` with higher priority first, so the
first element is always the next task and insertion is O(log n). Priority never
preempts: it decides what the worker picks up next, and a long task delays
everything behind it.

`timed_` is keyed on `(deadline, sequence)`. The worker sleeps on `work_cv_`
until the earliest deadline, promotes whatever is due, and only then looks at
priority. A delayed task therefore never runs early, and a busy handler may run
it late.

## Shutdown

`stop()` means "run what you have already accepted, then exit". It does not mean
"drop everything", because a `Blocked` caller cannot be left waiting on a
promise that nothing will ever fulfil.

Delayed tasks that are not yet due are the exception: they are discarded, since
waiting out an hour-long deadline is not a shutdown. The worker drops them as it
leaves, with the lock released, because a task's destructor is user code.

The awkward cases, all of which have regression tests:

- **`stop()` from inside a task.** A thread cannot join itself; doing so throws
  `std::system_error` and takes the process with it. The call records the
  request and returns, and the worker exits once it has drained.
- **`start()` after that.** The `std::thread` object stays joinable after the
  worker has exited, because nobody joined it. `start()` reaps it rather than
  mistaking it for a live worker, which used to leave such a handler stopped for
  good.
- **`start()` from inside a task.** It returns without doing anything. Taking
  `lifecycle_mutex_` would deadlock against a `stop()` holding it while waiting
  to join this very worker, and clearing the stop request instead would leave
  that `stop()` waiting on a worker that no longer intends to exit.
- **`flush()` from inside a task.** Returns immediately; waiting could only
  deadlock.

The one case that has no good answer is *destroying* a handler from inside one of
its own tasks. `stop()` can defer, but a destructor cannot: it has to leave the
object gone by the time it returns, and the worker is still running in it. So
that is a documented limit rather than something the library can absorb, the same
way joining a thread from itself is.

## Backpressure

The queue is unbounded by default, which is the right default for an event
handler and the wrong one for a pipeline whose producer is faster than its
consumer. `TaskHandlerOptions::max_pending` bounds it, and submission then
throws `TaskHandlerQueueFull` instead of letting the queue grow until the
process runs out of memory.

Throwing rather than blocking is deliberate. A blocking submit would make
`add_callable` a place where an unrelated thread can be parked indefinitely, and
in a graph of handlers that is a deadlock waiting to happen. A caller that wants
to wait can wait: it has `pending()` and it has the exception.

Recursive `Blocked` and `Future` submissions run inline without queueing, so the
limit cannot refuse them. A task that is already running is not the producer the
limit is there to slow down.

## Shared handlers

`instance()` hands out three process-wide handlers, created on first use. They
are deliberately never freed: a reference handed out has to stay valid for the
rest of the program, including while other static objects are running their
destructors, so `uninit()` stops their workers and leaves the objects in place.
Freeing them would turn a shutdown-ordering mistake in a consumer into a
use-after-free instead of a `TaskHandlerStopped`.

Three is arbitrary but fixed, because `instance<Index>()` checks the index at
compile time. A program that wants a different number owns its handlers, which
is what constructing one is for.

## Two consumption modes

The same source is either a header-only library or a compiled one:

| | Header-only | Compiled |
| --- | --- | --- |
| Selected by | nothing; the default | `TASKHANDLER_COMPILED_LIB` |
| `TASKHANDLER_INLINE` | `inline` | empty |
| Definitions in `detail/task_handler-inl.h` | included by the public header | compiled once, into `src/task_handler.cc` |

One set of definitions, two ways of compiling them, so the modes cannot drift.
The test suite is built twice, once against each, for the same reason.

What they cannot do is share a binary. Mixing them gives two sets of shared
handlers and two copies of the definitions; CMake propagates the macro from
`TaskHandler::task_handler` so that a consumer using the targets cannot get this
wrong by accident.

The shared build exports the ABI surface by hand -- `TASKHANDLER_API` on the
class, `TASKHANDLER_VISIBLE` on the exception types -- under
`-fvisibility=hidden`. The exceptions need default visibility even though they
have no compiled members: an exception thrown inside the shared object cannot be
caught by type on the other side of the boundary otherwise.

`TaskHandler` is not pimpl'd. It would cost an allocation and an indirection per
handler, and its only real benefit here -- freedom to add members without
breaking ABI -- is not worth that for a type whose members are a mutex, two maps
and a thread. The cost is that adding a member to `TaskHandler` or
`TaskHandlerOptions` is an ABI break, and MSVC needs C4251/C4275 suppressed.

## Versioning and ABI

The version lives in `project()` and in `TASKHANDLER_VERSION_*`, because a
header-only consumer has no CMake project to ask. Configuring checks the two
against each other, so they cannot drift.

`runtime_version()` reports what the *library* was built from. In a header-only
build that is trivially the header's own version; in a compiled build it is how
a header that has drifted from the shared object next to it shows up.

Semantic versioning, with the usual pre-1.0 caveat: while the major version is
0, a minor bump may break API or ABI, and the changelog says what broke. The
installed package config is written `COMPATIBILITY SameMajorVersion`, so
`find_package(TaskHandler 0.2)` will not silently accept an incompatible
install.

## Testing

Threading bugs do not reproduce on demand, so the suite leans on determinism
where it can and on repetition where it cannot:

- A `Gate` parks the worker inside a task, so tests about ordering and about
  what is still queued do not have to guess at a sleep duration.
- Every case runs against both consumption modes.
- CI runs the suite under ThreadSanitizer, AddressSanitizer and
  UndefinedBehaviorSanitizer, and repeats it twenty times under TSan, because
  one green run proves less here than it does elsewhere.
- Each fixed bug keeps a named regression test, since every one of them was a
  case that looked impossible until it happened.
- clang-tidy runs with `--warnings-as-errors`, because it exits 0 on findings and
  a check that reports without failing is a check nobody reads.

## Performance

`benchmarks/` times submission throughput, timer bookkeeping and the `Blocked`
and `Future` round trips. It pulls in no benchmark framework: a loop and a
`steady_clock` are enough to answer "did that change cost anything", which is
the only question being asked, and the alternative is a build dependency for a
library that otherwise has none.

The numbers only mean something against other numbers from the same machine.
Run it before and after a change to the queue.

## Alternatives considered

- **A binary heap instead of `std::map` for `ready_`.** Fewer allocations and
  better locality, but `cancel()` becomes a linear scan or a tombstone set. The
  map keeps cancellation O(log n) for a cost that is one node allocation per
  task, next to the task's own.
- **A lock-free queue.** The worker has to sleep when idle and wake on
  submission, so a futex is involved either way, and priority ordering plus
  cancellation is most of what makes a lock-free design hard. The mutex is held
  for a map insert, never across user code.
- **Gating `work_cv_.notify_one()` on whether the worker is actually waiting.**
  Measured as noise on glibc, which already avoids the syscall when there is no
  waiter, so the extra state was not worth it.
- **A thread-local "current handler" pointer, to make `is_current_thread()`
  lock-free.** It saves one uncontended lock per `Blocked` or `Future`
  submission, but an inline function's thread-local can be duplicated across
  shared objects, and a false `is_current_thread()` turns recursive use back
  into the deadlock the check exists to prevent. Not worth it for a submission
  path that already takes the lock to insert.
- **A pimpl'd `TaskHandler`.** See [Two consumption
  modes](#two-consumption-modes).
