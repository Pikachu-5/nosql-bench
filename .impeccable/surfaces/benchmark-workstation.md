# BenchForge workbench direction contract

## THESIS

Make the requested operation distribution the first thing an operator can inspect. BenchForge is a measurement workstation; it configures a synthetic workload, starts a local worker, and reports recorded run data. The interface should read like a cool-toned logic analyzer without pretending to be physical equipment.

## OWN-WORLD

Logic analyzer display grammar: six labeled channels, one horizontal percentage scale, aligned numeric readouts, restrained signal colors, and cool blue-gray instrument surfaces. The palette is a translation for a browser dashboard, not a simulated bezel or knob set.

## STORY

The operator sets six operation weights, confirms their sum is 100%, configures scenario and run bounds, then starts a local run. The adjacent ledger reflects runs actually returned by the API. A selected successful run reveals its recorded summary. A `noop` adapter remains explicitly identified as a harness baseline; its timings are not database performance.

## FIRST VIEWPORT

The page title and API connection state establish the task and system status. The six-channel operation profile is the dominant visual surface and binds directly to the existing weight inputs: timeline read, profile read, post like, post create, hashtag search, and follow. The total stays visible. At desktop widths the run ledger remains alongside the setup; at narrow widths both sections stack without horizontal page scrolling.

## FORM

Preserve all current controls, labels, validation, start/cancel actions, polling, run selection, and results. Keep native keyboard-operable form controls. Implemented from seed `b6241d49`; the selected source card is `.impeccable/mocks/decision/model-pick.png`. No synthetic run history or benchmark values. The lane bars visualize configured weights only, never throughput, runtime progress, or measured results. Result tables may scroll within their own viewport.

## FINISH

Use a cool slate and blue palette with crisp contrast, measured borders, tabular values, and one dark instrument field for the channel allocation display. Keep status color paired with readable text. Preserve all offline, empty, loading, error, active, cancelled, and completed states. At mobile widths, controls remain legible and usable with no sideways page scroll.

## MOTION THESIS

- **Focal moment:** the six channel fills ease to their newly configured percentages when a weight changes; the number remains readable throughout.
- **Continuity:** selecting a run updates the adjacent detail area without making the operator wait through a page transition.
- **Feedback:** a restrained status pulse indicates an actually running worker; control focus and button press transitions acknowledge input.
- **Budget:** CSS transitions on the six narrow fill elements and a single low-cost active-run pulse. No animated page entrance, fake signal stream, or benchmark-progress animation. Under `prefers-reduced-motion`, lane fills update immediately and the running indicator remains static but visible.
