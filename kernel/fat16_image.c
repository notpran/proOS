#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_SIZE 512
#define TOTAL_SECTORS 128
#define RESERVED_SECTORS 1
#define FAT_COUNT 1
#define ROOT_ENTRIES 64
#define SECTORS_PER_CLUSTER 1
#define FAT_SECTORS 1
#define ROOT_DIR_SECTORS 4
#define DATA_START (RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS + ROOT_DIR_SECTORS)
#define FONT_SOURCE_PRIMARY "font.psf"
#define FONT_SOURCE_FALLBACK "assets/font.psf"

static uint8_t image[TOTAL_SECTORS * SECTOR_SIZE];
static uint8_t *fat_area = NULL;
static uint8_t *root_area = NULL;
static uint8_t *data_area = NULL;
static uint16_t next_cluster = 2;
static uint16_t max_clusters = 0;
static uint32_t cluster_bytes = 0;
static int root_entries_used = 0;

static void write_boot_sector(void)
{
    uint8_t *b = image;
    memset(b, 0, SECTOR_SIZE);
    b[0] = 0xEB;
    b[1] = 0x3C;
    b[2] = 0x90;
    memcpy(b + 3, "PROOS   ", 8);
    b[11] = 0x00;
    b[12] = 0x02; /* 512 bytes per sector */
    b[13] = SECTORS_PER_CLUSTER;
    b[14] = RESERVED_SECTORS & 0xFF;
    b[15] = RESERVED_SECTORS >> 8;
    b[16] = FAT_COUNT;
    b[17] = ROOT_ENTRIES & 0xFF;
    b[18] = ROOT_ENTRIES >> 8;
    b[19] = TOTAL_SECTORS & 0xFF;
    b[20] = TOTAL_SECTORS >> 8;
    b[21] = 0xF8;
    b[22] = FAT_SECTORS & 0xFF;
    b[23] = FAT_SECTORS >> 8;
    b[24] = 0x01;
    b[26] = 0x01;
    b[28] = 0x00;
    b[29] = 0x00;
    b[36] = 'F';
    b[37] = 'A';
    b[38] = 'T';
    b[39] = '1';
    b[40] = '6';
    b[41] = ' ';
    b[42] = ' ';
    b[43] = ' ';
    b[44] = ' ';
    b[510] = 0x55;
    b[511] = 0xAA;
}

static void set_fat_entry(uint16_t cluster, uint16_t value)
{
    uint8_t *entry = fat_area + (size_t)cluster * 2u;
    entry[0] = (uint8_t)(value & 0xFF);
    entry[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void filesystem_init(void)
{
    memset(image, 0, sizeof(image));
    write_boot_sector();

    fat_area = image + RESERVED_SECTORS * SECTOR_SIZE;
    root_area = image + (RESERVED_SECTORS + FAT_COUNT * FAT_SECTORS) * SECTOR_SIZE;
    data_area = image + DATA_START * SECTOR_SIZE;

    memset(fat_area, 0, FAT_SECTORS * SECTOR_SIZE);
    memset(root_area, 0, ROOT_DIR_SECTORS * SECTOR_SIZE);
    memset(data_area, 0, (TOTAL_SECTORS - DATA_START) * SECTOR_SIZE);

    set_fat_entry(0, 0xFFF8);
    set_fat_entry(1, 0xFFFF);

    next_cluster = 2;
    max_clusters = (uint16_t)((TOTAL_SECTORS - DATA_START) / SECTORS_PER_CLUSTER);
    cluster_bytes = SECTOR_SIZE * SECTORS_PER_CLUSTER;
    root_entries_used = 0;
}

static void append_file(const char name[11], const uint8_t *data, uint32_t size)
{
    if (!data || root_entries_used >= ROOT_ENTRIES)
        return;

    uint32_t clusters_needed = (size + cluster_bytes - 1) / cluster_bytes;
    uint16_t start_cluster = 0;

    if (clusters_needed > 0)
    {
        uint32_t used = (uint32_t)(next_cluster - 2);
        if (used + clusters_needed > max_clusters)
        {
            fprintf(stderr, "[fat16_image] skipping %.*s (insufficient space)\n", 11, name);
            return;
        }

        start_cluster = next_cluster;
        uint16_t cluster = start_cluster;
        uint32_t remaining = size;
        uint8_t *dst = data_area + (size_t)(cluster - 2u) * cluster_bytes;

        while (clusters_needed-- > 0)
        {
            uint32_t to_copy = (remaining < cluster_bytes) ? remaining : cluster_bytes;
            if (to_copy > 0)
                memcpy(dst, data, to_copy);
            if (to_copy < cluster_bytes)
                memset(dst + to_copy, 0, cluster_bytes - to_copy);

            remaining -= to_copy;
            data += to_copy;

            uint16_t value = (remaining > 0) ? (uint16_t)(cluster + 1) : 0xFFFF;
            set_fat_entry(cluster, value);

            ++cluster;
            dst += cluster_bytes;
        }

        next_cluster = start_cluster + (uint16_t)((size + cluster_bytes - 1) / cluster_bytes);
    }

    uint8_t *entry = root_area + (size_t)root_entries_used * 32u;
    memset(entry, 0, 32);
    memcpy(entry, name, 11);
    entry[11] = 0x20;
    entry[26] = (uint8_t)(start_cluster & 0xFF);
    entry[27] = (uint8_t)(start_cluster >> 8);
    entry[28] = (uint8_t)(size & 0xFF);
    entry[29] = (uint8_t)((size >> 8) & 0xFF);
    entry[30] = (uint8_t)((size >> 16) & 0xFF);
    entry[31] = (uint8_t)((size >> 24) & 0xFF);

    ++root_entries_used;
}

static uint8_t *load_font_file(uint32_t *size_out)
{
    const char *paths[] = {FONT_SOURCE_PRIMARY, FONT_SOURCE_FALLBACK};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
    {
        FILE *f = fopen(paths[i], "rb");
        if (!f)
            continue;

        if (fseek(f, 0, SEEK_END) != 0)
        {
            fclose(f);
            continue;
        }
        long len = ftell(f);
        if (len <= 0)
        {
            fclose(f);
            continue;
        }
        rewind(f);

        uint8_t *buffer = (uint8_t *)malloc((size_t)len);
        if (!buffer)
        {
            fclose(f);
            continue;
        }
        size_t read = fread(buffer, 1, (size_t)len, f);
        fclose(f);
        if (read != (size_t)len)
        {
            free(buffer);
            continue;
        }

        if (size_out)
            *size_out = (uint32_t)len;

        printf("[fat16_image] embedded %s (%lu bytes)\n", paths[i], (unsigned long)len);
        return buffer;
    }

    return NULL;
}

static void build_image(void)
{
    filesystem_init();

    static const char readme_name[11] = {'R','E','A','D','M','E',' ',' ','T','X','T'};
    static const char readme_text[] = "Hello from proOS FAT16!\n";
    append_file(readme_name, (const uint8_t *)readme_text, (uint32_t)strlen(readme_text));

    uint32_t font_size = 0;
    uint8_t *font_data = load_font_file(&font_size);
    if (font_data && font_size > 0)
    {
        static const char font_name[11] = {'F','O','N','T',' ',' ',' ',' ','P','S','F'};
        append_file(font_name, font_data, font_size);
        free(font_data);
    }
}

#ifndef FAT16_IMAGE_STANDALONE
size_t fat16_image_generate(uint8_t *buffer, size_t max_len)
{
    if (!buffer || max_len < sizeof(image))
        return 0;
    build_image();
    memcpy(buffer, image, sizeof(image));
    return sizeof(image);
}
#else
int main(int argc, char **argv)
{
    build_image();
    const char *path = (argc > 1) ? argv[1] : NULL;

    if (path)
    {
        FILE *f = fopen(path, "wb");
        if (!f)
            return 1;
        fwrite(image, 1, sizeof(image), f);
        fclose(f);
    }
    else
    {
        fwrite(image, 1, sizeof(image), stdout);
    }
    return 0;
}
#endif
