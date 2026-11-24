# Developing DCS Control

How work is organised in this repository: branches, worktrees, who may build where, the quiet lock, and what a
release is built from. Building and testing themselves are in the [README](../README.md#build-and-test).

## Branches

- `main` is what is published. Every release is a commit on it, tagged `v<version>-preview`.
- One branch holds the version in work: `release/<version>` (for example `release/0.6.8`). It is merged into `main`
  by fast-forward when the release is published.
- Work goes on short topic branches, `feat/...`, `fix/...` or `cleanup/...`, cut from the release branch, merged into
  it and deleted.
- Never port commits by hand between two lines. Merge or cherry-pick, so git knows the commits are the same; a line
  that was copied by hand can no longer be fast-forwarded and its merge conflicts in every file both sides touched.

## Maintenance lines

A maintenance line (a 0.7.0.x next to 0.7.1, say) gets its own permanent worktree in its own folder:

```powershell
git worktree add -b release/0.6.6.1 ..\DcsControl-0.6.6 v0.6.6-preview
```

`external\`, `artifacts\native\` and the build stamps then belong to that line. Switching branches in one folder
instead leaves `external\` patched for the other line, and the next build starts with the 20-minute OFXR rebuild.

## Working in parallel (people, agents, scripts)

- Build only in your own worktree. Never build, test or run git in someone else's checkout.
- Let the build scripts clone `external\*` in a new worktree. If you copy it to save time, copy the sources only:
  never together with `obj`, `bin` or the CMake build folders. Those hold absolute paths to the folder they were
  made in, and MSBuild's Clean follows them: a Clean in the copy once deleted the build outputs of the checkout it
  was copied from.
- No git command and no build command inside another checkout's `external\`. The build scripts reset and patch those
  trees; a second hand in them changes what the first one builds.
- Inputs that are not tracked (`.cache`, `external`, `artifacts\native`, a built release) can be lent to a worktree
  with directory junctions. Remove the junction itself afterwards, never its target, and never delete recursively
  through one.
- Nothing a test run or a tool writes belongs in the tree: rig output goes to `artifacts\rig`, runtime-probe output
  to `artifacts\runtime-probe`. `artifacts\` and `external\` are ignored by git.

## The quiet lock

Pacing tests and fps measurements need a quiet machine. Instead of fixed hours there is one lock file,
`artifacts\.lane\quiet.lock`:

- You need a quiet machine (the pacing suite, a measurement flight): create the file with one line saying who, what,
  started HH:MM and the expected minutes. Delete it the moment you are done, also after a failure.
- You are about to start a build, a GPU bench or a test suite: look first. While the file exists, wait (check every
  20 s), then start. Reading, writing code and analysing logs never wait.
- Before taking the lock, check that no build or bench is running (`cl.exe`, `link.exe`, MSBuild, `ninja`, `ctest`,
  the bench executables). If one is, wait for it to end, then take the lock.
- A lock older than its expected minutes plus 15 is stale: say so in your report and remove it.
- A flight also needs DCS and the headset: one flight at a time on a machine, and no test suite while `DCS.exe` runs.

A machine has one lock, whatever the number of checkouts: in a second checkout or worktree set `DCSC_QUIET_LOCK` to
the first one's file. The rig's scripts read it (`tools\rig`).

## Build speed

- Native builds run as many compile jobs as the PC has logical processors less two (`scripts/build-jobs.ps1`);
  `DCSC_BUILD_JOBS=<n>` sets the number. Start builds that share the PC with someone at BelowNormal priority (lower
  your shell first: `(Get-Process -Id $PID).PriorityClass = 'BelowNormal'`): compilers and tests inherit it and yield
  to the user's programs however many jobs there are. The job count never changes what is built.
- Quad Views is reset and patched again only when its patches or its own sources (`native/quadviews`) changed, or the
  checkout is no longer exactly as the last apply left it (a record in its `.git` folder). It is built again only
  when those inputs, the folder, the compiler or the MSBuild options changed, or its outputs are not the ones the last
  build left (a record in `obj`); that build is always a full one (`obj` and `bin\x64\Release` deleted first). An
  incremental MSBuild run would relink every time (the project's pre-build step rewrites `version.h`) and a relink
  does not give the same bytes as a full link.
- Every native test suite is skipped when nothing it reads changed since it last passed (`scripts/test-cache.ps1`:
  the built programs and libraries, the tests' sources, the scripts, the environment the tests read, DCS's files).
  The DcsQvCull tests read the Quad Views DLL, so a new Quad Views build runs them again.
  OFXR also keeps the outcome of its last run: with `-LocalOnly`, a run that failed (the known environmental
  `xrfg_layer_two_layers_deep_hold_arming_single`) is reused and stamped local-only again; without it the tests run.
  `DCSC_FORCE_TESTS=1` runs every suite.
- `build-release.ps1` builds and tests DcsQvCull and pupil shift side by side, in two processes of their own, while
  it publishes the app. Their output goes to `artifacts\native\dcsqvcull-release.log` and `pupil-shift-release.log`
  and is printed when both have ended. With `-Sign` they always run their tests (`DCSC_FORCE_TESTS=1`); without it a
  suite whose inputs are unchanged is skipped as above.
- Run the component builds (Cheeky, OFXR, Quad Views) one after another, not side by side: OFXR's second test phase
  needs the machine to itself, Cheeky's build resets `external\cheeky`, whose OpenXR headers the pupil shift build
  reads, and the Cheeky and Quad Views tests both use the GPU.
- ctest parallelism stays as it was: DcsQvCull and pupil shift are one long test each (about 4 minutes, the pupil shift
  harness in real time), OFXR's labelled `alone` / `pacing` tests run one at a time, its other tests and Cheeky's two
  at a time (GPU and WARP tests, never checked with more neighbours).

## Releases

- A release to publish is built with `scripts/build-release.ps1 -Sign` from a clean tree, at the commit that is then
  tagged. The script refuses a tree with uncommitted or untracked files (`-AllowDirty` overrides it, for a build that
  is not published) and writes the commit into `release-manifest.json`.
- If the build adds lines to `src/DcsVr.Core/KnownComponents.txt`, commit them before the sources ZIP is made and
  tag that commit.
- The components (Quad Views, OFXR, Cheeky) are not rebuilt by a release: it takes them as built and refuses one
  that is older than its inputs (`scripts/build-stamp.ps1` lists the inputs of each). An edit to one of those files,
  a comment included, means a rebuild of that component and its tests.
- Before a release is published, the release flight matrix must pass: `tools/release-matrix/run-matrix.ps1` with the
  package's CLI and `-Baseline` set to the previous release's `results.json` (see [its README](../tools/release-matrix/README.md)).
- Before a commit: `pwsh -File scripts/check-personal-data.ps1` (the pre-commit hook runs it). No user path, e-mail
  address or key belongs in the repository, in a release or in the sources ZIP. The repository holds only generic
  patterns; the values that identify you (user name, PC name, device serials) go in a local file outside it,
  `%USERPROFILE%\.dcs-control\personal-data.txt` (or `DCSC_PERSONAL_PATTERNS`), as `pattern <name> <regex>` lines.
  Create it on every PC you commit from: without it the checker warns.

## Parked features

- **Three views** (DcsQvCull `[Views] ThreeViews=1`, `native/quadviews/dcsvr_threeviews.h`, Quad Views patch 0020):
  parked after the headset test on 2026-11-21 (too many artefacts in the right eye). The code stays in place and off;
  the app has no control for it and never writes the key, so only a hand edit of
  `DcsQvCull.ini` (lost at the next apply) or of the Developer-mode ini turns it on.
