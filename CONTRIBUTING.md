# Contributing

Tab5 PC accepts one kind of pull request: a fix for a bug that can be reproduced on a Tab5. Other pull requests (new
features, other boards or hardware, build and tool changes, documentation, clean-ups) are closed without review.

## A bug fix

- Reproduce the bug on a Tab5 running the latest release or `main`. A bug found only by reading the code, in a host
  build or in another emulator is not enough.
- Fix it with the smallest change that does it, and check on the Tab5 that the bug is gone and the PC still starts.
- One bug per pull request. No tests, test tools, README or documentation changes, refactoring or reformatting.
- A change to tiny386 goes in as a new numbered patch in `vendor/patches/`, like the ones already there.

In the pull request, write:

- the firmware version or commit you tested;
- how to reproduce the bug on a Tab5: the machine (its `.ini`), what you ran and how, what you saw and what you
  expected;
- what the same steps show with your fix.

We merge a fix only after reproducing the bug from your steps and testing the fix on a Tab5.

## Reporting a bug

A bug without a fix is welcome as an issue, with the same version and steps.

## License

Your contribution is under the license of what it changes: the MIT License (`LICENSE`) for this repository's own
files, and the vendor's license for a patch in `vendor/patches/` (see `THIRD_PARTY_NOTICES.md`).
