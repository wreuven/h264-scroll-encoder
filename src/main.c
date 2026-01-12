/*
 * Composer v0.1 - UI-Aware Hybrid H.264 Encoder
 *
 * Takes two externally-encoded I-frames and generates P-frames with
 * scroll motion vectors.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "composer.h"

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  --ref-a FILE      First reference I-frame (required)\n");
    printf("  --ref-b FILE      Second reference I-frame (required)\n");
    printf("  -n, --frames N    Number of P-frames to generate (default: 250)\n");
    printf("  -s, --speed N     Scroll speed in pixels/frame (default: 4)\n");
    printf("  -o, --output FILE Output H.264 file (default: output.h264)\n");
    printf("  -h, --help        Show this help\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s --ref-a ref_a.h264 --ref-b ref_b.h264 -n 250 -o scroll.h264\n", prog);
    printf("\n");
    printf("Generate reference frames with:\n");
    printf("  ./scripts/generate_refs.sh\n");
}

int main(int argc, char **argv) {
    const char *ref_a_path = NULL;
    const char *ref_b_path = NULL;
    const char *output_path = "output.h264";
    int num_frames = 250;
    int scroll_speed = 4;

    static struct option long_options[] = {
        {"ref-a",   required_argument, 0, 'a'},
        {"ref-b",   required_argument, 0, 'b'},
        {"frames",  required_argument, 0, 'n'},
        {"speed",   required_argument, 0, 's'},
        {"output",  required_argument, 0, 'o'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "a:b:n:s:o:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'a':
                ref_a_path = optarg;
                break;
            case 'b':
                ref_b_path = optarg;
                break;
            case 'n':
                num_frames = atoi(optarg);
                break;
            case 's':
                scroll_speed = atoi(optarg);
                break;
            case 'o':
                output_path = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* Validate required arguments */
    if (!ref_a_path || !ref_b_path) {
        fprintf(stderr, "Error: --ref-a and --ref-b are required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (num_frames <= 0) {
        fprintf(stderr, "Error: --frames must be positive\n");
        return 1;
    }

    if (scroll_speed <= 0) {
        fprintf(stderr, "Error: --speed must be positive\n");
        return 1;
    }

    /* Initialize composer */
    Composer c;
    if (composer_init(&c, ref_a_path, ref_b_path) < 0) {
        return 1;
    }

    int height = composer_get_height(&c);
    int max_offset = height;  /* Scroll from 0 to height */

    printf("Generating %d frames, scroll speed %d px/frame\n", num_frames, scroll_speed);
    printf("Max scroll offset: %d pixels\n", max_offset);

    /* Write header (SPS + PPS + I-frames) */
    composer_write_header(&c);

    /* Generate P-frames with scroll animation */
    int start_offset = 0;
    for (int i = 0; i < num_frames; i++) {
        /* Scroll pattern: 0 → max → 0 → max ... */
        int cycle_len = max_offset * 2;
        int cycle_pos = (i * scroll_speed + start_offset) % cycle_len;
        int offset_px;

        if (cycle_pos < max_offset) {
            offset_px = cycle_pos;  /* Scrolling down */
        } else {
            offset_px = cycle_len - cycle_pos;  /* Scrolling back up */
        }

        composer_write_scroll_frame(&c, offset_px);

        /* Progress indicator */
        if ((i + 1) % 50 == 0 || i == num_frames - 1) {
            printf("  Frame %d/%d (offset %d px)\n", i + 1, num_frames, offset_px);
        }
    }

    /* Write output */
    if (composer_write_to_file(&c, output_path) < 0) {
        composer_finish(&c);
        return 1;
    }

    printf("\nDone! To play:\n");
    printf("  ffmpeg -i %s -c:v copy output.mp4 && ffplay output.mp4\n", output_path);

    composer_finish(&c);
    return 0;
}
