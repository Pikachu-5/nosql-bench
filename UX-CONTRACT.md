# BenchForge interaction contract

The local workstation has two routes: Configure (`/`) creates and observes workers; Analysis (`/analysis`) reads saved captures. The workload is synthetic. Never invent measurements or treat `noop` as database performance.

## Canonical UI Map

| Capability | Canonical owner | Source of truth | Allowed variants | Verification |
|---|---|---|---|---|
| Navigation | `ui/Layout/WorkspaceHeader.razor`, Blazor Router | This contract | Configure / Analysis | Route links, page titles, keyboard |
| Select/Listbox | Native Blazor/native HTML select, global `app.css` | This contract + DESIGN.md | Platform popup geometry and keyboard are accepted | Open popup, long IDs, narrow viewport |
| Form | Blazor EditForm and InputBase; `UnsignedInputNumber.razor` | Config validation + Home.razor | Numeric workload fields | Bounded run submission |
| Scrollbar | `ui/wwwroot/css/app.css` | DESIGN.md | Document scrolling; horizontal table regions | Computed widths and keyboard scrolling |
| CRUD | `BenchForgeApi` + local control API | ARCHITECTURE.md | Create/cancel worker, read archive; no archive delete | Successful run, archive restart and read errors |
| Feedback | Inline `.notice`, status live regions | This contract | Status / recoverable error | Offline, retry, loading, unreadable summary |
| Comparison | `ui/Services/RunComparison.cs` | Measurement metadata + this contract | Two selected captures | ComparisonChecks console checks + browser |

## Saved results and comparison

- Catalog only existing schema-v2 summaries under `runs/<run-id>` and `runs/<bucket>/<run-id>`. No fabricated rows. Scan limits: 5,000 entries, 32 MiB total source data, at most 500 results. Each summary is bounded to 1 MiB; symbolic links, unsafe names and incomplete summaries are rejected. Show skipped/truncated counts.
- Preserve baseline and candidate keys in URL query parameters. Back/forward and reload restore committed selection. Native options include source and complete IDs; detail evidence keeps complete IDs accessible when the closed select clips text.
- Fetch selected captures on change. Cancel superseded requests, reject stale replies, clear obsolete measurement panels, and show a bounded loading state. Refresh retries archive/detail failures. Offline errors retain the list as potentially stale and withhold comparative percentages.
- Show recorded throughput, operation counts, errors/timeouts, p50/p95/p99/p99.9 and scheduled send lag. Zero samples show a dash. Tables use native semantics, captions, scoped headers and focusable horizontal scroll regions; the document owns vertical scrolling.
- Only show changes for different valid database captures with consistent counts and matching workload, scheduling, histogram and host metadata. Report all blocking differences. Verification-folder captures are correctness evidence and do not show comparative percentages.
- Show full adapter/version, endpoint, storage/durability/query model, calibration, environment, dataset, weights, validity and cleanup evidence. Host capacity is not database usage. Two captures do not establish a ranking; repeat important configurations at least three times and report median/range. Calibration is never subtracted.

## Accessibility and supported presentation

English interface, locally bundled Cascadia Code, light workstation theme. Native popup locale/geometry follow the user's platform. Keep units and tabular figures visible. Target WCAG 2.2 AA, label controls explicitly, preserve native keyboard behavior and readable state text, and maintain high-contrast focus. At 760px the selection and evidence columns stack; tables scroll internally. No authored motion is needed for recorded static measurements; reduced-motion preferences remain supported by the global stylesheet. This contract describes the target, not a claim of formal certification.
