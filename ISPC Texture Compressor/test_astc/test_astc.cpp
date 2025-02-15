/*
  Copyright (c) 2015, Intel Corporation
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.


   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  
*/

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cassert>
#include <windows.h>
#include <vector>
#include "ispc_texcomp.h"

size_t align(size_t bytes, const int alignement)
{
    return (bytes + alignement - 1) & ~(alignement - 1);
}

void load_bmp(rgba_surface* img, char* filename)
{
    FILE* f = fopen(filename, "rb");
    assert(f && "load_bmp: couldn't open file");

    BITMAPFILEHEADER file_header;
    BITMAPINFOHEADER info_header;
    fread(&file_header, sizeof(BITMAPFILEHEADER), 1, f);
    fread(&info_header, sizeof(BITMAPINFOHEADER), 1, f);

    if (!(info_header.biBitCount == 32 || info_header.biBitCount == 24))
    {
        assert(false && "load_bmp: unsupported format (only RGB32/RGB24 is supported)");
    }

    img->width = info_header.biWidth;
    img->height = info_header.biHeight;
    img->stride = img->width * 4;
    img->ptr = (uint8_t*)malloc(img->height * img->stride);

    int bytePerPixel = info_header.biBitCount / 8;
    size_t bmpStride = align(info_header.biWidth * bytePerPixel, 4);

    std::vector<uint8_t> raw_line;
    raw_line.resize(bmpStride);

    for (int y = 0; y < img->height; y++)
    {
        int yy = img->height - 1 - y;
        fread(raw_line.data(), bmpStride, 1, f);

        for (int x = 0; x < img->width; x++)
        {
            img->ptr[yy * img->stride + x * 4 + 0] = raw_line[x * bytePerPixel + 2];
            img->ptr[yy * img->stride + x * 4 + 1] = raw_line[x * bytePerPixel + 1];
            img->ptr[yy * img->stride + x * 4 + 2] = raw_line[x * bytePerPixel + 0];
        }
    }

    fclose(f);
}

#define MAGIC_FILE_CONSTANT 0x5CA1AB13

// little endian
struct astc_header
{
    uint8_t magic[4];
    uint8_t blockdim_x;
    uint8_t blockdim_y;
    uint8_t blockdim_z;
    uint8_t xsize[3];
    uint8_t ysize[3];			// x-size, y-size and z-size are given in texels;
    uint8_t zsize[3];			// block count is inferred
};

void store_astc(rgba_surface* img, int block_width, int block_height, char* filename)
{
    FILE* f = fopen(filename, "wb");

    astc_header file_header;

    uint32_t magic = MAGIC_FILE_CONSTANT;
    memcpy(file_header.magic, &magic, 4);
    file_header.blockdim_x = block_width;
    file_header.blockdim_y = block_height;
    file_header.blockdim_z = 1;

    int xsize = img->width;
    int ysize = img->height;
    int zsize = 1;

    memcpy(file_header.xsize, &xsize, 3);
    memcpy(file_header.ysize, &ysize, 3);
    memcpy(file_header.zsize, &zsize, 3);

    fwrite(&file_header, sizeof(astc_header), 1, f);

    size_t height_in_blocks = (block_height + img->height - 1) / block_height;
    fwrite(img->ptr, height_in_blocks * img->stride, 1, f);

    fclose(f);
}

void alloc_image(rgba_surface* img, int width, int height)
{
    img->width = width;
    img->height = height;
    img->stride = img->width * 4;
    img->ptr = (uint8_t*)malloc(img->height * img->stride);
}

void compress_astc_tex(rgba_surface* output_tex, rgba_surface* img, int block_width, int block_height)
{
    astc_enc_settings settings;
    GetProfile_astc_fast(&settings, block_width, block_height);
    CompressBlocksASTC(img, output_tex->ptr, &settings);
}

void compress_astc_tex_mt(rgba_surface* output_tex, rgba_surface* img, int block_width, int block_height)
{
    astc_enc_settings settings;
    GetProfile_astc_fast(&settings, block_width, block_height);

    int thread_count = 32;

#pragma omp parallel for
    for (int t = 0; t < thread_count; t++)
    {
        int t_y = (t * output_tex->height) / thread_count;
        int t_yy = ((t + 1) * output_tex->height) / thread_count;

        int i_y = t_y * block_height;
        int i_yy = t_yy * block_height;

        assert(i_yy <= img->height);

        uint8_t* dst_ptr = (uint8_t*)&output_tex->ptr[t_y * output_tex->stride];

        rgba_surface span;
        span = *img;
        span.ptr = &span.ptr[i_y * span.stride];
        span.height = i_yy - i_y;

        CompressBlocksASTC(&span, dst_ptr, &settings);
    }
}

int idiv_ceil(int n, int d)
{
    return (n + d - 1) / d;
}

void flip_image(rgba_surface* rec_img)
{
    rec_img->ptr += (rec_img->height - 1) * rec_img->stride;
    rec_img->stride *= -1;
}

void fill_borders(rgba_surface* dst, rgba_surface* src, int block_width, int block_height)
{
    int full_width = idiv_ceil(src->width, block_width) * block_width;
    int full_height = idiv_ceil(src->height, block_height) * block_height;
    alloc_image(dst, full_width, full_height);
    
    for (int y = 0; y < dst->height; y++)
    for (int x = 0; x < dst->width; x++)
    {
        int clipped_y = min(y, src->height - 1);
        int clipped_x = min(x, src->width - 1);

        for (int p = 0; p < 3; p++)
        {
            int value = src->ptr[src->stride * clipped_y + clipped_x * 4 + p];
            dst->ptr[dst->stride * y + x * 4 + p] = value;
        }
    }
}

void enc_astc_file(char* filename, char* dst_filename)
{
    rgba_surface src_img;
    load_bmp(&src_img, filename);
    flip_image(&src_img);

    int block_width = 6;
    int block_height = 6;

    rgba_surface output_tex;
    output_tex.width = idiv_ceil(src_img.width, block_width);
    output_tex.height = idiv_ceil(src_img.height, block_height);
    output_tex.stride = output_tex.width * 16;
    output_tex.ptr = (uint8_t*)malloc(output_tex.height * output_tex.stride);

    rgba_surface edged_img;
    fill_borders(&edged_img, &src_img, block_width, block_height);

    printf("encoding <%s>...", filename);

    compress_astc_tex(&output_tex, &edged_img, block_width, block_height);

    printf("done.\n");

    output_tex.width = src_img.width;
    output_tex.height = src_img.height;
    store_astc(&output_tex, block_width, block_height, dst_filename);
}

void main(int argc, char *argv[])
{
    if (argc == 3)
    {
        enc_astc_file(argv[1], argv[2]);
    }
    else
    {
        printf("usage:\ntest_astc input.bmp output.astc\n");
    }
}
