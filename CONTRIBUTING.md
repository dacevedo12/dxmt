
# Contributing to DXMT

## Commit Messages

We use semantic commit messages.

Format: `<type>(<scope>): <subject>`

Valid types:
- feat: A newly added feature
- fix: Just a fix
- refactor: Any changes to the implementation that do not introduce new functionality
- chore: Any changes to the code that do not change the implementation (e.g. formatting, adding comments)
- docs: For documentation only (typically .md files)
- build: For build script only
- ci: For CI script only

The scope can be any sub-directory in `src/`. If a patch involves multiple scopes, separate them by comma `, `. Scopes can be omitted if there are more than 4, but please try your best to constrain your changes into one single scope. For docs/build/ci, no need to add scope.

The subject is a setence describing the change. Do not capitalize the first letter.

And most importantly, please **sign off** your commit with your legal name.

## AI Policy

This fork accepts contributions whoever or whatever wrote them. A patch is judged
by the patch: whether it is correct, whether it fits the surrounding code, and
whether you can answer questions about it. That bar does not move because a
machine helped, and it is not lowered because a human typed every character.

What follows from that:

- CI must be green. A red pipeline is not a review comment to argue with.
- Small changes get reviewed quickly; large ones may be sent back to be split.
  A patch that touches one thing can be checked. A patch that touches twenty
  cannot, and reviewing it well takes longer than writing it did.
- Machine-written code tends to be voluminous, to restate what the code already
  says in comments, and to add abstraction nothing asked for. Those are the
  things that get a patch rejected here, not its authorship.
- Own what you send. If you cannot explain why a line is there, it is not ready.

## Running the tests

The host suite reads the tree and needs neither Wine nor a GPU, so it runs
everywhere and is what CI gates on:

```sh
meson test -C build --suite host
```

It covers the shader corpus lint, the cross-consistency of the D3D9 format
tables, zero-initialisation of the Metal descriptor structs, and `src/d3d9`
against its own `.clang-format`. That last one is pinned to clang-format 21.x,
which the build toolchain carries; otherwise `pip install "clang-format==21.1.*"`.
Releases format the same file differently, so an unpinned one reports errors
that are not there.

The two device suites drive real Direct3D 9 through Wine, so they need a machine
with a Metal GPU **and a logged-in graphical session**. Direct3D 9 creates a
device against a window, so neither suite can run headless. GitHub's and GitLab's
hosted macOS runners are both headless and offer no Metal, so these are run
locally rather than in CI.

Configure with Wine as the executable wrapper, then point the prefix at the layer
you just built:

```sh
printf "[binaries]\nexe_wrapper = '/path/to/wine/bin/wine'\n" > wine-wrapper.txt
meson setup build --cross-file build-win64.txt --cross-file wine-wrapper.txt \
  -Denable_tests=true -Dwine_install_path=/path/to/wine

mkdir -p overlay/x86_64-windows overlay/x86_64-unix
cp build/src/d3d9/d3d9.dll build/src/winemetal/winemetal.dll overlay/x86_64-windows/
cp build/src/winemetal/unix/winemetal.so overlay/x86_64-unix/

export WINEDLLPATH="$PWD/overlay" WINEDLLOVERRIDES="d3d9,winemetal=b"
meson test -C build --suite shader-oracle   # numeric corpus, ~10s
meson test -C build --suite conformance     # Wine's own d3d9 modules, minutes
```

Both compare against a checked-in baseline rather than an exit status, so a
corpus that is partially red still reports a regression. The suites assert that
they measured this layer: without the overlay above they would measure wined3d
and report on that instead.

To re-run one module, pass a substring of its name:

```sh
wine build/tests/dxmt-d3d9-conformance.exe visual
```

Wine's modules come from the `external/wine` submodule, so a fresh clone needs
`git submodule update --init --recursive` before they build.