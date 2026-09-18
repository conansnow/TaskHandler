# Security policy

## Supported versions

The latest release on `main` is the only version that receives fixes. While the
major version is 0, that means the newest minor release.

## Reporting a vulnerability

Report it privately through GitHub: open the **Security** tab of this repository
and use **Report a vulnerability**. That opens a private advisory visible only to
the maintainers, so please use it rather than a public issue for anything that
looks exploitable.

Include the version or commit, the platform and toolchain, and something that
reproduces it -- a test case in the shape of `tests/task_handler_test.cc` or
`tests/thread_pool_test.cc` is ideal.

## Scope

TaskHandler and ThreadPool run whatever callable they are given, on threads
they own. Anything a task does is the caller's responsibility; what belongs
here is the library's own behaviour. In practice that means memory safety and
thread safety:

- Use-after-free or data races inside the handler or the pool, including
  during `stop()`, `uninit()` and static destruction.
- A submission being accepted and then silently dropped, or a `Blocked`
  caller left waiting on a task that will never run.
- Unbounded memory growth that `max_pending` cannot bound.
- Anything that makes the library abort or terminate the process rather than
  reporting a failure to its caller.

Deadlocks that the documentation describes as expected -- `Blocked` in both
directions between two handlers, every `ThreadPool` worker blocked on more
pool work, or a task that waits on work queued behind itself -- are design
limits rather than vulnerabilities. They are listed under "What to watch out
for" in the [README](README.md).
