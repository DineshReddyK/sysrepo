/**
 * @file shm_main.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief main SHM routines
 *
 * @copyright
 * Copyright 2018 Deutsche Telekom AG.
 * Copyright 2018 - 2019 CESNET, z.s.p.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "common.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

/**
 * @brief Item holding information about a SHM object for debug printing.
 */
struct shm_item {
    off_t start;
    size_t size;
    char *name;
};

/**
 * @brief Collect data dependencies for printing.
 *
 * @param[in] ext_shm_addr Ext SHM mapping address.
 * @param[in] data_deps Data dependencies to be printed.
 * @param[in] data_dep_count Data dependency count.
 * @param[in] data_dep_name Name of these data dependencies to be printed.
 * @param[in] mod_name Module with these dependencies.
 * @param[in,out] items Array of print items.
 * @param[in,out] item_count Count of print items.
 */
static void
sr_shmmain_print_data_deps(char *ext_shm_addr, sr_mod_data_dep_t *data_deps, uint16_t data_dep_count,
        const char *data_dep_name, const char *mod_name, struct shm_item **items, size_t *item_count)
{
    uint16_t i;

    if (data_dep_count) {
        /* add data dep array */
        *items = sr_realloc(*items, (*item_count + 1) * sizeof **items);
        (*items)[*item_count].start = ((char *)data_deps) - ext_shm_addr;
        (*items)[*item_count].size = data_dep_count * sizeof *data_deps;
        asprintf(&((*items)[*item_count].name), "%s (%u, mod \"%s\")", data_dep_name, data_dep_count, mod_name);
        ++(*item_count);

        for (i = 0; i < data_dep_count; ++i) {
            if (data_deps[i].xpath) {
                /* add xpath */
                *items = sr_realloc(*items, (*item_count + 1) * sizeof **items);
                (*items)[*item_count].start = data_deps[i].xpath;
                (*items)[*item_count].size = sr_strshmlen(ext_shm_addr + data_deps[i].xpath);
                asprintf(&((*items)[*item_count].name), "%s xpath (\"%s\", mod \"%s\")", data_dep_name,
                        ext_shm_addr + data_deps[i].xpath, mod_name);
                ++(*item_count);
            }
        }
    }
}

/**
 * @brief Comparator for SHM print item qsort.
 *
 * @param[in] ptr1 First value pointer.
 * @param[in] ptr2 Second value pointer.
 * @return Less than, equal to, or greater than 0 if the first value is found
 * to be less than, equal to, or greater to the second value.
 */
static int
sr_shmmain_print_cmp(const void *ptr1, const void *ptr2)
{
    struct shm_item *item1, *item2;

    item1 = (struct shm_item *)ptr1;
    item2 = (struct shm_item *)ptr2;

    assert(item1->start != item2->start);
    assert((item1->start > item2->start) || (item1->start + item1->size <= (unsigned)item2->start));
    assert((item1->start < item2->start) || (item2->start + item2->size <= (unsigned)item1->start));

    if (item1->start < item2->start) {
        return -1;
    }
    return 1;
}

void
sr_shmmain_ext_print(sr_shm_t *shm_main, char *ext_shm_addr, size_t ext_shm_size)
{
    sr_mod_t *shm_mod;
    off_t *features, cur_off;
    sr_mod_op_dep_t *op_deps;
    sr_mod_change_sub_t *change_subs;
    sr_mod_oper_sub_t *oper_subs;
    sr_rpc_t *shm_rpc;
    sr_rpc_sub_t *rpc_subs;
    sr_main_shm_t *main_shm;
    sr_conn_state_t *conn_s;
    struct shm_item *items;
    size_t i, j, item_count, printed;
    sr_datastore_t ds;
    int msg_len = 0;
    char *msg;

    if ((stderr_ll < SR_LL_DBG) && (syslog_ll < SR_LL_DBG)) {
        /* nothing to print */
        return;
    }

    /* add wasted */
    item_count = 0;
    items = malloc(sizeof *items);
    items[item_count].start = 0;
    items[item_count].size = sizeof(size_t);
    asprintf(&(items[item_count].name), "ext wasted %lu", *((size_t *)ext_shm_addr));
    ++item_count;

    main_shm = (sr_main_shm_t *)shm_main->addr;

    if (main_shm->conn_state.conns) {
        /* add connection state */
        items = sr_realloc(items, (item_count + 1) * sizeof *items);
        items[item_count].start = main_shm->conn_state.conns;
        items[item_count].size = main_shm->conn_state.conn_count * sizeof *conn_s;
        asprintf(&(items[item_count].name), "connections (%u)", main_shm->conn_state.conn_count);
        ++item_count;
    }

    conn_s = (sr_conn_state_t *)(ext_shm_addr + main_shm->conn_state.conns);
    for (i = 0; i < main_shm->conn_state.conn_count; ++i) {
        if (conn_s[i].evpipes) {
            /* add connection evpipes */
            items = sr_realloc(items, (item_count + 1) * sizeof *items);
            items[item_count].start = conn_s[i].evpipes;
            items[item_count].size = conn_s[i].evpipe_count * sizeof(uint32_t);
            asprintf(&(items[item_count].name), "evpipes (%u, conn %p)", conn_s[i].evpipe_count, (void *)conn_s[i].conn_ctx);
            ++item_count;
        }
    }

    if (main_shm->rpc_sub_count) {
        /* add RPCs */
        items = sr_realloc(items, (item_count + 1) * sizeof *items);
        items[item_count].start = main_shm->rpc_subs;
        items[item_count].size = main_shm->rpc_sub_count * sizeof *shm_rpc;
        asprintf(&(items[item_count].name), "rpcs (%u)", main_shm->rpc_sub_count);
        ++item_count;

        shm_rpc = (sr_rpc_t *)(ext_shm_addr + main_shm->rpc_subs);
        for (i = 0; i < main_shm->rpc_sub_count; ++i) {
            /* add op_path */
            items = sr_realloc(items, (item_count + 1) * sizeof *items);
            items[item_count].start = shm_rpc[i].op_path;
            items[item_count].size = sr_strshmlen(ext_shm_addr + shm_rpc[i].op_path);
            asprintf(&(items[item_count].name), "rpc op_path (\"%s\")", ext_shm_addr + shm_rpc[i].op_path);
            ++item_count;

            if (shm_rpc[i].sub_count) {
                /* add RPC subscriptions */
                items = sr_realloc(items, (item_count + 1) * sizeof *items);
                items[item_count].start = shm_rpc[i].subs;
                items[item_count].size = shm_rpc[i].sub_count * sizeof *rpc_subs;
                asprintf(&(items[item_count].name), "rpc subs (%u, op_path \"%s\")", shm_rpc[i].sub_count,
                        ext_shm_addr + shm_rpc[i].op_path);
                ++item_count;

                rpc_subs = (sr_rpc_sub_t *)(ext_shm_addr + shm_rpc[i].subs);
                for (j = 0; j < shm_rpc[i].sub_count; ++j) {
                    /* add RPC subscription XPath */
                    items = sr_realloc(items, (item_count + 1) * sizeof *items);
                    items[item_count].start = rpc_subs[j].xpath;
                    items[item_count].size = sr_strshmlen(ext_shm_addr + rpc_subs[j].xpath);
                    asprintf(&(items[item_count].name), "rpc sub xpath (\"%s\", op_path \"%s\")",
                            ext_shm_addr + rpc_subs[j].xpath, ext_shm_addr + shm_rpc[i].op_path);
                    ++item_count;
                }
            }
        }
    }

    SR_SHM_MOD_FOR(shm_main->addr, shm_main->size, shm_mod) {
        /* add module name */
        items = sr_realloc(items, (item_count + 1) * sizeof *items);
        items[item_count].start = shm_mod->name;
        items[item_count].size = sr_strshmlen(ext_shm_addr + shm_mod->name);
        asprintf(&(items[item_count].name), "module name (\"%s\")", ext_shm_addr + shm_mod->name);
        ++item_count;

        if (shm_mod->features) {
            /* add features array */
            items = sr_realloc(items, (item_count + 1) * sizeof *items);
            items[item_count].start = shm_mod->features;
            items[item_count].size = shm_mod->feat_count * sizeof(off_t);
            asprintf(&(items[item_count].name), "features (%u, mod \"%s\")", shm_mod->feat_count,
                    ext_shm_addr + shm_mod->name);
            ++item_count;

            /* add feature names */
            features = (off_t *)(ext_shm_addr + shm_mod->features);
            for (i = 0; i < shm_mod->feat_count; ++i) {
                items = sr_realloc(items, (item_count + 1) * sizeof *items);
                items[item_count].start = features[i];
                items[item_count].size = sr_strshmlen(ext_shm_addr + features[i]);
                asprintf(&(items[item_count].name), "feature name (\"%s\", mod \"%s\")", ext_shm_addr + features[i],
                        ext_shm_addr + shm_mod->name);
                ++item_count;
            }
        }

        /* add data deps */
        sr_shmmain_print_data_deps(ext_shm_addr, (sr_mod_data_dep_t *)(ext_shm_addr + shm_mod->data_deps),
                shm_mod->data_dep_count, "data deps", ext_shm_addr + shm_mod->name, &items, &item_count);

        if (shm_mod->inv_data_dep_count) {
            /* add inverse data deps */
            items = sr_realloc(items, (item_count + 1) * sizeof *items);
            items[item_count].start = shm_mod->inv_data_deps;
            items[item_count].size = shm_mod->inv_data_dep_count * sizeof(off_t);
            asprintf(&(items[item_count].name), "inv data deps (%u, mod \"%s\")", shm_mod->inv_data_dep_count,
                    ext_shm_addr + shm_mod->name);
            ++item_count;
        }

        if (shm_mod->op_dep_count) {
            /* add op deps array */
            items = sr_realloc(items, (item_count + 1) * sizeof *items);
            items[item_count].start = shm_mod->op_deps;
            items[item_count].size = shm_mod->op_dep_count * sizeof(sr_mod_op_dep_t);
            asprintf(&(items[item_count].name), "op deps (%u, mod \"%s\")", shm_mod->op_dep_count,
                    ext_shm_addr + shm_mod->name);
            ++item_count;

            /* add op deps */
            op_deps = (sr_mod_op_dep_t *)(ext_shm_addr + shm_mod->op_deps);
            for (i = 0; i < shm_mod->op_dep_count; ++i) {
                /* add xpath */
                items = sr_realloc(items, (item_count + 1) * sizeof *items);
                items[item_count].start = op_deps[i].xpath;
                items[item_count].size = sr_strshmlen(ext_shm_addr + op_deps[i].xpath);
                asprintf(&(items[item_count].name), "op dep xpath (\"%s\", mod \"%s\")", ext_shm_addr + op_deps[i].xpath,
                        ext_shm_addr + shm_mod->name);
                ++item_count;

                /* add op dep input data deps */
                sr_shmmain_print_data_deps(ext_shm_addr, (sr_mod_data_dep_t *)(ext_shm_addr + op_deps[i].in_deps),
                        op_deps[i].in_dep_count, "op input data deps", ext_shm_addr + shm_mod->name, &items, &item_count);

                /* add op dep output data deps */
                sr_shmmain_print_data_deps(ext_shm_addr, (sr_mod_data_dep_t *)(ext_shm_addr + op_deps[i].out_deps),
                        op_deps[i].out_dep_count, "op output data deps", ext_shm_addr + shm_mod->name, &items, &item_count);
            }
        }

        for (ds = 0; ds < SR_DS_COUNT; ++ds) {
            if (shm_mod->change_sub[ds].sub_count) {
                /* add change subscriptions */
                items = sr_realloc(items, (item_count + 1) * sizeof *items);
                items[item_count].start = shm_mod->change_sub[ds].subs;
                items[item_count].size = shm_mod->change_sub[ds].sub_count * sizeof *change_subs;
                asprintf(&(items[item_count].name), "%s change subs (%u, mod \"%s\")", sr_ds2str(ds),
                        shm_mod->change_sub[ds].sub_count, ext_shm_addr + shm_mod->name);
                ++item_count;

                /* add xpaths */
                change_subs = (sr_mod_change_sub_t *)(ext_shm_addr + shm_mod->change_sub[ds].subs);
                for (i = 0; i < shm_mod->change_sub[ds].sub_count; ++i) {
                    if (change_subs[i].xpath) {
                        items = sr_realloc(items, (item_count + 1) * sizeof *items);
                        items[item_count].start = change_subs[i].xpath;
                        items[item_count].size = sr_strshmlen(ext_shm_addr + change_subs[i].xpath);
                        asprintf(&(items[item_count].name), "%s change sub xpath (\"%s\", mod \"%s\")", sr_ds2str(ds),
                                ext_shm_addr + change_subs[i].xpath, ext_shm_addr + shm_mod->name);
                        ++item_count;
                    }
                }
            }
        }

        if (shm_mod->oper_sub_count) {
            /* add DP subscriptions */
            items = sr_realloc(items, (item_count + 1) * sizeof *items);
            items[item_count].start = shm_mod->oper_subs;
            items[item_count].size = shm_mod->oper_sub_count * sizeof *oper_subs;
            asprintf(&(items[item_count].name), "oper subs (%u, mod \"%s\")", shm_mod->oper_sub_count,
                    ext_shm_addr + shm_mod->name);
            ++item_count;

            /* add xpaths */
            oper_subs = (sr_mod_oper_sub_t *)(ext_shm_addr + shm_mod->oper_subs);
            for (i = 0; i < shm_mod->oper_sub_count; ++i) {
                items = sr_realloc(items, (item_count + 1) * sizeof *items);
                items[item_count].start = oper_subs[i].xpath;
                items[item_count].size = sr_strshmlen(ext_shm_addr + oper_subs[i].xpath);
                asprintf(&(items[item_count].name), "oper sub xpath (\"%s\", mod \"%s\")",
                        ext_shm_addr + oper_subs[i].xpath, ext_shm_addr + shm_mod->name);
                ++item_count;
            }
        }
    }

    /* sort all items */
    qsort(items, item_count, sizeof *items, sr_shmmain_print_cmp);

    /* print it */
    cur_off = 0;
    printed = 0;
    for (i = 0; i < item_count; ++i) {
        if (items[i].start > cur_off) {
            printed += sr_sprintf(&msg, &msg_len, printed, "%04ld-%04ld: (wasted %ld)\n",
                    cur_off, items[i].start, items[i].start - cur_off);
            cur_off = items[i].start;
        }
        printed += sr_sprintf(&msg, &msg_len, printed, "%04ld-%04ld: %s\n",
                items[i].start, items[i].start + items[i].size, items[i].name);
        cur_off += items[i].size;

        free(items[i].name);
    }
    if ((unsigned)cur_off < ext_shm_size) {
        printed += sr_sprintf(&msg, &msg_len, printed, "%04ld-%04ld: (wasted %ld)\n",
                cur_off, ext_shm_size, ext_shm_size - cur_off);
    }

    free(items);

    SR_LOG_DBG("#SHM:\n%s", msg);
    free(msg);
}

/**
 * @brief Copy data deps array from ext SHM to buffer to defragment it.
 *
 * @param[in] shm_main Main SHM.
 * @param[in] ext_shm_addr Ext SHM mapping address.
 * @param[in] data_deps SHM ext offset of data deps.
 * @param[in] data_dep_count Data dep count.
 * @param[in] ext_buf SHM ext buffer.
 * @param[in,out] ext_buf_cur Current ext SHM buffer position.
 * @return Memory offset of the copy.
 */
static off_t
sr_shmmain_defrag_copy_data_deps(sr_shm_t *shm_main, char *ext_shm_addr, off_t data_deps, uint16_t data_dep_count,
        char *ext_buf, char **ext_buf_cur)
{
    sr_mod_data_dep_t *new_data_deps, *old_data_deps;
    sr_mod_t *ref_mod;
    char *str;
    uint16_t i;
    off_t ret;

    if (!data_deps && !data_dep_count) {
        /* no data dependencies */
        return 0;
    }
    assert(data_deps && data_dep_count);

    old_data_deps = (sr_mod_data_dep_t *)(ext_shm_addr + data_deps);

    /* current offset */
    ret = *ext_buf_cur - ext_buf;

    /* allocate array */
    new_data_deps = (sr_mod_data_dep_t *)(ext_buf + sr_shmcpy(ext_buf, ext_shm_addr + data_deps,
            data_dep_count * sizeof *new_data_deps, ext_buf_cur));

    /* copy all items */
    for (i = 0; i < data_dep_count; ++i) {
        /* assign module */
        if (old_data_deps[i].module) {
            ref_mod = sr_shmmain_find_module(shm_main, ext_buf, ext_shm_addr + old_data_deps[i].module, 0);
            new_data_deps[i].module = ref_mod->name;
        }

        /* copy xpath */
        if (old_data_deps[i].xpath) {
            str = ext_shm_addr + old_data_deps[i].xpath;
            new_data_deps[i].xpath = sr_shmstrcpy(ext_buf, str, ext_buf_cur);
        }
    }

    return ret;
}

/**
 * @brief Copy inverse data deps array from main SHM to memory to defragment it.
 *
 * @param[in] shm_main Main SHM.
 * @param[in] ext_shm_addr Ext SHM address.
 * @param[in] inv_data_deps SHM offset of inverse data deps.
 * @param[in] inv_data_dep_count Inverse data dep count.
 * @param[in] ext_buf SHM memory copy.
 * @param[in,out] ext_buf_cur Current SHM memory position.
 * @return Memory offset of the copy.
 */
static off_t
sr_shmmain_defrag_copy_inv_data_deps(sr_shm_t *shm_main, char *ext_shm_addr, off_t inv_data_deps,
        uint16_t inv_data_dep_count, char *ext_buf, char **ext_buf_cur)
{
    off_t *new_inv_data_deps, *old_inv_data_deps;
    sr_mod_t *ref_mod;
    uint16_t i;
    off_t ret;

    if (!inv_data_deps && !inv_data_dep_count) {
        /* no inverse data dependencies */
        return 0;
    }
    assert(inv_data_deps && inv_data_dep_count);

    old_inv_data_deps = (off_t *)(ext_shm_addr + inv_data_deps);

    /* current offset */
    ret = *ext_buf_cur - ext_buf;

    /* allocate array */
    new_inv_data_deps = (off_t *)(ext_buf + sr_shmcpy(ext_buf, ext_shm_addr + inv_data_deps,
            inv_data_dep_count * sizeof *new_inv_data_deps, ext_buf_cur));

    /* copy all items */
    for (i = 0; i < inv_data_dep_count; ++i) {
        /* assign module */
        ref_mod = sr_shmmain_find_module(shm_main, ext_buf, ext_shm_addr + old_inv_data_deps[i], 0);
        new_inv_data_deps[i] = ref_mod->name;
    }

    return ret;
}

/**
 * @brief Copy an array from ext SHM to buffer to defragment it.
 *
 * @param[in] ext_shm_addr Ext SHM mapping address.
 * @param[in] array SHM offset of the array.
 * @param[in] size Array item size.
 * @param[in] count Array item count.
 * @param[in] ext_buf SHM ext buffer.
 * @param[in,out] ext_buf_cur Current SHM ext buffer position.
 * @return Buffer offset of the copy.
 */
static off_t
sr_shmmain_defrag_copy_array_with_string(char *ext_shm_addr, off_t array, size_t size, uint16_t count, char *ext_buf,
        char **ext_buf_cur)
{
    off_t ret, *item;
    uint16_t i;

    if (!array && !count) {
        /* empty array */
        return 0;
    }
    assert(array && count);

    /* current offset */
    ret = *ext_buf_cur - ext_buf;

    /* copy whole array */
    item = (off_t *)(ext_buf + sr_shmcpy(ext_buf, ext_shm_addr + array, count * size, ext_buf_cur));

    /* copy string for each item */
    for (i = 0; i < count; ++i) {
        if (*item) {
            *item = sr_shmcpy(ext_buf, ext_shm_addr + *item, sr_strshmlen(ext_shm_addr + *item), ext_buf_cur);
        }

        /* next item */
        item = (off_t *)(((uintptr_t)item) + size);
    }

    return ret;
}

sr_error_info_t *
sr_shmmain_ext_defrag(sr_shm_t *shm_main, sr_shm_t *shm_ext, char **defrag_ext_buf)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod;
    sr_rpc_t *shm_rpc;
    sr_mod_op_dep_t *old_op_deps, *new_op_deps;
    char *ext_buf, *ext_buf_cur, *mod_name;
    sr_conn_state_t *conn_s;
    sr_main_shm_t *main_shm;
    uint32_t *evpipes;
    uint16_t i;

    *defrag_ext_buf = NULL;

    /* resulting defragmented size is known */
    ext_buf_cur = ext_buf = malloc(shm_ext->size - *((size_t *)shm_ext->addr));
    SR_CHECK_MEM_RET(!ext_buf, err_info);

    /* wasted ext number */
    *((size_t *)ext_buf_cur) = 0;
    ext_buf_cur += sizeof(size_t);

    /* 1) copy all module names so that dependencies can reference them */
    SR_SHM_MOD_FOR(shm_main->addr, shm_main->size, shm_mod) {
        /* copy module name and update offset */
        mod_name = shm_ext->addr + shm_mod->name;
        shm_mod->name = sr_shmstrcpy(ext_buf, mod_name, &ext_buf_cur);
    }

    /* 2) copy the rest of arrays */
    SR_SHM_MOD_FOR(shm_main->addr, shm_main->size, shm_mod) {
        /* copy and update features */
        shm_mod->features = sr_shmmain_defrag_copy_array_with_string(shm_ext->addr, shm_mod->features, sizeof(off_t),
                shm_mod->feat_count, ext_buf, &ext_buf_cur);

        /* copy and update data deps */
        shm_mod->data_deps = sr_shmmain_defrag_copy_data_deps(shm_main, shm_ext->addr, shm_mod->data_deps,
                shm_mod->data_dep_count, ext_buf, &ext_buf_cur);

        /* allocate and copy inverse data deps */
        shm_mod->inv_data_deps = sr_shmmain_defrag_copy_inv_data_deps(shm_main, shm_ext->addr, shm_mod->inv_data_deps,
                shm_mod->inv_data_dep_count, ext_buf, &ext_buf_cur);

        /* allocate and copy op deps, first only with their xpath ... */
        old_op_deps = (sr_mod_op_dep_t *)(shm_ext->addr + shm_mod->op_deps);
        shm_mod->op_deps = sr_shmmain_defrag_copy_array_with_string(shm_ext->addr, shm_mod->op_deps, sizeof(sr_mod_op_dep_t),
                shm_mod->op_dep_count, ext_buf, &ext_buf_cur);

        /* ... then copy both arrays as well */
        new_op_deps = (sr_mod_op_dep_t *)(ext_buf + shm_mod->op_deps);
        for (i = 0; i < shm_mod->op_dep_count; ++i) {
            new_op_deps[i].in_deps = sr_shmmain_defrag_copy_data_deps(shm_main, shm_ext->addr, old_op_deps[i].in_deps,
                    old_op_deps[i].in_dep_count, ext_buf, &ext_buf_cur);
            new_op_deps[i].out_deps = sr_shmmain_defrag_copy_data_deps(shm_main, shm_ext->addr, old_op_deps[i].out_deps,
                    old_op_deps[i].out_dep_count, ext_buf, &ext_buf_cur);
        }

        /* copy change subscriptions */
        for (i = 0; i < SR_DS_COUNT; ++i) {
            shm_mod->change_sub[i].subs = sr_shmmain_defrag_copy_array_with_string(shm_ext->addr, shm_mod->change_sub[i].subs,
                    sizeof(sr_mod_change_sub_t), shm_mod->change_sub[i].sub_count, ext_buf, &ext_buf_cur);
        }

        /* copy operational subscriptions */
        shm_mod->oper_subs = sr_shmmain_defrag_copy_array_with_string(shm_ext->addr, shm_mod->oper_subs,
                sizeof(sr_mod_oper_sub_t), shm_mod->oper_sub_count, ext_buf, &ext_buf_cur);
    }

    main_shm = (sr_main_shm_t *)shm_main->addr;

    /* 3) copy connection state */
    conn_s = (sr_conn_state_t *)(shm_ext->addr + main_shm->conn_state.conns);
    /* copy connections */
    main_shm->conn_state.conns = sr_shmcpy(ext_buf, conn_s, main_shm->conn_state.conn_count * sizeof *conn_s,
            &ext_buf_cur);

    conn_s = (sr_conn_state_t *)(ext_buf + main_shm->conn_state.conns);
    for (i = 0; i < main_shm->conn_state.conn_count; ++i) {
        /* copy evpipes for each connection */
        evpipes = (uint32_t *)(shm_ext->addr + conn_s[i].evpipes);
        conn_s[i].evpipes = sr_shmcpy(ext_buf, evpipes, conn_s[i].evpipe_count * sizeof *evpipes, &ext_buf_cur);
    }

    /* 4) copy RPCs and their subscriptions */
    main_shm->rpc_subs = sr_shmmain_defrag_copy_array_with_string(shm_ext->addr, main_shm->rpc_subs,
                sizeof(sr_rpc_t), main_shm->rpc_sub_count, ext_buf, &ext_buf_cur);

    /* copy RPC subscriptions */
    shm_rpc = (sr_rpc_t *)(ext_buf + main_shm->rpc_subs);
    for (i = 0; i < main_shm->rpc_sub_count; ++i) {
        shm_rpc[i].subs = sr_shmmain_defrag_copy_array_with_string(shm_ext->addr, shm_rpc[i].subs,
                sizeof(sr_rpc_sub_t), shm_rpc[i].sub_count, ext_buf, &ext_buf_cur);
    }

    /* check size */
    if ((unsigned)(ext_buf_cur - ext_buf) != shm_ext->size - *((size_t *)shm_ext->addr)) {
        SR_ERRINFO_INT(&err_info);
        free(ext_buf);
        return err_info;
    }

    *defrag_ext_buf = ext_buf;
    return NULL;
}

sr_error_info_t *
sr_shmmain_check_dirs(void)
{
    char *dir_path;
    sr_error_info_t *err_info = NULL;
    int ret;

    /* startup data dir */
    if ((err_info = sr_path_startup_dir(&dir_path))) {
        return err_info;
    }
    if (((ret = access(dir_path, F_OK)) == -1) && (errno != ENOENT)) {
        free(dir_path);
        SR_ERRINFO_SYSERRNO(&err_info, "access");
        return err_info;
    }
    if (ret && (err_info = sr_mkpath(dir_path, SR_DIR_PERM))) {
        free(dir_path);
        return err_info;
    }
    free(dir_path);

    /* notif dir */
    if ((err_info = sr_path_notif_dir(&dir_path))) {
        return err_info;
    }
    if (((ret = access(dir_path, F_OK)) == -1) && (errno != ENOENT)) {
        free(dir_path);
        SR_ERRINFO_SYSERRNO(&err_info, "access");
        return err_info;
    }
    if (ret && (err_info = sr_mkpath(dir_path, SR_DIR_PERM))) {
        free(dir_path);
        return err_info;
    }
    free(dir_path);

    /* YANG module dir */
    if ((err_info = sr_path_yang_dir(&dir_path))) {
        return err_info;
    }
    if (((ret = access(dir_path, F_OK)) == -1) && (errno != ENOENT)) {
        free(dir_path);
        SR_ERRINFO_SYSERRNO(&err_info, "access");
        return err_info;
    }
    if (ret && (err_info = sr_mkpath(dir_path, SR_DIR_PERM))) {
        free(dir_path);
        return err_info;
    }
    free(dir_path);

    return NULL;
}

sr_error_info_t *
sr_shmmain_createlock_open(int *shm_lock)
{
    sr_error_info_t *err_info = NULL;
    char *path;

    if (asprintf(&path, "%s/%s", sr_get_repo_path(), SR_MAIN_SHM_LOCK) == -1) {
        SR_ERRINFO_MEM(&err_info);
        return err_info;
    }

    *shm_lock = open(path, O_RDWR | O_CREAT, SR_MAIN_SHM_PERM);
    free(path);
    if (*shm_lock == -1) {
        SR_ERRINFO_SYSERRNO(&err_info, "open");
        return err_info;
    }

    return NULL;
}

sr_error_info_t *
sr_shmmain_createlock(int shm_lock)
{
    struct flock fl;
    int ret;
    sr_error_info_t *err_info = NULL;

    assert(shm_lock > -1);

    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    do {
        ret = fcntl(shm_lock, F_SETLKW, &fl);
    } while ((ret == -1) && (errno == EINTR));
    if (ret == -1) {
        SR_ERRINFO_SYSERRNO(&err_info, "fcntl");
        return err_info;
    }

    return NULL;
}

void
sr_shmmain_createunlock(int shm_lock)
{
    struct flock fl;

    memset(&fl, 0, sizeof fl);
    fl.l_type = F_UNLCK;
    if (fcntl(shm_lock, F_SETLK, &fl) == -1) {
        assert(0);
    }
}

sr_error_info_t *
sr_shmmain_state_add_conn(sr_conn_ctx_t *conn)
{
    sr_error_info_t *err_info = NULL;
    sr_main_shm_t *main_shm;
    off_t conn_state_off;
    sr_conn_state_t *conn_s;
    uint32_t new_ext_size;

    main_shm = (sr_main_shm_t *)conn->main_shm.addr;

    /* moving existing state */
    conn_state_off = conn->ext_shm.size;
    new_ext_size = conn_state_off + (main_shm->conn_state.conn_count + 1) * sizeof *conn_s;

    /* remap ext SHM */
    if ((err_info = sr_shm_remap(&conn->ext_shm, new_ext_size))) {
        return err_info;
    }

    /* add wasted memory */
    *((size_t *)conn->ext_shm.addr) += main_shm->conn_state.conn_count * sizeof *conn_s;

    /* move the state */
    memcpy(conn->ext_shm.addr + conn_state_off, conn->ext_shm.addr + main_shm->conn_state.conns,
            main_shm->conn_state.conn_count * sizeof *conn_s);
    main_shm->conn_state.conns = conn_state_off;

    /* add new connection */
    conn_s = (sr_conn_state_t *)(conn->ext_shm.addr + main_shm->conn_state.conns);
    conn_s += main_shm->conn_state.conn_count;
    ++main_shm->conn_state.conn_count;

    /* fill attributes */
    conn_s->conn_ctx = conn;
    conn_s->pid = getpid();
    conn_s->evpipes = 0;
    conn_s->evpipe_count = 0;

    return NULL;
}

void
sr_shmmain_state_del_conn(sr_main_shm_t *main_shm, char *ext_shm_addr, sr_conn_ctx_t *conn, pid_t pid)
{
    sr_error_info_t *err_info = NULL;
    sr_conn_state_t *conn_s;
    uint32_t i;

    /* find the connection */
    conn_s = (sr_conn_state_t *)(ext_shm_addr + main_shm->conn_state.conns);
    for (i = 0; i < main_shm->conn_state.conn_count; ++i) {
        if ((conn == conn_s[i].conn_ctx) && (pid == conn_s[i].pid)) {
            break;
        }
    }
    if (i == main_shm->conn_state.conn_count) {
        SR_ERRINFO_INT(&err_info);
        sr_errinfo_free(&err_info);
        return;
    }

    /* add wasted memory for evpipes and connection itself */
    *((size_t *)ext_shm_addr) += (conn_s[i].evpipe_count * sizeof(uint32_t)) + sizeof *conn_s;

    --main_shm->conn_state.conn_count;
    if (!main_shm->conn_state.conn_count) {
        /* the only connection removed */
        main_shm->conn_state.conns = 0;
    } else if (i < main_shm->conn_state.conn_count) {
        /* replace the deleted connection with the last one */
        memcpy(&conn_s[i], &conn_s[main_shm->conn_state.conn_count], sizeof *conn_s);
    }
}

sr_conn_state_t *
sr_shmmain_state_find_conn(sr_main_shm_t *main_shm, char *ext_shm_addr, sr_conn_ctx_t *conn, pid_t pid)
{
    sr_conn_state_t *conn_s;
    uint32_t i;

    conn_s = (sr_conn_state_t *)(ext_shm_addr + main_shm->conn_state.conns);
    for (i = 0; i < main_shm->conn_state.conn_count; ++i) {
        if ((conn == conn_s[i].conn_ctx) && (pid == conn_s[i].pid)) {
            return &conn_s[i];
        }
    }

    return NULL;
}

sr_error_info_t *
sr_shmmain_state_add_evpipe(sr_conn_ctx_t *conn, uint32_t evpipe_num)
{
    sr_error_info_t *err_info = NULL;
    sr_main_shm_t *main_shm;
    off_t evpipes_off;
    sr_conn_state_t *conn_s;
    uint32_t new_ext_size;

    main_shm = (sr_main_shm_t *)conn->main_shm.addr;

    /* find the connection */
    conn_s = sr_shmmain_state_find_conn(main_shm, conn->ext_shm.addr, conn, getpid());
    if (!conn_s) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, NULL,
                "Connection not found in internal state (perhaps fork() was used and PID has changed).");
        return err_info;
    }

    /* moving existing evpipes */
    evpipes_off = conn->ext_shm.size;
    new_ext_size = evpipes_off + (conn_s->evpipe_count + 1) * sizeof evpipe_num;

    /* remap ext SHM */
    if ((err_info = sr_shm_remap(&conn->ext_shm, new_ext_size))) {
        return err_info;
    }

    /* find the connection again, could have moved */
    conn_s = sr_shmmain_state_find_conn(main_shm, conn->ext_shm.addr, conn, getpid());
    assert(conn_s);

    /* add wasted memory */
    *((size_t *)conn->ext_shm.addr) += conn_s->evpipe_count * sizeof evpipe_num;

    /* move the evpipes */
    memcpy(conn->ext_shm.addr + evpipes_off, conn->ext_shm.addr + conn_s->evpipes,
            conn_s->evpipe_count * sizeof evpipe_num);
    conn_s->evpipes = evpipes_off;

    /* add new evpipe */
    ((uint32_t *)(conn->ext_shm.addr + conn_s->evpipes))[conn_s->evpipe_count] = evpipe_num;
    ++conn_s->evpipe_count;

    return NULL;
}

void
sr_shmmain_state_del_evpipe(sr_conn_ctx_t *conn, uint32_t evpipe_num)
{
    sr_error_info_t *err_info = NULL;
    sr_main_shm_t *main_shm;
    sr_conn_state_t *conn_s;
    uint32_t i, *evpipes;

    main_shm = (sr_main_shm_t *)conn->main_shm.addr;

    /* find the connection */
    conn_s = sr_shmmain_state_find_conn(main_shm, conn->ext_shm.addr, conn, getpid());
    if (!conn_s) {
        SR_ERRINFO_INT(&err_info);
        sr_errinfo_free(&err_info);
        return;
    }

    /* find the evpipe */
    evpipes = (uint32_t *)(conn->ext_shm.addr + conn_s->evpipes);
    for (i = 0; i < conn_s->evpipe_count; ++i) {
        if (evpipes[i] == evpipe_num) {
            break;
        }
    }
    if (i == conn_s->evpipe_count) {
        SR_ERRINFO_INT(&err_info);
        sr_errinfo_free(&err_info);
        return;
    }

    /* add wasted memory */
    *((size_t *)conn->ext_shm.addr) += sizeof evpipe_num;

    --conn_s->evpipe_count;
    if (!conn_s->evpipe_count) {
        /* the only evpipe removed */
        conn_s->evpipes = 0;
    } else if (i < conn_s->evpipe_count) {
        /* replace the deleted evpipe with the last one */
        evpipes[i] = evpipes[conn_s->evpipe_count];
    }
}

sr_error_info_t *
sr_shmmain_state_recover(sr_conn_ctx_t *conn)
{
    sr_error_info_t *err_info = NULL, *tmp_err;
    sr_conn_state_t *conn_s;
    sr_mod_t *shm_mod;
    sr_rpc_t *shm_rpc;
    sr_main_shm_t *main_shm;
    uint32_t i, j, k, *evpipes;
    int last_removed;

    main_shm = (sr_main_shm_t *)conn->main_shm.addr;

    conn_s = (sr_conn_state_t *)(conn->ext_shm.addr + main_shm->conn_state.conns);
    i = 0;
    while (i < main_shm->conn_state.conn_count) {
        if (!sr_process_exists(conn_s[i].pid)) {
            SR_LOG_WRN("Cleaning up after a non-existent sysrepo client with PID %ld.", (long)conn_s[i].pid);

            /* recover any held locks */
            switch (conn_s[i].lock.main) {
            case SR_LOCK_READ:
                /* remove all read locks */
                assert(conn_s[i].lock.main_rcount && (main_shm->lock.readers >= conn_s[i].lock.main_rcount));
                main_shm->lock.readers -= conn_s[i].lock.main_rcount;
                break;
            default:
                /* not supported */
                SR_ERRINFO_INT(&err_info);
                break;
            }

            /* go through all the modules and their subscriptions and delete any matching (stale) ones */
            evpipes = (uint32_t *)(conn->ext_shm.addr + conn_s[i].evpipes);
            for (j = 0; j < conn_s[i].evpipe_count; ++j) {
                SR_SHM_MOD_FOR(conn->main_shm.addr, conn->main_shm.size, shm_mod) {
                    for (k = 0; k < SR_DS_COUNT; ++k) {
                        if ((tmp_err = sr_shmmod_change_subscription_del(conn->ext_shm.addr, shm_mod, NULL, k, 0, 0,
                                evpipes[j], 1, NULL))) {
                            sr_errinfo_merge(&err_info, tmp_err);
                        }
                        if (k == SR_DS_RUNNING) {
                            /* technically, operational datastore changed */
                            if ((tmp_err = sr_module_update_oper_diff(conn, conn->ext_shm.addr + shm_mod->name))) {
                                sr_errinfo_merge(&err_info, tmp_err);
                            }
                        }
                    }
                    if ((tmp_err = sr_shmmod_oper_subscription_del(conn->ext_shm.addr, shm_mod, NULL, evpipes[j], 1))) {
                        sr_errinfo_merge(&err_info, tmp_err);
                    }
                    if ((tmp_err = sr_shmmod_notif_subscription_del(conn->ext_shm.addr, shm_mod, evpipes[j], 1, NULL))) {
                        sr_errinfo_merge(&err_info, tmp_err);
                    }
                }

                shm_rpc = (sr_rpc_t *)(conn->ext_shm.addr + main_shm->rpc_subs);
                for (k = 0; k < main_shm->rpc_sub_count; ++k) {
                    tmp_err = sr_shmmain_rpc_subscription_del(conn->ext_shm.addr, &shm_rpc[k], NULL, 0, evpipes[j], 1, &last_removed);
                    if (tmp_err) {
                        sr_errinfo_merge(&err_info, tmp_err);
                    }

                    if (last_removed) {
                        /* remove the parent RPC subscription structure */
                        if ((tmp_err = sr_shmmain_del_rpc(main_shm, conn->ext_shm.addr, NULL, shm_rpc[k].op_path))) {
                            sr_errinfo_merge(&err_info, tmp_err);
                        }
                    }
                }
            }

            /* remove this connection from state */
            sr_shmmain_state_del_conn(main_shm, conn->ext_shm.addr, conn_s[i].conn_ctx, conn_s[i].pid);

            /* remove any stored operational data of this connection */
            if ((tmp_err = sr_shmmod_oper_stored_del_conn(conn, conn_s[i].conn_ctx, conn_s[i].pid))) {
                sr_errinfo_merge(&err_info, tmp_err);
            }
        } else {
            ++i;
        }
    }

    return err_info;
}

/**
 * @brief Calculate how much ext SHM space is taken by connection state, RPCs,
 * their subscriptions, and any existing module subscriptions in main and ext SHM.
 *
 * @param[in] shm_main Main SHM.
 * @param[in] ext_shm_addr Ext SHM mapping address.
 * @return Ext SHM size of the considered data.
 */
static size_t
sr_shmmain_ext_get_size_main_shm(sr_shm_t *shm_main, char *ext_shm_addr)
{
    size_t shm_size = 0;
    uint32_t i, j;
    sr_main_shm_t *main_shm;
    sr_rpc_t *shm_rpc;
    sr_rpc_sub_t *rpc_subs;
    sr_mod_t *shm_mod;
    sr_mod_change_sub_t *change_subs;
    sr_mod_oper_sub_t *oper_subs;
    sr_conn_state_t *conn_s;

    main_shm = (sr_main_shm_t *)shm_main->addr;

    /* connection state */
    conn_s = (sr_conn_state_t *)(ext_shm_addr + main_shm->conn_state.conns);
    for (i = 0; i < main_shm->conn_state.conn_count; ++i) {
        shm_size += conn_s[i].evpipe_count * sizeof(uint32_t);
        shm_size += sizeof *conn_s;
    }

    /* RPCs and their subscriptions */
    shm_rpc = (sr_rpc_t *)(ext_shm_addr + main_shm->rpc_subs);
    for (i = 0; i < main_shm->rpc_sub_count; ++i) {
        assert(shm_rpc[i].op_path);
        shm_size += sr_strshmlen(ext_shm_addr + shm_rpc[i].op_path);

        rpc_subs = (sr_rpc_sub_t *)(ext_shm_addr + shm_rpc[i].subs);
        for (j = 0; j < shm_rpc[i].sub_count; ++j) {
            assert(rpc_subs[j].xpath);
            shm_size += sr_strshmlen(ext_shm_addr + rpc_subs[j].xpath);
        }
        shm_size += shm_rpc[i].sub_count * sizeof *rpc_subs;
    }
    shm_size += main_shm->rpc_sub_count * sizeof *shm_rpc;

    /* existing module subscriptions */
    SR_SHM_MOD_FOR(shm_main->addr, shm_main->size, shm_mod) {
        /* change subscriptions */
        for (i = 0; i < SR_DS_COUNT; ++i) {
            change_subs = (sr_mod_change_sub_t *)(ext_shm_addr + shm_mod->change_sub[i].subs);
            for (j = 0; j < shm_mod->change_sub[i].sub_count; ++j) {
                if (change_subs[j].xpath) {
                    shm_size += sr_strshmlen(ext_shm_addr + change_subs[j].xpath);
                }
            }
            shm_size += shm_mod->change_sub[i].sub_count * sizeof *change_subs;
        }

        /* oper subscriptions */
        oper_subs = (sr_mod_oper_sub_t *)(ext_shm_addr + shm_mod->oper_subs);
        for (i = 0; i < shm_mod->oper_sub_count; ++i) {
            assert(oper_subs[i].xpath);
            shm_size += sr_strshmlen(ext_shm_addr + oper_subs[i].xpath);
        }
        shm_size += shm_mod->oper_subs * sizeof *oper_subs;

        /* notif subscriptions */
        shm_size += shm_mod->notif_subs * sizeof(sr_mod_notif_sub_t);
    }

    return shm_size;
}

/**
 * @brief Calculate how much ext SHM space is taken by sysrepo module data.
 *
 * @param[in] sr_mods Sysrepo module data.
 * @return Ext SHM size of the considered data.
 */
static size_t
sr_shmmain_ext_get_lydmods_size(struct lyd_node *sr_mods)
{
    struct lyd_node *sr_mod, *sr_child, *sr_op_dep, *sr_dep, *sr_instid;
    size_t shm_size = 0;

    assert(sr_mods);

    LY_TREE_FOR(sr_mods->child, sr_mod) {
        LY_TREE_FOR(sr_mod->child, sr_child) {
            if (!strcmp(sr_child->schema->name, "name")) {
                /* a string */
                shm_size += sr_strshmlen(((struct lyd_node_leaf_list *)sr_child)->value_str);
            } else if (!strcmp(sr_child->schema->name, "enabled-feature")) {
                /* another feature */
                shm_size += sizeof(off_t);
                /* a string */
                shm_size += sr_strshmlen(((struct lyd_node_leaf_list *)sr_child)->value_str);
            } else if (!strcmp(sr_child->schema->name, "data-deps")) {
                LY_TREE_FOR(sr_child->child, sr_dep) {
                    /* another data dependency */
                    shm_size += sizeof(sr_mod_data_dep_t);

                    /* module name was already counted and type is an enum */
                    if (!strcmp(sr_dep->schema->name, "inst-id")) {
                        LY_TREE_FOR(sr_dep->child, sr_instid) {
                            if (!strcmp(sr_instid->schema->name, "xpath")) {
                                /* a string */
                                shm_size += sr_strshmlen(((struct lyd_node_leaf_list *)sr_instid)->value_str);
                            }
                        }
                    }
                }
            } else if (!strcmp(sr_child->schema->name, "inverse-data-deps")) {
                /* another inverse dependency */
                shm_size += sizeof(off_t);
            } else if (!strcmp(sr_child->schema->name, "op-deps")) {
                /* another op with dependencies */
                shm_size += sizeof(sr_mod_op_dep_t);

                LY_TREE_FOR(sr_child->child, sr_op_dep) {
                    if (!strcmp(sr_op_dep->schema->name, "xpath")) {
                        /* operation xpath (a string) */
                        shm_size += sr_strshmlen(((struct lyd_node_leaf_list *)sr_op_dep)->value_str);
                    } else if (!strcmp(sr_op_dep->schema->name, "in") || !strcmp(sr_op_dep->schema->name, "out")) {
                        LY_TREE_FOR(sr_op_dep->child, sr_dep) {
                            /* another data dependency */
                            shm_size += sizeof(sr_mod_data_dep_t);

                            if (!strcmp(sr_dep->schema->name, "inst-id")) {
                                LY_TREE_FOR(sr_dep->child, sr_instid) {
                                    if (!strcmp(sr_instid->schema->name, "xpath")) {
                                        /* a string */
                                        shm_size += sr_strshmlen(((struct lyd_node_leaf_list *)sr_instid)->value_str);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return shm_size;
}

sr_error_info_t *
sr_shmmain_ly_ctx_init(struct ly_ctx **ly_ctx)
{
    sr_error_info_t *err_info = NULL;

    /* libyang context init */
    if ((err_info = sr_ly_ctx_new(ly_ctx))) {
        return err_info;
    }

    /* load just the internal module */
    if (!lys_parse_mem(*ly_ctx, sysrepo_yang, LYS_YANG)) {
        sr_errinfo_new_ly(&err_info, *ly_ctx);
        ly_ctx_destroy(*ly_ctx, NULL);
        *ly_ctx = NULL;
        return err_info;
    }

    return NULL;
}

sr_error_info_t *
sr_shmmain_files_startup2running(sr_conn_ctx_t *conn)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod = NULL;
    char *startup_path, *running_path;

    SR_SHM_MOD_FOR(conn->main_shm.addr, conn->main_shm.size, shm_mod) {
        if ((err_info = sr_path_ds_shm(conn->ext_shm.addr + shm_mod->name, SR_DS_RUNNING, 0, &running_path))) {
            goto error;
        }
        if ((err_info = sr_path_startup_file(conn->ext_shm.addr + shm_mod->name, &startup_path))) {
            free(running_path);
            goto error;
        }
        err_info = sr_cp_file2shm(running_path, startup_path, SR_FILE_PERM);
        free(startup_path);
        free(running_path);
        if (err_info) {
            goto error;
        }
    }

    SR_LOG_INFMSG("Datastore copied from <startup> to <running>.");
    return NULL;

error:
    sr_errinfo_new(&err_info, SR_ERR_INIT_FAILED, NULL, "Copying datastore from <startup> to <running> failed.");
    return err_info;
}

/**
 * @brief Fill main SHM data dependency information based on internal sysrepo data.
 *
 * @param[in] shm_main Main SHM.
 * @param[in] ext_shm_addr Ext SHM address.
 * @param[in] sr_dep_parent Dependencies in internal sysrepo data.
 * @param[in] shm_deps Main SHM data dependencies to fill.
 * @param[out] dep_i Number of dependencies filled.
 * @param[in,out] ext_cur Current ext SHM position.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_shmmain_fill_data_deps(sr_shm_t *shm_main, char *ext_shm_addr, struct lyd_node *sr_dep_parent,
        sr_mod_data_dep_t *shm_deps, uint32_t *dep_i, char **ext_cur)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *ref_shm_mod = NULL;
    struct lyd_node *sr_dep, *sr_instid;
    const char *str;
    int dep_found;

    assert(!*dep_i);

    LY_TREE_FOR(sr_dep_parent->child, sr_dep) {
        dep_found = 0;

        if (!strcmp(sr_dep->schema->name, "module")) {
            dep_found = 1;

            /* set dep type */
            shm_deps[*dep_i].type = SR_DEP_REF;

            /* copy module name offset */
            str = sr_ly_leaf_value_str(sr_dep);
            ref_shm_mod = sr_shmmain_find_module(shm_main, ext_shm_addr, str, 0);
            SR_CHECK_INT_RET(!ref_shm_mod, err_info);
            shm_deps[*dep_i].module = ref_shm_mod->name;

            /* no xpath */
            shm_deps[*dep_i].xpath = 0;
        } else if (!strcmp(sr_dep->schema->name, "inst-id")) {
            dep_found = 1;

            /* set dep type */
            shm_deps[*dep_i].type = SR_DEP_INSTID;

            /* there may be no default value */
            shm_deps[*dep_i].module = 0;

            LY_TREE_FOR(sr_dep->child, sr_instid) {
                if (!strcmp(sr_instid->schema->name, "xpath")) {
                    /* copy xpath */
                    str = sr_ly_leaf_value_str(sr_instid);
                    shm_deps[*dep_i].xpath = sr_shmstrcpy(ext_shm_addr, str, ext_cur);
                } else if (!strcmp(sr_instid->schema->name, "default-module")) {
                    /* copy module name offset */
                    str = sr_ly_leaf_value_str(sr_instid);
                    ref_shm_mod = sr_shmmain_find_module(shm_main, ext_shm_addr, str, 0);
                    SR_CHECK_INT_RET(!ref_shm_mod, err_info);
                    shm_deps[*dep_i].module = ref_shm_mod->name;
                }
            }
        }

        assert(!dep_found || shm_deps[*dep_i].module || shm_deps[*dep_i].xpath);
        if (dep_found) {
            ++(*dep_i);
        }
    }

    return NULL;
}

/**
 * @brief Add modules and their features into main SHM. Does not add data/op/inverse dependencies.
 *
 * @param[in] ext_shm_addr Ext SHM address.
 * @param[in] first_sr_mod First new module to add.
 * @param[in] first_shm_mod First empty main SHM module for @p first_sr_mod to be filled in.
 * @param[in,out] shm_end Current main SHM end (does not equal to size if was preallocated).
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_shmmain_add_modules(char *ext_shm_addr, struct lyd_node *first_sr_mod, sr_mod_t *first_shm_mod, off_t *ext_end)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_child;
    off_t *shm_features;
    char *ext_cur;
    const char *str;
    uint32_t i, feat_i;

    assert(first_sr_mod && first_shm_mod);
    ext_cur = ext_shm_addr + *ext_end;

    LY_TREE_FOR(first_sr_mod, first_sr_mod) {
        /* set module structure */
        memset(first_shm_mod, 0, sizeof *first_shm_mod);
        for (i = 0; i < SR_DS_COUNT; ++i) {
            if ((err_info = sr_rwlock_init(&first_shm_mod->data_lock_info[i].lock, 1))) {
                return err_info;
            }
        }
        if ((err_info = sr_rwlock_init(&first_shm_mod->replay_lock, 1))) {
            return err_info;
        }
        first_shm_mod->ver = 1;

        /* set all arrays and pointers to ext SHM */
        LY_TREE_FOR(first_sr_mod->child, sr_child) {
            if (!strcmp(sr_child->schema->name, "name")) {
                /* copy module name */
                str = sr_ly_leaf_value_str(sr_child);
                first_shm_mod->name = sr_shmstrcpy(ext_shm_addr, str, &ext_cur);
            } else if (!strcmp(sr_child->schema->name, "revision")) {
                /* copy revision */
                str = sr_ly_leaf_value_str(sr_child);
                strcpy(first_shm_mod->rev, str);
            } else if (!strcmp(sr_child->schema->name, "replay-support")) {
                /* set replay-support flag */
                first_shm_mod->flags |= SR_MOD_REPLAY_SUPPORT;
            } else if (!strcmp(sr_child->schema->name, "enabled-feature")) {
                /* just count features */
                ++first_shm_mod->feat_count;
            }
        }

        /* allocate and fill features */
        first_shm_mod->features = sr_shmcpy(ext_shm_addr, NULL, first_shm_mod->feat_count * sizeof(off_t), &ext_cur);
        shm_features = (off_t *)(ext_shm_addr + first_shm_mod->features);
        feat_i = 0;

        LY_TREE_FOR(first_sr_mod->child, sr_child) {
            if (!strcmp(sr_child->schema->name, "enabled-feature")) {
                /* copy feature name */
                str = sr_ly_leaf_value_str(sr_child);
                shm_features[feat_i] = sr_shmstrcpy(ext_shm_addr, str, &ext_cur);

                ++feat_i;
            }
        }
        SR_CHECK_INT_RET(feat_i != first_shm_mod->feat_count, err_info);

        /* next iteration */
        ++first_shm_mod;
    }

    *ext_end = ext_cur - ext_shm_addr;
    return NULL;
}

/**
 * @brief Add modules data/op/inverse dependencies.
 *
 * @param[in] shm_main Main SHM.
 * @param[in] ext_shm_addr Ext SHM address.
 * @param[in] first_sr_mod First module whose dependencies to add.
 * @param[in] first_shm_mod First main SHM module corresponding to @p first_sr_mod.
 * @param[in,out] shm_end Current main SHM end (does not equal to size if was preallocated).
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_shmmain_add_modules_deps(sr_shm_t *shm_main, char *ext_shm_addr, struct lyd_node *first_sr_mod, sr_mod_t *first_shm_mod,
        off_t *ext_end)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *sr_child, *sr_dep, *sr_op, *sr_op_dep;
    sr_mod_t *ref_shm_mod;
    sr_mod_data_dep_t *shm_data_deps, *shm_op_data_deps;
    sr_mod_op_dep_t *shm_op_deps;
    off_t *shm_inv_data_deps;
    char *ext_cur;
    const char *str;
    uint32_t data_dep_i, inv_data_dep_i, op_dep_i, op_data_dep_i;

    assert(first_sr_mod && first_shm_mod);
    ext_cur = ext_shm_addr + *ext_end;

    LY_TREE_FOR(first_sr_mod, first_sr_mod) {
        assert(!first_shm_mod->data_dep_count);
        assert(!first_shm_mod->inv_data_dep_count);
        assert(!first_shm_mod->op_dep_count);

        /* set all arrays and pointers to ext SHM */
        LY_TREE_FOR(first_sr_mod->child, sr_child) {
            if (!strcmp(sr_child->schema->name, "data-deps")) {
                /* just count data dependencies */
                LY_TREE_FOR(sr_child->child, sr_dep) {
                    ++first_shm_mod->data_dep_count;
                }
            } else if (!strcmp(sr_child->schema->name, "inverse-data-deps")) {
                /* just count inverse data dependencies */
                ++first_shm_mod->inv_data_dep_count;
            } else if (!strcmp(sr_child->schema->name, "op-deps")) {
                /* just count op dependencies */
                ++first_shm_mod->op_dep_count;
            }
        }

        /* allocate and fill arrays */
        first_shm_mod->data_deps = sr_shmcpy(ext_shm_addr, NULL, first_shm_mod->data_dep_count * sizeof(sr_mod_data_dep_t), &ext_cur);
        shm_data_deps = (sr_mod_data_dep_t *)(ext_shm_addr + first_shm_mod->data_deps);
        data_dep_i = 0;

        first_shm_mod->inv_data_deps = sr_shmcpy(ext_shm_addr, NULL, first_shm_mod->inv_data_dep_count * sizeof(off_t), &ext_cur);
        shm_inv_data_deps = (off_t *)(ext_shm_addr + first_shm_mod->inv_data_deps);
        inv_data_dep_i = 0;

        first_shm_mod->op_deps = sr_shmcpy(ext_shm_addr, NULL, first_shm_mod->op_dep_count * sizeof(sr_mod_op_dep_t), &ext_cur);
        shm_op_deps = (sr_mod_op_dep_t *)(ext_shm_addr + first_shm_mod->op_deps);
        op_dep_i = 0;

        LY_TREE_FOR(first_sr_mod->child, sr_child) {
            if (!strcmp(sr_child->schema->name, "data-deps")) {
                /* now fill the dependency array */
                if ((err_info = sr_shmmain_fill_data_deps(shm_main, ext_shm_addr, sr_child, shm_data_deps, &data_dep_i,
                            &ext_cur))) {
                    return err_info;
                }
            } else if (!strcmp(sr_child->schema->name, "inverse-data-deps")) {
                /* now fill module references */
                str = sr_ly_leaf_value_str(sr_child);
                ref_shm_mod = sr_shmmain_find_module(shm_main, ext_shm_addr, str, 0);
                SR_CHECK_INT_RET(!ref_shm_mod, err_info);
                shm_inv_data_deps[inv_data_dep_i] = ref_shm_mod->name;

                ++inv_data_dep_i;
            } else if (!strcmp(sr_child->schema->name, "op-deps")) {
                LY_TREE_FOR(sr_child->child, sr_op) {
                    if (!strcmp(sr_op->schema->name, "xpath")) {
                        /* copy xpath name */
                        str = sr_ly_leaf_value_str(sr_op);
                        shm_op_deps[op_dep_i].xpath = sr_shmstrcpy(ext_shm_addr, str, &ext_cur);
                    } else if (!strcmp(sr_op->schema->name, "in")) {
                        LY_TREE_FOR(sr_op->child, sr_op_dep) {
                            /* count op input data deps first */
                            ++shm_op_deps[op_dep_i].in_dep_count;
                        }

                        /* allocate array */
                        shm_op_deps[op_dep_i].in_deps = sr_shmcpy(ext_shm_addr, NULL,
                                shm_op_deps[op_dep_i].in_dep_count * sizeof(sr_mod_data_dep_t), &ext_cur);

                        /* fill the array */
                        shm_op_data_deps = (sr_mod_data_dep_t *)(ext_shm_addr + shm_op_deps[op_dep_i].in_deps);
                        op_data_dep_i = 0;
                        if ((err_info = sr_shmmain_fill_data_deps(shm_main, ext_shm_addr, sr_op, shm_op_data_deps,
                                    &op_data_dep_i, &ext_cur))) {
                            return err_info;
                        }
                        SR_CHECK_INT_RET(op_data_dep_i != shm_op_deps[op_dep_i].in_dep_count, err_info);
                    } else if (!strcmp(sr_op->schema->name, "out")) {
                        LY_TREE_FOR(sr_op->child, sr_op_dep) {
                            /* count op output data deps first */
                            ++shm_op_deps[op_dep_i].out_dep_count;
                        }

                        /* allocate array */
                        shm_op_deps[op_dep_i].out_deps = sr_shmcpy(ext_shm_addr, NULL,
                                shm_op_deps[op_dep_i].out_dep_count * sizeof(sr_mod_data_dep_t), &ext_cur);

                        /* fill the array */
                        shm_op_data_deps = (sr_mod_data_dep_t *)(ext_shm_addr + shm_op_deps[op_dep_i].out_deps);
                        op_data_dep_i = 0;
                        if ((err_info = sr_shmmain_fill_data_deps(shm_main, ext_shm_addr, sr_op, shm_op_data_deps,
                                    &op_data_dep_i, &ext_cur))) {
                            return err_info;
                        }
                        SR_CHECK_INT_RET(op_data_dep_i != shm_op_deps[op_dep_i].out_dep_count, err_info);
                    }
                }

                ++op_dep_i;
            }
        }
        SR_CHECK_INT_RET(data_dep_i != first_shm_mod->data_dep_count, err_info);
        SR_CHECK_INT_RET(inv_data_dep_i != first_shm_mod->inv_data_dep_count, err_info);
        SR_CHECK_INT_RET(op_dep_i != first_shm_mod->op_dep_count, err_info);

        /* next iteration */
        ++first_shm_mod;
    }

    *ext_end = ext_cur - ext_shm_addr;
    return NULL;
}

/**
 * @brief Remove modules data/op/inverse dependencies.
 *
 * @param[in] shm_main Main SHM.
 * @param[in] ext_shm_addr Ext SHM address.
 * @param[in] first_shm_mod First main SHM module whose dependencies to remove.
 */
static void
sr_shmmain_del_modules_deps(sr_shm_t *shm_main, char *ext_shm_addr, sr_mod_t *first_shm_mod)
{
    sr_mod_data_dep_t *shm_data_deps, *shm_op_data_deps;
    sr_mod_op_dep_t *shm_op_deps;
    size_t *ext_wasted;
    uint32_t i, j;

    assert(first_shm_mod);
    ext_wasted = (size_t *)ext_shm_addr;

    do {
        shm_data_deps = (sr_mod_data_dep_t *)(ext_shm_addr + first_shm_mod->data_deps);
        for (i = 0; i < first_shm_mod->data_dep_count; ++i) {
            /* add wasted for xpath */
            if (shm_data_deps[i].xpath) {
                *ext_wasted += sr_strshmlen(ext_shm_addr + shm_data_deps[i].xpath);
            }
        }

        /* add wasted for data deps array and clear it */
        *ext_wasted += first_shm_mod->data_dep_count * sizeof(sr_mod_data_dep_t);
        first_shm_mod->data_deps = 0;
        first_shm_mod->data_dep_count = 0;

        /* add wasted for inv data deps array and clear it */
        *ext_wasted += first_shm_mod->inv_data_dep_count * sizeof(off_t);
        first_shm_mod->inv_data_deps = 0;
        first_shm_mod->inv_data_dep_count = 0;

        shm_op_deps = (sr_mod_op_dep_t *)(ext_shm_addr + first_shm_mod->op_deps);
        for (i = 0; i < first_shm_mod->op_dep_count; ++i) {
            if (shm_op_deps[i].xpath) {
                /* add wasted for xpath */
                *ext_wasted += sr_strshmlen(ext_shm_addr + shm_op_deps[i].xpath);
            }

            shm_op_data_deps = (sr_mod_data_dep_t *)(ext_shm_addr + shm_op_deps[i].in_deps);
            for (j = 0; j < shm_op_deps[i].in_dep_count; ++j) {
                if (shm_op_data_deps[j].xpath) {
                    /* add wasted for xpath */
                    *ext_wasted += sr_strshmlen(ext_shm_addr + shm_op_data_deps[j].xpath);
                }
            }

            /* add wasted for in deps array */
            *ext_wasted += shm_op_deps[i].in_dep_count * sizeof(sr_mod_data_dep_t);

            shm_op_data_deps = (sr_mod_data_dep_t *)(ext_shm_addr + shm_op_deps[i].out_deps);
            for (j = 0; j < shm_op_deps[i].out_dep_count; ++j) {
                if (shm_op_data_deps[j].xpath) {
                    /* add wasted for xpath */
                    *ext_wasted += sr_strshmlen(ext_shm_addr + shm_op_data_deps[j].xpath);
                }
            }

            /* add wasted for out deps array */
            *ext_wasted += shm_op_deps[i].out_dep_count * sizeof(sr_mod_data_dep_t);
        }

        /* add wasted for op deps array and clear it */
        *ext_wasted += first_shm_mod->op_dep_count * sizeof(sr_mod_op_dep_t);
        first_shm_mod->op_deps = 0;
        first_shm_mod->op_dep_count = 0;

        /* next iteration */
        ++first_shm_mod;
    } while ((char *)first_shm_mod != shm_main->addr + shm_main->size);
}

sr_error_info_t *
sr_shmmain_add(sr_conn_ctx_t *conn, struct lyd_node *sr_mod)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *next;
    sr_mod_t *shm_mod;
    off_t main_end, ext_end;
    size_t *wasted_ext, new_ext_size, new_mod_count;

    /* count how many modules are we going to add */
    new_mod_count = 0;
    LY_TREE_FOR(sr_mod, next) {
        ++new_mod_count;
    }

    /* remember current SHM and ext SHM end (size) */
    main_end = conn->main_shm.size;
    ext_end = conn->ext_shm.size;

    /* enlarge main SHM for the new modules */
    if ((err_info = sr_shm_remap(&conn->main_shm, conn->main_shm.size + new_mod_count * sizeof *shm_mod))) {
        return err_info;
    }

    /* enlarge ext SHM */
    wasted_ext = (size_t *)conn->ext_shm.addr;
    new_ext_size = sizeof(size_t) + sr_shmmain_ext_get_size_main_shm(&conn->main_shm, conn->ext_shm.addr) +
            sr_shmmain_ext_get_lydmods_size(sr_mod->parent);
    if ((err_info = sr_shm_remap(&conn->ext_shm, new_ext_size + *wasted_ext))) {
        return err_info;
    }
    wasted_ext = (size_t *)conn->ext_shm.addr;

    /* add all newly implemented modules into SHM */
    if ((err_info = sr_shmmain_add_modules(conn->ext_shm.addr, sr_mod, (sr_mod_t *)(conn->main_shm.addr + main_end),
                &ext_end))) {
        return err_info;
    }

    /*
     * Dependencies of old modules are rebuild because of possible
     * 1) new inverse dependencies when new modules depend on the old ones;
     * 2) new dependencies in the old modules in case they were added by foreign augments in the new modules.
     * Checking these cases would probably be more costly than just always rebuilding all dependencies.
     */

    /* remove all dependencies of all modules from SHM */
    sr_shmmain_del_modules_deps(&conn->main_shm, conn->ext_shm.addr, (sr_mod_t *)(conn->main_shm.addr + sizeof(sr_main_shm_t)));

    /* enlarge ext SHM to account for the newly wasted memory */
    if ((err_info = sr_shm_remap(&conn->ext_shm, new_ext_size + *wasted_ext))) {
        return err_info;
    }
    wasted_ext = (size_t *)conn->ext_shm.addr;

    /* add all dependencies for all modules in SHM */
    if ((err_info = sr_shmmain_add_modules_deps(&conn->main_shm, conn->ext_shm.addr, sr_mod->parent->child,
                (sr_mod_t *)(conn->main_shm.addr + sizeof(sr_main_shm_t)), &ext_end))) {
        return err_info;
    }

    /* check expected size */
    SR_CHECK_INT_RET((unsigned)ext_end != new_ext_size + *wasted_ext, err_info);

    return NULL;
}

sr_error_info_t *
sr_shmmain_main_open(sr_shm_t *shm, int *created)
{
    sr_error_info_t *err_info = NULL;
    sr_main_shm_t *main_shm;
    int creat = 0;

    /* try to open the shared memory */
    shm->fd = shm_open(SR_MAIN_SHM, O_RDWR, SR_MAIN_SHM_PERM);
    if ((shm->fd == -1) && (errno == ENOENT)) {
        if (!created) {
            /* we do not want to create the memory now */
            return NULL;
        }

        /* create shared memory */
        shm->fd = shm_open(SR_MAIN_SHM, O_RDWR | O_CREAT | O_EXCL, SR_MAIN_SHM_PERM);
        creat = 1;
    }
    if (shm->fd == -1) {
        sr_errinfo_new(&err_info, SR_ERR_SYS, NULL, "Failed to open shared memory (%s).", strerror(errno));
        goto error;
    }

    /* map it with proper size */
    if ((err_info = sr_shm_remap(shm, creat ? sizeof *main_shm : 0))) {
        goto error;
    }

    if (creat) {
        /* init the memory */
        main_shm = (sr_main_shm_t *)shm->addr;
        if ((err_info = sr_rwlock_init(&main_shm->lock, 1))) {
            goto error;
        }
        if ((err_info = sr_mutex_init(&main_shm->lydmods_lock, 1))) {
            goto error;
        }
        ATOMIC_STORE_RELAXED(main_shm->new_sr_sid, 1);
        ATOMIC_STORE_RELAXED(main_shm->new_evpipe_num, 1);

        /* remove leftover event pipes */
        sr_remove_evpipes();
    }

    if (created) {
        *created = creat;
    }
    return NULL;

error:
    sr_shm_clear(shm);
    return err_info;
}

sr_error_info_t *
sr_shmmain_ext_open(sr_shm_t *shm, int zero)
{
    sr_error_info_t *err_info = NULL;

    shm->fd = shm_open(SR_EXT_SHM, O_RDWR | O_CREAT, SR_MAIN_SHM_PERM);
    if (shm->fd == -1) {
        sr_errinfo_new(&err_info, SR_ERR_SYS, NULL, "Failed to open ext shared memory (%s).", strerror(errno));
        goto error;
    }

    /* either zero the memory or keep it exactly the way it was */
    if ((err_info = sr_shm_remap(shm, zero ? sizeof(size_t) : 0))) {
        goto error;
    }
    if (zero) {
        *((size_t *)shm->addr) = 0;
    }

    return NULL;

error:
    sr_shm_clear(shm);
    return err_info;
}

sr_mod_t *
sr_shmmain_find_module(sr_shm_t *shm_main, char *ext_shm_addr, const char *name, off_t name_off)
{
    sr_mod_t *shm_mod;

    assert(name || name_off);

    SR_SHM_MOD_FOR(shm_main->addr, shm_main->size, shm_mod) {
        if (name_off && (shm_mod->name == name_off)) {
            return shm_mod;
        } else if (name && !strcmp(ext_shm_addr + shm_mod->name, name)) {
            return shm_mod;
        }
    }

    return NULL;
}

sr_rpc_t *
sr_shmmain_find_rpc(sr_main_shm_t *main_shm, char *ext_shm_addr, const char *op_path, off_t op_path_off)
{
    sr_rpc_t *shm_rpc;
    uint16_t i;

    assert(op_path || op_path_off);

    shm_rpc = (sr_rpc_t *)(ext_shm_addr + main_shm->rpc_subs);
    for (i = 0; i < main_shm->rpc_sub_count; ++i) {
        if (op_path_off && (shm_rpc[i].op_path == op_path_off)) {
            return &shm_rpc[i];
        } else if (op_path && !strcmp(ext_shm_addr + shm_rpc[i].op_path, op_path)) {
            return &shm_rpc[i];
        }
    }

    return NULL;
}

sr_error_info_t *
sr_shmmain_lock_remap(sr_conn_ctx_t *conn, sr_lock_mode_t mode, int remap, int lydmods)
{
    sr_error_info_t *err_info = NULL;
    sr_main_shm_t *main_shm;
    sr_conn_state_t *conn_s;

    assert((mode == SR_LOCK_READ) || (mode == SR_LOCK_WRITE) || (mode == SR_LOCK_WRITE_NOSTATE));

    /* REMAP READ/WRITE LOCK */
    if ((err_info = sr_rwlock(&conn->ext_remap_lock, SR_MAIN_LOCK_TIMEOUT * 1000,
            remap ? SR_LOCK_WRITE : SR_LOCK_READ, __func__))) {
        return err_info;
    }
    main_shm = (sr_main_shm_t *)conn->main_shm.addr;

    /* MAIN SHM READ/WRITE LOCK */
    if ((err_info = sr_rwlock_with_recovery(&main_shm->lock, SR_MAIN_LOCK_TIMEOUT * 1000,
            mode == SR_LOCK_WRITE_NOSTATE ? SR_LOCK_WRITE : mode, conn, __func__))) {
        goto error_remap_unlock;
    }

    /* if SHM changed, we can safely remap it because no other session can be using the mapping (because SHM cannot
     * change while an API call is executing and SHM would be remapped already if the change happened before)
     */

    /* remap ext SHM */
    if ((err_info = sr_shm_remap(&conn->ext_shm, 0))) {
        goto error_remap_shm_unlock;
    }

    /* LYDMODS LOCK */
    if (lydmods && (err_info = sr_mlock(&main_shm->lydmods_lock, SR_MAIN_LOCK_TIMEOUT * 1000, __func__))) {
        goto error_remap_shm_unlock;
    }

    if (mode != SR_LOCK_WRITE_NOSTATE) {
        /* store information about the held lock */
        conn_s = sr_shmmain_state_find_conn(main_shm, conn->ext_shm.addr, conn, getpid());
        SR_CHECK_INT_GOTO(!conn_s, err_info, error_remap_shm_lydmods_unlock);

        if (mode == SR_LOCK_READ) {
            /* recursive read locks are supported */
            assert(((conn_s->lock.main == SR_LOCK_NONE) && !conn_s->lock.main_rcount)
                || ((conn_s->lock.main == SR_LOCK_READ) && conn_s->lock.main_rcount));
            conn_s->lock.main = mode;
            ++conn_s->lock.main_rcount;
        } else {
            assert(conn_s->lock.main == SR_LOCK_NONE);
            conn_s->lock.main = mode;
        }
    }

    return NULL;

error_remap_shm_lydmods_unlock:
    if (lydmods) {
        /* LYDMODS UNLOCK */
        sr_munlock(&main_shm->lydmods_lock);
    }
error_remap_shm_unlock:
    sr_rwunlock(&main_shm->lock, mode == SR_LOCK_WRITE_NOSTATE ? SR_LOCK_WRITE : mode, __func__);
error_remap_unlock:
    sr_rwunlock(&conn->ext_remap_lock, remap ? SR_LOCK_WRITE : SR_LOCK_READ, __func__);
    return err_info;
}

void
sr_shmmain_unlock(sr_conn_ctx_t *conn, sr_lock_mode_t mode, int remap, int lydmods)
{
    sr_error_info_t *err_info = NULL;
    sr_main_shm_t *main_shm;
    sr_conn_state_t *conn_s;

    assert((mode == SR_LOCK_READ) || (mode == SR_LOCK_WRITE) || (mode == SR_LOCK_WRITE_NOSTATE));

    main_shm = (sr_main_shm_t *)conn->main_shm.addr;
    assert(main_shm);

    if (mode != SR_LOCK_WRITE_NOSTATE) {
        /* update information about the held lock */
        conn_s = sr_shmmain_state_find_conn(main_shm, conn->ext_shm.addr, conn, getpid());
        if (!conn_s) {
            SR_ERRINFO_INT(&err_info);
            sr_errinfo_free(&err_info);
        } else if (mode == SR_LOCK_READ) {
            /* handle recursive read locks */
            assert(conn_s->lock.main == mode);
            --conn_s->lock.main_rcount;
            if (!conn_s->lock.main_rcount) {
                conn_s->lock.main = SR_LOCK_NONE;
            }
        } else {
            assert(conn_s->lock.main == mode);
            conn_s->lock.main = SR_LOCK_NONE;
        }
    }

    /* MAIN SHM UNLOCK */
    sr_rwunlock(&main_shm->lock, mode == SR_LOCK_WRITE_NOSTATE ? SR_LOCK_WRITE : mode, __func__);

    /* REMAP UNLOCK */
    sr_rwunlock(&conn->ext_remap_lock, remap ? SR_LOCK_WRITE : SR_LOCK_READ, __func__);

    if (lydmods) {
        /* LYDMODS UNLOCK */
        sr_munlock(&main_shm->lydmods_lock);
    }
}

sr_error_info_t *
sr_shmmain_rpc_subscription_add(sr_shm_t *shm_ext, off_t shm_rpc_off, const char *xpath, uint32_t priority, int sub_opts,
        uint32_t evpipe_num)
{
    sr_error_info_t *err_info = NULL;
    sr_rpc_t *shm_rpc;
    off_t xpath_off, subs_off;
    sr_rpc_sub_t *shm_sub;
    size_t new_ext_size;

    assert(xpath);

    shm_rpc = (sr_rpc_t *)(shm_ext->addr + shm_rpc_off);

    /* moving all existing subscriptions (if any) and adding a new one */
    subs_off = shm_ext->size;
    xpath_off = subs_off + (shm_rpc->sub_count + 1) * sizeof *shm_sub;
    new_ext_size = xpath_off + sr_strshmlen(xpath);

    /* remap ext SHM */
    if ((err_info = sr_shm_remap(shm_ext, new_ext_size))) {
        return err_info;
    }
    shm_rpc = (sr_rpc_t *)(shm_ext->addr + shm_rpc_off);

    /* add wasted memory */
    *((size_t *)shm_ext->addr) += shm_rpc->sub_count * sizeof *shm_sub;

    /* move subscriptions */
    memcpy(shm_ext->addr + subs_off, shm_ext->addr + shm_rpc->subs, shm_rpc->sub_count * sizeof *shm_sub);
    shm_rpc->subs = subs_off;

    /* fill new subscription */
    shm_sub = (sr_rpc_sub_t *)(shm_ext->addr + shm_rpc->subs);
    shm_sub += shm_rpc->sub_count;
    strcpy(shm_ext->addr + xpath_off, xpath);
    shm_sub->xpath = xpath_off;
    shm_sub->priority = priority;
    shm_sub->opts = sub_opts;
    shm_sub->evpipe_num = evpipe_num;

    ++shm_rpc->sub_count;

    return NULL;
}

sr_error_info_t *
sr_shmmain_rpc_subscription_del(char *ext_shm_addr, sr_rpc_t *shm_rpc, const char *xpath, uint32_t priority,
        uint32_t evpipe_num, int all_evpipe, int *last_removed)
{
    sr_error_info_t *err_info = NULL;
    sr_rpc_sub_t *shm_sub;
    uint16_t i;

    if (last_removed) {
        *last_removed = 0;
    }

    /* find the subscription */
    shm_sub = (sr_rpc_sub_t *)(ext_shm_addr + shm_rpc->subs);
    for (i = 0; i < shm_rpc->sub_count; ++i) {
continue_loop:
        if (all_evpipe) {
            if (shm_sub[i].evpipe_num == evpipe_num) {
                break;
            }
        } else if (!strcmp(ext_shm_addr + shm_sub[i].xpath, xpath) && (shm_sub[i].priority == priority)) {
            break;
        }
    }
    if (all_evpipe && (i == shm_rpc->sub_count)) {
        return NULL;
    }
    SR_CHECK_INT_RET(i == shm_rpc->sub_count, err_info);

    /* add wasted memory */
    *((size_t *)ext_shm_addr) += sizeof *shm_sub + sr_strshmlen(ext_shm_addr + shm_sub[i].xpath);

    --shm_rpc->sub_count;
    if (!shm_rpc->sub_count) {
        /* the only subscription removed */
        shm_rpc->subs = 0;
        if (last_removed) {
            *last_removed = 1;
        }
    } else if (i < shm_rpc->sub_count) {
        /* replace the removed subscription with the last one */
        memcpy(&shm_sub[i], &shm_sub[shm_rpc->sub_count], sizeof *shm_sub);
    }

    if (all_evpipe) {
        goto continue_loop;
    }

    return NULL;
}

sr_error_info_t *
sr_shmmain_add_rpc(sr_conn_ctx_t *conn, const char *op_path, sr_rpc_t **shm_rpc_p)
{
    sr_error_info_t *err_info = NULL;
    sr_main_shm_t *main_shm;
    off_t op_path_off, rpc_subs_off;
    sr_rpc_t *shm_rpc;
    size_t new_ext_size;

    main_shm = (sr_main_shm_t *)conn->main_shm.addr;
    shm_rpc = (sr_rpc_t *)(conn->ext_shm.addr + main_shm->rpc_subs);

#ifndef NDEBUG
    uint16_t i;

    /* check that this RPC does not exist yet */
    for (i = 0; i < main_shm->rpc_sub_count; ++i) {
        assert(strcmp(conn->ext_shm.addr + shm_rpc[i].op_path, op_path));
    }
#endif

    /* moving all existing RPCs (if any) and adding a new one */
    rpc_subs_off = conn->ext_shm.size;
    op_path_off = rpc_subs_off + (main_shm->rpc_sub_count + 1) * sizeof *shm_rpc;
    new_ext_size = op_path_off + sr_strshmlen(op_path);

    /* remap ext SHM, update pointers */
    if ((err_info = sr_shm_remap(&conn->ext_shm, new_ext_size))) {
        return err_info;
    }
    main_shm = (sr_main_shm_t *)conn->main_shm.addr;

    /* add wasted memory */
    *((size_t *)conn->ext_shm.addr) += main_shm->rpc_sub_count * sizeof *shm_rpc;

    /* move RPCs */
    memcpy(conn->ext_shm.addr + rpc_subs_off, conn->ext_shm.addr + main_shm->rpc_subs,
            main_shm->rpc_sub_count * sizeof *shm_rpc);
    main_shm->rpc_subs = rpc_subs_off;
    shm_rpc = (sr_rpc_t *)(conn->ext_shm.addr + main_shm->rpc_subs);

    /* fill new RPC */
    shm_rpc += main_shm->rpc_sub_count;
    strcpy(conn->ext_shm.addr + op_path_off, op_path);
    shm_rpc->op_path = op_path_off;
    shm_rpc->subs = 0;
    shm_rpc->sub_count = 0;

    ++main_shm->rpc_sub_count;

    if (shm_rpc_p) {
        *shm_rpc_p = shm_rpc;
    }
    return NULL;
}

sr_error_info_t *
sr_shmmain_del_rpc(sr_main_shm_t *main_shm, char *ext_shm_addr, const char *op_path, off_t op_path_off)
{
    sr_error_info_t *err_info = NULL;
    sr_rpc_t *shm_rpc;
    uint16_t i;

    shm_rpc = sr_shmmain_find_rpc(main_shm, ext_shm_addr, op_path, op_path_off);
    SR_CHECK_INT_RET(!shm_rpc, err_info);

    /* get index instead */
    i = shm_rpc - ((sr_rpc_t *)(ext_shm_addr + main_shm->rpc_subs));
    shm_rpc = (sr_rpc_t *)(ext_shm_addr + main_shm->rpc_subs);

    /* add wasted memory */
    *((size_t *)ext_shm_addr) += sizeof *shm_rpc + sr_strshmlen(ext_shm_addr + shm_rpc[i].op_path);

    --main_shm->rpc_sub_count;
    if (!main_shm->rpc_sub_count) {
        /* the only RPC removed */
        main_shm->rpc_subs = 0;
    } else if (i < main_shm->rpc_sub_count) {
        /* replace the removed RPC with the last one */
        memcpy(&shm_rpc[i], &shm_rpc[main_shm->rpc_sub_count], sizeof *shm_rpc);
    }

    return NULL;
}

sr_error_info_t *
sr_shmmain_update_replay_support(sr_shm_t *shm_main, char *ext_shm_addr, const char *mod_name, int replay_support)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod;

    shm_mod = sr_shmmain_find_module(shm_main, ext_shm_addr, mod_name, 0);
    SR_CHECK_INT_RET(!shm_mod, err_info);

    if ((replay_support && !(shm_mod->flags & SR_MOD_REPLAY_SUPPORT))
            || (!replay_support && (shm_mod->flags & SR_MOD_REPLAY_SUPPORT))) {
        /* update flag */
        if (replay_support) {
            shm_mod->flags |= SR_MOD_REPLAY_SUPPORT;
        } else {
            shm_mod->flags &= ~SR_MOD_REPLAY_SUPPORT;
        }
    }

    return NULL;
}
