---
name: BenchForge
description: A logic-analyzer-inspired workstation for configuring and inspecting synthetic database benchmark runs.
colors:
  work-surface: "#eaf0f3"
  panel: "#ffffff"
  text-primary: "#172c39"
  text-secondary: "#536a77"
  divider: "#cbd7dd"
  brand-blue: "#276f8b"
  brand-blue-strong: "#1c5c74"
  instrument-navy: "#122a39"
  instrument-raised: "#1a3545"
  trace-blue: "#58b6f2"
  trace-cyan: "#55d1d2"
  trace-teal: "#4bd0b1"
  trace-amber: "#ffbd58"
  trace-violet: "#bb9aff"
  trace-green: "#84db90"
  status-amber: "#9a671e"
  status-green: "#23684d"
  status-red: "#a04443"
typography:
  display:
    fontFamily: "Cascadia Code"
    fontSize: "clamp(27px, 3vw, 34px)"
    fontWeight: 660
    lineHeight: 1.15
    letterSpacing: "-0.035em"
  body:
    fontFamily: "Cascadia Code"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: 1.5
  label:
    fontFamily: "Cascadia Code"
    fontSize: "11px"
    fontWeight: 550
    lineHeight: 1.4
rounded:
  control: "4px"
  instrument-board: "5px"
  panel: "7px"
spacing:
  tight: "8px"
  standard: "16px"
  generous: "24px"
components:
  primary-action:
    backgroundColor: "{colors.brand-blue}"
    textColor: "{colors.panel}"
    rounded: "{rounded.control}"
    padding: "0 17px"
    height: "40px"
  secondary-action:
    backgroundColor: "#fffaf3"
    textColor: "#713d17"
    rounded: "{rounded.control}"
    padding: "0 12px"
    height: "34px"
  weight-input:
    backgroundColor: "{colors.instrument-raised}"
    textColor: "{colors.panel}"
    rounded: "{rounded.control}"
    padding: "0 5px"
    height: "27px"
  run-state-chip:
    backgroundColor: "#fff4df"
    textColor: "#7b5019"
    rounded: "3px"
    padding: "2px 7px"
    height: "21px"
  instrument-board:
    backgroundColor: "{colors.instrument-navy}"
    textColor: "{colors.panel}"
    rounded: "{rounded.instrument-board}"
    padding: "13px 15px 8px"
  configuration-panel:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.text-primary}"
    rounded: "{rounded.panel}"
    padding: "21px 23px 22px"
---

## Overview

**Creative North Star: "The Bench Measurement Workstation"**

BenchForge is a local measurement workstation for synthetic database runs. Its visual language borrows the scan-and-compare clarity of a logic analyzer: six named channels, one shared scale, aligned readouts, and quiet status cues. It stays a browser tool with ordinary form controls, not a fake hardware console.

**Key Characteristics:**
- Channel allocation is the first and clearest object on the page.
- Cool slate surfaces support one dark analyzer field and six distinct trace hues.
- Self-hosted Cascadia Code gives labels, prose, and values one technical voice.
- Run history and results reflect API data; empty and offline states stay explicit.

**The Bench Truth Rule.** Show configuration as configuration and recorded measurements as measurements.

## Colors

Use cool blue-gray neutrals for the page and panels. The strong blue identifies the main action and focused controls; the dark navy is reserved for the allocation board. Six trace hues distinguish operation channels, while amber, green, and red identify real run states. Always pair state color with readable text.

**The Trace Role Rule.** A trace hue names an operation channel; its length alone encodes the configured percentage. Never make color imply throughput or success.

## Typography

Use the locally bundled Cascadia Code face throughout the interface, with tabular figures for weights, seeds, IDs, and measurements. Keep the page title prominent; section titles and labels step down in size without introducing a second display face. The font and its license are bundled under `ui/wwwroot/fonts/`.

**The Readout Rule.** Keep numeric values aligned and units visible; reserve compact uppercase lettering for analyzer headers and channel IDs.

## Layout

At wide sizes, place the configuration beside the run ledger. The six-channel profile leads the form; adapter, scenario, run settings, and synthetic ID bounds follow in distinct sections. Below the stack breakpoint (760px), place the ledger under the form and preserve natural document scrolling. Keep dense result tables scrollable inside their own surface.

**The Six-Channel Rule.** Every operation uses the same horizontal 0–100 scale, with its label and editable weight on the same row.

## Elevation & Depth

Build depth with surface contrast and hairline borders instead of card shadows. The analyzer board is the single dark surface inside the light workbench. Use a restrained shadow only for the framework error overlay.

**The One Dark Field Rule.** Keep the allocation board as the only large dark instrument surface; do not add faux bezels, knobs, or simulated equipment.

## Shapes

Use crisp, modest corners for controls and slightly softer corners on the main panels. Keep borders visible and focus rings high contrast against both the cool work surface and dark channel board.

**The Plain Control Rule.** Native controls should look precise and remain recognizable as controls.

## Components

The allocation board contains six labeled lanes and a shared percentage scale. Number fields remain directly editable and show a visible distribution total. The primary action sits after all run settings; cancellation appears only for a real active worker. The ledger shows API-backed runs, with an explicit empty state before the first run and a detail panel only after selection.

**The Honest Ledger Rule.** Never seed fake runs, values, progress, or waveform output into the interface.

The Analysis route extends this same workstation with native baseline/candidate selectors, aligned measurement tables and two evidence columns. Baseline rows use the existing blue wash; candidate rows use the panel surface. Color identifies the row, never a winner. Tables scroll horizontally while the page scrolls naturally. On narrow screens, selection and evidence stack in document order. Shared navigation lives in `WorkspaceHeader.razor`; `app.css` remains the runtime token owner. `UX-CONTRACT.md` records selection, asynchronous recovery, comparison eligibility and native-control ownership.

## Do's and Don'ts

- **Do** identify synthetic workloads and explain the limits of the `noop` harness baseline.
- **Do** pair status color with text and preserve the offline, empty, active, error, cancelled, and completed states.
- **Do** preserve keyboard-operable native controls, visible focus, and reduced-motion behavior.
- **Don't** present `noop` timings as database performance.
- **Don't** add decorative signal traces, simulated hardware, or benchmark-progress motion.
- **Don't** hide meaning in color or shrink the allocation labels to fit.
