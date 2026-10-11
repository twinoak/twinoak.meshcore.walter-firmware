# jsmn

| | |
|---|---|
| Origin | <https://github.com/zserge/jsmn>, Serge Zaitsev |
| Version | tag `v1.1.0` (commit `fdcef3ebf886fa210d14956d3c068a653e76a24e`) |
| Licence | MIT (`LICENSE` beside it, verbatim) |
| SHA-256 of `jsmn.h` | `1ed6154dedf009212a08a397e9c4ed50a0ce31d5a8301bb294e137ae3188c13b` |
| SHA-256 of `LICENSE` | `4675b94a50d2afe811c52785463c854f1156056632cce17cc7133939eac8ed55` |
| Vendored here | `jsmn.h`, unchanged |

A JSON tokenizer in one header: it writes token boundaries into an array the caller provides and
allocates nothing, which is what `components/core/` needs (no heap), and it refuses the input when
the array is full, which is the bound `core/image/manifest.cpp` names (`kMaxJsonTokens`). It is
included from that one file only, with `JSMN_STATIC` (no symbols exported), `JSMN_STRICT` (only
standard JSON) and `JSMN_PARENT_LINKS` (each token knows its parent, so walking an object needs no
recursion). The directory is a system include path in both builds, so its warnings — if a
compiler has any — are its own, as doctest's are.

It does not unescape strings. The manifest's strings are names, versions, hex and ASCII words,
none of which needs an escape; a string the firmware reads that contains a backslash is refused
(`core/image/manifest.cpp`), and a string it ignores may contain anything.
