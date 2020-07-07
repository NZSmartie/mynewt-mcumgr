/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/**
 * @file 
 * @details There are three parts to this file for suporting mcumgr log 
 * management:
 * 
 * - ``log_output`` - For formatting log messages into a final string for
 *   passing back to log_mgmt.
 * 
 * - ``log_backend`` - zephyr_log_mgmt will regiter itself a logging backend to
 *   capture log messages and add mcumgr filtering (timestamp and/or index).
 * 
 * - ``log_mgmt_impl`` - The implementation of required stub functions to
 *   transport log messages to mcumgr.
 * 
 */

#include <sys/slist.h>
#include <sys/util.h>
#include <logging/log_backend.h>
#include <logging/log_ctrl.h>
#include <logging/log_output.h>

#include <log_mgmt/log_mgmt.h>
#include <log_mgmt/log_mgmt_impl.h>
#include "log_mgmt/log_mgmt_config.h"
#include <mgmt/mgmt.h>

static const char * const severity_lvls[] = {
	"none",
	"err",
	"wrn",
	"inf",
	"dbg",
};

/** 
 * @brief Map of Zephyr LOG_LEVEL to mcumgr's severity level
 * @note For the mapping values, see:
 *       https://github.com/apache/mynewt-newtmgr/blob/5d1446c4361ce155d7817086959c301b64fcef4d/nmxact/nmp/log.go#L30-L38
 */
static const uint8_t severity_level_map[] = {
    4, /* LOG_LEVEL_NONE */
    3, /* LOG_LEVEL_ERR */
    2, /* LOG_LEVEL_WRN */
    1, /* LOG_LEVEL_INF */
    0, /* LOG_LEVEL_DBG */
};

uint8_t log_mgmt_output_buffer[CONFIG_LOG_MGMT_BODY_LEN];

struct zephyr_log_mgmt_walk_arg {
    log_mgmt_foreach_entry_fn *cb;
    struct log_mgmt_entry entry;
    void *arg;
};

int log_mgmt_output_func(uint8_t *buf, size_t size, void *ctx)
{
    struct zephyr_log_mgmt_walk_arg *walk_arg;
    
    walk_arg = (struct zephyr_log_mgmt_walk_arg*)ctx;

    walk_arg->entry.data = (void*)buf;
    walk_arg->entry.len = size;
    walk_arg->cb(&walk_arg->entry, walk_arg->arg);

    // TODO: Verify this is the intended use of offset.
    walk_arg->entry.offset += size;

    return size;
}

LOG_OUTPUT_DEFINE(log_mgmt_output, log_mgmt_output_func, log_mgmt_output_buffer,
                  CONFIG_LOG_MGMT_BODY_LEN);

/** @brief Linked-list backing for storing log messages */
struct log_mgmt_backend_msg {
    sys_snode_t node;
    struct log_msg *msg;
    uint32_t index;
};

/** @brief local index counter. */
uint32_t log_mgmt_msg_index = 0;

/* Allocate a slab of memory for holding the log_mgmt_backend_msg entries */
K_MEM_SLAB_DEFINE(log_mgmt_slab, sizeof(struct log_mgmt_backend_msg),
                  CONFIG_LOG_MGMT_LOG_QUEUE_SIZE, 4);

/** @brief linked-list as an iterable FIFO queue */
static sys_slist_t log_mgmt_queue;

/** @brief Mutex for preventing any unwanted changes to the log_mgmt_queue */
static struct k_mutex log_mgmt_queue_mutex;

/** @brief Remove all expired messages from the head of the queue */
static void flush_expired_messages()
{
	int err;
	struct log_mgmt_backend_msg *msg;
	u32_t timeout = CONFIG_LOG_MGMT_LOG_TIMEOUT * 1000;
	u32_t now = k_uptime_get_32();

    err = k_mutex_lock(&log_mgmt_queue_mutex, K_FOREVER);

	while (1) {
        msg = SYS_SLIST_PEEK_HEAD_CONTAINER(&log_mgmt_queue, msg, node);
		
        if (msg != NULL && ((now - msg->msg->hdr.timestamp) > timeout)) {
			(void)sys_slist_get(&log_mgmt_queue);
			log_msg_put(msg->msg);
            k_mem_slab_free(&log_mgmt_slab, (void**)&msg);
		} else {
			break;
		}
	}

    k_mutex_unlock(&log_mgmt_queue_mutex);
}

static void enqueue_log_msg(struct log_msg *msg)
{
	int err;
	struct log_mgmt_backend_msg *t_msg;
    
    k_mutex_lock(&log_mgmt_queue_mutex, K_FOREVER);

    err = k_mem_slab_alloc(&log_mgmt_slab, (void**)&t_msg, K_MSEC(100));

	switch (err)
    {
	    case 0:
            t_msg->msg = msg;
            t_msg->index = log_mgmt_msg_index++;
            sys_slist_append(&log_mgmt_queue, &t_msg->node);
		    break;
        case -EAGAIN:
        case -ENOMEM:
            flush_expired_messages();

            err = k_mem_slab_alloc(&log_mgmt_slab, (void**)&t_msg, K_NO_WAIT);
            if (err) {
                /* Unexpected case as we just freed one element and
                * there is no other context that puts into the msgq.
                */
                __ASSERT_NO_MSG(0);
                break;
    		}

            t_msg->msg = msg;
            t_msg->index = log_mgmt_msg_index++;
            sys_slist_append(&log_mgmt_queue, &t_msg->node);
	    	break;
        default:
            /* Other errors are not expected. */
            __ASSERT_NO_MSG(0);
            break;
	}

    k_mutex_unlock(&log_mgmt_queue_mutex);
}

static void put(const struct log_backend *const backend,
                struct log_msg *msg)
{
    /* Increment the reference counter to msg to prevent de-allocation */
	log_msg_get(msg);
    enqueue_log_msg(msg);
}

static void dropped(const struct log_backend *const backend, u32_t cnt)
{
    /* 🤷‍♀️ */
}

static void panic(const struct log_backend *const backend)
{
    /* 🤷‍♀️ */
}

static void init(void)
{
    sys_slist_init(&log_mgmt_queue);
    k_mutex_init(&log_mgmt_queue_mutex);

    log_mgmt_register_group();
}

const struct log_backend_api log_mgmt_backend_api = {
    .put = put,
	.put_sync_string = NULL,
	.put_sync_hexdump = NULL,
	.dropped = dropped,
	.panic = panic,
	.init = init,
};

LOG_BACKEND_DEFINE(log_mgmt_backend, log_mgmt_backend_api, true);

int
log_mgmt_impl_get_log(int idx, struct log_mgmt_log *out_log)
{
    if (idx >= log_src_cnt_get(CONFIG_LOG_DOMAIN_ID)) {
        return LOG_MGMT_ERR_ENOENT;
    }

    out_log->name = log_source_name_get(CONFIG_LOG_DOMAIN_ID, idx);
    /* Logs received in zephyr_log_mgmt backend are stored in memory */
    out_log->type = LOG_MGMT_TYPE_MEMORY;
    out_log->index = idx;
    return 0;
}

int
log_mgmt_impl_get_module(int idx, const char **out_module_name)
{
    if (idx != 0) {
        return LOG_MGMT_ERR_ENOENT;
    }

    *out_module_name = "DEFAULT";
    return 0;
}

int
log_mgmt_impl_get_level(int idx, const char **out_level_name)
{
    uint32_t log_level;

    if (idx >= log_src_cnt_get(CONFIG_LOG_DOMAIN_ID)) {
        return LOG_MGMT_ERR_ENOENT;
    } else {
        log_level = log_filter_get(&log_mgmt_backend, CONFIG_LOG_DOMAIN_ID, idx, true);
        *out_level_name = severity_lvls[log_level];
        return 0;
    }
}

int
log_mgmt_impl_foreach_entry(const char *log_name,
                            const struct log_mgmt_filter *filter,
                            log_mgmt_foreach_entry_fn *cb, void *arg) {
    struct log_mgmt_backend_msg *msg;
    sys_snode_t *sn, *sns;
    struct zephyr_log_mgmt_walk_arg walk_arg = {
        .cb = cb,
        .entry = {0},
        .arg = arg,
    };

    /* Try to resolve the log module's source id from log_name */ 
    int log_source;
    int source_count = log_src_cnt_get(CONFIG_LOG_DOMAIN_ID);
    for(log_source = 0; log_source < source_count; log_source++) {
        const char *log_candidate;
        log_candidate = log_source_name_get(CONFIG_LOG_DOMAIN_ID, log_source);
        
        if (strcmp(log_name, log_candidate) == 0) {
            break;
        }
    }
    if (log_source >= source_count) {
        return LOG_MGMT_ERR_ENOENT;
    }

    /* Helper macro for interating over linked-list. */
    SYS_SLIST_FOR_EACH_NODE_SAFE(&log_mgmt_queue, sn, sns) {
        msg = CONTAINER_OF(sn, struct log_mgmt_backend_msg, node);
        
        // TODO: attach an index to incoming log_msgs so that we may filter them here
        if (msg->msg->hdr.ids.source_id != log_source ||
            msg->msg->hdr.timestamp < filter->min_timestamp) {
            continue;
        }

        walk_arg.entry.ts = msg->msg->hdr.timestamp;
        /* https://github.com/apache/mynewt-newtmgr/blob/5d1446c4361ce155d7817086959c301b64fcef4d/nmxact/nmp/log.go#L46-L67 */
        walk_arg.entry.module = 0; /* Set the module type to default */
        walk_arg.entry.level = severity_level_map[msg->msg->hdr.ids.level];
        /* log index isn't tracked */
        walk_arg.entry.index = 0;
        walk_arg.entry.type = LOG_MGMT_ETYPE_STRING;
        /* Reset offset as log_output may be called several times. */
        walk_arg.entry.offset = 0;

        log_output_ctx_set(&log_mgmt_output, &walk_arg);
        log_output_msg_process(&log_mgmt_output, msg->msg, 0);
    }

    return LOG_MGMT_ERR_ENOENT;
}

int log_mgmt_impl_clear(const char *log_name)
{
    int err;
    int log_source;
    struct log_mgmt_backend_msg *msg;

    /* Try to resolve the log module's source id from log_name */ 
    int source_count = log_src_cnt_get(CONFIG_LOG_DOMAIN_ID);
    for(log_source = 0; log_source < source_count; log_source++) {
        const char *log_candidate;
        log_candidate = log_source_name_get(CONFIG_LOG_DOMAIN_ID, log_source);
        
        if (strcmp(log_name, log_candidate) == 0) {
            break;
        }
    }

    if (log_source >= source_count) {
        return LOG_MGMT_ERR_ENOENT;
    }

    err = k_mutex_lock(&log_mgmt_queue_mutex, K_FOREVER);

	while (1) {
        msg = SYS_SLIST_PEEK_HEAD_CONTAINER(&log_mgmt_queue, msg, node);
		
        if (msg == NULL) {
            break;
        }
        if (msg->msg->hdr.ids.source_id == log_source) {
			(void)sys_slist_get(&log_mgmt_queue);
			log_msg_put(msg->msg);
            k_mem_slab_free(&log_mgmt_slab, (void**)&msg);
		}
	}

    k_mutex_unlock(&log_mgmt_queue_mutex);

    return 0;
}
