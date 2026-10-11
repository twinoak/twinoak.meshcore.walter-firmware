# doctest — vendored, pinned

| | |
|---|---|
| What | [doctest](https://github.com/doctest/doctest), the single-header C++ test framework |
| Version | **v2.5.3** (released 2026-07-06; tag `v2.5.3`, commit `2d0a9359a60c51affe2a9bebb1be1dca47868151`) |
| File | `doctest.h`, copied unedited from `doctest/doctest.h` at that tag |
| SHA-256 | `cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421` (362 930 bytes) |
| Licence | MIT (the file's own header) |

Why this one (task p0-01): one header, so the tests need nothing from the
network at test time and nothing the container does not already have; MIT;
compiles in seconds under `-Wall -Wextra -Werror` with the sanitizers on; the
assertions decompose expressions (`CHECK(a == b)` prints both sides), which is
what a failing fixture test should show. Catch2 v3 is many files, and
GoogleTest is fetched at configure time by habit — both were rejected for
that.

A bump re-copies the file, re-records the hash here, and says so in the
task's report. Nothing here is edited.
