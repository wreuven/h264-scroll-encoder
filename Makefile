# UI-Aware Hybrid H.264 Encoder
# Main Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -I$(INCDIR)
LDFLAGS = -lm

SRCDIR = src
INCDIR = include
BUILDDIR = build

SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

.PHONY: all clean experiments

all: $(BUILDDIR) $(OBJECTS)
	@echo "Build complete. Main encoder is under development."

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build experiments
experiments:
	$(MAKE) -C experiments/scroll-encoder
	$(MAKE) -C experiments/trans-resizer

clean:
	rm -rf $(BUILDDIR)
	$(MAKE) -C experiments/scroll-encoder clean 2>/dev/null || true
	$(MAKE) -C experiments/trans-resizer clean 2>/dev/null || true
