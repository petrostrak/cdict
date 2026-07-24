#ifndef TUI_H
#define TUI_H

#include "../dict/dict.h"

/* Run the interactive terminal UI against an already-open dictionary.
 * Blocks until the user quits. Returns 0 on a normal exit. */
int tui_run(DictDB *db);

#endif /* TUI_H */
