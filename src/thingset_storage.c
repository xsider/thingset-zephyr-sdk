/*
 * Copyright (c) The ThingSet Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "thingset/sdk.h"
#include "thingset/storage.h"

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdio.h>

LOG_MODULE_REGISTER(thingset_storage, CONFIG_LOG_DEFAULT_LEVEL);

#if defined(CONFIG_EEPROM) && defined(CONFIG_SOC_FAMILY_STM32)

#include <stm32_ll_bus.h>
#include <zephyr/drivers/eeprom.h>

/*
 * EEPROM header bytes:
 * - 0-1: Data objects version number
 * - 2-3: Number of data bytes
 * - 4-7: CRC32
 *
 * Data starts from byte 8
 */
#define EEPROM_HEADER_SIZE 8

uint32_t _calc_crc(const uint8_t *buf, size_t len)
{
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_CRC);

    // we keep standard polynomial 0x04C11DB7 (same for STM32L0 and STM32F0)
    // and we don't care for endianness here
    CRC->CR |= CRC_CR_RESET;
    for (size_t i = 0; i < len; i += 4) {
        // printf("CRC buf: %.8x, CRC->DR: %.8x\n", *((uint32_t*)&buf[i]), CRC->DR);
        size_t remaining_bytes = len - i;
        if (remaining_bytes >= 4) {
            CRC->DR = *((uint32_t *)&buf[i]);
        }
        else {
            // ignore bytes >= len if len is not a multiple of 4
            CRC->DR = *((uint32_t *)&buf[i]) & (0xFFFFFFFFU >> (32 - remaining_bytes * 8));
        }
    }

    LL_AHB1_GRP1_DisableClock(LL_AHB1_GRP1_PERIPH_CRC);

    return CRC->DR;
}

void thingset_storage_load()
{
    int err;

    const struct device *eeprom_dev = device_get_binding("EEPROM_0");

    uint8_t buf_header[EEPROM_HEADER_SIZE] = {};
    err = eeprom_read(eeprom_dev, 0, buf_header, EEPROM_HEADER_SIZE);
    if (err != 0) {
        LOG_ERR("EEPROM read error %d", err);
        return;
    }
    uint16_t version = *((uint16_t *)&buf_header[0]);
    uint16_t len = *((uint16_t *)&buf_header[2]);
    uint32_t crc = *((uint32_t *)&buf_header[4]);

    LOG_DBG("EEPROM header restore: ver %d, len %d, CRC %.8x", version, len, (unsigned int)crc);
    LOG_DBG("Header: %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x", buf_header[0], buf_header[1],
            buf_header[2], buf_header[3], buf_header[4], buf_header[5], buf_header[6],
            buf_header[7]);

    struct shared_buffer *sbuf = thingset_sdk_shared_buffer();
    if (version == CONFIG_THINGSET_STORAGE_DATA_VERSION && len <= sbuf->size) {
        k_sem_take(&sbuf->lock, K_FOREVER);

        err = eeprom_read(eeprom_dev, EEPROM_HEADER_SIZE, sbuf->data, len);

        // printf("Data (len=%d): ", len);
        // for (int i = 0; i < len; i++) printf("%.2x ", buf[i]);

        if (_calc_crc(sbuf->data, len) == crc) {
            int status = ts_bin_import(&ts, sbuf->data, sbuf->size, TS_WRITE_MASK, SUBSET_NVM);
            LOG_INF("EEPROM read and data objects updated, ThingSet result: 0x%x", status);
        }
        else {
            LOG_ERR("EEPROM data CRC invalid, expected 0x%x (data_len = %d)", (unsigned int)crc,
                    len);
        }
        k_sem_give(&sbuf->lock);
    }
    else {
        LOG_INF("EEPROM empty or data layout version changed");
    }
}

void thingset_storage_save()
{
    int err;

    const struct device *eeprom_dev = device_get_binding("EEPROM_0");
    struct shared_buffer *sbuf = thingset_sdk_shared_buffer();

    k_sem_take(&sbuf->lock, K_FOREVER);

    int len = ts_bin_export(&ts, sbuf->data + EEPROM_HEADER_SIZE, sbuf->size - EEPROM_HEADER_SIZE,
                            SUBSET_NVM);
    uint32_t crc = _calc_crc(sbuf->data + EEPROM_HEADER_SIZE, len);

    *((uint16_t *)&sbuf->data[0]) = (uint16_t)CONFIG_THINGSET_STORAGE_DATA_VERSION;
    *((uint16_t *)&sbuf->data[2]) = (uint16_t)(len);
    *((uint32_t *)&sbuf->data[4]) = crc;

    // printf("Data (len=%d): ", len);
    // for (int i = 0; i < len; i++) printf("%.2x ", buf[i + EEPROM_HEADER_SIZE]);

    LOG_DBG("Header: %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x", sbuf->data[0], sbuf->data[1],
            sbuf->data[2], sbuf->data[3], sbuf->data[4], sbuf->data[5], sbuf->data[6],
            sbuf->data[7]);

    if (len == 0) {
        LOG_ERR("EEPROM data could not be stored. ThingSet error (len = %d)", len);
    }
    else {
        err = eeprom_write(eeprom_dev, 0, sbuf->data, len + EEPROM_HEADER_SIZE);
        if (err == 0) {
            LOG_INF("EEPROM data successfully stored");
        }
        else {
            LOG_ERR("EEPROM write error %d", err);
        }
    }
    k_sem_give(&sbuf->lock);
}

#elif defined(CONFIG_NVS)

#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>

/*
 * NVS header bytes:
 * - 0-1: Data objects version number
 *
 * Data starts from byte 2
 */
#define NVS_HEADER_SIZE 2

#define NVS_PARTITION storage_partition

#define THINGSET_DATA_ID 1

static struct nvs_fs fs;
static bool nvs_initialized = false;

static int data_storage_init()
{
    int err;
    struct flash_pages_info page_info;

    fs.flash_device = FIXED_PARTITION_DEVICE(NVS_PARTITION);
    if (!device_is_ready(fs.flash_device)) {
        LOG_ERR("Flash device not ready");
        return -ENODEV;
    }
    fs.offset = FIXED_PARTITION_OFFSET(NVS_PARTITION);
    err = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &page_info);
    if (err) {
        LOG_ERR("Unable to get flash page info");
        return err;
    }
    fs.sector_size = page_info.size;
    fs.sector_count = FIXED_PARTITION_SIZE(NVS_PARTITION) / page_info.size;

    err = nvs_mount(&fs);
    if (err) {
        LOG_ERR("NVS mount failed: %d", err);
        return err;
    }

    nvs_initialized = true;
    return 0;
}

void thingset_storage_load()
{
    if (!nvs_initialized) {
        int err = data_storage_init();
        if (err) {
            return;
        }
    }

    struct shared_buffer *sbuf = thingset_sdk_shared_buffer();
    k_sem_take(&sbuf->lock, K_FOREVER);

    int num_bytes = nvs_read(&fs, THINGSET_DATA_ID, &sbuf->data, sbuf->size);
    if (num_bytes < 0) {
        LOG_INF("NVS empty (read error %d)", num_bytes);
        goto out;
    }

    uint16_t version = *((uint16_t *)&sbuf->data[0]);
    if (version == CONFIG_THINGSET_STORAGE_DATA_VERSION && num_bytes <= sbuf->size) {
        int status = ts_bin_import(&ts, sbuf->data + NVS_HEADER_SIZE, num_bytes - NVS_HEADER_SIZE,
                                   TS_WRITE_MASK, SUBSET_NVM);

        LOG_INF("NVS read and data objects updated, ThingSet result: 0x%x", status);
    }
    else {
        LOG_INF("NVS data layout version changed");
    }

out:
    k_sem_give(&sbuf->lock);
}

void thingset_storage_save()
{
    if (!nvs_initialized) {
        int err = data_storage_init();
        if (err) {
            return;
        }
    }

    struct shared_buffer *sbuf = thingset_sdk_shared_buffer();
    k_sem_take(&sbuf->lock, K_FOREVER);

    *((uint16_t *)&sbuf->data[0]) = (uint16_t)CONFIG_THINGSET_STORAGE_DATA_VERSION;

    int len =
        ts_bin_export(&ts, sbuf->data + NVS_HEADER_SIZE, sbuf->size - NVS_HEADER_SIZE, SUBSET_NVM);

    if (len == 0) {
        LOG_ERR("NVS data could not be stored. ThingSet error (len = %d)", len);
    }
    else {
        int ret = nvs_write(&fs, THINGSET_DATA_ID, &sbuf->data, len + NVS_HEADER_SIZE);
        if (ret == len + NVS_HEADER_SIZE) {
            LOG_DBG("NVS data successfully stored");
        }
        else if (ret == 0) {
            LOG_DBG("NVS data unchanged");
        }
        else {
            LOG_ERR("NVS write error %d", ret);
        }
    }

    k_sem_give(&sbuf->lock);
}

#endif /* CONFIG_NVS */

#ifdef CONFIG_THINGSET_STORAGE_REGULAR

static struct k_work_delayable storage_work;

static void regular_storage_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);

    thingset_storage_save();

    k_work_reschedule(dwork, K_HOURS(CONFIG_THINGSET_STORAGE_INTERVAL));
}

static int thingset_storage_init(const struct device *dev)
{
    k_work_init_delayable(&storage_work, regular_storage_handler);

    k_work_reschedule(&storage_work, K_HOURS(CONFIG_THINGSET_STORAGE_INTERVAL));

    return 0;
}

SYS_INIT(thingset_storage_init, APPLICATION, THINGSET_INIT_PRIORITY_STORAGE);

#endif /* CONFIG_THINGSET_STORAGE_REGULAR */