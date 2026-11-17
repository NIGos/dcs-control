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

A maintenance line (a 0.6.6.x next to 0.6.7, say) gets its own permanent worktree in its own folder:

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

## Releases

- A release to publish is built with `scripts/build-release.ps1 -Sign` from a clean tree, at the commit that is then
  tagged. The script refuses a tree with uncommitted or untracked files (`-AllowDirty` overrides it, for a build that
  is not published) and writes the commit into `release-manifest.json`.
- If the build adds lines to `src/DcsVr.Core/KnownComponents.txt`, commit them before the sources ZIP is made and
  tag that commit.
- The components (Quad Views, OFXR, Cheeky) are not rebuilt by a release: it takes them as built and refuses one
  that is older than its inputs (`scripts/build-stamp.ps1` lists the inputs of each). An edit to one of those files,
  a comment included, means a rebuild of that component and its tests.
- Before a commit: `pwsh -File scripts/check-personal-data.ps1`. No user path, e-mail address or key belongs in the
  repository, in a release or in the sources ZIP.
