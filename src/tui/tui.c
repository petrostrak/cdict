/* Wide-character curses is required for Cyrillic. The Makefile already
 * passes -D_XOPEN_SOURCE_EXTENDED; the guard keeps a standalone compile
 * working too, without triggering a redefinition warning. */
#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED
#endif

#include <curses.h>
#include <form.h>
#include <menu.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../dict/dict.h"
#include "table_data.h"
#include "tui.h"

/* ---- layout ------------------------------------------------------------ */
#define MENU_W 30  /* width of the left-hand match list */
#define BODY_Y 3   /* first row of the body (below the search line)   */
#define PAD_H 4000 /* max lines of definition text held in the pad     */

/* ---- tabs ---------------------------------------------------------------- */
enum
{
  TAB_DICT = 0,
  TAB_TABLE = 1
};

/* ---- state ------------------------------------------------------------- */
static DictDB *DB;
static int active_tab = TAB_DICT;

static FORM *form;
static FIELD *fields[2];
static WINDOW *fwin, *fsub;

static WINDOW *mwin, *msub;
static MENU *menu;
static ITEM **items;
static char **matches;
static size_t n_matches;

static WINDOW *def_win;
static WINDOW *pad;
static int pad_rows, pad_off;              /* content height, scroll offset */
static int def_iy, def_ix, def_ih, def_iw; /* def pane inner viewport  */
static int body_h;

static WINDOW *table_win;
static WINDOW *table_pad;
static int table_pad_rows, table_pad_off;
static int table_iy, table_ix, table_ih,
    table_iw; /* table pane inner viewport */

/* ---- helpers ----------------------------------------------------------- */

/* Current field text with trailing padding trimmed (caller frees). */
static char *current_query(void)
{
  form_driver(form, REQ_VALIDATION); /* flush field buffer */
  char *buf = field_buffer(fields[0], 0);
  if (!buf)
    return NULL;
  size_t len = strlen(buf);
  while (len > 0 && buf[len - 1] == ' ')
    len--;
  char *out = malloc(len + 1);
  if (!out)
    return NULL;
  memcpy(out, buf, len);
  out[len] = '\0';
  return out;
}

static void draw_def_frame(void)
{
  box(def_win, 0, 0);
  mvwprintw(def_win, 0, 2, " определение ");
  wnoutrefresh(def_win);
}

/* Blit the visible slice of the pad into the definition pane. */
static void draw_def(void)
{
  int maxoff = pad_rows - def_ih;
  if (maxoff < 0)
    maxoff = 0;
  if (pad_off > maxoff)
    pad_off = maxoff;
  if (pad_off < 0)
    pad_off = 0;
  draw_def_frame();
  pnoutrefresh(pad, pad_off, 0, def_iy, def_ix, def_iy + def_ih - 1,
               def_ix + def_iw - 1);
}

/* Tab bar on row 0. Таблица = "Table". */
static void draw_tabbar(void)
{
  wmove(stdscr, 0, 0);
  wclrtoeol(stdscr);
  mvwprintw(stdscr, 0, 0, "rusdict  ");
  wattrset(stdscr, active_tab == TAB_DICT ? A_REVERSE : A_NORMAL);
  waddstr(stdscr, " Словарь ");
  wattrset(stdscr, A_NORMAL);
  waddstr(stdscr, "  ");
  wattrset(stdscr, active_tab == TAB_TABLE ? A_REVERSE : A_NORMAL);
  waddstr(stdscr, " Таблица ");
  wattrset(stdscr, A_NORMAL);
  wnoutrefresh(stdscr);
}

static void draw_table_frame(void)
{
  box(table_win, 0, 0);
  mvwprintw(table_win, 0, 2, " таблица ");
  wnoutrefresh(table_win);
}

/* Total width of the grid: one border/divider column before each of the
 * TABLE_NCOLS columns plus one after the last. */
static int table_grid_width(void)
{
  int w = TABLE_NCOLS + 1;
  for (int c = 0; c < TABLE_NCOLS; c++)
    w += TABLE_COL_W[c];
  return w;
}

/* Render TABLE_ROWS into table_pad as a real ncurses box-drawn grid: ACS
 * line/junction glyphs for borders, cell text in between. Replaces what used
 * to be a literal "#"/"|" ASCII-art blob. */
static void build_table_grid(void)
{
  size_t nrows = TABLE_NROWS;

  for (size_t r = 0; r < nrows; r++)
  {
    const TableRow *row = &TABLE_ROWS[r];

    if (row->is_separator)
    {
      chtype left, mid, right;
      if (r == 0)
      {
        left = ACS_ULCORNER;
        mid = ACS_TTEE;
        right = ACS_URCORNER;
      }
      else if (r == nrows - 1)
      {
        left = ACS_LLCORNER;
        mid = ACS_BTEE;
        right = ACS_LRCORNER;
      }
      else
      {
        left = ACS_LTEE;
        mid = ACS_PLUS;
        right = ACS_RTEE;
      }

      waddch(table_pad, left);
      for (int c = 0; c < TABLE_NCOLS; c++)
      {
        for (int i = 0; i < TABLE_COL_W[c]; i++)
          waddch(table_pad, ACS_HLINE);
        waddch(table_pad, c == TABLE_NCOLS - 1 ? right : mid);
      }
    }
    else
    {
      waddch(table_pad, ACS_VLINE);
      for (int c = 0; c < TABLE_NCOLS; c++)
      {
        waddstr(table_pad, row->col[c]);
        waddch(table_pad, ACS_VLINE);
      }
    }
    waddch(table_pad, '\n');
  }
}

/* Blit the visible slice of the table pad into the table pane. */
static void draw_table(void)
{
  int maxoff = table_pad_rows - table_ih;
  if (maxoff < 0)
    maxoff = 0;
  if (table_pad_off > maxoff)
    table_pad_off = maxoff;
  if (table_pad_off < 0)
    table_pad_off = 0;
  draw_table_frame();
  pnoutrefresh(table_pad, table_pad_off, 0, table_iy, table_ix,
               table_iy + table_ih - 1, table_ix + table_iw - 1);
}

/* Switch the visible tab. Dictionary and table panes share the same body
 * rows, so whichever one isn't showing needs an explicit touchwin() to force
 * its stale content back over what the other tab last drew there. */
static void switch_tab(int tab)
{
  active_tab = tab;
  draw_tabbar();

  if (active_tab == TAB_TABLE)
  {
    curs_set(0);
    touchwin(table_win);
    draw_table();
  }
  else
  {
    curs_set(1);
    /* table_win spans the full body width, so switching away from it leaves
     * its content sitting in the 1-col gap between mwin and def_win - that
     * gap belongs to neither window, so nothing else repaints it. */
    mvwvline(stdscr, BODY_Y, MENU_W, ' ', body_h);
    wnoutrefresh(stdscr);
    touchwin(mwin);
    wnoutrefresh(mwin);
    touchwin(def_win);
    draw_def();
  }
}

static void show_message(const char *msg)
{
  werase(pad);
  wmove(pad, 0, 0);
  wprintw(pad, "%s", msg);
  pad_rows = getcury(pad) + 1;
  pad_off = 0;
  draw_def();
}

/* Look up one word and render its entry (or a miss message) into the pad.
 * waddstr/wprintw in the wide library render the UTF-8 bytes per the locale,
 * and the pad (width == pane width) wraps long lines at its right margin. */
static void render_lookup(const char *word)
{
  DictEntry e;
  int r = dict_lookup(DB, word, &e);

  werase(pad);
  wmove(pad, 0, 0);

  if (r == 1)
  {
    wattron(pad, A_BOLD);
    wprintw(pad, "%s", e.headword ? e.headword : word);
    wattroff(pad, A_BOLD);
    if (e.part_of_speech)
      wprintw(pad, "  (%s)", e.part_of_speech);
    wprintw(pad, "\n");
    if (e.ipa)
      wprintw(pad, "[%s]\n", e.ipa);
    if (e.grammar)
      wprintw(pad, "%s\n", e.grammar);
    wprintw(pad, "\n");

    for (size_t i = 0; i < e.n_senses; i++)
    {
      wattron(pad, A_BOLD);
      wprintw(pad, "%zu. %s\n", i + 1,
              e.senses[i].translation ? e.senses[i].translation : "");
      wattroff(pad, A_BOLD);
      if (e.senses[i].gloss)
        wprintw(pad, "   %s\n", e.senses[i].gloss);
      wprintw(pad, "\n");
    }

    if (e.n_examples)
    {
      wprintw(pad, "\nExamples:\n");
      for (size_t i = 0; i < e.n_examples; i++)
      {
        if (e.examples[i].ru)
          wprintw(pad, "  \u2022 %s\n", e.examples[i].ru);
        if (e.examples[i].en)
          wprintw(pad, "    %s\n", e.examples[i].en);
      }
    }
    dict_entry_free(&e);
  }
  else if (r == 0)
  {
    wprintw(pad, "No entry for \u201c%s\u201d.", word);
  }
  else
  {
    wprintw(pad, "Lookup error.");
  }

  pad_rows = getcury(pad) + 1;
  pad_off = 0;
  draw_def();
}

/* Tear down the current match menu and its backing strings. Order matters:
 * items hold pointers into `matches`, so free the items before the strings. */
static void clear_menu(void)
{
  if (menu)
  {
    unpost_menu(menu);
    free_menu(menu);
    menu = NULL;
  }
  if (msub)
  {
    delwin(msub);
    msub = NULL;
  }
  if (items)
  {
    for (size_t i = 0; i < n_matches; i++)
      free_item(items[i]);
    free(items);
    items = NULL;
  }
  if (matches)
  {
    dict_matches_free(matches, n_matches);
    matches = NULL;
  }
  n_matches = 0;
}

static void build_menu(void)
{
  werase(mwin);
  box(mwin, 0, 0);

  if (n_matches == 0)
  {
    mvwprintw(mwin, 0, 2, " matches ");
    wnoutrefresh(mwin);
    return;
  }

  items = calloc(n_matches + 1, sizeof *items);
  for (size_t i = 0; i < n_matches; i++)
    items[i] = new_item(matches[i], NULL); /* keeps the pointer */
  items[n_matches] = NULL;

  menu = new_menu(items);
  msub = derwin(mwin, body_h - 2, MENU_W - 2, 1, 1);
  set_menu_win(menu, mwin);
  set_menu_sub(menu, msub);
  set_menu_format(menu, body_h - 2, 1);
  set_menu_mark(menu, "> ");
  post_menu(menu);

  mvwprintw(mwin, 0, 2, " результаты: %zu ", n_matches);
  wnoutrefresh(mwin);
}

/* Re-run the prefix search for the current query and preview the top hit. */
static void refresh_matches(void)
{
  char *q = current_query();
  clear_menu();

  if (q && *q)
    dict_matches(DB, q, &matches, &n_matches);

  build_menu();

  if (n_matches > 0)
    render_lookup(matches[0]);
  else if (q && *q)
    render_lookup(q); /* no prefix hit; try lemma/miss */
  else
    show_message("Введите русское слово для поиска.\n\n"
                 "\u2191/\u2193 выбрать матч   посмотреть\n"
                 "PgUp/PgDn прокрутить   Esc выйти");
  free(q);
}

static void sync_def_to_menu(void)
{
  ITEM *it = current_item(menu);
  if (it)
    render_lookup(item_name(it));
}

static void place_cursor(void)
{
  pos_form_cursor(form); /* park the caret back in the search field */
  wnoutrefresh(fwin);
}

/* ---- lifecycle --------------------------------------------------------- */

static void ui_init(void)
{
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(1);

  /* chrome on stdscr */
  draw_tabbar();
  mvaddstr(1, 0, "Поиск: ");
  mvhline(2, 0, ACS_HLINE, COLS);
  mvaddstr(LINES - 1, 0,
           "Tab switch tab   \u2191/\u2193 match/scroll   Enter look up   "
           "PgUp/PgDn scroll   Esc/Ctrl-Q quit");
  wnoutrefresh(stdscr);

  /* search field / form (row 1, starting after the "Search: " label) */
  int fx = 8;
  int fw = COLS - fx - 1;
  fields[0] = new_field(1, fw, 0, 0, 0, 0);
  fields[1] = NULL;
  set_field_back(fields[0], A_UNDERLINE);
  field_opts_off(fields[0], O_AUTOSKIP);
  field_opts_off(fields[0], O_STATIC); /* allow the text to grow */
  set_max_field(fields[0], 200);

  form = new_form(fields);
  fwin = newwin(1, fw, 1, fx);
  fsub = derwin(fwin, 1, fw, 0, 0);
  keypad(fwin, TRUE);
  set_form_win(form, fwin);
  set_form_sub(form, fsub);
  post_form(form);
  wnoutrefresh(fwin);

  /* body: menu on the left, definition pane on the right */
  body_h = LINES - BODY_Y - 1;
  mwin = newwin(body_h, MENU_W, BODY_Y, 0);

  int def_w = COLS - MENU_W - 1;
  def_win = newwin(body_h, def_w, BODY_Y, MENU_W + 1);
  def_iy = BODY_Y + 1;
  def_ix = MENU_W + 2;
  def_ih = body_h - 2;
  def_iw = def_w - 2;

  pad = newpad(PAD_H, def_iw);

  /* second tab: static reference table, occupies the full body width */
  table_win = newwin(body_h, COLS, BODY_Y, 0);
  table_iy = BODY_Y + 1;
  table_ix = 1;
  table_ih = body_h - 2;
  table_iw = COLS - 2;

  /* +1 beyond the grid's own width: a row that exactly fills the pad's
   * width leaves ncurses's cursor in a pending-wrap state, so the '\n' that
   * follows produces a spurious blank row (see build_table_grid()). */
  table_pad = newpad(PAD_H, table_grid_width() + 1);
  build_table_grid();
  table_pad_rows = getcury(table_pad) + 1;
  table_pad_off = 0;
}

static void ui_teardown(void)
{
  clear_menu();
  if (form)
  {
    unpost_form(form);
    free_form(form);
    form = NULL;
  }
  if (fsub)
    delwin(fsub);
  if (fields[0])
    free_field(fields[0]);
  if (pad)
    delwin(pad);
  if (def_win)
    delwin(def_win);
  if (mwin)
    delwin(mwin);
  if (fwin)
    delwin(fwin);
  if (table_pad)
    delwin(table_pad);
  if (table_win)
    delwin(table_win);
  endwin();
}

/* ---- event loop -------------------------------------------------------- */

static void loop(void)
{
  refresh_matches();
  draw_def();
  place_cursor();
  doupdate();

  int running = 1;
  wint_t wch;
  while (running)
  {
    int r = wget_wch(fwin, &wch);
    if (r == ERR)
      continue;

    if (r == KEY_CODE_YES && active_tab == TAB_TABLE)
    { /* table tab only scrolls; the search field/menu keys don't apply */
      switch (wch)
      {
      case KEY_DOWN:
        table_pad_off++;
        draw_table();
        break;
      case KEY_UP:
        table_pad_off--;
        draw_table();
        break;
      case KEY_NPAGE:
        table_pad_off += table_ih - 1;
        draw_table();
        break;
      case KEY_PPAGE:
        table_pad_off -= table_ih - 1;
        draw_table();
        break;
      default:
        break;
      }
    }
    else if (r == KEY_CODE_YES)
    { /* function / arrow keys */
      switch (wch)
      {
      case KEY_DOWN:
        if (menu)
        {
          menu_driver(menu, REQ_DOWN_ITEM);
          wnoutrefresh(mwin);
          sync_def_to_menu();
        }
        break;
      case KEY_UP:
        if (menu)
        {
          menu_driver(menu, REQ_UP_ITEM);
          wnoutrefresh(mwin);
          sync_def_to_menu();
        }
        break;
      case KEY_NPAGE:
        pad_off += def_ih - 1;
        draw_def();
        break;
      case KEY_PPAGE:
        pad_off -= def_ih - 1;
        draw_def();
        break;
      case KEY_LEFT:
        form_driver(form, REQ_PREV_CHAR);
        break;
      case KEY_RIGHT:
        form_driver(form, REQ_NEXT_CHAR);
        break;
      case KEY_BACKSPACE:
        form_driver(form, REQ_DEL_PREV);
        refresh_matches();
        break;
      case KEY_DC:
        form_driver(form, REQ_DEL_CHAR);
        refresh_matches();
        break;
      case KEY_ENTER:
      {
        char *q = current_query();
        if (q && *q)
          render_lookup(q);
        free(q);
        break;
      }
      default:
        break;
      }
    }
    else if (wch == L'\t')
    { /* r == OK */
      switch_tab(active_tab == TAB_DICT ? TAB_TABLE : TAB_DICT);
    }
    else if (wch == 27 /*Esc*/ || wch == 17 /*Ctrl-Q*/)
    {
      running = 0;
    }
    else if (active_tab == TAB_TABLE)
    {
      /* table tab ignores other char input; nothing to type here */
    }
    else
    { /* r == OK, dictionary tab: wch is a real char */
      if (wch == L'\n' || wch == L'\r')
      {
        char *q = current_query();
        if (q && *q)
          render_lookup(q);
        free(q);
      }
      else if (wch == 127 || wch == 8)
      { /* Backspace variants */
        form_driver(form, REQ_DEL_PREV);
        refresh_matches();
      }
      else
      {
        form_driver_w(form, OK, wch); /* insert typed char */
        refresh_matches();
      }
    }

    if (active_tab == TAB_DICT)
      place_cursor();
    doupdate();
  }
}

int tui_run(DictDB *db)
{
  DB = db;
  ui_init();
  loop();
  ui_teardown();
  return 0;
}
