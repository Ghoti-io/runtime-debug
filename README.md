# Ghoti.io Runtime-debug

The debugger of the Ghoti.io language runtime stack, in C: one *debug model* of
breakpoints, stepping and the state of a stop, attached to a
[`runtime-core`](../runtime-core) context, and a thin Debug Adapter Protocol
(1.71) adapter over a transport the host binds. A developer sets a breakpoint
in an editor such as VS Code, the guest stops at the line, and the editor shows
the call stack, the scopes and the variables; it steps in, over and out and
continues. The debugger names no engine: it learns what a frame means from the
descriptor the engine registered, and an engine never names the debugger.

Attaching a debugger changes nothing about the program: it allocates only from
its own allocator, charges no fuel, reads frames only where a debugger may, and
while it has nothing to do it leaves the poll's one-load fast path alone. That
claim is tested, with a comparison that is itself shown to fail on a "debugger"
that does charge fuel or write a slot.

Nothing is released. So far there is the model, the adapter and its framing and
limits, two transports (POSIX descriptors and a memory buffer), a fuzz
harness, the gates, an example host and the benchmark harness.

## Example

```c
#include <ghoti.io/runtime-debug/runtime-debug.h>
#include <ghoti.io/runtime-core/runtime-core.h>

/* group, context and engine set up as for any host, then: */
GRDBG_Debugger * debugger;
grdbg_debugger_attach(context, NULL, &debugger);     /* dies with the context */

GRDBG_Transport * transport;
grdbg_transport_create_fd(connection, connection, NULL, &transport);
GRDBG_Dap * dap;
grdbg_dap_create(debugger, transport, NULL, &dap);

GRDBG_ServeResult serve;
grdbg_dap_serve(dap, &serve);                         /* until configurationDone */
GRCORE_Outcome outcome;
grcore_run(context, entry, state, &outcome);
while (outcome == GRCORE_OUTCOME_PAUSED) {
  grdbg_dap_notify_stopped(dap);                      /* why, and where */
  grdbg_dap_serve(dap, &serve);                       /* stackTrace, variables, next... */
  grcore_resume(context, &outcome);
}
grdbg_dap_notify_finished(dap, 0);
```

[examples/dap_session.c](examples/dap_session.c) is a complete host: it binds
and accepts a loopback socket (the library never does), runs a client thread
that plays a scripted session, and prints the conversation.

## Building

`runtime-debug` depends on `cutil`, `runtime-core` and `text` (for JSON),
found through pkg-config only. Build the suite first from the workspace root
(`./bootstrap.sh`), then:

```bash
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/runtime-debug test PREFIX="$PWD/.local"
```

Pass `PREFIX=` to every `make`, including a throwaway one: the rpath is added
only when it is set. `make help` lists the targets. The ones particular to
this library:

| Target | Does |
| --- | --- |
| `test` | build, `check-symbols`, `check-aliasing` (gcc only), `check-stamps`, the gates below, the fuzz replay, the examples, the unit tests, and one smoke run of the benchmark |
| `examples` | build each program under `examples/` and run it; a failing example fails `test` |
| `check-labels` | fail if a public header has no `@stability stable` label (every header here is `stable`) |
| `check-edges` | fail on any `#include` or shared-object dependency on a Ghoti library other than `cutil`, `text`, `runtime-core` and this one |
| `check-gates` | run each gate against a planted defect and a control, and against an empty tree, and fail unless each behaves |
| `fuzz-replay` | feed every file of `tests/fuzz/corpus` once through the fuzz entry point in an ordinary build; fails on an empty corpus |
| `fuzz-dap`, `fuzz-run-dap` | build the libFuzzer harness (clang) and run it for `FUZZ_TIME` seconds |
| `bench` | run the benchmark harness in full; it prints a calibration result first |
| `test-asan`, `test-tsan`, `test-valgrind-quiet` | the same tests under ASan+UBSan, ThreadSanitizer and Valgrind |
| `coverage` | instrumented run and line report |

## The API

All headers are in `include/ghoti.io/runtime-debug/`, all labelled `stable`:
this is the C embedding API of the debugger (AD-14). `runtime-debug.h`
includes them all.

| Header | Holds |
| --- | --- |
| `model.h` | `GRDBG_Debugger`: attached to a context under a cardinality-one YIELD key. Line breakpoints (`grdbg_debugger_set_breakpoints` replaces a source's whole set), `step` (in, over, out), `continue`, `request_pause`, `disarm`, and the state of a stop: frames, scopes and variables. Knows nothing of JSON or of any protocol |
| `dap.h` | `GRDBG_Dap`: a Debug Adapter Protocol session. `serve` answers requests until one asks the host to proceed, and `notify_stopped` and `notify_finished` emit the events |
| `transport.h` | `GRDBG_Transport`: a stream pair the host binds, with ready-made ones over POSIX descriptors and a memory buffer |
| `limits.h` | `GRDBG_Limits`: the caps on headers, messages, JSON depth, breakpoints, frames and variables, with `grdbg_limits_default` |
| `core.h`, `allocator.h`, `libver.h`, `macros.h`, `namespace.h` | the suite's results, allocator, version and symbol namespacing |

**One model, thin adapters.** The model has the debug logic and the adapter
translates and maps ids. A second protocol (CDP, later) is another adapter
over the same model.

**Who owns what.** The debugger owns every string and id it returns, and they
are valid until the next resume. The host owns the transport, the session and
the context; destroy the session before the context.

**How it stops a program.** The debugger is a YIELD handler plus a sticky
request kind of its own. It arms (posts the kind through the context's port)
when it holds a breakpoint, a step or a pause request, and disarms when it
holds none, so an attached debugger with nothing to do costs a poll nothing.
At a slow poll the handler reads the innermost frame, and votes to pause when a
breakpoint matches its (source, line), a step is satisfied or a pause was
asked for. It never votes at a poll that cannot pause to the host (the runtime
poll of a native, or a poll inside a nested activation), because core would
turn that pause into an unwind and the program would no longer be the one that
was being looked at.

**The granularity limit.** A line breakpoint fires where the engine polls at
that line. An engine that polls only at function entry and loop back-edges
stops only there. This is the contract between an engine and its debugger, and
`documentation/design.md` states it for the engine's authors.

**Stepping** is measured on the abstract frame walk: depth is the number of
frames, a location is (file, line). *In* stops at the next poll whose
(location, depth) differs; *over* at the next poll with depth not greater and a
different location, or a smaller depth; *out* at the next poll with a smaller
depth, and with no outer frame it runs to the end. A breakpoint reached on the
way wins; a pause for any other reason cancels the step.

**Threads.** A debugger is used from its context's owner thread; every
function that acts on it refuses another thread with `GRDBG_ERR_INVALID` (its
queries answer NULL, false or zero). It migrates with the context. There is no
I/O thread: protocol I/O runs on the owner thread, while the context is
stopped, and reading guest state from another thread is never offered (AD-6).

**What is not offered.** No expression evaluation (`evaluate` looks a name up),
no structured expansion of a value, no conditional, function or data
breakpoints, no `setVariable`, no `source` request, no remote-debugging policy,
and nothing listens or accepts inside the library.

[examples/README.md](examples/README.md) indexes the runnable examples by what
they show, and `make examples` builds and runs each.

## Status

Version 0.0.0, unreleased. The design and the alternatives it rejected are in
[documentation/design.md](documentation/design.md). Patches are not being
accepted at this time; see [CONTRIBUTING.md](CONTRIBUTING.md).
