#pragma once

// =============================================================================
// EditorState - what other engine modules may ask about the editor, without
// including Editor.hpp (which pulls in the terrain, reflection and gizmo).
//
//   if (ve::editorPaused(engine)) return; // the game is paused in Edit mode
// =============================================================================

namespace ve {

class Engine;

// The editor is in Edit mode (game paused). False without an editor.
bool editorPaused(Engine& engine);

} // namespace ve
