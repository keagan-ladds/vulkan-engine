# vulkan-engine

Start with `docs/04-agent-handoff.md` (build, run, the work loop, the checks)
and `docs/03-conventions.md` (code conventions and recipes).

Every change:
- Builds warning-free (GCC and Clang) and passes `python3 tools/check_layering.py`.
- Documents every public declaration it adds or changes in a header: `///`
  above it or `///<` after a member, saying what it does in the caller's
  terms, with units, ranges, special values and `@param` for any parameter
  whose name isn't self-explanatory. `python3 tools/check_docs.py --changed`
  must report 0 undocumented declarations.
- Is exercised by the sandbox (ideally a `SAVANNA_AUTOTEST` check) and keeps
  the docs (`docs/*.md`, `CHANGELOG.md`) current.
