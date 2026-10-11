# Tests

The regression tests for the FEX patches live with the code, in
[StonkGuy/FEX `eac-tests/`](https://github.com/StonkGuy/FEX/tree/eac-2610/eac-tests) (branch `eac-2610`, tag
`eac-2610.2`): one freestanding x86-64 program per behaviour, each checked against a real x86-64 Linux kernel, and
`run-all.sh` to build and run them all. [docs/patches.md](../docs/patches.md) names the test for each patch, and
[docs/tools.md](../docs/tools.md) has the native reference results.

`scripts/build-fex.sh` clones that tag into `work/FEX`, so after a build the tests are in `work/FEX/eac-tests/`:

```
work/FEX/eac-tests/run-all.sh                                      # where FEX runs x86-64 binaries
(cd work/FEX/eac-tests/ptrace-inject && ./build.sh clang)
scripts/vm/run-test-in-vm.sh work/FEX/eac-tests/ptrace-inject/ptrace_inject_test   # in a throw-away VM
```

What stays here tests this repository's own scripts:

| test | checks |
|---|---|
| [`set-launch-options/test.sh`](set-launch-options/test.sh) | `scripts/set-launch-options.sh` changes only VRChat's launch options in `localconfig.vdf` |
