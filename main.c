#include <locale.h>
#include <stdio.h>

#include "src/dict/dict.h"
#include "src/tui/tui.h"

int main(int argc, char **argv)
{
  /* MUST come before any ncurses call so the wide-character routines
   * interpret the terminal's UTF-8 correctly — this is what makes
   * Cyrillic render instead of turning into mojibake. */
  setlocale(LC_ALL, "");

  if (argc < 3)
  {
    fprintf(stderr,
            "usage: %s <reader-dict.index> <reader-dict.dict[.dz]> "
            "[examples.index examples.dict[.dz]]\n",
            argv[0]);
    return 2;
  }

  const char *ex_index = (argc >= 5) ? argv[3] : NULL;
  const char *ex_data = (argc >= 5) ? argv[4] : NULL;

  DictDB *db = dict_open(argv[1], argv[2], ex_index, ex_data);
  if (!db)
  {
    fprintf(stderr, "error: could not open dictionary files\n");
    return 1;
  }

  /* When you wire up the Snowball stemmer, install it here:
   *     dict_set_lemmatizer(db, ru_lemma);
   * dict_lookup() will then fall back to the lemma on a miss. */

  int rc = tui_run(db);
  dict_close(db);
  return rc;
}
