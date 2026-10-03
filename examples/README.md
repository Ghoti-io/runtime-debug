# Examples

Indexed by task. `make examples` builds and runs every one; a failing example
fails `make test`.

| I want to... | Example | What it shows |
| --- | --- | --- |
| Let a DAP client such as VS Code stop my guest at a line, step it and read its variables | [`dap_session.c`](dap_session.c) | The host loop of `dap.h` end to end: bind and accept a loopback socket, hand the descriptor over as a transport, serve the client until `configurationDone`, run the context, and at every pause notify, serve and resume. A client thread plays a scripted session; the program prints the conversation |

`dap_session.c` uses `runtime-debug` and `runtime-core` only. Its "guest" is
a frame on the guest stack that polls with `grcore_stack_poll`, so a
descriptor's `locate` can name the line, which is what a real engine does and
the only thing an engine has to do to be debuggable (the debugger names no
engine and an engine does not name the debugger: AD-2).

A line breakpoint stops where the engine polls. An engine that polls only at
function entry and loop back-edges stops only there; `documentation/design.md`
says so, and says what that means for the engine's authors.
