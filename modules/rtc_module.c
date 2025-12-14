#include <stddef.h>
#include <stdint.h>

#include "module_api.h"

#include "devmgr.h"
#include "io.h"
#include "klog.h"
#include "ramfs.h"

MODULE_METADATA("rtc", "0.1.0", MODULE_FLAG_AUTOSTART);

static uint8_t read_cmos(uint8_t reg)
{
    outb(0x70, reg);
    io_wait();
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10u) + (value & 0x0Fu));
}

static int snapshot_timestamp(char *buffer, size_t length, size_t *out_len)
{
    if (!buffer || length < 20)
        return -1;

    uint8_t status_b = read_cmos(0x0B);
    int is_bcd = ((status_b & 0x04u) == 0);

    int attempts = 0;
    while (((read_cmos(0x0A) & 0x80u) != 0) && attempts < 1000)
        ++attempts;

    uint8_t second = read_cmos(0x00);
    uint8_t minute = read_cmos(0x02);
    uint8_t hour = read_cmos(0x04);
    uint8_t day = read_cmos(0x07);
    uint8_t month = read_cmos(0x08);
    uint8_t year = read_cmos(0x09);

    if (is_bcd)
    {
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour = bcd_to_bin(hour & 0x7Fu);
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
    }

    uint16_t full_year = 2000u + year;

    char temp[32];
    size_t pos = 0;

    uint16_t fields[] = { full_year, month, day, hour, minute, second };
    const size_t field_widths[] = { 4, 2, 2, 2, 2, 2 };
    const char separators[] = { '-', '-', ' ', ':', ':' };

    for (size_t i = 0; i < 6 && pos < sizeof(temp); ++i)
    {
        uint16_t value = fields[i];
        size_t width = field_widths[i];
        for (size_t digit = width; digit > 0; --digit)
        {
            uint16_t divisor = 1;
            for (size_t j = 1; j < digit; ++j)
                divisor *= 10u;
            uint16_t d = (uint16_t)((value / divisor) % 10u);
            if (pos < sizeof(temp))
                temp[pos++] = (char)('0' + d);
        }
        if (i < 5 && pos < sizeof(temp))
            temp[pos++] = separators[i];
    }

    if (pos >= length)
        pos = length - 1;
    for (size_t i = 0; i < pos; ++i)
        buffer[i] = temp[i];
    buffer[pos] = '\0';

    if (out_len)
        *out_len = pos;
    return 0;
}

static int rtc_start(struct device_node *node)
{
    (void)node;
    klog_info("rtc.driver: initialized");
    return 0;
}

static int rtc_read(struct device_node *node, void *buffer, size_t length, size_t *out_read)
{
    (void)node;
    if (!buffer || length == 0)
        return -1;

    size_t written = 0;
    if (snapshot_timestamp((char *)buffer, length, &written) < 0)
        return -1;

    if (out_read)
        *out_read = written;
    return 0;
}

static const struct device_ops rtc_ops = {
    rtc_start,
    NULL,
    rtc_read,
    NULL,
    NULL
};

int module_init(void)
{
    struct device_descriptor desc = {
        "rtc0",
        "clock.rtc",
        "platform0",
        &rtc_ops,
        DEVICE_FLAG_PUBLISH,
        NULL
    };

    if (devmgr_register_device(&desc, NULL) < 0)
    {
        klog_error("rtc.driver: registration failed");
        return -1;
    }

    char snapshot[32];
    size_t written = 0;
    if (snapshot_timestamp(snapshot, sizeof(snapshot), &written) == 0)
        ramfs_write_file("/dev/rtc0.now", snapshot, written);

    return 0;
}

void module_exit(void)
{
    devmgr_unregister_device("rtc0");
    ramfs_remove("/dev/rtc0.now");
}
