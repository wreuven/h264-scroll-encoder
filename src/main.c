#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "bitwriter.h"
#include "nal.h"
#include "nal_parser.h"
#include "h264_encoder.h"

#define MAX_STREAM_SIZE (64 * 1024 * 1024)  /* 64 MB max output */
#define MAX_INPUT_SIZE  (16 * 1024 * 1024)  /* 16 MB max input */

/* YCbCr color values (BT.601) */
typedef struct {
    const char *name;
    uint8_t y, cb, cr;
} ColorEntry;

static const ColorEntry color_table[] = {
    {"red",    81,  90, 240},
    {"blue",   41, 240, 110},
    {"green", 145,  54,  34},
    {"yellow",210,  16, 146},
    {"cyan",  170, 166,  16},
    {"magenta",106,202, 222},
    {"white", 235, 128, 128},
    {"black",  16, 128, 128},
    {"gray",  128, 128, 128},
    {NULL, 0, 0, 0}
};

static int parse_color(const char *name, uint8_t *y, uint8_t *cb, uint8_t *cr) {
    for (const ColorEntry *c = color_table; c->name; c++) {
        if (strcasecmp(name, c->name) == 0) {
            *y = c->y;
            *cb = c->cb;
            *cr = c->cr;
            return 0;
        }
    }
    return -1;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "H.264 Scroll Encoder - Generate scrolling animation from two reference frames\n\n");
    fprintf(stderr, "Usage: %s [options]\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -i, --input FILE     Input H.264 file with 2 I-frames (A and B)\n");
    fprintf(stderr, "  -o, --output FILE    Output H.264 file (default: output.h264)\n");
    fprintf(stderr, "  -n, --frames N       Number of scroll frames to generate (default: 60)\n");
    fprintf(stderr, "  -w, --width W        Frame width (if not using input file)\n");
    fprintf(stderr, "  -h, --height H       Frame height (if not using input file)\n");
    fprintf(stderr, "  -t, --test           Generate test stream (no input file needed)\n");
    fprintf(stderr, "  --color-a COLOR      Color for frame A (default: gray)\n");
    fprintf(stderr, "  --color-b COLOR      Color for frame B (default: gray)\n");
    fprintf(stderr, "  --help               Show this help\n");
    fprintf(stderr, "\nColors: red, blue, green, yellow, cyan, magenta, white, black, gray\n");
}

/*
 * Load file into memory
 */
static uint8_t *load_file(const char *filename, size_t *size) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (*size > MAX_INPUT_SIZE) {
        fprintf(stderr, "Input file too large (max %d bytes)\n", MAX_INPUT_SIZE);
        fclose(f);
        return NULL;
    }

    uint8_t *data = malloc(*size);
    if (!data) {
        fprintf(stderr, "Out of memory\n");
        fclose(f);
        return NULL;
    }

    if (fread(data, 1, *size, f) != *size) {
        fprintf(stderr, "Failed to read file\n");
        free(data);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return data;
}

/*
 * Write data to file
 */
static int write_file(const char *filename, const uint8_t *data, size_t size) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror(filename);
        return -1;
    }

    if (fwrite(data, 1, size, f) != size) {
        fprintf(stderr, "Failed to write file\n");
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *input_file = NULL;
    const char *output_file = "output.h264";
    int num_frames = 60;
    int width = 0, height = 0;
    int test_mode = 0;
    const char *color_a_name = "gray";
    const char *color_b_name = "gray";

    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"frames", required_argument, 0, 'n'},
        {"width", required_argument, 0, 'w'},
        {"height", required_argument, 0, 'H'},
        {"test", no_argument, 0, 't'},
        {"color-a", required_argument, 0, 'A'},
        {"color-b", required_argument, 0, 'B'},
        {"help", no_argument, 0, '?'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:o:n:w:H:t", long_options, NULL)) != -1) {
        switch (c) {
            case 'i': input_file = optarg; break;
            case 'o': output_file = optarg; break;
            case 'n': num_frames = atoi(optarg); break;
            case 'w': width = atoi(optarg); break;
            case 'H': height = atoi(optarg); break;
            case 't': test_mode = 1; break;
            case 'A': color_a_name = optarg; break;
            case 'B': color_b_name = optarg; break;
            case '?':
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* Parse color names to YCbCr */
    uint8_t color_a_y, color_a_cb, color_a_cr;
    uint8_t color_b_y, color_b_cb, color_b_cr;
    if (parse_color(color_a_name, &color_a_y, &color_a_cb, &color_a_cr) < 0) {
        fprintf(stderr, "Error: Unknown color '%s'\n", color_a_name);
        return 1;
    }
    if (parse_color(color_b_name, &color_b_y, &color_b_cb, &color_b_cr) < 0) {
        fprintf(stderr, "Error: Unknown color '%s'\n", color_b_name);
        return 1;
    }

    if (!input_file && !test_mode) {
        fprintf(stderr, "Error: Must specify input file (-i) or test mode (-t)\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Allocate output buffer */
    uint8_t *output = malloc(MAX_STREAM_SIZE);
    uint8_t *rbsp_temp = malloc(1024 * 1024);
    if (!output || !rbsp_temp) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    NALWriter nw;
    nal_writer_init(&nw, output, MAX_STREAM_SIZE, rbsp_temp, 1024 * 1024);

    H264EncoderConfig cfg;

    if (test_mode) {
        /* Generate test stream with our own SPS/PPS and I-frames */
        if (width == 0) width = 640;
        if (height == 0) height = 480;

        h264_encoder_init(&cfg, width, height);

        printf("Test mode: %dx%d, %d frames\n", width, height, num_frames);
        printf("  Color A: %s (Y=%d, Cb=%d, Cr=%d)\n",
               color_a_name, color_a_y, color_a_cb, color_a_cr);
        printf("  Color B: %s (Y=%d, Cb=%d, Cr=%d)\n",
               color_b_name, color_b_y, color_b_cb, color_b_cr);

        /* Generate and write SPS */
        uint8_t sps[256];
        size_t sps_size = h264_generate_sps(sps, sizeof(sps), width, height);
        nal_write_unit(&nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_SPS, sps, sps_size, 1);
        printf("  SPS: %zu bytes\n", sps_size);

        /* Generate and write PPS */
        uint8_t pps[256];
        size_t pps_size = h264_generate_pps(pps, sizeof(pps));
        nal_write_unit(&nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_PPS, pps, pps_size, 1);
        printf("  PPS: %zu bytes\n", pps_size);

        /* Generate reference frame A (IDR) with color */
        printf("  Generating IDR frame A (%s)...\n", color_a_name);
        h264_write_idr_frame_color(&nw, &cfg, color_a_y, color_a_cb, color_a_cr);

        /* Generate reference frame B (non-IDR I-frame) with color */
        printf("  Generating non-IDR I-frame B (%s)...\n", color_b_name);
        h264_write_non_idr_i_frame_color(&nw, &cfg, color_b_y, color_b_cb, color_b_cr);

        printf("  Setup complete: frame_num=%d\n", cfg.frame_num);

    } else {
        /* Load input file */
        size_t input_size;
        uint8_t *input = load_file(input_file, &input_size);
        if (!input) {
            return 1;
        }

        printf("Loaded input: %zu bytes\n", input_size);

        /* Parse NAL units from input */
        NALParser parser;
        nal_parser_init(&parser, input, input_size);

        NALUnit unit;
        int found_sps = 0, found_pps = 0;
        int idr_count = 0;
        size_t nal_positions[16];
        size_t nal_sizes[16];
        int nal_count = 0;

        /* First pass: find all NAL units */
        while (nal_parser_next(&parser, &unit) && nal_count < 16) {
            size_t nal_start = (unit.data - 1) - input;  /* Include header byte */

            /* Find the start code before this NAL */
            size_t sc_start = nal_start;
            while (sc_start > 0 && input[sc_start - 1] == 0) sc_start--;

            nal_positions[nal_count] = sc_start;

            printf("NAL %d: type=%d, ref_idc=%d, pos=%zu\n",
                   nal_count, unit.nal_unit_type, unit.nal_ref_idc, sc_start);

            if (unit.nal_unit_type == NAL_TYPE_SPS) {
                found_sps = 1;
                /* Parse SPS */
                uint8_t rbsp[1024];
                size_t rbsp_size = ebsp_to_rbsp(rbsp, unit.data, unit.size);
                int w, h, log2_mfn, poc_type, log2_poc;
                if (parse_sps(rbsp, rbsp_size, &w, &h, &log2_mfn, &poc_type, &log2_poc) == 0) {
                    width = w;
                    height = h;
                    h264_encoder_init(&cfg, width, height);
                    h264_encoder_set_sps(&cfg, rbsp, rbsp_size, log2_mfn, poc_type, log2_poc);
                    printf("SPS: %dx%d, log2_max_frame_num=%d, poc_type=%d\n",
                           width, height, log2_mfn, poc_type);
                }
            } else if (unit.nal_unit_type == NAL_TYPE_PPS) {
                found_pps = 1;
                uint8_t rbsp[1024];
                size_t rbsp_size = ebsp_to_rbsp(rbsp, unit.data, unit.size);
                int num_ref_l0, deblock_ctrl;
                if (parse_pps(rbsp, rbsp_size, &num_ref_l0, &deblock_ctrl) == 0) {
                    h264_encoder_set_pps(&cfg, rbsp, rbsp_size, num_ref_l0, deblock_ctrl);
                    printf("PPS: num_ref_l0=%d, deblock_ctrl=%d\n", num_ref_l0, deblock_ctrl);
                }
            } else if (unit.nal_unit_type == NAL_TYPE_IDR) {
                idr_count++;
            }

            nal_count++;
        }

        /* Set sizes based on next NAL position */
        for (int i = 0; i < nal_count - 1; i++) {
            nal_sizes[i] = nal_positions[i + 1] - nal_positions[i];
        }
        nal_sizes[nal_count - 1] = input_size - nal_positions[nal_count - 1];

        if (!found_sps || !found_pps) {
            fprintf(stderr, "Error: Input must contain SPS and PPS\n");
            free(input);
            return 1;
        }

        printf("Found SPS, PPS, %d IDR frames\n", idr_count);

        /* Write our own SPS with larger max_frame_num to avoid wrap-around issues */
        uint8_t our_sps[256];
        size_t our_sps_size = h264_generate_sps(our_sps, sizeof(our_sps), width, height);
        nal_write_unit(&nw, NAL_REF_IDC_HIGH, NAL_TYPE_SPS, our_sps, our_sps_size, 1);

        /* Copy PPS and I-frames from input, skip x264's SPS */
        for (int i = 0; i < nal_count; i++) {
            uint8_t *nal_start = input + nal_positions[i];
            size_t nal_size = nal_sizes[i];
            /* Find NAL type after start code */
            int offset = (nal_start[2] == 1) ? 3 : 4;
            uint8_t nal_type = nal_start[offset] & 0x1f;

            /* Skip SPS (we wrote our own), copy everything else */
            if (nal_type != NAL_TYPE_SPS) {
                memcpy(output + nw.output_pos, nal_start, nal_size);
                nw.output_pos += nal_size;
            }
        }

        /* Use our larger log2_max_frame_num */
        cfg.log2_max_frame_num = 9;

        /* Set frame_num to 2 since we have 2 reference frames from input */
        cfg.frame_num = 2;

        free(input);
    }

    printf("Generating %d scroll frames...\n", num_frames);

    /* Generate scroll P-frames */
    int mb_height = height / 16;

    for (int i = 0; i < num_frames; i++) {
        /* Calculate scroll offset: go from 0 to mb_height and back */
        int cycle_pos = i % (mb_height * 2);
        int offset_mb;

        if (cycle_pos < mb_height) {
            offset_mb = cycle_pos;  /* Scrolling down (A up, B appears) */
        } else {
            offset_mb = mb_height * 2 - cycle_pos;  /* Scrolling back */
        }

        h264_write_scroll_p_frame(&nw, &cfg, offset_mb);

        if ((i + 1) % 10 == 0) {
            printf("  Frame %d/%d (offset=%d MB)\n", i + 1, num_frames, offset_mb);
        }
    }

    size_t output_size = nal_writer_get_size(&nw);
    printf("Output size: %zu bytes\n", output_size);

    /* Write output file */
    if (write_file(output_file, output, output_size) < 0) {
        return 1;
    }

    printf("Written to %s\n", output_file);

    free(output);
    free(rbsp_temp);
    return 0;
}
