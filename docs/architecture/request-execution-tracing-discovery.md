# Request Execution and Tracing Discovery

## Files

- `seceda_edge/cpp/src/runtime/request_executor.cpp`
- `seceda_edge/cpp/src/runtime/edge_daemon.cpp`

## Problem

`RequestExecutor` owns routing and fallback execution, response hydration, identity mapping,
metrics lifecycle, and prompt trace JSON construction. The request execution module is useful,
but its interface hides less than it could because observability details are mixed into the
implementation.

The result is weak locality for trace schema changes: updating trace payload shape requires
working inside the same module that decides local versus cloud execution.

## Solution

Move prompt trace construction into a dedicated observability module behind the existing trace
registry seam. `RequestExecutor` should describe execution events in domain terms, while the
observability module maps those events into `PromptTraceEvent` records and JSON payloads.

## Benefits

This would improve locality for trace schema changes and keep routing/fallback tests focused on
execution policy. It would also deepen `RequestExecutor`: callers still get the same routing and
fallback leverage, while less trace-specific implementation detail sits behind that interface.
