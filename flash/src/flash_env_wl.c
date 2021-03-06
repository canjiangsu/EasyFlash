/*
 * This file is part of the EasyFlash Library.
 *
 * Copyright (c) 2015, Armink, <armink.ztl@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Function: Environment variables operating interface. (wear leveling mode)
 * Created on: 2015-02-11
 */

#include "flash.h"
#include <string.h>
#include <stdlib.h>

#ifdef FLASH_ENV_USING_WEAR_LEVELING_MODE

/**
 * Environment variables area has 2 sections
 * 1. System section
 *    Storage Environment variables current using data section address.
 *    Units: Word. Total size: @see FLASH_ERASE_MIN_SIZE.
 * 2. Data section
 *    The data section storage environment variables's parameters and detail.
 *    When an exception has occurred on flash erase or write. The current using data section
 *    address will move to next available position. This position depends on FLASH_MIN_ERASE_SIZE.
 *    2.1 Environment variables parameters part
 *        It storage environment variables's parameters.
 *    2.2 Environment variables detail part
 *        It storage all environment variables. Storage format is key=value\0.
 *        All environment variables must be 4 bytes alignment. The remaining part must fill '\0'.
 *
 * @note Word = 4 Bytes in this file
 */

/* flash ENV parameters part index and size */
enum {
    /* data section environment variables detail part end address index */
    ENV_PARAM_PART_INDEX_END_ADDR = 0,

#ifdef FLASH_ENV_USING_CRC_CHECK
    /* data section CRC32 code index */
    ENV_PARAM_PART_INDEX_DATA_CRC,
#endif

    /* environment variables parameters part word size */
    ENV_PARAM_PART_WORD_SIZE,
    /* environment variables parameters part byte size */
    ENV_PARAM_PART_BYTE_SIZE = ENV_PARAM_PART_WORD_SIZE * 4,
};

/* default environment variables set, must be initialized by user */
static flash_env const *default_env_set = NULL;
/* default environment variables set size, must be initialized by user */
static size_t default_env_set_size = NULL;
/* flash environment variables all section total size */
static size_t env_total_size = NULL;
/* the minimum size of flash erasure */
static size_t flash_erase_min_size = NULL;
/* environment variables RAM cache */
static uint32_t *env_cache = NULL;
/* environment variables start address in flash */
static uint32_t env_start_addr = NULL;
/* current using data section address */
static uint32_t cur_using_data_addr = NULL;

static uint32_t get_env_start_addr(void);
static uint32_t get_cur_using_data_addr(void);
static uint32_t get_env_detail_addr(void);
static uint32_t get_env_detail_end_addr(void);
static void set_cur_using_data_addr(uint32_t using_data_addr);
static void set_env_detail_end_addr(uint32_t end_addr);
static FlashErrCode write_env(const char *key, const char *value);
static uint32_t *find_env(const char *key);
static size_t get_env_detail_size(void);
static FlashErrCode create_env(const char *key, const char *value);
static FlashErrCode save_cur_using_data_addr(uint32_t cur_data_addr);

#ifdef FLASH_ENV_USING_CRC_CHECK
static uint32_t calc_env_crc(void);
static bool_t env_crc_is_ok(void);
#endif

/**
 * Flash environment variables initialize.
 *
 * @param start_addr environment variables start address in flash
 * @param total_size environment variables section total size (@note must be word alignment)
 * @param erase_min_size the minimum size of flash erasure
 * @param default_env default environment variables set for user
 * @param default_env_size default environment variables set size
 *
 * @return result
 */
FlashErrCode flash_env_init(uint32_t start_addr, size_t total_size, size_t erase_min_size,
        flash_env const *default_env, size_t default_env_size) {
    FlashErrCode result = FLASH_NO_ERR;

    FLASH_ASSERT(start_addr);
    FLASH_ASSERT(total_size);
    FLASH_ASSERT(erase_min_size);
    FLASH_ASSERT(default_env);
    FLASH_ASSERT(default_env_size < total_size);
    /* must be word alignment for environment variables */
    FLASH_ASSERT(total_size % 4 == 0);
    /* make true only be initialized once */
    FLASH_ASSERT(!env_cache);

    env_start_addr = start_addr;
    env_total_size = total_size;
    flash_erase_min_size = erase_min_size;
    default_env_set = default_env;
    default_env_set_size = default_env_size;

    FLASH_DEBUG("Env start address is 0x%08X, size is %d bytes.\n", start_addr, total_size);

    /* create environment variables ram cache */
    env_cache = (uint32_t *) flash_malloc(sizeof(uint8_t) * total_size);
    FLASH_ASSERT(env_cache);

    flash_load_env();

    return result;
}

/**
 * Environment variables set default.
 *
 * @return result
 */
FlashErrCode flash_env_set_default(void){
    FlashErrCode result = FLASH_NO_ERR;
    size_t i;

    FLASH_ASSERT(env_cache);
    FLASH_ASSERT(default_env_set);
    FLASH_ASSERT(default_env_set_size);

    /* set ENV detail part end address is at ENV detail part start address */
    set_env_detail_end_addr(get_env_detail_addr());

    /* create default environment variables */
    for (i = 0; i < default_env_set_size; i++) {
        create_env(default_env_set[i].key, default_env_set[i].value);
    }

    flash_save_env();

    return result;
}

/**
 * Get environment variables start address in flash.
 *
 * @return environment variables start address in flash
 */
static uint32_t get_env_start_addr(void) {
    FLASH_ASSERT(env_start_addr);
    return env_start_addr;
}
/**
 * Get current using data section address.
 *
 * @return current using data section address
 */
static uint32_t get_cur_using_data_addr(void) {
    FLASH_ASSERT(cur_using_data_addr);
    return cur_using_data_addr;
}

/**
 * Set current using data section address.
 *
 * @param using_data_addr current using data section address
 */
static void set_cur_using_data_addr(uint32_t using_data_addr) {
    cur_using_data_addr = using_data_addr;
}

/**
 * Get environment variables detail part start address.
 *
 * @return detail part start address
 */
static uint32_t get_env_detail_addr(void) {
    FLASH_ASSERT(cur_using_data_addr);
    return cur_using_data_addr + ENV_PARAM_PART_BYTE_SIZE;
}

/**
 * Get environment variables detail part end address.
 * It's the first word in environment variables.
 *
 * @return environment variables end address
 */
static uint32_t get_env_detail_end_addr(void) {
    /* it is the first word */
    return env_cache[ENV_PARAM_PART_INDEX_END_ADDR];
}

/**
 * Set environment variables detail part end address.
 * It's the first word in environment variables.
 *
 * @param end_addr environment variables end address
 */
static void set_env_detail_end_addr(uint32_t end_addr) {
    env_cache[ENV_PARAM_PART_INDEX_END_ADDR] = end_addr;
}

/**
 * Get current environment variables detail part size.
 *
 * @return size
 */
static size_t get_env_detail_size(void) {
    return get_env_detail_end_addr() - get_env_detail_addr();
}

/**
 * Get current environment variables section total size.
 *
 * @return size
 */
size_t flash_get_env_total_size(void) {
    /* must be initialized */
    FLASH_ASSERT(env_total_size);

    return env_total_size;
}

/**
 * Get current environment variables used byte size.
 *
 * @return size
 */
uint32_t flash_get_env_used_size(void) {
    return get_env_detail_end_addr() - get_env_start_addr();
}

/**
 * Write an environment variable at the end of cache.
 *
 * @param key environment variable name
 * @param value environment variable value
 *
 * @return result
 */
static FlashErrCode write_env(const char *key, const char *value) {
    FlashErrCode result = FLASH_NO_ERR;
    uint16_t env_str_index = 0, env_str_length, i;
    char *env_str;

    /* calculate environment variables storage length, contain '=' and '\0'. */
    env_str_length = strlen(key) + strlen(value) + 2;
    if (env_str_length % 4 != 0) {
        env_str_length = (env_str_length / 4 + 1) * 4;
    }
    /* check capacity of environment variables  */
    if (env_str_length + get_env_detail_size() >= flash_get_env_total_size()) {
        return FLASH_ENV_FULL;
    }
    /* use ram to process string key=value\0 */
    env_str = flash_malloc(env_str_length * sizeof(char));
    FLASH_ASSERT(env_str);
    memset(env_str, 0, env_str_length * sizeof(char));
    /* copy key name */
    for (env_str_index = 0; env_str_index < strlen(key); env_str_index++) {
        env_str[env_str_index] = key[env_str_index];
    }
    /* copy equal sign */
    env_str[env_str_index] = '=';
    env_str_index++;
    /* copy value */
    for (i = 0; i < strlen(value); env_str_index++, i++) {
        env_str[env_str_index] = value[i];
    }

    //TODO ���ǿɷ��Ż�
    memcpy((char *) env_cache + ENV_PARAM_PART_BYTE_SIZE + get_env_detail_size(),
            (uint32_t *) env_str, env_str_length);
    set_env_detail_end_addr(get_env_detail_end_addr() + env_str_length);

    /* free ram */
    flash_free(env_str);
    env_str = NULL;

    return result;
}

/**
 * Find environment variables.
 *
 * @param key environment variables name
 *
 * @return index of environment variables in ram cache
 */
static uint32_t *find_env(const char *key) {
    uint32_t *env_cache_addr = NULL;
    char *env_start, *env_end, *env, *env_bak;

    FLASH_ASSERT(cur_using_data_addr);

    if (*key == NULL) {
        FLASH_INFO("Flash environment variables name must be not empty!\n");
        return NULL;
    }

    /* from data section start to data section end */
    env_start = (char *) ((char *) env_cache + ENV_PARAM_PART_BYTE_SIZE);
    env_end = (char *) ((char *) env_cache + ENV_PARAM_PART_BYTE_SIZE + get_env_detail_size());

    /* environment variables is null */
    if (env_start == env_end) {
        return NULL;
    }

    env = env_start;
    while (env < env_end) {
        /* storage model is key=value\0 */
        env_bak = strstr(env, key);
        /* the key name length must be equal */
        if (env_bak && (env_bak[strlen(key)] == '=')) {
            env_cache_addr = (uint32_t *) env;
            break;
        } else {
            /* next environment variables */
            env += strlen(env) + 1;
        }
    }
    return env_cache_addr;
}

/**
 * If the environment variable is not exist, create it.
 * @see flash_write_env
 *
 * @param key environment variable name
 * @param value environment variable value
 *
 * @return result
 */
static FlashErrCode create_env(const char *key, const char *value) {
    FlashErrCode result = FLASH_NO_ERR;

    FLASH_ASSERT(key);
    FLASH_ASSERT(value);

    if (*key == NULL) {
        FLASH_INFO("Flash environment variables name must be not empty!\n");
        return FLASH_ENV_NAME_ERR;
    }

    if (strstr(key, "=")) {
        FLASH_INFO("Flash environment variables name can't contain '='.\n");
        return FLASH_ENV_NAME_ERR;
    }

    /* find environment variables */
    if (find_env(key)) {
        FLASH_INFO("The name of \"%s\" is already exist.\n", key);
        return FLASH_ENV_NAME_EXIST;
    }
    /* write environment variables to flash */
    result = write_env(key, value);

    return result;
}

/**
 * Delete an environment variable in cache.
 *
 * @param key environment variable name
 *
 * @return result
 */
FlashErrCode flash_del_env(const char *key){
    FlashErrCode result = FLASH_NO_ERR;
    char *del_env_str = NULL;
    uint32_t del_env_length, remain_env_length;

    FLASH_ASSERT(key);
    FLASH_ASSERT(env_cache);

    if (*key == NULL) {
        FLASH_INFO("Flash environment variables name must be not NULL!\n");
        return FLASH_ENV_NAME_ERR;
    }

    if (strstr(key, "=")) {
        FLASH_INFO("Flash environment variables name or value can't contain '='.\n");
        return FLASH_ENV_NAME_ERR;
    }

    /* find environment variables */
    del_env_str = (char *) find_env(key);
    if (!del_env_str) {
        FLASH_INFO("Not find \"%s\" in environment variables.\n", key);
        return FLASH_ENV_NAME_ERR;
    }
    del_env_length = strlen(del_env_str);
    /* '\0' also must be as environment variable length */
    del_env_length ++;
    /* the address must multiple of 4 */
    if (del_env_length % 4 != 0) {
        del_env_length = (del_env_length / 4 + 1) * 4;
    }
    /* calculate remain environment variables length */
    remain_env_length = get_env_detail_size() - ((uint32_t) del_env_str - (uint32_t) env_cache);
    /* remain environment variables move forward */
    memcpy(del_env_str, del_env_str + del_env_length, remain_env_length);
    /* reset environment variables detail part end address */
    set_env_detail_end_addr(get_env_detail_end_addr() - del_env_length);

    return result;
}

/**
 * Set an environment variable. If it value is empty, delete it.
 * If not find it in environment variables table, then create it.
 *
 * @param key environment variable name
 * @param value environment variable value
 *
 * @return result
 */
FlashErrCode flash_set_env(const char *key, const char *value) {
    FlashErrCode result = FLASH_NO_ERR;

    FLASH_ASSERT(env_cache);

    /* if ENV value is empty, delete it */
    if (*value == NULL) {
        result = flash_del_env(key);
    } else {
        /* if find this variables, then delete it and recreate it  */
        if (find_env(key)) {
            result = flash_del_env(key);
        }
        if (result == FLASH_NO_ERR) {
            result = create_env(key, value);
        }
    }
    return result;
}

/**
 * Get an environment variable value by key name.
 *
 * @param key environment variable name
 *
 * @return value
 */
char *flash_get_env(const char *key) {
    uint32_t *env_cache_addr = NULL;
    char *value = NULL;

    FLASH_ASSERT(env_cache);

    /* find environment variables */
    env_cache_addr = find_env(key);
    if (env_cache_addr == NULL) {
        return NULL;
    }
    /* get value address */
    value = strstr((char *) env_cache_addr, "=");
    if (value != NULL) {
        /* the equal sign next character is value */
        value++;
    }
    return value;
}
/**
 * Print environment variables.
 */
void flash_print_env(void) {
    uint32_t *env_cache_detail_addr = env_cache + ENV_PARAM_PART_WORD_SIZE,
            *env_cache_end_addr =
            (uint32_t *) (env_cache + ENV_PARAM_PART_WORD_SIZE + get_env_detail_size() / 4);
    uint8_t j;
    char c;

    FLASH_ASSERT(env_cache);

    for (; env_cache_detail_addr < env_cache_end_addr; env_cache_detail_addr += 1) {
        for (j = 0; j < 4; j++) {
            c = (*env_cache_detail_addr) >> (8 * j);
            flash_print("%c", c);
            if (c == NULL) {
                flash_print("\n");
                break;
            }
        }
    }
    flash_print("\nEnvironment variables size: %ld/%ld bytes, mode: wear leveling.\n",
            flash_get_env_used_size(), flash_get_env_total_size());
}

/**
 * Load flash environment variables to ram.
 */
void flash_load_env(void) {
    uint32_t *env_cache_bak, env_end_addr, using_data_addr;

    FLASH_ASSERT(env_cache);

    /* read current using data section address */
    flash_read(get_env_start_addr(), &using_data_addr, 4);
    /* if environment variables is not initialize or flash has dirty data, set default for it */
    if ((using_data_addr == 0xFFFFFFFF) || (using_data_addr > env_start_addr + env_total_size)) {
        /* initialize current using data section address */
        set_cur_using_data_addr(get_env_start_addr() + flash_erase_min_size);
        /* save current using data section address to flash*/
        save_cur_using_data_addr(get_cur_using_data_addr());
        /* set default environment variables */
        flash_env_set_default();
    } else {
        /* set current using data section address */
        set_cur_using_data_addr(using_data_addr);
        /* read environment variables detail part end address from flash */
        flash_read(get_cur_using_data_addr() + ENV_PARAM_PART_INDEX_END_ADDR * 4, &env_end_addr, 4);
        /* if environment variables end address has error, set default for environment variables */
        if (env_end_addr > env_start_addr + env_total_size) {
            flash_env_set_default();
        } else {
            /* set environment variables detail part end address */
            set_env_detail_end_addr(env_end_addr);

            env_cache_bak = env_cache + ENV_PARAM_PART_WORD_SIZE;
            /* read all environment variables from flash */
            flash_read(get_env_detail_addr(), env_cache_bak, get_env_detail_size());

#ifdef FLASH_ENV_USING_CRC_CHECK
            /* read environment variables CRC code from flash */
            flash_read(get_cur_using_data_addr() + ENV_PARAM_PART_INDEX_DATA_CRC * 4,
                    &env_cache[ENV_PARAM_PART_INDEX_DATA_CRC], 4);

            /* if environment variables CRC32 check is fault, set default for it */
            if (!env_crc_is_ok()) {
                FLASH_INFO("Warning: Environment variables CRC check failed. Set it to default.\n");
                flash_env_set_default();
            }
        }
#endif

    }
}

/**
 * Save environment variables to flash.
 */
FlashErrCode flash_save_env(void) {
    FlashErrCode result = FLASH_NO_ERR;
    uint32_t cur_data_addr_bak = get_cur_using_data_addr(), move_offset_addr;
    size_t env_detail_size = get_env_detail_size();

    FLASH_ASSERT(env_cache);

    /* wear leveling process, automatic move environment variables to next available position */
    while (get_cur_using_data_addr() + env_detail_size
            < get_env_start_addr() + flash_get_env_total_size()) {

#ifdef FLASH_ENV_USING_CRC_CHECK
    /* calculate and cache CRC32 code */
    env_cache[ENV_PARAM_PART_INDEX_DATA_CRC] = calc_env_crc();
#endif
        /* erase environment variables */
        result = flash_erase(get_cur_using_data_addr(), ENV_PARAM_PART_BYTE_SIZE + env_detail_size);
        switch (result) {
        case FLASH_NO_ERR: {
            FLASH_INFO("Erased environment variables OK.\n");
            break;
        }
        case FLASH_ERASE_ERR: {
            FLASH_INFO("Warning: Erased environment variables fault!\n");
            FLASH_INFO("Moving environment variables to next available position.\n");
            /* calculate move offset address */
            move_offset_addr = (env_detail_size / flash_erase_min_size + 1) * flash_erase_min_size;
            /* calculate and set next available data section address */
            set_cur_using_data_addr(get_cur_using_data_addr() + move_offset_addr);
            /* calculate and set next available environment variables detail part end address */
            set_env_detail_end_addr(get_env_detail_end_addr() + move_offset_addr);
            continue;
        }
        }
        /* write environment variables to flash */
        result = flash_write(get_cur_using_data_addr(), env_cache,
                ENV_PARAM_PART_BYTE_SIZE + env_detail_size);
        switch (result) {
        case FLASH_NO_ERR: {
            FLASH_INFO("Saved environment variables OK.\n");
            break;
        }
        case FLASH_WRITE_ERR: {
            FLASH_INFO("Warning: Saved environment variables fault!\n");
            FLASH_INFO("Moving environment variables to next available position.\n");
            /* calculate move offset address */
            move_offset_addr = (env_detail_size / flash_erase_min_size + 1) * flash_erase_min_size;
            /* calculate and set next available data section address */
            set_cur_using_data_addr(get_cur_using_data_addr() + move_offset_addr);
            /* calculate and set next available environment variables detail part end address */
            set_env_detail_end_addr(get_env_detail_end_addr() + move_offset_addr);
            continue;
        }
        }
        /* save environment variables success */
        if (result == FLASH_NO_ERR) {
            break;
        }
    }

    if (get_cur_using_data_addr() + env_detail_size
            < get_env_start_addr() + flash_get_env_total_size()) {
        /* current using data section address has changed, save it */
        if (get_cur_using_data_addr() != cur_data_addr_bak) {
            save_cur_using_data_addr(get_cur_using_data_addr());
        }
    } else {
        result = FLASH_ENV_FULL;
        FLASH_INFO("Error: The flash has no available space to save environment variables.\n");
        /* clear current using data section address on flash */
        save_cur_using_data_addr(0xFFFFFFFF);
    }

    return result;
}

#ifdef FLASH_ENV_USING_CRC_CHECK
/**
 * Calculate the cached environment variables CRC32 value.
 *
 * @return CRC32 value
 */
static uint32_t calc_env_crc(void) {
    uint32_t crc32 = 0;

    extern uint32_t calc_crc32(uint32_t crc, const void *buf, size_t size);
    /* Calculate the environment variables end address and all environment variables data CRC32.
     * The 4 is environment variables end address bytes size. */
    crc32 = calc_crc32(crc32, &env_cache[ENV_PARAM_PART_INDEX_END_ADDR], 4);
    crc32 = calc_crc32(crc32, &env_cache[ENV_PARAM_PART_WORD_SIZE], get_env_detail_size());
    FLASH_DEBUG("Calculate Env CRC32 number is 0x%08X.\n", crc32);

    return crc32;
}
#endif

#ifdef FLASH_ENV_USING_CRC_CHECK
/**
 * Check the environment variables CRC32
 *
 * @return true is ok
 */
static bool_t env_crc_is_ok(void) {
    if (calc_env_crc() == env_cache[ENV_PARAM_PART_INDEX_DATA_CRC]) {
        FLASH_DEBUG("Verify Env CRC32 result is OK.\n");
        return TRUE;
    } else {
        return FALSE;
    }
}
#endif

/**
 * Save current using data section address to flash.
 *
 * @param cur_data_addr current using data section address
 *
 * @return result
 */
static FlashErrCode save_cur_using_data_addr(uint32_t cur_data_addr) {
    FlashErrCode result = FLASH_NO_ERR;
    /* erase environment variables system section */
    result = flash_erase(get_env_start_addr(), 4);
    if (result == FLASH_NO_ERR) {
        /* write current using data section address to flash */
        result = flash_write(get_env_start_addr(), &cur_data_addr, 4);
        if (result == FLASH_WRITE_ERR) {
            FLASH_INFO("Error: Write system section fault!\n");
            FLASH_INFO("Note: The environment variables can not be used.\n");
        }
    } else {
        FLASH_INFO("Error: Erased system section fault!\n");
        FLASH_INFO("Note: The environment variables can not be used\n");
    }
    return result;
}
#endif
