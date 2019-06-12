#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "bswap.h"
#include "minivhd_internal.h"
#include "minivhd_io.h"
#include "minivhd_util.h"
#include "minivhd_struct_rw.h"
#include "minivhd.h"

static void mvhd_read_footer(MVHDMeta* vhdm);
static void mvhd_read_sparse_header(MVHDMeta* vhdm);
static uint32_t mvhd_gen_footer_checksum(MVHDMeta* vhdm);
static uint32_t mvhd_gen_sparse_checksum(MVHDMeta* vhdm);
static bool mvhd_footer_checksum_valid(MVHDMeta* vhdm);
static bool mvhd_sparse_checksum_valid(MVHDMeta* vhdm);
static int mvhd_read_bat(MVHDMeta *vhdm, MVHDError* err);
static void mvhd_calc_sparse_values(MVHDMeta* vhdm);
static int mvhd_init_sector_bitmap(MVHDMeta* vhdm, MVHDError* err);

static void mvhd_read_footer(MVHDMeta* vhdm) {
    uint8_t buffer[MVHD_FOOTER_SIZE];
    fseeko64(vhdm->f, -MVHD_FOOTER_SIZE, SEEK_END);
    fread(buffer, sizeof buffer, 1, vhdm->f);
    mvhd_buffer_to_footer(&vhdm->footer, buffer);
}

static void mvhd_read_sparse_header(MVHDMeta* vhdm) {
    uint8_t buffer[MVHD_SPARSE_SIZE];
    fseeko64(vhdm->f, vhdm->footer.data_offset, SEEK_SET);
    fread(buffer, sizeof buffer, 1, vhdm->f);
    mvhd_buffer_to_header(&vhdm->sparse, buffer);
}

static uint32_t mvhd_gen_footer_checksum(MVHDMeta* vhdm) {
    uint32_t new_chk = 0;
    uint32_t orig_chk = vhdm->footer.checksum;
    vhdm->footer.checksum = 0;
    uint8_t* footer_bytes = (uint8_t*)&vhdm->footer;
    for (size_t i = 0; i < sizeof vhdm->footer; i++) {
        new_chk += footer_bytes[i];
    }
    vhdm->footer.checksum = orig_chk;
    return ~new_chk;
}
static uint32_t mvhd_gen_sparse_checksum(MVHDMeta* vhdm) {
    uint32_t new_chk = 0;
    uint32_t orig_chk = vhdm->sparse.checksum;
    vhdm->sparse.checksum = 0;
    uint8_t* sparse_bytes = (uint8_t*)&vhdm->sparse;
    for (size_t i = 0; i < sizeof vhdm->sparse; i++) {
        new_chk += sparse_bytes[i];
    }
    vhdm->sparse.checksum = orig_chk;
    return ~new_chk;
}

static bool mvhd_footer_checksum_valid(MVHDMeta* vhdm) {
    return vhdm->footer.checksum == mvhd_gen_footer_checksum(vhdm);
}

static bool mvhd_sparse_checksum_valid(MVHDMeta* vhdm) {
    return vhdm->sparse.checksum == mvhd_gen_sparse_checksum(vhdm);
}

static int mvhd_read_bat(MVHDMeta *vhdm, MVHDError* err) {
    vhdm->block_offset = calloc(vhdm->sparse.max_bat_ent, sizeof *vhdm->block_offset);
    if (vhdm->block_offset == NULL) {
        *err = MVHD_ERR_MEM;
        return -1;
    }
    fseeko64(vhdm->f, vhdm->sparse.bat_offset, SEEK_SET);
    for (uint32_t i = 0; i < vhdm->sparse.max_bat_ent; i++) {
        fread(&vhdm->block_offset[i], sizeof *vhdm->block_offset, 1, vhdm->f);
        vhdm->block_offset[i] = be32_to_cpu(vhdm->block_offset[i]);
    }
    return 0;
}

static void mvhd_calc_sparse_values(MVHDMeta* vhdm) {
    vhdm->sect_per_block = vhdm->sparse.block_sz / MVHD_SECTOR_SIZE;
    int bm_bytes = vhdm->sect_per_block / 8;
    vhdm->bitmap.sector_count = bm_bytes / MVHD_SECTOR_SIZE;
    if (bm_bytes % MVHD_SECTOR_SIZE > 0) {
        vhdm->bitmap.sector_count++;
    }
}

static int mvhd_init_sector_bitmap(MVHDMeta* vhdm, MVHDError* err) {
    vhdm->bitmap.curr_bitmap = calloc(vhdm->bitmap.sector_count, MVHD_SECTOR_SIZE);
    if (vhdm->bitmap.curr_bitmap == NULL) {
        *err = MVHD_ERR_MEM;
        return -1;
    }
    vhdm->bitmap.curr_block = -1;
    return 0;
}

static void mvhd_assign_io_funcs(MVHDMeta* vhdm) {
    switch (vhdm->footer.disk_type) {
    case MVHD_TYPE_FIXED:
        vhdm->read_sectors = mvhd_fixed_read;
        vhdm->write_sectors = mvhd_fixed_write;
        break;
    case MVHD_TYPE_DYNAMIC:
        vhdm->read_sectors = mvhd_sparse_read;
        vhdm->write_sectors = mvhd_sparse_diff_write;
        break;
    case MVHD_TYPE_DIFF:
        vhdm->read_sectors = mvhd_diff_read;
        vhdm->write_sectors = mvhd_sparse_diff_write;
        break;
    }
    if (vhdm->readonly) {
        vhdm->write_sectors = mvhd_noop_write;
    }
}

bool mvhd_file_is_vhd(FILE* f) {
    if (f) {
        uint8_t con_str[8];
        fseeko64(f, -MVHD_FOOTER_SIZE, SEEK_END);
        fread(con_str, sizeof con_str, 1, f);
        return mvhd_is_conectix_str(con_str);
    } else {
        return false;
    }
}

MVHDGeom mvhd_calculate_geometry(int size_mb, int* new_size) {
    MVHDGeom chs;
    uint32_t ts = ((uint64_t)size_mb * 1024 * 1024) / MVHD_SECTOR_SIZE;
    uint32_t spt, heads, cyl, cth;
    if (ts > 65535 * 16 * 255) {
        ts = 65535 * 16 * 255;
    }
    if (ts >= 65535 * 16 * 63) {
        ts = 65535 * 16 * 63;
        spt = 255;
        heads = 16;
        cth = ts / spt;
    } else {
        spt = 17;
        cth = ts / spt;
        heads = (cth + 1023) / 1024;
        if (heads < 4) {
            heads = 4;
        }
        if (cth >= (heads * 1024) || heads > 16) {
            spt = 31;
            heads = 16;
            cth = ts / spt;
        }
        if (cth >= (heads * 1024)) {
            spt = 63;
            heads = 16;
            cth = ts / spt;
        }
    }
    cyl = cth / heads;
    chs.heads = heads;
    chs.spt = spt;
    chs.cyl = cyl;

    *new_size = chs.cyl * chs.heads * chs.spt * MVHD_SECTOR_SIZE;
    return chs;
}

MVHDMeta* mvhd_open(const char* path, bool readonly, int* err) {
    int open_err;
    MVHDMeta *vhdm = calloc(sizeof *vhdm, 1);
    if (vhdm == NULL) {
        *err = MVHD_ERR_MEM;
        goto end;
    }
    vhdm->f = readonly ? fopen64(path, "rb") : fopen64(path, "rb+");
    if (vhdm->f == NULL) {
        *err = MVHD_ERR_FILE;
        goto cleanup_vhdm;
    }
    vhdm->readonly = readonly;
    if (!mvhd_file_is_vhd(vhdm->f)) {
        *err = MVHD_ERR_NOT_VHD;
        goto cleanup_file;
    }
    mvhd_read_footer(vhdm);
    if (!mvhd_footer_checksum_valid(vhdm)) {
        *err = MVHD_ERR_FOOTER_CHECKSUM;
        goto cleanup_file;
    }
    if (vhdm->footer.disk_type == MVHD_TYPE_DIFF || vhdm->footer.disk_type == MVHD_TYPE_DYNAMIC) {
        mvhd_read_sparse_header(vhdm);
        if (!mvhd_sparse_checksum_valid(vhdm)) {
            *err = MVHD_ERR_SPARSE_CHECKSUM;
            goto cleanup_file;
        }
        if (mvhd_read_bat(vhdm, &open_err) == -1) {
            *err = open_err;
            goto cleanup_file;
        }
        mvhd_calc_sparse_values(vhdm);
        if (mvhd_init_sector_bitmap(vhdm, &open_err) == -1) {
            *err = open_err;
            goto cleanup_bat;
        }

    } else if (vhdm->footer.disk_type == MVHD_TYPE_FIXED) {
        *err = MVHD_ERR_TYPE;
        goto cleanup_bitmap;
    }
    mvhd_assign_io_funcs(vhdm);
    /* If we've reached this point, we are good to go, so skip the cleanup steps */
    goto end;
cleanup_bitmap:
    free(vhdm->bitmap.curr_bitmap);
    vhdm->bitmap.curr_bitmap = NULL;
cleanup_bat:
    free(vhdm->block_offset);
    vhdm->block_offset = NULL;
cleanup_file:
    fclose(vhdm->f);
    vhdm->f = NULL;
cleanup_vhdm:
    free(vhdm);
    vhdm = NULL;
end:
    return vhdm;
}

void mvhd_close(MVHDMeta* vhdm) {
    if (vhdm->parent != NULL) {
        mvhd_close(vhdm->parent);
    }
    fclose(vhdm->f);
    if (vhdm->block_offset != NULL) {
        free(vhdm->block_offset);
        vhdm->block_offset = NULL;
    }
    if (vhdm->bitmap.curr_bitmap != NULL) {
        free(vhdm->bitmap.curr_bitmap);
        vhdm->bitmap.curr_bitmap = NULL;
    }
    free(vhdm);
    vhdm = NULL;
}

int mvhd_read_sectors(MVHDMeta* vhdm, int offset, int num_sectors, void* out_buff) {
    return vhdm->read_sectors(vhdm, offset, num_sectors, out_buff);
}

int mvhd_write_sectors(MVHDMeta* vhdm, int offset, int num_sectors, void* in_buff) {
    return vhdm->write_sectors(vhdm, offset, num_sectors, in_buff);
}

int mvhd_format_sectors(MVHDMeta* vhdm, int offset, int num_sectors) {
    return vhdm->format_sectors(vhdm, offset, num_sectors);
}