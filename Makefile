# ---------------------------------------------------------------------------
# rusdict — Russian TUI dictionary
# Links: ncursesw + formw + menuw (wide char, needed for Cyrillic) and zlib.
# ---------------------------------------------------------------------------

CC      ?= cc
CSTD    := -std=c11
WARN    := -Wall -Wextra
OPT     := -O2 -g

# Wide-char curses must be requested before the headers are included.
FEATURE := -D_XOPEN_SOURCE_EXTENDED

# Headers now live in per-module dirs, so the compiler needs to be told where
# to find "dict.h" and "tui.h" when main.c / tui.c include them.
INCLUDES := -Isrc/dict -Isrc/tui

UNAME_S := $(shell uname -s)

# On macOS, Homebrew's ncurses is keg-only (macOS ships its own old copy),
# so pkg-config can't see it until we point it at the keg. On Apple Silicon
# that keg lives under /opt/homebrew, which isn't on the default search path.
ifeq ($(UNAME_S),Darwin)
  BREW_NCURSES := $(shell brew --prefix ncurses 2>/dev/null)
  PKGCONF := PKG_CONFIG_PATH="$(BREW_NCURSES)/lib/pkgconfig:$$PKG_CONFIG_PATH" pkg-config
else
  PKGCONF := pkg-config
endif

# Darwin 8.x == Mac OS X 10.4 Tiger (PowerPC G3/G4/G5, e.g. an iBook G4).
# No Homebrew, no pkg-config, no C11 in its gcc 4.0/3.3, and the system
# ncurses has no wide-char (widec) support, so form_driver_w()/wget_wch()
# (needed for Cyrillic) won't link against it. `make deps-tiger` builds a
# private widec ncurses+form+menu from source; once it exists we point
# straight at it below and skip pkg-config entirely.
UNAME_R        := $(shell uname -r)
IS_TIGER       := $(shell echo "$(UNAME_S)/$(UNAME_R)" | grep -q '^Darwin/8\.' && echo yes)
NCURSES_PREFIX ?= /usr/local/ncursesw
ifeq ($(IS_TIGER),yes)
  CSTD := -std=gnu99
endif

# --- ncurses (wide) via pkg-config, with a hand-rolled fallback ------------
NC_PKGS := ncursesw formw menuw
# Nested ifeq/else (not the "else ifeq" shorthand) -- Tiger's bundled GNU
# Make 3.79 predates 3.81, which is when that chained-conditional syntax
# was added, and rejects it with "Extraneous text after `else' directive".
ifeq ($(shell $(PKGCONF) --exists $(NC_PKGS) 2>/dev/null && echo yes),yes)
  NC_CFLAGS := $(shell $(PKGCONF) --cflags $(NC_PKGS))
  NC_LIBS   := $(shell $(PKGCONF) --libs   $(NC_PKGS))
else
  ifeq ($(IS_TIGER),yes)
    NC_CFLAGS := -I$(NCURSES_PREFIX)/include/ncursesw
    NC_LIBS   := -L$(NCURSES_PREFIX)/lib -lformw -lmenuw -lncursesw
  else
    ifeq ($(UNAME_S),Darwin)
      # Last resort: Apple's system ncurses (older, but wide-capable). Prefer
      # installing Homebrew's ncurses so the pkg-config path above is used.
      NC_CFLAGS :=
      NC_LIBS   := -lform -lmenu -lncurses
    else
      NC_CFLAGS := -I/usr/include/ncursesw
      NC_LIBS   := -lformw -lmenuw -lncursesw
    endif
  endif
endif

# --- zlib (for the dictzip .dict.dz bodies) --------------------------------
# macOS provides libz in the SDK, so the -lz fallback is all that's needed.
ifeq ($(shell $(PKGCONF) --exists zlib 2>/dev/null && echo yes),yes)
  Z_CFLAGS := $(shell $(PKGCONF) --cflags zlib)
  Z_LIBS   := $(shell $(PKGCONF) --libs   zlib)
else
  Z_LIBS   := -lz
endif

CFLAGS  := $(CSTD) $(WARN) $(OPT) $(FEATURE) $(INCLUDES) $(NC_CFLAGS) $(Z_CFLAGS)
LDLIBS  := $(NC_LIBS) $(Z_LIBS)

# --- sources (src/ tree) ---------------------------------------------------
SRCS    := main.c src/tui/tui.c src/dict/dict.c
OBJS    := $(SRCS:.c=.o)
HEADERS := src/dict/dict.h src/tui/tui.h src/tui/table_data.h
BIN     := rusdict

.PHONY: all clean smoke deps-macos deps-tiger

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Objects are built next to their sources (src/main.o, src/tui/tui.o, ...).
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Any header change rebuilds everything — coarse but always correct.
$(OBJS): $(HEADERS)

clean:
	$(RM) $(OBJS) $(BIN) smoke_test smoke_test.c
	$(RM) -r smoke_test.dSYM *.dSYM

# Prove the libraries are installed and actually link, before main() exists.
# Builds a throwaway program referencing all four libs, runs it, cleans up.
smoke:
	@printf '%s\n' \
	  '#include <stdio.h>' \
	  '#include <curses.h>' \
	  '#include <form.h>' \
	  '#include <menu.h>' \
	  '#include <zlib.h>' \
	  'int main(void){' \
	  '  printf("ncurses: %s\n", curses_version());' \
	  '  printf("zlib:    %s\n", zlibVersion());' \
	  '  (void)new_form; (void)new_menu;   /* force form/menu to link */' \
	  '  return 0;' \
	  '}' > smoke_test.c
	$(CC) $(CFLAGS) smoke_test.c -o smoke_test $(LDLIBS)
	@./smoke_test
	@$(RM) smoke_test.c smoke_test
	@$(RM) -r smoke_test.dSYM

# --- dependency installers -------------------------------------------------
deps-macos:
	brew install ncurses pkg-config

# For Mac OS X 10.4 Tiger on PowerPC (Darwin 8.x). There's no package
# manager on this platform, so this builds ncursesw+formw+menuw from
# source into NCURSES_PREFIX (override on the command line to change it).
# Run this once, then plain `make` picks it up automatically via IS_TIGER.
#
# Tiger's curl/OpenSSL can't do TLS 1.2+, so this only tries a plain-HTTP
# fetch. If that fails (offline, mirror moved, etc.), download the tarball
# on a modern machine and drop it next to this Makefile before re-running.
NCURSES_VERSION := 5.9
NCURSES_TARBALL := ncurses-$(NCURSES_VERSION).tar.gz
NCURSES_URL     := http://ftp.gnu.org/gnu/ncurses/$(NCURSES_TARBALL)

deps-tiger:
	@if [ -d "$(NCURSES_PREFIX)/lib" ]; then \
	  echo "$(NCURSES_PREFIX) already exists, skipping. rm -rf it first to rebuild."; \
	  exit 0; \
	fi
	@[ -f $(NCURSES_TARBALL) ] || \
	  curl -fL -o $(NCURSES_TARBALL) $(NCURSES_URL) || \
	  { echo "error: could not fetch $(NCURSES_TARBALL)."; \
	    echo "Download $(NCURSES_URL) on another machine and copy it"; \
	    echo "next to this Makefile, then re-run 'make deps-tiger'."; \
	    exit 1; }
	rm -rf ncurses-$(NCURSES_VERSION)
	tar xzf $(NCURSES_TARBALL)
	cd ncurses-$(NCURSES_VERSION) && \
	  CC=gcc CFLAGS="-O2" ./configure \
	    --prefix=$(NCURSES_PREFIX) \
	    --with-shared --enable-widec --enable-sigwinch \
	    --without-ada --without-cxx --without-cxx-binding && \
	  $(MAKE) && \
	  sudo make install
	rm -rf ncurses-$(NCURSES_VERSION)
	@echo ""
	@echo "Done. $(NCURSES_PREFIX) is now in place -- 'make' will pick it up"
	@echo "automatically on Tiger (via IS_TIGER)."
