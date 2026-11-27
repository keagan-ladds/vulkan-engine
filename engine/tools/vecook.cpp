// vecook - the asset cooker and release packager as a command-line tool.
// Everything is in the engine (ve/app/CookTool.hpp), so a game can offer the
// same as `<game> --cook`; see engine/src/ve/app/CookTool.cpp for the options.

#include <ve/app/CookTool.hpp>

int main(int argc, char** argv)
{
    return ve::cookToolMain(argc, argv);
}
