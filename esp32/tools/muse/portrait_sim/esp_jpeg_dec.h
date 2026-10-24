#pragma once
#include <stdint.h>
typedef enum { JPEG_ERR_OK = 0, JPEG_ERR_FAIL = -1, JPEG_ERR_INVALID_PARAM = -4 } jpeg_error_t;
#define JPEG_PIXEL_FORMAT_RGB565_LE 0
typedef struct { int output_type; } jpeg_dec_config_t;
typedef void *jpeg_dec_handle_t;
typedef struct { uint8_t *inbuf; int inbuf_len; uint8_t *outbuf; } jpeg_dec_io_t;
typedef struct { int width, height; } jpeg_dec_header_info_t;
jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *cfg, jpeg_dec_handle_t *dec);
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t dec, jpeg_dec_io_t *io, jpeg_dec_header_info_t *info);
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t dec, jpeg_dec_io_t *io);
void jpeg_dec_close(jpeg_dec_handle_t dec);
