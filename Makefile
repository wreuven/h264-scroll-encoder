# UI-Aware Hybrid H.264 Encoder - Composer v0.1
# Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -I$(INCDIR)
LDFLAGS = -lm

SRCDIR = src
INCDIR = include
BUILDDIR = build

SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
TARGET = composer

.PHONY: all clean test refs experiments help

all: $(BUILDDIR) $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Generate reference frames
refs:
	./scripts/generate_refs.sh

# Quick test
test: $(TARGET) refs
	./$(TARGET) --ref-a ref_a.h264 --ref-b ref_b.h264 -n 100 -o test_output.h264
	@echo ""
	@echo "To play: ffmpeg -i test_output.h264 -c:v copy test.mp4 && ffplay test.mp4"

# Build experiments (scroll-encoder, trans-resizer)
experiments:
	$(MAKE) -C experiments/scroll-encoder
	$(MAKE) -C experiments/trans-resizer

clean:
	rm -rf $(BUILDDIR) $(TARGET) *.h264 *.mp4
	$(MAKE) -C experiments/scroll-encoder clean 2>/dev/null || true
	$(MAKE) -C experiments/trans-resizer clean 2>/dev/null || true

help:
	@echo "Composer v0.1 - UI-Aware Hybrid H.264 Encoder"
	@echo ""
	@echo "Targets:"
	@echo "  all         Build the composer executable"
	@echo "  refs        Generate reference frames (requires ffmpeg)"
	@echo "  test        Build, generate refs, and run a quick test"
	@echo "  experiments Build the learning experiments"
	@echo "  clean       Remove build artifacts"
	@echo "  help        Show this help"
	@echo ""
	@echo "Usage:"
	@echo "  make refs"
	@echo "  ./composer --ref-a ref_a.h264 --ref-b ref_b.h264 -n 250 -o scroll.h264"
	@echo "  ffmpeg -i scroll.h264 -c:v copy scroll.mp4"
	@echo "  ffplay scroll.mp4"
