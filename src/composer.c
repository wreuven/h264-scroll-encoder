#include "composer.h"
#include "nal_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default buffer sizes */
#define OUTPUT_BUFFER_SIZE (64 * 1024 * 1024)  /* 64 MB */
#define RBSP_BUFFER_SIZE   (4 * 1024 * 1024)   /* 4 MB */

/*
 * Load a file into memory
 */
static uint8_t *load_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(*size);
    if (!data) {
        fclose(f);
        return NULL;
    }

    if (fread(data, 1, *size, f) != *size) {
        fprintf(stderr, "Error: Failed to read %s\n", path);
        free(data);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return data;
}

/*
 * Parse reference file to extract SPS, PPS, and IDR RBSP
 */
static int parse_reference_file(const uint8_t *data, size_t size,
                                 uint8_t **sps_out, size_t *sps_size,
                                 uint8_t **pps_out, size_t *pps_size,
                                 uint8_t **idr_rbsp_out, size_t *idr_size,
                                 int *width, int *height,
                                 int *log2_max_frame_num,
                                 int *pic_order_cnt_type,
                                 int *log2_max_pic_order_cnt_lsb,
                                 int *num_ref_idx_l0_default_minus1,
                                 int *deblocking_filter_control_present_flag) {
    NALParser parser;
    NALUnit unit;
    uint8_t *rbsp_temp = malloc(size);
    int found_sps = 0, found_pps = 0, found_idr = 0;

    nal_parser_init(&parser, data, size);

    while (nal_parser_next(&parser, &unit)) {
        switch (unit.nal_unit_type) {
            case NAL_TYPE_SPS:
                if (!found_sps) {
                    /* Convert to RBSP */
                    size_t rbsp_size = ebsp_to_rbsp(rbsp_temp, unit.data, unit.size);

                    /* Parse SPS */
                    if (parse_sps(rbsp_temp, rbsp_size, width, height,
                                  log2_max_frame_num, pic_order_cnt_type,
                                  log2_max_pic_order_cnt_lsb) < 0) {
                        fprintf(stderr, "Error: Failed to parse SPS\n");
                        free(rbsp_temp);
                        return -1;
                    }

                    /* Store original SPS RBSP */
                    *sps_out = malloc(rbsp_size);
                    memcpy(*sps_out, rbsp_temp, rbsp_size);
                    *sps_size = rbsp_size;
                    found_sps = 1;
                }
                break;

            case NAL_TYPE_PPS:
                if (!found_pps) {
                    size_t rbsp_size = ebsp_to_rbsp(rbsp_temp, unit.data, unit.size);

                    if (parse_pps(rbsp_temp, rbsp_size,
                                  num_ref_idx_l0_default_minus1,
                                  deblocking_filter_control_present_flag) < 0) {
                        fprintf(stderr, "Error: Failed to parse PPS\n");
                        free(rbsp_temp);
                        return -1;
                    }

                    *pps_out = malloc(rbsp_size);
                    memcpy(*pps_out, rbsp_temp, rbsp_size);
                    *pps_size = rbsp_size;
                    found_pps = 1;
                }
                break;

            case NAL_TYPE_IDR:
                if (!found_idr) {
                    size_t rbsp_size = ebsp_to_rbsp(rbsp_temp, unit.data, unit.size);
                    *idr_rbsp_out = malloc(rbsp_size);
                    memcpy(*idr_rbsp_out, rbsp_temp, rbsp_size);
                    *idr_size = rbsp_size;
                    found_idr = 1;
                }
                break;
        }
    }

    free(rbsp_temp);

    if (!found_sps || !found_pps || !found_idr) {
        fprintf(stderr, "Error: Reference file missing SPS/PPS/IDR\n");
        return -1;
    }

    return 0;
}

int composer_init(Composer *c, const char *ref_a_path, const char *ref_b_path) {
    memset(c, 0, sizeof(*c));

    /* Load reference files */
    size_t ref_a_file_size, ref_b_file_size;
    uint8_t *ref_a_data = load_file(ref_a_path, &ref_a_file_size);
    uint8_t *ref_b_data = load_file(ref_b_path, &ref_b_file_size);

    if (!ref_a_data || !ref_b_data) {
        free(ref_a_data);
        free(ref_b_data);
        return -1;
    }

    /* Parse reference A */
    int width, height;
    int log2_max_frame_num, pic_order_cnt_type, log2_max_pic_order_cnt_lsb;
    int num_ref_idx_l0, deblock_flag;

    if (parse_reference_file(ref_a_data, ref_a_file_size,
                             &c->orig_sps, &c->orig_sps_size,
                             &c->orig_pps, &c->orig_pps_size,
                             &c->ref_a_rbsp, &c->ref_a_size,
                             &width, &height,
                             &log2_max_frame_num, &pic_order_cnt_type,
                             &log2_max_pic_order_cnt_lsb,
                             &num_ref_idx_l0, &deblock_flag) < 0) {
        free(ref_a_data);
        free(ref_b_data);
        return -1;
    }

    /* Parse reference B (only need IDR RBSP, SPS/PPS should match) */
    uint8_t *temp_sps, *temp_pps;
    size_t temp_sps_size, temp_pps_size;
    int width_b, height_b;
    int l2mfn, poct, l2mpoclsb, nridx, dbf;

    if (parse_reference_file(ref_b_data, ref_b_file_size,
                             &temp_sps, &temp_sps_size,
                             &temp_pps, &temp_pps_size,
                             &c->ref_b_rbsp, &c->ref_b_size,
                             &width_b, &height_b,
                             &l2mfn, &poct, &l2mpoclsb, &nridx, &dbf) < 0) {
        free(ref_a_data);
        free(ref_b_data);
        return -1;
    }

    /* Verify dimensions match */
    if (width != width_b || height != height_b) {
        fprintf(stderr, "Error: Reference frame dimensions don't match\n");
        fprintf(stderr, "  RefA: %dx%d, RefB: %dx%d\n", width, height, width_b, height_b);
        free(temp_sps);
        free(temp_pps);
        free(ref_a_data);
        free(ref_b_data);
        return -1;
    }

    free(temp_sps);
    free(temp_pps);
    free(ref_a_data);
    free(ref_b_data);

    /* Initialize parse config (external encoder's params) */
    composer_config_init(&c->parse_cfg, width, height);
    composer_config_set_sps_params(&c->parse_cfg, log2_max_frame_num,
                                    pic_order_cnt_type, log2_max_pic_order_cnt_lsb);
    composer_config_set_pps_params(&c->parse_cfg, num_ref_idx_l0, deblock_flag);

    /* Initialize write config (our params) */
    composer_config_init(&c->cfg, width, height);
    /* Use our own log2_max_frame_num=4 for more frame headroom */
    composer_config_set_sps_params(&c->cfg, 4, 2, 4);
    /* Preserve deblocking flag from input */
    composer_config_set_pps_params(&c->cfg, 1, deblock_flag);

    /* Allocate output buffers */
    c->output_capacity = OUTPUT_BUFFER_SIZE;
    c->output_buffer = malloc(c->output_capacity);
    c->rbsp_capacity = RBSP_BUFFER_SIZE;
    c->rbsp_temp = malloc(c->rbsp_capacity);

    if (!c->output_buffer || !c->rbsp_temp) {
        fprintf(stderr, "Error: Failed to allocate output buffers\n");
        return -1;
    }

    /* Initialize NAL writer */
    nal_writer_init(&c->nw, c->output_buffer, c->output_capacity,
                    c->rbsp_temp, c->rbsp_capacity);

    printf("Composer initialized: %dx%d\n", width, height);
    return 0;
}

int composer_get_width(Composer *c) {
    return c->cfg.width;
}

int composer_get_height(Composer *c) {
    return c->cfg.height;
}

void composer_write_header(Composer *c) {
    /* Generate and write our SPS */
    size_t sps_size = h264_generate_sps(c->rbsp_temp, c->rbsp_capacity,
                                        c->cfg.width, c->cfg.height);
    nal_write_unit(&c->nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_SPS,
                   c->rbsp_temp, sps_size, 1);

    /* Generate and write our PPS */
    size_t pps_size = h264_generate_pps(c->rbsp_temp, c->rbsp_capacity);
    nal_write_unit(&c->nw, NAL_REF_IDC_HIGHEST, NAL_TYPE_PPS,
                   c->rbsp_temp, pps_size, 1);

    /* Rewrite RefA as IDR with long_term_reference_flag=1 */
    h264_rewrite_idr_frame(&c->nw, &c->cfg, &c->parse_cfg,
                           c->ref_a_rbsp, c->ref_a_size);

    /* Rewrite RefB as non-IDR I-frame with MMCO long-term marking */
    h264_rewrite_as_non_idr_i_frame(&c->nw, &c->cfg, &c->parse_cfg,
                                     c->ref_b_rbsp, c->ref_b_size, 1);

    printf("Header written: SPS + PPS + 2 reference frames\n");
}

void composer_write_scroll_frame(Composer *c, int offset_px) {
    /* Check if waypoint needed */
    if (h264_needs_waypoint(&c->cfg, offset_px)) {
        h264_write_waypoint_p_frame(&c->nw, &c->cfg, offset_px);
        printf("  Waypoint at offset %d\n", offset_px);
    }

    h264_write_scroll_p_frame(&c->nw, &c->cfg, offset_px);
    c->frames_written++;
}

size_t composer_get_output_size(Composer *c) {
    return nal_writer_get_size(&c->nw);
}

uint8_t *composer_get_output(Composer *c) {
    return nal_writer_get_output(&c->nw);
}

int composer_write_to_file(Composer *c, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: Cannot create %s\n", path);
        return -1;
    }

    size_t size = composer_get_output_size(c);
    if (fwrite(c->output_buffer, 1, size, f) != size) {
        fprintf(stderr, "Error: Failed to write %s\n", path);
        fclose(f);
        return -1;
    }

    fclose(f);
    printf("Written %zu bytes to %s\n", size, path);
    return 0;
}

void composer_finish(Composer *c) {
    free(c->ref_a_rbsp);
    free(c->ref_b_rbsp);
    free(c->orig_sps);
    free(c->orig_pps);
    free(c->output_buffer);
    free(c->rbsp_temp);
    memset(c, 0, sizeof(*c));
}
