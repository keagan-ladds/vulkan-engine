#pragma once

// =============================================================================
// CookTool - the `vecook` command line as a function: cooks a game's assets
// into its cook cache, checks them, or packages a release (the options are
// listed at the top of CookTool.cpp; `vecook --help`).
//
// A game can offer it as `<game> --cook [options]`: run from the game's own
// executable it knows the game's registered types, which prefab icons of
// game-defined objects need (vecook alone skips those):
//
//   int main(int argc, char** argv) {
//       if (argc > 1 && std::string(argv[1]) == "--cook") {
//           argv[1] = argv[0];                   // (drop "--cook": the options follow)
//           return ve::cookToolMain(argc - 1, argv + 1);
//       }
//       ...
// =============================================================================

namespace ve {

/// Runs the cooker / packager with command-line arguments (argv[0] is the
/// program name). Creates a headless Engine only if a cook needs the renderer.
/// @return the process exit code (0 = everything cooked / current).
int cookToolMain(int argc, char** argv);

} // namespace ve
