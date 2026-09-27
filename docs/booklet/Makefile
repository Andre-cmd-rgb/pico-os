# The pocket booklets, printed to PDF with Chromium: guide.pdf (how to use
# the machine) and pico.pdf (the language). `make check` says whether every
# page still fits after an edit.

CHROMIUM ?= chromium
HEADLESS = $(CHROMIUM) --headless --disable-gpu
BOOKLETS = guide pico

all: $(BOOKLETS:=.pdf)

%.pdf: %.html zine.css zine.js $(wildcard img/*.png)
	$(HEADLESS) --no-pdf-header-footer --virtual-time-budget=3000 --print-to-pdf=$@ \
		file://$(CURDIR)/$<

check:
	@for b in $(BOOKLETS); do \
		$(HEADLESS) --virtual-time-budget=3000 --dump-dom "file://$(CURDIR)/$$b.html#check" \
			2>/dev/null | grep -o 'data-check="OVER[^"]*"' | sed "s/^/$$b: /"; \
	done | grep . && exit 1 || echo "every page fits"

.PHONY: all check
