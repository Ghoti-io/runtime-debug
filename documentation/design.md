# Design

**Status:** In progress. Describes what exists: the debug model (breakpoints,
stepping, the state of a stop), the Debug Adapter Protocol adapter with its
framing, limits and fuzz harness, two transports, the gates and the benchmark
harness, taken from the runtime stack's architecture spine (AD-2, AD-4, AD-5,
AD-6, AD-13, AD-14, AD-15, AD-16, AD-18 to AD-21, AD-26). What is not here is
listed at the end.

## What this library is

`runtime-debug` is the debugger. It holds one *model* of what a debugger is:
line breakpoints, three kinds of step, and the state of a stop (the frames, and
the scopes and variables of each). It attaches to a `runtime-core` context
through one cardinality-one key in the poll's YIELD phase, together with a
request kind of its own that makes a poll slow only while the debugger has
something to do, and it dies with the context. On top of the model sits a
*Debug Adapter Protocol* adapter (1.71) that reads requests from a transport the
host binds, calls the model, and writes responses and events. An editor such as
VS Code is a client of that adapter.

It names no engine. What a frame means (its file and line, its scopes and
variables, how a value is shown) it asks the engine's descriptor, through the
abstract frame and the frame walk of A (AD-18). An engine does not name the
debugger either: nothing in an engine depends on `debug`, and the gate checks
that this library includes no engine (AD-2). The debugger is attached by the
host.

## One model with adapters, not a DAP-shaped core

AD-15 says protocols are thin adapters over one model, and this is where that
is made true. `src/model/` and `model.h` know nothing of JSON, of DAP, of
`seq` numbers or of `Content-Length`. `src/dap/` holds no debug logic: it maps
ids (a frame id is a frame index plus one; a `variablesReference` names a
(frame, scope) pair), converts lines between the client's base and the model's,
and shapes messages. A breakpoint is set by `grdbg_debugger_set_breakpoints`;
the adapter's `setBreakpoints` is a loop around it.

**Rejected: a core shaped like DAP.** The protocol's own vocabulary (threads, a
`stackTrace` that pages, `variablesReference`s) would then be the model, and the
second adapter (CDP, which has its own call frames, scope chains and object ids)
would have to translate away from DAP to get to what it wants. The model has
what both need and neither's wire shape.

## The YIELD handler, and why not OBSERVE or a dedicated hook

The debugger is a YIELD handler. YIELD is the phase the spine gives to the
debugger, host callbacks and jobs (AD-5), and it is the only phase that may
vote *after* DECIDE and ACT have run. That ordering is what a debugger needs: a
fuel or time pause has already been decided and carried out when YIELD begins,
so a budget outranks a breakpoint, and the host sees the budget as the reason
(YIELD runs only when the verdict is continue). Stopping is a pause vote, so the
host sees an ordinary paused `run` whose pause keys include the debugger's key,
and everything the debugger reads it reads between that pause and the next
`resume` (AD-20).

**Rejected: OBSERVE.** OBSERVE is read-only and cannot vote, so a breakpoint
there could not stop anything. It is the right phase for the frame-level
differential and the recorder (the tests use one to prove that attaching changes
nothing), and a debugger must not be one.

**Rejected: a dedicated hook in `core`.** It would put the debugger in the
kernel (AD-1): core would know the debugger's type, and every service after it
would ask for its own hook. A key and a phase already say everything a hook
would.

## A sticky service request, not a handler that runs every poll

The poll's fast path is one load and one branch (AD-4). A handler registered in
a phase runs only when a request is pending, so a debugger that wanted to look
at every poll would have to keep a request pending, and then every poll of an
attached debugger would be a slow poll.

So the debugger defines a request kind of its own when it is attached
(`grcore_context_request_kind`, which makes the bit attributable to its key,
AD-19) and keeps it *clear*. It posts the kind through the context's own port
only while it holds a breakpoint, a step or a pause request, and clears it when
it holds none. The state is a pure function of the debugger's tables, and a
test checks it from the outside: an unarmed debugger runs a whole program
without any other service's handler seeing a slow poll. The benchmark shows the
same thing as a number (below).

**The unarmed-cost claim.** A poll with an unarmed debugger attached is the
same instructions as a poll with no debugger: the first measurement is 3.37 ns
against 3.30 ns, within the noise of the machine. This is a property of the
design and not an optimisation: nothing about the debugger is on the fast path.

**Rejected: the debugger polls the transport at every armed poll.** It would
turn each armed poll into a system call, make the guest's speed a function of
the network, and give the debugger a way to block a program that has no client.
Instead the debugger acts only where it can stop the program, and a stop is when
the host hands control to the transport.

## Breakpoints: by (file, line), not by poll identity

A breakpoint is a (source string, line) pair, and the source is compared byte
for byte with the file of the innermost frame's `location`. A poll identity
(function, offset) is the engine's own name for a place and means nothing to a
client; an editor sends a path and a line. The engine's `locate` turns the
identity into a location when a pause or an unwind needs one, so the debugger
asks it and compares.

The cost is that a line may be many polls and a poll may be no line. The
adapter's `setBreakpoints` replaces a source's whole set (the protocol's
semantics), assigns fresh ids that are never reused, and reports each as
verified, because the debugger cannot know whether a line exists in a source it
has not been shown.

**Rejected: breakpoints by poll identity.** They would need the client to know
the engine's function and offset numbering, or the adapter to keep a table from
lines to identities that only the engine can build.

### What a line breakpoint needs from an engine (the contract for story 13)

A breakpoint can only fire where the engine *polls*. An engine that polls only
at function entry and loop back-edges (as AD-4 puts the minimum) stops only
there, and a breakpoint on a line in the middle of a straight run of statements
never fires. The debugger cannot do anything about that: it cannot ask the
engine to poll more, because an engine cannot ask whether a debugger is attached
(AD-2), and a poll that did would not be one load and one branch for anyone.

So the contract is stated here and not hidden. For line breakpoints and
single-stepping to mean what a developer expects, an engine polls at every
statement it wants to be steppable, with a poll identity whose `locate` names
the line, and it is the engine's host that decides whether that cost is worth it
(lang-tang's story 13 decides how the interpreter polls at statements, and what
a statement poll costs against the benchmark). Everything else here is
independent of that choice.

## Stepping: depth by frame count

A step is measured on the abstract frame walk (AD-18): *depth* is the number of
frames and a *location* is (file, line). The step records both at the stop it
starts from.

- *In* stops at the next poll whose (location, depth) differs from the start.
- *Over* stops at the next poll with depth not greater than the start and a
  different location, or a smaller depth. A call made on the way is deeper than
  the start, so the poll inside it is not a stop, however many times the same
  line recurs below.
- *Out* stops at the next poll with a smaller depth. With no outer frame no
  poll has one, so it runs to the end.

A breakpoint reached on the way wins and is reported as a breakpoint, and the
step is cancelled by it. A pause of any other cause (a budget, an interrupt)
cancels the step too, because the program is no longer where the step started;
the debugger learns of that when the host begins the stop
(`grdbg_debugger_stopped`), since its YIELD handler does not run for a pause
another key caused.

**Rejected: depth by the call-site identity of each frame, or by the engine's
own call depth.** Frame count is something the walk gives for every engine and
every tier, it is the figure the stack-depth budget counts (AD-16, AD-21), and
it needs nothing the descriptor does not already provide.

**The walk is paid for only while stepping.** The handler reads the innermost
frame only. It walks the whole stack to count frames only while a step is
active, which is the cost of an O(depth) step, not of every armed poll.

## Polls that cannot pause

Core turns a pause verdict into a limit unwind in two places: the runtime poll
of a native (AD-21) and any poll inside a nested activation (AD-5). A debugger
that voted to pause at either would end the program it only meant to look at.
The handler is not told which kind of poll it is in, so this story added one
small accessor to `runtime-core`, `grcore_pollcall_pause_allowed`, which reports
the `allow_pause` decision the poll had already made. The handler also checks
the nesting count, for the same reason from the other side. A test makes the
mistake on purpose, with a handler that votes to pause at every poll without
asking, and shows the run unwind with `ERR_LIMIT` at the native's poll; the
debugger on the same program finishes it unchanged. Removing the check from the
debugger's own handler fails the three tests that cover natives, nested
activations and a step across them.

## Resuming

After a stop the engine continues after the poll that paused, as
`runtime-core`'s `pause_resume` example does: the entry function's own state says
where. The debugger does not suppress a stop at the same poll identity after a
resume, because a one-statement loop polls at one identity on every iteration and
must stop on each. What prevents a program from being stopped twice at one poll
is the engine, which does not take the same poll again, and a test shows it
(the toy engine keeps a "polled" flag in its frame).

The debugger cannot see a resume. `grcore_resume` is the host's, and core tells
no one. What a stop recorded (the frames, the strings) is therefore dropped by
the calls that mean "going on" (`continue`, `step`, `disarm`), by every
armed poll its handler sees, and when the host *begins* a stop, which it must
do at every pause (`grdbg_debugger_stopped`, which `grdbg_dap_notify_stopped`
calls). As a last defence a recorded frame is checked against the stack before
it is used: its frame must still be valid, and its poll identity must be the one
recorded. A host that resumed without telling the debugger and paused again on a
budget is refused where it would have been misread, and a test does exactly
that.

The adapter keeps its `variablesReference`s only while the model's generation
number is the one they were issued under. Frame ids are numbered from 1 and
never reused: when the generation moves, the next stop's ids start above every
id the last could have issued. A frame id or reference from before a resume is
therefore unknown, an error response, and never another stop's frame.

## Reading is allowed only where AD-20 says

A context's state may be read only at-poll or paused, and only by the thread
that holds it. Every read of the model asks `grcore_context_guest_state_readable`
and refuses with `GRDBG_ERR_INVALID` otherwise, and every call that acts on the
debugger refuses a thread that is not the owner. The queries (`get`, `armed`,
`breakpoint_count`, `generation`) answer NULL, false or zero instead. The
debugger and its recorded state migrate with the context: a test pauses a
debugged context on one thread and reads, steps and finishes it on two others
(under ThreadSanitizer).

## The allocator: its own, never the context's

The debugger allocates only from its `GRDBG_Allocator`, by default cutil's,
never from the context's counting allocator or page provider. If it drew on the
context's, being attached would change the memory the program is charged for,
and a budget could pause a program for the debugger's sake. That is what AD-20's
"B does all accounting" and AD-15's "a debugger that changes program behaviour"
are about. The same choice is made for the JSON reader and writer: `text` takes
an allocator in its parse and write options, and the adapter hands it the
debugger's. The port the debugger posts through is the group's, as every port
is, and is charged to the group.

**Rejected: the context's allocator.** It is the heap's and the engine's
choice, where the charge is the point. Here it would be the defect.

## Transports: a stream pair the host binds

AD-13 makes the transport a CONVENTIONS section 5 stream pair that the host
binds: a `read(user, buffer, capacity, &n)` where zero is end of input, a
`write(user, buffer, n)` that takes all of it or fails, and an optional
`close(user)`. The library never listens, binds or accepts, so it has no opinion
about remote debugging, authentication or a port. Two are provided: one over
POSIX file descriptors (stdio is 0 and 1; a connected socket is the same
descriptor twice) that continues a partial `write(2)`, retries `EINTR` and sends
on a socket with `MSG_NOSIGNAL` so that a client that went away is an error and
not a signal, and one over a memory buffer for the tests and the fuzzer.

**A transport states its own size.** `GRDBG_Transport` is the one struct of this
library that a host defines and the library reads (the Windows host binds its
own handles through one, which is what the struct is for), so it follows
runtime-core's rule for `GRCORE_Key` (that library's `b/key.h` and design
document have the argument and the rejected alternatives): the first member is
the `sizeof(GRDBG_Transport)` the host compiled against, written by
`GRDBG_TRANSPORT_INIT(user, read, write, close)`, and a callback added at the
end (a flush, a readiness wait) would be read only where that size covers it,
an absent one being NULL. `grdbg_dap_create` and `grdbg_transport_memory_output`
refuse with `ERR_INVALID`, before anything is read or called, a transport whose
size is below `GRDBG_TRANSPORT_MIN_SIZE` or off the struct's alignment (zero is
a struct filled by assignment that forgot its size); a larger size is accepted.
The session *copies* the transport, so the copy is built as this library's own
full-size struct: the bytes the host's size covers, the rest zero, and `size`
set. Nothing has been added after `close`, so the tests (`test_transport_size.cpp`)
cover the rule, the library's own transports, the refusals with a full-size
control, and a newer size, and have no older layout to copy. `GRDBG_Limits` is
not given a size: it is a value filled by `grdbg_limits_default` or by
assignment, where a zero field means the default, and the library's own
function is how a caller gets one. It has the same exposure to a struct built
before a cap was added, and is left as an open decision, not decided here.

**Rejected: `FILE *`.** Buffering and `fflush` are decisions the adapter would
have to know about, and a socket needs `fdopen` and a mode that makes reading
and writing share state. **Rejected: a callback pair with no object.** It cannot
carry the descriptor or the buffer.

## Framing and limits

Messages are `Content-Length: N\r\n\r\n` and N bytes of UTF-8 JSON. The reader is
buffered and checks every cap before the bytes it governs are read: a header
block past `max_header_bytes` (1024), a length past `max_message_bytes` (4 MiB),
JSON deeper than `max_json_depth` (64). A missing, non-numeric, negative,
repeated or overlong-in-digits `Content-Length` ends the session as
`GRDBG_ERR_FORMAT` or `GRDBG_ERR_LIMIT`. **That a framing error ends the session
is recorded as a decision**: there is no marker to resynchronise on, and a
client that sent `Content-Length: abc` might have sent the body that follows as
a header. Guessing where the next message begins is worse than stopping. The
session then stays over (every later call returns the same error) and the
debugger is disarmed, so the host can resume the run free.

A well-framed body is a different thing from a broken frame. Bytes that are not
JSON, JSON that is not an object, or an object without a readable `seq` have
nothing to be answered to and are dropped; a request with a `seq` but the wrong
`type` or no `command` gets an error response; an unknown command gets
`success:false` and `unsupported request: <command>`. Nothing is parsed by hand:
the JSON is `text`'s parser with a depth and a size limit, and responses are
built with its streaming writer. A parse that failed for want of memory is
reported as such and not as a message that was not JSON.

A list a client may page through is cut at its cap and says how long it was: a
`stackTrace` returns at most `max_frames` frames (default 1000) and reports the
total in `totalFrames`; a `variables` response returns at most `max_variables`.
The model records only that many frames, though it counts them all, so a hostile
stack ten thousand frames deep costs a thousand records.

## DAP surface

The requests are `initialize` (capabilities: only
`supportsConfigurationDoneRequest`; then the `initialized` event), `launch` and
`attach` (accepted and ignored: the host already started the context),
`setBreakpoints` (line breakpoints only; conditions, hit counts and log messages
are ignored and the capabilities do not claim them), `setExceptionBreakpoints`,
`configurationDone`, `threads` (one thread, id 1, `main`), `stackTrace`,
`scopes`, `variables`, `continue`, `next`, `stepIn`, `stepOut`, `pause` (a no-op
while stopped: pausing a running guest needs the host to post
`GRCORE_REQUEST_INTERRUPT`, since the adapter cannot reach a running guest from
another thread), `evaluate` (a variable name looked up in the frame's scopes,
innermost first; anything else is `success:false` with `evaluation of
expressions is not supported`), `terminate` and `disconnect`.

The events are `initialized`, `stopped` (`breakpoint` with `hitBreakpointIds`,
`step`, or `pause` with a `description` naming the core key that caused it, such
as `paused by fuel`), `exited` and `terminated`. `continued` is not sent: the
client asked for it. `linesStartAt1` and `columnsStartAt1` are honoured; columns
are reported as 1 (0 for a client that counts from zero).

**What is not offered, and why.** Structured expansion of a value is not
offered: the descriptor has no child interface, so a variable is its name, the
inspector's text and the slot kind, and every `variablesReference` a variable
carries is 0. Object ids are `gc`'s stable ids (AD-12, AD-15), and the debugger
does not use them yet: using them would need `runtime-heap`, which this library
is forbidden to include (AD-2) until a descriptor says how to reach children
without it. **Expression evaluation** has no side-effect-free default decided
(the spine defers it to this library's design) and is not built. No `setVariable`,
no conditional or function or data breakpoints, no `source` request, no
`restart`, no reverse execution.

## No I/O thread

AD-6 allows debugger I/O on its own thread so long as it reads guest state only
through the owner. This library takes the simpler end of that: all protocol I/O
runs on the owner thread, while the context is stopped. `grdbg_dap_serve` blocks
reading the client until a request asks the host to proceed, and returns
`RESUME`, `TERMINATE` or `DETACH`. The host's loop (`dap.h`, and
`examples/dap_session.c`) is: serve until configured, run, and at every pause
notify, serve and resume.

**Rejected: a reader thread with a command queue.** Requests that arrive while
the guest runs cannot be answered (the guest state cannot be read from another
thread), so the queue would either hold them until a stop, in which case it is a
buffer the transport already is, or it would answer from stale data. It would
also give the library a thread, a lock and a shutdown protocol that the host,
which owns the process, has not asked for. **Rejected: the debugger polling the
transport at every armed poll** (see above).

## Fuzzing

The DAP reader and dispatcher have a libFuzzer harness (`make fuzz-dap`, clang)
that feeds arbitrary bytes through a memory transport against a context paused
three frames deep in an engine with scopes. The first byte picks the limits, so
one harness covers framing and JSON at every cap, and everything the adapter
writes is checked to be whole, framed messages that parse. The seed corpus is
valid sessions and the malformed frames the tests also use, and `make
fuzz-replay` feeds every file once through the same entry point under the gcc
build and fails if the corpus is empty. `make test` runs the replay, so a
regression the fuzzer found is a failing test. The first minutes of fuzzing
found one defect, a pointer offset on a buffer that did not exist yet, which is
in the corpus.

## Gates

`check-labels` requires every public header to carry exactly one `@stability
stable` label (AD-14: the C embedding API includes `debug`). `check-edges`
checks every `#include` under `src/` and `include/` and the `NEEDED` list of the
built shared object against an allowlist of `cutil`, `text`, `runtime-core` and
this library, so `runtime-heap`, `runtime-jit`, the `lang-*` libraries and
`tang` are forbidden edges without being named. It allows all of `runtime-core`,
A included, because the debugger reads the abstract frame. `text` brings
`chron`, `regex` and `unicode` along in its pkg-config entry; the shared library
is linked `--as-needed`, and the gate fails if one ever appears in its list.
`check-gates` runs each gate against a planted defect (a header with no label,
with the wrong label, with two; an include of `runtime-heap` and of `lang-tang`,
in a source and in a header; a forbidden link; a link to `chron`) and a control,
and an empty tree, and fails unless each behaves. The direction gate runtime-core
has is dropped: there is no `a/`/`b/` split here.

## Benchmarks

Every library ships a benchmark harness from its first commit (AD-26). This one
holds a calibration case, fixed integer work that touches no library code, so
that a figure from a real case can be read against the machine it was taken on,
and the debugger's cases. The first measurement, gcc 14 `-O2`, an Intel Core 7
150U, best of seven:

| Case | ns per op | What an op is |
| --- | ---: | --- |
| calibration | 1.12 | one xorshift step |
| poll-detached | 3.30 | a `grcore_stack_poll`, fast path, no debugger |
| poll-unarmed | 3.37 | the same with a debugger attached and nothing to do |
| poll-armed-1 | 88.5 | a poll through the handler with one breakpoint never reached |
| poll-armed-100 | 126.0 | the same with a hundred |
| stop-resume | 338.9 | a stop, reading it and frame 0, `continue` and `resume` |
| parse-request | 2070.7 | a framed `threads` request parsed and answered |
| stacktrace-100 | 69652 | a `stackTrace` of 100 frames, parsed, answered and encoded |

The unarmed poll is the detached poll. The armed poll is the core's slow path
(the four phases) plus a frame walk through the engine's `locate`, which is why
one breakpoint costs most of what a hundred do: the lines are compared before
the strings. No numeric budget is asserted; the spine records budgets once a first
measurement exists, and these are it.

## What is not here

- **Wiring into the `tang` command and the web-server host** was story 13, and
  is done. Nothing in `lang-tang` changes here, and `lang-tang` depends on this
  library only as a host does (AD-2: the `tang` command uses `?debug`).
- **CDP**, and any remote-debugging policy: who may connect, over what.
- **Expression evaluation, structured values, object ids**, as above.
- **CI** was deferred and now exists (`.github/workflows`, commit `9ce6768`): both compilers, the
  sanitizers, Valgrind, fuzz, a coverage floor and MSYS2. GitHub Actions is
  disabled for cost, so a local `make test` is the gate wherever it matters.
- **The Windows arm** of the descriptor transport: `grdbg_transport_create_fd`
  is `GRDBG_ERR_UNSUPPORTED` there, whatever it is given (a stub that is compiled
  and tested under wine), and a Windows host binds its
  own handles through a `GRDBG_Transport` of its own.
