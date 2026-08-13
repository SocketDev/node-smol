# Build performance experiment journal

This journal owns the chronological record for node-smol build-time experiments:
anything that changes how long a build takes rather than what it produces. Each
entry states the hypothesis, the measured phase, the runtime and command, the
result, the decision, and the next safe step.
[node-smol-build-flags](../agents.md/repo/node-smol-build-flags.md) describes the
flags and linker a build currently uses, and
[build-caching](../agents.md/repo/build-caching.md) describes the cache layers an
experiment has to account for before it can attribute a change.

A build-time result is only comparable against a stated cache state. A warm
ccache or a restored checkpoint moves a phase by more than most experiments do,
so an entry that does not name its cache state cannot be read as a delta.

## How to read an entry

- **Hypothesis** states the one behavior the experiment expects to improve.
- **Phase and command** identify the measured work and how to repeat it.
- **Result** records the observed change. An entry that measured nothing says so
  and names the number it would have needed.
- **Decision** says whether the change was retained or rejected.
- **Constraint found** records a fact about the toolchain that bounds the
  experiment, whether or not timings were taken.
- **Follow-up** names the next experiment without treating it as completed work.

## 1. Replace gold with mold on the Linux release link

**Hypothesis.** mold links several times faster than gold on ELF targets, so
swapping the Linux release linker would cut the link phase of a release build.

**Phase and command.** The final link edge of the release build, produced by
`ninja -C out/Release` in
`packages/node-smol-builder/scripts/binary-released/shared/build-released.mts`.
Repeat a full build with
`pnpm --filter local-node-smol-builder run build --prod`.

**Result.** No timing was taken. The experiment was rejected on a constraint that
makes the timing moot, described below. The number it would have needed is the
final link edge's share of total build time, which `out/Release/.ninja_log`
records as per-edge start and end times.

**Constraint found.** Two facts, either of which sinks the swap on its own.

- The Linux release link uses gold to get `-Wl,--icf=safe`, set by
  `001-common-gypi-lto.patch`. That folds byte-identical functions, and `safe` is
  the mode that preserves function-pointer identity, which V8 relies on because
  it compares code pointers. mold's manual states its `--icf=safe` needs a
  compiler emitting `.llvm_addrsig`, which Clang does and GCC does not, and
  `packages/node-smol-builder/scripts/setup-build-toolchain.mts` installs `gcc`
  for Linux. Swapping the linker therefore trades binary size, the metric this
  package exists to minimize, for link speed.
- The release link is an LTO link (`-flto=4 -ffat-lto-objects` for GCC). On an
  LTO link most of the wall time is the compiler's LTO backend generating code
  rather than the linker resolving symbols. mold makes symbol resolution fast; it
  does not make LTO codegen fast, so the published speedup, measured on ordinary
  links, largely does not transfer.

There is also no second build to speed up. The pipeline builds
`out/Release` only, with no `out/Debug` path, so the debug-edit-rebuild cycle
mold is designed for is not part of this build.

**Decision.** Rejected on 2026-08-12. The mold pin added to the fleet
`external-tools.json` was removed in the same pass, since an unpinned tool costs
nothing while a pinned one carries version upkeep.

**Follow-up.** Two, in order of cost.

- Collect the link edge's share of total build time from
  `out/Release/.ninja_log` on a Linux release build. If linking is a few percent,
  this entry closes permanently and no linker experiment is worth running.
- Only if that share is material: evaluate moving the Linux release build to
  Clang. With `.llvm_addrsig` available, mold folds properly, so it becomes speed
  and size with nothing traded, and the patch already takes the ThinLTO path for
  Clang. Changing the compiler changes the optimizer, so that experiment owes
  binary-size and startup numbers against the current GCC baseline before it
  could ship.
