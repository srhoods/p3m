# p3m - Parallel POSIX Permission Manager
# Top-level Makefile: builds all tools into ./bin, man pages into ./obj/man

CC      ?= gcc
CFLAGS  ?= -O2
LDFLAGS ?=

# Flags the code needs regardless of how CFLAGS is overridden (rpmbuild and
# other packagers replace CFLAGS wholesale).
REQFLAGS := -std=gnu11 -Wall -Wextra -pthread -D_GNU_SOURCE

# The suite is versioned as a whole; VERSION is the single source of truth.
VERSION  := $(strip $(shell cat VERSION))
VERFLAGS := -DP3M_VERSION='"$(VERSION)"'
# Release date of VERSION from its CHANGELOG.md heading (used in man pages).
RELDATE  := $(shell sed -n 's/^\#\# \[$(VERSION)\] - \(.*\)$$/\1/p' CHANGELOG.md | head -1)

BIN     := bin
SRC     := src
OBJ     := obj
MANSRC  := man
MANOUT  := $(OBJ)/man
DIST    := dist
RPMTOP  := $(CURDIR)/rpmbuild

PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man
DOCDIR  ?= $(PREFIX)/share/doc/p3m
DESTDIR ?=

# Tools are added here as they are implemented
NAMES := p3m-ls p3m-ch p3m-rm p3m-du p3m-cp p3m-mv p3m-find p3m-diff p3m-stats
TOOLS := $(addprefix $(BIN)/,$(NAMES))

MAN1 := $(patsubst %,$(MANOUT)/%.1,$(NAMES))
MAN7 := $(MANOUT)/p3m.7

# Shared engine linked into every tool
CORE := $(OBJ)/p3mcore.o

.PHONY: all man clean install uninstall dist srpm rpm check-version

all: $(TOOLS) man

$(BIN) $(OBJ) $(MANOUT):
	mkdir -p $@

# Rebuild everything when VERSION changes (the version is baked into p3mcore).
$(OBJ)/.version: VERSION | $(OBJ)
	echo '$(VERSION)' > $@

$(OBJ)/p3mcore.o: $(SRC)/p3mcore.c $(SRC)/p3mcore.h $(OBJ)/.version | $(OBJ)
	$(CC) $(CFLAGS) $(REQFLAGS) $(VERFLAGS) -c -o $@ $<

$(BIN)/%: $(SRC)/%.c $(CORE) $(SRC)/p3mcore.h | $(BIN)
	$(CC) $(CFLAGS) $(REQFLAGS) $(VERFLAGS) -o $@ $< $(CORE) $(LDFLAGS) -pthread

# ---- man pages ---------------------------------------------------------

man: $(MAN1) $(MAN7)

$(MANOUT)/%: $(MANSRC)/%.in VERSION CHANGELOG.md | $(MANOUT)
	@test -n '$(RELDATE)' || { echo "CHANGELOG.md has no '## [$(VERSION)] - DATE' heading" >&2; exit 1; }
	sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@DATE@/$(RELDATE)/g' $< > $@

# ---- install -----------------------------------------------------------

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1 \
	           $(DESTDIR)$(MANDIR)/man7 $(DESTDIR)$(DOCDIR)
	install -m 0755 $(TOOLS) $(DESTDIR)$(BINDIR)/
	install -m 0644 $(MAN1) $(DESTDIR)$(MANDIR)/man1/
	install -m 0644 $(MAN7) $(DESTDIR)$(MANDIR)/man7/
	install -m 0644 README.md CHANGELOG.md $(DESTDIR)$(DOCDIR)/
	install -m 0644 docs/*.md $(DESTDIR)$(DOCDIR)/

uninstall:
	rm -f $(addprefix $(DESTDIR)$(BINDIR)/,$(NAMES))
	rm -f $(addprefix $(DESTDIR)$(MANDIR)/man1/,$(addsuffix .1,$(NAMES)))
	rm -f $(DESTDIR)$(MANDIR)/man7/p3m.7
	rm -rf $(DESTDIR)$(DOCDIR)

# ---- release / packaging ----------------------------------------------

# Fails unless VERSION, the CHANGELOG release heading and the spec agree.
check-version:
	@sh packaging/check-version.sh

# Source tarball of the working tree (tracked + untracked-but-not-ignored).
dist: check-version
	mkdir -p $(DIST)
	git ls-files -co --exclude-standard | \
	    tar -czf $(DIST)/p3m-$(VERSION).tar.gz \
	        --transform 's,^,p3m-$(VERSION)/,' -T -
	@echo "wrote $(DIST)/p3m-$(VERSION).tar.gz"

$(RPMTOP)/SPECS/p3m.spec: packaging/p3m.spec.in packaging/mk-spec.sh VERSION CHANGELOG.md
	mkdir -p $(RPMTOP)/SPECS
	sh packaging/mk-spec.sh > $@

RPMDEFS = --define '_topdir $(RPMTOP)'

srpm: dist $(RPMTOP)/SPECS/p3m.spec
	mkdir -p $(RPMTOP)/SOURCES
	cp $(DIST)/p3m-$(VERSION).tar.gz $(RPMTOP)/SOURCES/
	rpmbuild $(RPMDEFS) -bs $(RPMTOP)/SPECS/p3m.spec

rpm: dist $(RPMTOP)/SPECS/p3m.spec
	mkdir -p $(RPMTOP)/SOURCES
	cp $(DIST)/p3m-$(VERSION).tar.gz $(RPMTOP)/SOURCES/
	rpmbuild $(RPMDEFS) -bb $(RPMTOP)/SPECS/p3m.spec

clean:
	rm -rf $(BIN) $(OBJ) $(DIST) $(RPMTOP)
