/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2019-2021 The Fluent Bit Authors
 *  Copyright (C) 2015-2018 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_output_plugin.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_metrics.h>
#include <msgpack.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "file.h"

#ifdef FLB_SYSTEM_WINDOWS
#define NEWLINE "\r\n"
#else
#define NEWLINE "\n"
#endif

struct flb_file_conf {
    const char *out_path;
    const char *out_file;
    const char *delimiter;
    const char *label_delimiter;
    const char *template;
    int format;
    int csv_column_names;
    struct flb_output_instance *ins;
};

static char *check_delimiter(const char *str)
{
    if (str == NULL) {
        return NULL;
    }

    if (!strcasecmp(str, "\\t") || !strcasecmp(str, "tab")) {
        return "\t";
    }
    else if (!strcasecmp(str, "space")) {
        return " ";
    }
    else if (!strcasecmp(str, "comma")) {
        return ",";
    }

    return NULL;
}


static int cb_file_init(struct flb_output_instance *ins,
                        struct flb_config *config,
                        void *data)
{
    int ret;
    const char *tmp;
    char *ret_str;
    (void) config;
    (void) data;
    struct flb_file_conf *ctx;

    ctx = flb_calloc(1, sizeof(struct flb_file_conf));
    if (!ctx) {
        flb_errno();
        return -1;
    }
    ctx->ins = ins;
    ctx->format = FLB_OUT_FILE_FMT_JSON; /* default */
    ctx->delimiter = NULL;
    ctx->label_delimiter = NULL;
    ctx->template = NULL;

    ret = flb_output_config_map_set(ins, (void *) ctx);
    if (ret == -1) {
        flb_free(ctx);
        return -1;
    }

    /* Optional, file format */
    tmp = flb_output_get_property("Format", ins);
    if (tmp) {
        if (!strcasecmp(tmp, "csv")) {
            ctx->format    = FLB_OUT_FILE_FMT_CSV;
            ctx->delimiter = ",";
        }
        else if (!strcasecmp(tmp, "ltsv")) {
            ctx->format    = FLB_OUT_FILE_FMT_LTSV;
            ctx->delimiter = "\t";
            ctx->label_delimiter = ":";
        }
        else if (!strcasecmp(tmp, "plain")) {
            ctx->format    = FLB_OUT_FILE_FMT_PLAIN;
            ctx->delimiter = NULL;
            ctx->label_delimiter = NULL;
        }
        else if (!strcasecmp(tmp, "msgpack")) {
            ctx->format    = FLB_OUT_FILE_FMT_MSGPACK;
            ctx->delimiter = NULL;
            ctx->label_delimiter = NULL;
        }
        else if (!strcasecmp(tmp, "template")) {
            ctx->format    = FLB_OUT_FILE_FMT_TEMPLATE;
        }
        else {
            flb_plg_error(ctx->ins, "unknown format %s. abort.", tmp);
            flb_free(ctx);
            return -1;
        }
    }

    tmp = flb_output_get_property("delimiter", ins);
    ret_str = check_delimiter(tmp);
    if (ret_str != NULL) {
        ctx->delimiter = ret_str;
    }

    tmp = flb_output_get_property("label_delimiter", ins);
    ret_str = check_delimiter(tmp);
    if (ret_str != NULL) {
        ctx->label_delimiter = ret_str;
    }

    /* Set the context */
    flb_output_set_context(ins, ctx);

    return 0;
}

static int csv_output(FILE *fp, int column_names,
                      struct flb_time *tm, msgpack_object *obj,
                      struct flb_file_conf *ctx)
{
    int i;
    int map_size;
    msgpack_object_kv *kv = NULL;

    if (obj->type == MSGPACK_OBJECT_MAP && obj->via.map.size > 0) {
        kv = obj->via.map.ptr;
        map_size = obj->via.map.size;

        if (column_names == FLB_TRUE) {
            fprintf(fp, "timestamp%s", ctx->delimiter);
            for (i = 0; i < map_size; i++) {
                msgpack_object_print(fp, (kv+i)->key);
                if (i + 1 < map_size) {
                    fprintf(fp, "%s", ctx->delimiter);
                }
            }
            fprintf(fp, NEWLINE);
        }

        fprintf(fp, "%lld.%.09ld%s",
                (long long) tm->tm.tv_sec, tm->tm.tv_nsec, ctx->delimiter);

        for (i = 0; i < map_size - 1; i++) {
            msgpack_object_print(fp, (kv+i)->val);
            fprintf(fp, "%s", ctx->delimiter);
        }

        msgpack_object_print(fp, (kv+(map_size-1))->val);
        fprintf(fp, NEWLINE);
    }
    return 0;
}

static int ltsv_output(FILE *fp, struct flb_time *tm, msgpack_object *obj,
                       struct flb_file_conf *ctx)
{
    msgpack_object_kv *kv = NULL;
    int i;
    int map_size;

    if (obj->type == MSGPACK_OBJECT_MAP && obj->via.map.size > 0) {
        kv = obj->via.map.ptr;
        map_size = obj->via.map.size;
        fprintf(fp, "\"time\"%s%f%s",
                ctx->label_delimiter,
                flb_time_to_double(tm),
                ctx->delimiter);

        for (i = 0; i < map_size - 1; i++) {
            msgpack_object_print(fp, (kv+i)->key);
            fprintf(fp, "%s", ctx->label_delimiter);
            msgpack_object_print(fp, (kv+i)->val);
            fprintf(fp, "%s", ctx->delimiter);
        }

        msgpack_object_print(fp, (kv+(map_size-1))->key);
        fprintf(fp, "%s", ctx->label_delimiter);
        msgpack_object_print(fp, (kv+(map_size-1))->val);
        fprintf(fp, NEWLINE);
    }
    return 0;
}

static int template_output_write(struct flb_file_conf *ctx,
                                 FILE *fp, struct flb_time *tm, msgpack_object *obj,
                                 const char *key, int size)
{
    int i;
    msgpack_object_kv *kv;

    /*
     * Right now we treat "{time}" specially and fill the placeholder
     * with the metadata timestamp (formatted as float).
     */
    if (!strncmp(key, "time", size)) {
        fprintf(fp, "%f", flb_time_to_double(tm));
        return 0;
    }

    if (obj->type != MSGPACK_OBJECT_MAP) {
        flb_plg_error(ctx->ins, "invalid object type (type=%i)", obj->type);
        return -1;
    }

    for (i = 0; i < obj->via.map.size; i++) {
        kv = obj->via.map.ptr + i;

        if (size != kv->key.via.str.size) {
            continue;
        }

        if (!memcmp(key, kv->key.via.str.ptr, size)) {
            if (kv->val.type == MSGPACK_OBJECT_STR) {
                fwrite(kv->val.via.str.ptr, 1, kv->val.via.str.size, fp);
            }
            else {
                msgpack_object_print(fp, kv->val);
            }
            return 0;
        }
    }
    return -1;
}

/*
 * Python-like string templating for out_file.
 *
 * This accepts a format string like "my name is {name}" and fills
 * placeholders using corresponding values in a record.
 *
 * e.g. {"name":"Tom"} => "my name is Tom"
 */
static int template_output(FILE *fp, struct flb_time *tm, msgpack_object *obj,
                           struct flb_file_conf *ctx)
{
    int i;
    int len = strlen(ctx->template);
    int keysize;
    const char *key;
    const char *pos;
    const char *inbrace = NULL;  /* points to the last open brace */

    for (i = 0; i < len; i++) {
        pos = ctx->template + i;
        if (*pos == '{') {
            if (inbrace) {
                /*
                 * This means that we find another open brace inside
                 * braces (e.g. "{a{b}"). Ignore the previous one.
                 */
                fwrite(inbrace, 1, pos - inbrace, fp);
            }
            inbrace = pos;
        }
        else if (*pos == '}' && inbrace) {
            key = inbrace + 1;
            keysize = pos - inbrace - 1;

            if (template_output_write(ctx, fp, tm, obj, key, keysize)) {
                fwrite(inbrace, 1, pos - inbrace + 1, fp);
            }
            inbrace = NULL;
        }
        else {
            if (!inbrace) {
                fputc(*pos, fp);
            }
        }
    }

    /* Handle an unclosed brace like "{abc" */
    if (inbrace) {
        fputs(inbrace, fp);
    }
    fputs(NEWLINE, fp);
    return 0;
}


static int plain_output(FILE *fp, msgpack_object *obj, size_t alloc_size)
{
    char *buf;

    buf = flb_msgpack_to_json_str(alloc_size, obj);
    if (buf) {
        fprintf(fp, "%s" NEWLINE,
                buf);
        flb_free(buf);
    }
    return 0;
}

static void print_metrics_text(struct flb_output_instance *ins,
                               FILE *fp,
                               const void *data, size_t bytes)
{
    int ret;
    size_t off = 0;
    cmt_sds_t text;
    struct cmt *cmt = NULL;

    /* get cmetrics context */
    ret = cmt_decode_msgpack_create(&cmt, (char *) data, bytes, &off);
    if (ret != 0) {
        flb_plg_error(ins, "could not process metrics payload");
        return;
    }

    /* convert to text representation */
    text = cmt_encode_text_create(cmt);

    /* destroy cmt context */
    cmt_destroy(cmt);

    fprintf(fp, "%s", text);
    cmt_encode_text_destroy(text);
}

static void cb_file_flush(const void *data, size_t bytes,
                          const char *tag, int tag_len,
                          struct flb_input_instance *ins,
                          void *out_context,
                          struct flb_config *config)
{
    int ret;
    int column_names;
    FILE * fp;
    msgpack_unpacked result;
    size_t off = 0;
    size_t last_off = 0;
    size_t alloc_size = 0;
    size_t total;
    char out_file[PATH_MAX];
    char *buf;
    char *tag_buf;
    long file_pos;
    msgpack_object *obj;
    struct flb_file_conf *ctx = out_context;
    struct flb_time tm;
    (void) config;

    /* Set the right output file */
    if (ctx->out_path) {
        if (ctx->out_file) {
            snprintf(out_file, PATH_MAX - 1, "%s/%s",
                     ctx->out_path, ctx->out_file);
        }
        else {
            snprintf(out_file, PATH_MAX - 1, "%s/%s",
                     ctx->out_path, tag);
        }
    }
    else {
        if (ctx->out_file) {
            snprintf(out_file, PATH_MAX - 1, "%s", ctx->out_file);
        }
        else {
            snprintf(out_file, PATH_MAX - 1, "%s", tag);
        }
    }

    /* Open output file with default name as the Tag */
    fp = fopen(out_file, "ab+");
    if (fp == NULL) {
        flb_errno();
        flb_plg_error(ctx->ins, "error opening: %s", out_file);
        FLB_OUTPUT_RETURN(FLB_ERROR);
    }

    /*
     * Get current file stream position, we gather this in case 'csv' format
     * needs to write the column names.
     */
    file_pos = ftell(fp);

    /* Check if the event type is metrics, handle the payload differently */
    if (flb_input_event_type_is_metric(ins)) {
        print_metrics_text(ctx->ins, fp, (char *) data, bytes);
        fclose(fp);
        FLB_OUTPUT_RETURN(FLB_OK);
    }

    tag_buf = flb_malloc(tag_len + 1);
    if (!tag_buf) {
        flb_errno();
        fclose(fp);
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }
    memcpy(tag_buf, tag, tag_len);
    tag_buf[tag_len] = '\0';


    /*
     * Msgpack output format used to create unit tests files, useful for
     * Fluent Bit developers.
     */
    if (ctx->format == FLB_OUT_FILE_FMT_MSGPACK) {
        off = 0;
        total = 0;

        do {
            ret = fwrite((char *)data + off, 1, bytes - off, fp);
            if (ret < 0) {
                flb_errno();
                fclose(fp);
                flb_free(tag_buf);
                FLB_OUTPUT_RETURN(FLB_RETRY);
            }
            total += ret;
        } while (total < bytes);

        fclose(fp);
        flb_free(tag_buf);
        FLB_OUTPUT_RETURN(FLB_OK);
    }

    /*
     * Upon flush, for each array, lookup the time and the first field
     * of the map to use as a data point.
     */
    msgpack_unpacked_init(&result);
    while (msgpack_unpack_next(&result, data, bytes, &off) == MSGPACK_UNPACK_SUCCESS) {
        alloc_size = (off - last_off) + 128; /* JSON is larger than msgpack */
        last_off = off;

        flb_time_pop_from_msgpack(&tm, &result, &obj);

        switch (ctx->format){
        case FLB_OUT_FILE_FMT_JSON:
            buf = flb_msgpack_to_json_str(alloc_size, obj);
            if (buf) {
                fprintf(fp, "%s: [%"PRIu64".%09lu, %s]" NEWLINE,
                        tag_buf,
                        tm.tm.tv_sec, tm.tm.tv_nsec,
                        buf);
                flb_free(buf);
            }
            else {
                msgpack_unpacked_destroy(&result);
                fclose(fp);
                flb_free(tag_buf);
                FLB_OUTPUT_RETURN(FLB_RETRY);
            }
            break;
        case FLB_OUT_FILE_FMT_CSV:
            if (ctx->csv_column_names == FLB_TRUE && file_pos == 0) {
                column_names = FLB_TRUE;
                file_pos = 1;
            }
            else {
                column_names = FLB_FALSE;
            }
            csv_output(fp, column_names, &tm, obj, ctx);
            break;
        case FLB_OUT_FILE_FMT_LTSV:
            ltsv_output(fp, &tm, obj, ctx);
            break;
        case FLB_OUT_FILE_FMT_PLAIN:
            plain_output(fp, obj, alloc_size);
            break;
        case FLB_OUT_FILE_FMT_TEMPLATE:
            template_output(fp, &tm, obj, ctx);
            break;
        }
    }

    flb_free(tag_buf);
    msgpack_unpacked_destroy(&result);
    fclose(fp);

    FLB_OUTPUT_RETURN(FLB_OK);
}

static int cb_file_exit(void *data, struct flb_config *config)
{
    struct flb_file_conf *ctx = data;

    if (!ctx) {
        return 0;
    }

    flb_free(ctx);
    return 0;
}

/* Configuration properties map */
static struct flb_config_map config_map[] = {
    {
     FLB_CONFIG_MAP_STR, "path", NULL,
     0, FLB_TRUE, offsetof(struct flb_file_conf, out_path),
     "Absolute path to store the files. This parameter is optional"
    },

    {
     FLB_CONFIG_MAP_STR, "file", NULL,
     0, FLB_TRUE, offsetof(struct flb_file_conf, out_file),
     "Name of the target file to write the records. If 'path' is specified, "
     "the value is prefixed"
    },

    {
     FLB_CONFIG_MAP_STR, "format", NULL,
     0, FLB_FALSE, 0,
     "Specify the output data format, the available options are: plain (json), "
     "csv, ltsv and template. If no value is set the outgoing data is formatted "
     "using the tag and the record in json"
    },

    {
     FLB_CONFIG_MAP_STR, "delimiter", NULL,
     0, FLB_FALSE, 0,
     "Set a custom delimiter for the records"
    },

    {
     FLB_CONFIG_MAP_STR, "label_delimiter", NULL,
     0, FLB_FALSE, 0,
     "Set a custom label delimiter, to be used with 'ltsv' format"
    },

    {
     FLB_CONFIG_MAP_STR, "template", "{time} {message}",
     0, FLB_TRUE, offsetof(struct flb_file_conf, template),
     "Set a custom template format for the data"
    },

    {
     FLB_CONFIG_MAP_BOOL, "csv_column_names", "false",
     0, FLB_TRUE, offsetof(struct flb_file_conf, csv_column_names),
     "Add column names (keys) in the first line of the target file"
    },

    /* EOF */
    {0}
};

struct flb_output_plugin out_file_plugin = {
    .name         = "file",
    .description  = "Generate log file",
    .cb_init      = cb_file_init,
    .cb_flush     = cb_file_flush,
    .cb_exit      = cb_file_exit,
    .flags        = 0,
    .event_type   = FLB_OUTPUT_LOGS | FLB_OUTPUT_METRICS,
    .config_map   = config_map,
};
