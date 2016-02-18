/**
 * @file request_processor.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief Implementation of Sysrepo's Request Processor.
 *
 * @copyright
 * Copyright 2015 Cisco Systems, Inc.
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

#include <time.h>
#include <inttypes.h>
#include <pthread.h>

#include "sr_common.h"
#include "connection_manager.h"
#include "data_manager.h"
#include "rp_data_tree.h"

#define RP_THREAD_COUNT 4            /**< Number of threads that RP uses for processing. */
#define RP_INIT_REQ_QUEUE_SIZE 10    /**< Initial size of the request queue. */

/*
 * Attributes that can significantly affect performance of the threadpool.
 */
#define RP_REQ_PER_THREADS 2           /**< Number of requests that can be WAITING in queue per each thread before waking up another thread. */
#define RP_THREAD_SPIN_TIMEOUT 500000  /**< Time in nanoseconds (500000 equals to a half of a millisecond).
                                            Enables thread spinning if a thread needs to be woken up again in less than this timeout. */
#define RP_THREAD_SPIN_MIN 1000        /**< Minimum number of cycles that a thread will spin before going to sleep, if spin is enabled. */
#define RP_THREAD_SPIN_MAX 1000000     /**< Maximum number of cycles that a thread can spin before going to sleep. */

/**
 * @brief Structure that holds the context of an instance of Request Processor.
 */
typedef struct rp_ctx_s {
    cm_ctx_t *cm_ctx;                        /**< Connection Manager context. */
    dm_ctx_t *dm_ctx;                        /**< Data Manager Context. */

    pthread_t thread_pool[RP_THREAD_COUNT];  /**< Thread pool. */
    size_t active_threads;                   /**< Number of active (non-sleeping) threads. */
    struct timespec last_thread_wakeup;      /**< Timestamp of the last thread wake-up event. */
    size_t thread_spin_limit;                /**< Current limit of thread spinning before going to sleep. */
    bool stop_requested;                     /**< Stopping of all threads has been requested. */

    sr_cbuff_t *request_queue;               /**< Input request queue. */
    pthread_mutex_t request_queue_mutex;     /**< Request queue mutex. */
    pthread_cond_t request_queue_cv;         /**< Request queue condition variable. */
} rp_ctx_t;

/**
 * @brief Structure that holds Request Processor's per-session context.
 */
typedef struct rp_session_s {
    uint32_t id;                         /**< Assigned session id. */
    const ac_ucred_t *user_credentials;  /**< Credentials of the user who the session belongs to. */
    sr_datastore_t datastore;            /**< Datastore selected for this session. */
    uint32_t msg_count;                  /**< Count of unprocessed messages (including waiting in queue). */
    pthread_mutex_t msg_count_mutex;     /**< Mutex for msg_count counter. */
    bool stop_requested;                 /**< Session stop has been requested. */
    dm_session_t *dm_session;            /**< Per session data manager context */
    rp_dt_get_items_ctx_t get_items_ctx; /**< Context for get_items_iter calls */
} rp_session_t;

/**
 * @brief Request context (for storing requests inside of the request queue).
 */
typedef struct rp_request_s {
    rp_session_t *session;  /**< Request Processor's session. */
    Sr__Msg *msg;           /**< Message to be processed. */
} rp_request_t;

/**
 * @brief Processes a list_schemas request.
 */
static int
rp_list_schemas_req_process(const rp_ctx_t *rp_ctx, const rp_session_t *session, Sr__Msg *msg)
{
    Sr__Msg *resp = NULL;
    sr_schema_t *schemas = NULL;
    size_t schema_cnt = 0;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(rp_ctx, session, msg, msg->request, msg->request->list_schemas_req);

    SR_LOG_DBG_MSG("Processing list_schemas request.");

    /* allocate the response */
    rc = sr_pb_resp_alloc(SR__OPERATION__LIST_SCHEMAS, session->id, &resp);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("Cannot allocate list_schemas response.");
        return SR_ERR_NOMEM;
    }

    /* retrieve schemas from DM */
    rc = dm_list_schemas(rp_ctx->dm_ctx, session->dm_session, &schemas, &schema_cnt);

    /* copy schemas to response */
    if (SR_ERR_OK == rc) {
        rc = sr_schemas_sr_to_gpb(schemas, schema_cnt, &resp->response->list_schemas_resp->schemas);
    }
    if (SR_ERR_OK == rc) {
        resp->response->list_schemas_resp->n_schemas = schema_cnt;
    }
    sr_free_schemas(schemas, schema_cnt);

    /* set response result code */
    resp->response->result = rc;

    /* send the response */
    rc = cm_msg_send(rp_ctx->cm_ctx, resp);

    return rc;
}

/**
 * @brief Processes a get_item request.
 */
static int
rp_get_item_req_process(const rp_ctx_t *rp_ctx, const rp_session_t *session, Sr__Msg *msg)
{
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(rp_ctx, session, msg, msg->request, msg->request->get_item_req);

    SR_LOG_DBG_MSG("Processing get_item request.");

    Sr__Msg *resp = NULL;
    rc = sr_pb_resp_alloc(SR__OPERATION__GET_ITEM, session->id, &resp);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("Memory allocation failed");
        return SR_ERR_NOMEM;
    }

    sr_val_t *value = NULL;
    char *xpath = msg->request->get_item_req->path;

    /* get value from data manager*/
    rc = rp_dt_get_value_wrapper(rp_ctx->dm_ctx, session->dm_session, xpath, &value);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR("Get item failed for '%s', session id=%"PRIu32".", xpath, session->id);
    }

    /* copy value to gpb*/
    if (SR_ERR_OK == rc){
        rc = sr_dup_val_t_to_gpb(value, &resp->response->get_item_resp->value);
        if (SR_ERR_OK != rc){
            SR_LOG_ERR("Copying sr_val_t to gpb failed for xpath '%s'", xpath);
        }
    }

    /* set response code */
    resp->response->result = rc;

    rc = cm_msg_send(rp_ctx->cm_ctx, resp);

    sr_free_val(value);

    return rc;
}

/**
 * @brief Processes a get_items request.
 */
static int
rp_get_items_req_process(const rp_ctx_t *rp_ctx, rp_session_t *session, Sr__Msg *msg)
{
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(rp_ctx, session, msg, msg->request, msg->request->get_items_req);

    SR_LOG_DBG_MSG("Processing get_items request.");

    Sr__Msg *resp = NULL;
    rc = sr_pb_resp_alloc(SR__OPERATION__GET_ITEMS, session->id, &resp);

    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("Memory allocation failed");
        return SR_ERR_NOMEM;
    }

    sr_val_t **values = NULL;
    size_t count = 0;
    char *xpath = msg->request->get_items_req->path;
    bool recursive = msg->request->get_items_req->recursive;
    size_t offset = msg->request->get_items_req->offset;
    size_t limit = msg->request->get_items_req->limit;

    if (msg->request->get_items_req->has_recursive || msg->request->get_items_req->has_offset ||
            msg->request->get_items_req->has_limit){

        rc = rp_dt_get_values_wrapper_with_opts(rp_ctx->dm_ctx, session->dm_session, &session->get_items_ctx, xpath,
        recursive, offset, limit, &values, &count);
    }
    else {
        rc = rp_dt_get_values_wrapper(rp_ctx->dm_ctx, session->dm_session, xpath, &values, &count);
    }

    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Get items failed for '%s', session id=%"PRIu32".", xpath, session->id);
        goto cleanup;
    }
    SR_LOG_DBG("%zu items found for '%s', session id=%"PRIu32".", count, xpath, session->id);

    if (0 == count){
        SR_LOG_DBG("No items found for '%s', session id=%"PRIu32".", xpath, session->id);
        rc = SR_ERR_NOT_FOUND;
        resp->response->get_items_resp->n_values = 0;
        goto cleanup;
    }


    resp->response->get_items_resp->values = calloc(count, sizeof(Sr__Value *));
    if (NULL == resp->response->get_items_resp->values){
        SR_LOG_ERR_MSG("Memory allocation failed");
        rc = SR_ERR_NOMEM;
        goto cleanup;
    }

    /* copy value to gpb*/
    if (SR_ERR_OK == rc) {
        for (size_t i = 0; i< count; i++){
            rc = sr_dup_val_t_to_gpb(values[i], &resp->response->get_items_resp->values[i]);
            if (SR_ERR_OK != rc) {
                SR_LOG_ERR("Copying sr_val_t to gpb failed for xpath '%s'", xpath);
                for (size_t j = 0; j<i; j++){
                    sr__value__free_unpacked(resp->response->get_items_resp->values[j], NULL);
                }
                free(resp->response->get_items_resp->values);
            }
        }
        resp->response->get_items_resp->n_values = count;
    }

cleanup:
    /* set response code */
    resp->response->result = rc;

    rc = cm_msg_send(rp_ctx->cm_ctx, resp);
    for (size_t i = 0; i< count; i++){
        sr_free_val(values[i]);
    }
    free(values);

    return rc;
}

/**
 * @brief Processes a set_item request.
 */
static int
rp_set_item_req_process(const rp_ctx_t *rp_ctx, const rp_session_t *session, Sr__Msg *msg)
{
    Sr__Msg *resp = NULL;
    char *xpath = NULL;
    sr_val_t value = { 0 };
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(rp_ctx, session, msg, msg->request, msg->request->set_item_req);

    SR_LOG_DBG_MSG("Processing set_item request.");

    xpath = msg->request->set_item_req->path;

    /* allocate the response */
    rc = sr_pb_resp_alloc(SR__OPERATION__SET_ITEM, session->id, &resp);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("Allocation of set_item response failed.");
        return SR_ERR_NOMEM;
    }

    if (NULL != msg->request->set_item_req->value) {
        /* copy the value from gpb */
        rc = sr_copy_gpb_to_val_t(msg->request->set_item_req->value, &value);
        if (SR_ERR_OK != rc) {
            SR_LOG_ERR("Copying gpb value to sr_val_t failed for xpath '%s'", xpath);
        }

        /* set the value in data manager */
        if (SR_ERR_OK == rc) {
            rc = rp_dt_set_item(rp_ctx->dm_ctx, session->dm_session,
                    xpath, msg->request->set_item_req->options, &value);
        }
        sr_free_val_content(&value);
    }
    else{
        /* when creating list or presence container value can be NULL */
        rc = rp_dt_set_item(rp_ctx->dm_ctx, session->dm_session,
                    xpath, msg->request->set_item_req->options, NULL);
    }

    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Set item failed for '%s', session id=%"PRIu32".", xpath, session->id);
    }

    /* set response code */
    resp->response->result = rc;

    /* send the response */
    rc = cm_msg_send(rp_ctx->cm_ctx, resp);

    return rc;
}

/**
 * @brief Processes a delete_item request.
 */
static int
rp_delete_item_req_process(const rp_ctx_t *rp_ctx, const rp_session_t *session, Sr__Msg *msg)
{
    Sr__Msg *resp = NULL;
    char *xpath = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(rp_ctx, session, msg, msg->request, msg->request->delete_item_req);

    SR_LOG_DBG_MSG("Processing delete_item request.");

    xpath = msg->request->delete_item_req->path;

    /* allocate the response */
    rc = sr_pb_resp_alloc(SR__OPERATION__DELETE_ITEM, session->id, &resp);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("Allocation of delete_item response failed.");
        return SR_ERR_NOMEM;
    }

    /* delete the item in data manager */
    rc = rp_dt_delete_item(rp_ctx->dm_ctx, session->dm_session,
            xpath, msg->request->delete_item_req->options);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR("Delete item failed for '%s', session id=%"PRIu32".", xpath, session->id);
    }

    /* set response code */
    resp->response->result = rc;

    /* send the response */
    rc = cm_msg_send(rp_ctx->cm_ctx, resp);

    return rc;
}

/**
 * @brief Processes a move_item request.
 */
static int
rp_move_item_req_process(const rp_ctx_t *rp_ctx, const rp_session_t *session, Sr__Msg *msg)
{
    Sr__Msg *resp = NULL;
    char *xpath = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(rp_ctx, session, msg, msg->request, msg->request->move_item_req);

    SR_LOG_DBG_MSG("Processing move_item request.");

    xpath = msg->request->move_item_req->path;

    /* allocate the response */
    rc = sr_pb_resp_alloc(SR__OPERATION__MOVE_ITEM, session->id, &resp);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("Allocation of move_item response failed.");
        return SR_ERR_NOMEM;
    }

    rc = rp_dt_move_list(rp_ctx->dm_ctx, session->dm_session, xpath, sr_move_direction_gpb_to_sr( msg->request->move_item_req->direction));

    /* set response code */
    resp->response->result = rc;

    /* send the response */
    rc = cm_msg_send(rp_ctx->cm_ctx, resp);

    return rc;
}

/**
 * @brief Processes a validate request.
 */
static int
rp_validate_req_process(const rp_ctx_t *rp_ctx, const rp_session_t *session, Sr__Msg *msg)
{
    Sr__Msg *resp = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(rp_ctx, session, msg, msg->request, msg->request->validate_req);

    SR_LOG_DBG_MSG("Processing validate request.");

    /* allocate the response */
    rc = sr_pb_resp_alloc(SR__OPERATION__VALIDATE, session->id, &resp);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("Allocation of validate response failed.");
        return SR_ERR_NOMEM;
    }

    sr_error_info_t *errors = NULL;
    size_t err_cnt = 0;
    rc = dm_validate_session_data_trees(rp_ctx->dm_ctx, session->dm_session, &errors, &err_cnt);

    /* set response code */
    resp->response->result = rc;

    /* copy error information to GPB  (if any) */
    if (err_cnt > 0) {
        sr_gpb_fill_errors(errors, err_cnt, &resp->response->validate_resp->errors, &resp->response->validate_resp->n_errors);
        sr_free_errors(errors, err_cnt);
    }

    /* send the response */
    rc = cm_msg_send(rp_ctx->cm_ctx, resp);

    return rc;
}

/**
 * @brief Processes a commit request.
 */
static int
rp_commit_req_process(const rp_ctx_t *rp_ctx, const rp_session_t *session, Sr__Msg *msg)
{
    Sr__Msg *resp = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(rp_ctx, session, msg, msg->request, msg->request->commit_req);

    SR_LOG_DBG_MSG("Processing commit request.");

    /* allocate the response */
    rc = sr_pb_resp_alloc(SR__OPERATION__COMMIT, session->id, &resp);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("Allocation of commit response failed.");
        return SR_ERR_NOMEM;
    }

    sr_error_info_t *errors = NULL;
    size_t err_cnt = 0;
    rc = dm_commit(rp_ctx->dm_ctx, session->dm_session, &errors, &err_cnt);

    /* set response code */
    resp->response->result = rc;

    /* copy error information to GPB  (if any) */
    if (err_cnt > 0) {
        sr_gpb_fill_errors(errors, err_cnt, &resp->response->commit_resp->errors, &resp->response->commit_resp->n_errors);
        sr_free_errors(errors, err_cnt);
    }

    /* send the response */
    rc = cm_msg_send(rp_ctx->cm_ctx, resp);

    return rc;
}

/**
 * @brief Processes a discard_changes request.
 */
static int
rp_discard_changes_req_process(const rp_ctx_t *rp_ctx, const rp_session_t *session, Sr__Msg *msg)
{
    Sr__Msg *resp = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG5(rp_ctx, session, msg, msg->request, msg->request->discard_changes_req);

    SR_LOG_DBG_MSG("Processing discard_changes request.");

    /* allocate the response */
    rc = sr_pb_resp_alloc(SR__OPERATION__DISCARD_CHANGES, session->id, &resp);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("Allocation of discard_changes response failed.");
        return SR_ERR_NOMEM;
    }

    rc = dm_discard_changes(rp_ctx->dm_ctx, session->dm_session);

    /* set response code */
    resp->response->result = rc;

    /* send the response */
    rc = cm_msg_send(rp_ctx->cm_ctx, resp);

    return rc;
}

/**
 * @brief Dispatches the received message.
 */
static int
rp_msg_dispatch(const rp_ctx_t *rp_ctx, rp_session_t *session, Sr__Msg *msg)
{
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG3(rp_ctx, session, msg);

    if (SR__MSG__MSG_TYPE__REQUEST == msg->type) {
        /* request handling */
        switch (msg->request->operation) {
            case SR__OPERATION__LIST_SCHEMAS:
                rc = rp_list_schemas_req_process(rp_ctx, session, msg);
                break;
            case SR__OPERATION__GET_ITEM:
                rc = rp_get_item_req_process(rp_ctx, session, msg);
                break;
            case SR__OPERATION__GET_ITEMS:
                rc = rp_get_items_req_process(rp_ctx, session, msg);
                break;
            case SR__OPERATION__SET_ITEM:
                rc = rp_set_item_req_process(rp_ctx, session, msg);
                break;
            case SR__OPERATION__DELETE_ITEM:
                rc = rp_delete_item_req_process(rp_ctx, session, msg);
                break;
            case SR__OPERATION__MOVE_ITEM:
                rc = rp_move_item_req_process(rp_ctx, session, msg);
                break;
            case SR__OPERATION__VALIDATE:
                rc = rp_validate_req_process(rp_ctx, session, msg);
                break;
            case SR__OPERATION__COMMIT:
                rc = rp_commit_req_process(rp_ctx, session, msg);
                break;
            case SR__OPERATION__DISCARD_CHANGES:
                rc = rp_discard_changes_req_process(rp_ctx, session, msg);
                break;
            default:
                SR_LOG_ERR("Unsupported request received (session id=%"PRIu32", operation=%d).",
                        session->id, msg->request->operation);
                rc = SR_ERR_UNSUPPORTED;
                break;
        }
    } else {
        /* response handling */
        SR_LOG_ERR("Unsupported response received (session id=%"PRIu32", operation=%d).",
                session->id, msg->response->operation);
        rc = SR_ERR_UNSUPPORTED;
    }

    /* release the message */
    sr__msg__free_unpacked(msg, NULL);

    if (SR_ERR_OK != rc) {
        SR_LOG_WRN("Error by processing of the message: %s.", sr_strerror(rc));
    }

    return rc;
}

/**
 * @brief Cleans up the session (releases the data allocated by Request Processor).
 */
static int
rp_session_cleanup(const rp_ctx_t *rp_ctx, rp_session_t *session)
{
    CHECK_NULL_ARG2(rp_ctx, session);

    SR_LOG_DBG("RP session cleanup, session id=%"PRIu32".", session->id);

    dm_session_stop(rp_ctx->dm_ctx, session->dm_session);

    rp_ns_clean(&session->get_items_ctx.stack);
    free(session->get_items_ctx.xpath);
    pthread_mutex_destroy(&session->msg_count_mutex);
    free(session);

    return SR_ERR_OK;
}

/**
 * @brief Executes the work of a worker thread.
 */
static void *
rp_worker_thread_execute(void *rp_ctx_p)
{
    if (NULL == rp_ctx_p) {
        return NULL;
    }
    rp_ctx_t *rp_ctx = (rp_ctx_t*)rp_ctx_p;
    rp_request_t req = { 0 };
    bool dequeued = false, dequeued_prev = false, exit = false;

    SR_LOG_DBG("Starting worker thread id=%lu.", (unsigned long)pthread_self());

    pthread_mutex_lock(&rp_ctx->request_queue_mutex);
    rp_ctx->active_threads++;
    pthread_mutex_unlock(&rp_ctx->request_queue_mutex);

    do {
        /* process requests while there are some */
        dequeued_prev = false;
        do {
            /* dequeue a request */
            pthread_mutex_lock(&rp_ctx->request_queue_mutex);
            dequeued = sr_cbuff_dequeue(rp_ctx->request_queue, &req);
            pthread_mutex_unlock(&rp_ctx->request_queue_mutex);

            if (dequeued) {
                /* process the request */
                if (NULL == req.msg || NULL == req.session) {
                    SR_LOG_DBG("Thread id=%lu received an empty request, exiting.", (unsigned long)pthread_self());
                    exit = true;
                } else {
                    rp_msg_dispatch(rp_ctx, req.session, req.msg);
                    /* update message count and release session if needed */
                    pthread_mutex_lock(&req.session->msg_count_mutex);
                    req.session->msg_count -= 1;
                    if (0 == req.session->msg_count && req.session->stop_requested) {
                        pthread_mutex_unlock(&req.session->msg_count_mutex);
                        rp_session_cleanup(rp_ctx, req.session);
                    } else {
                        pthread_mutex_unlock(&req.session->msg_count_mutex);
                    }
                }
                dequeued_prev = true;
            } else {
                /* no items in queue - spin for a while */
                if (dequeued_prev) {
                    /* only if the thread has actually processed something since the last wakeup */
                    size_t count = 0;
                    while ((0 == sr_cbuff_items_in_queue(rp_ctx->request_queue)) && (count < rp_ctx->thread_spin_limit)) {
                        count++;
                    }
                }
                pthread_mutex_lock(&rp_ctx->request_queue_mutex);
                if (0 != sr_cbuff_items_in_queue(rp_ctx->request_queue)) {
                    /* some items are in queue - process them */
                    pthread_mutex_unlock(&rp_ctx->request_queue_mutex);
                    dequeued = true;
                    continue;
                } else {
                    /* no items in queue - go to sleep */
                    rp_ctx->active_threads--;
                    pthread_mutex_unlock(&rp_ctx->request_queue_mutex);
                }
            }
        } while (dequeued && !exit);

        if (!exit) {
            /* wait until new request comes */
            SR_LOG_DBG("Thread id=%lu will wait.",  (unsigned long)pthread_self());

            /* wait for a signal */
            pthread_mutex_lock(&rp_ctx->request_queue_mutex);
            if (rp_ctx->stop_requested) {
                /* stop has been requested, do not wait anymore */
                pthread_mutex_unlock(&rp_ctx->request_queue_mutex);
                break;
            }
            pthread_cond_wait(&rp_ctx->request_queue_cv, &rp_ctx->request_queue_mutex);
            rp_ctx->active_threads++;

            SR_LOG_DBG("Thread id=%lu signaled.",  (unsigned long)pthread_self());
            pthread_mutex_unlock(&rp_ctx->request_queue_mutex);
        }
    } while (!exit);

    SR_LOG_DBG("Worker thread id=%lu is exiting.",  (unsigned long)pthread_self());

    return NULL;
}

int
rp_init(cm_ctx_t *cm_ctx, rp_ctx_t **rp_ctx_p)
{
    size_t i = 0, j = 0;
    rp_ctx_t *ctx = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG(rp_ctx_p);

    SR_LOG_DBG_MSG("Request Processor init started.");

    /* allocate the context */
    ctx = calloc(1, sizeof(*ctx));
    if (NULL == ctx) {
        SR_LOG_ERR_MSG("Cannot allocate memory for Request Processor context.");
        return SR_ERR_NOMEM;
    }
    ctx->cm_ctx = cm_ctx;

    /* initialize request queue */
    rc = sr_cbuff_init(RP_INIT_REQ_QUEUE_SIZE, sizeof(rp_request_t), &ctx->request_queue);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("RP request queue initialization failed.");
        goto cleanup;
    }

    /* initialize Data Manager */
    rc = dm_init(SR_SCHEMA_SEARCH_DIR, SR_DATA_SEARCH_DIR, &ctx->dm_ctx);
    if (SR_ERR_OK != rc){
        SR_LOG_ERR_MSG("Data Manager initialization failed.");
        goto cleanup;
    }

    /* run worker threads */
    pthread_mutex_init(&ctx->request_queue_mutex, NULL);
    pthread_cond_init(&ctx->request_queue_cv, NULL);

    for (i = 0; i < RP_THREAD_COUNT; i++) {
        rc = pthread_create(&ctx->thread_pool[i], NULL, rp_worker_thread_execute, ctx);
        if (0 != rc) {
            SR_LOG_ERR("Error by creating a new thread: %s", strerror(errno));
            for (j = 0; j < i; j++) {
                pthread_cancel(ctx->thread_pool[j]);
            }
            rc = SR_ERR_INTERNAL;
            goto cleanup;
        }
    }

    *rp_ctx_p = ctx;
    return SR_ERR_OK;

cleanup:
    dm_cleanup(ctx->dm_ctx);
    sr_cbuff_cleanup(ctx->request_queue);
    free(ctx);
    return rc;
}

void
rp_cleanup(rp_ctx_t *rp_ctx)
{
    size_t i = 0;
    rp_request_t req = { 0 };

    SR_LOG_DBG_MSG("Request Processor cleanup started, requesting cancel of each worker thread.");

    if (NULL != rp_ctx) {
        /* enqueue RP_THREAD_COUNT "empty" messages and send signal to all threads */
        pthread_mutex_lock(&rp_ctx->request_queue_mutex);
        rp_ctx->stop_requested = true;
        /* enqueue empty requests to request thread exits */
        for (i = 0; i < RP_THREAD_COUNT; i++) {
            sr_cbuff_enqueue(rp_ctx->request_queue, &req);
        }
        pthread_cond_broadcast(&rp_ctx->request_queue_cv);
        pthread_mutex_unlock(&rp_ctx->request_queue_mutex);

        /* wait for threads to exit */
        for (i = 0; i < RP_THREAD_COUNT; i++) {
            pthread_join(rp_ctx->thread_pool[i], NULL);
        }
        pthread_mutex_destroy(&rp_ctx->request_queue_mutex);
        pthread_cond_destroy(&rp_ctx->request_queue_cv);

        while (sr_cbuff_dequeue(rp_ctx->request_queue, &req)) {
            if (NULL != req.msg) {
                sr__msg__free_unpacked(req.msg, NULL);
            }
        }
        sr_cbuff_cleanup(rp_ctx->request_queue);
        dm_cleanup(rp_ctx->dm_ctx);
        free(rp_ctx);
    }

    SR_LOG_DBG_MSG("Request Processor cleanup finished.");
}

int
rp_session_start(const rp_ctx_t *rp_ctx, const uint32_t session_id,
        const ac_ucred_t *user_credentials, const sr_datastore_t datastore, rp_session_t **session_p)
{
    rp_session_t *session = NULL;
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG2(rp_ctx, session_p);

    SR_LOG_DBG("RP session start, session id=%"PRIu32".", session_id);

    session = calloc(1, sizeof(*session));
    if (NULL == session) {
        SR_LOG_ERR_MSG("Cannot allocate memory for RP session context.");
        return SR_ERR_NOMEM;
    }

    pthread_mutex_init(&session->msg_count_mutex, NULL);
    session->user_credentials = user_credentials;
    session->id = session_id;
    session->datastore = datastore;

    rc = dm_session_start(rp_ctx->dm_ctx, datastore, &session->dm_session);
    if (SR_ERR_OK  != rc){
        SR_LOG_ERR("Init of dm_session failed for session id=%"PRIu32".", session_id);
        free(session);
        return rc;
    }

    *session_p = session;

    return rc;
}

int
rp_session_stop(const rp_ctx_t *rp_ctx, rp_session_t *session)
{
    CHECK_NULL_ARG2(rp_ctx, session);

    SR_LOG_DBG("RP session stop, session id=%"PRIu32".", session->id);

    /* sanity check - normally there should not be any unprocessed messages
     * within the session when calling rp_session_stop */
    pthread_mutex_lock(&session->msg_count_mutex);
    if (session->msg_count > 0) {
        /* cleanup will be called after last message has been processed so
         * that RP can survive this unexpected situation */
        SR_LOG_WRN("There are some (%"PRIu32") unprocessed messages for the session id=%"PRIu32" when"
                " session stop has been requested, this can lead to unspecified behavior - check RP caller code!!!",
                session->msg_count, session->id);
        session->stop_requested = true;
        pthread_mutex_unlock(&session->msg_count_mutex);
    } else {
        pthread_mutex_unlock(&session->msg_count_mutex);
        rp_session_cleanup(rp_ctx, session);
    }

    return SR_ERR_OK;
}

int
rp_msg_process(rp_ctx_t *rp_ctx, rp_session_t *session, Sr__Msg *msg)
{
    rp_request_t req = { 0 };
    struct timespec now = { 0 };
    int rc = SR_ERR_OK;

    CHECK_NULL_ARG_NORET3(rc, rp_ctx, session, msg);

    if (SR_ERR_OK != rc) {
        if (NULL != msg) {
            sr__msg__free_unpacked(msg, NULL);
        }
        return rc;
    }

    pthread_mutex_lock(&session->msg_count_mutex);
    session->msg_count += 1;
    pthread_mutex_unlock(&session->msg_count_mutex);

    req.session = session;
    req.msg = msg;

    pthread_mutex_lock(&rp_ctx->request_queue_mutex);

    /* enqueue the request into buffer */
    rc = sr_cbuff_enqueue(rp_ctx->request_queue, &req);

    if (0 == rp_ctx->active_threads) {
        /* there is no active (non-sleeping) thread - if this is happening too
         * frequently, instruct the threads to spin before going to sleep */
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t diff = (1000000000L * (now.tv_sec - rp_ctx->last_thread_wakeup.tv_sec)) + now.tv_nsec - rp_ctx->last_thread_wakeup.tv_nsec;
        if (diff < RP_THREAD_SPIN_TIMEOUT) {
            /* a thread has been woken up in less than RP_THREAD_SPIN_TIMEOUT, increase the spin */
            if (0 == rp_ctx->thread_spin_limit) {
                /* no spin set yet, set to initial value */
                rp_ctx->thread_spin_limit = RP_THREAD_SPIN_MIN;
            } else if(rp_ctx->thread_spin_limit < RP_THREAD_SPIN_MAX) {
                /* double the spin limit */
                rp_ctx->thread_spin_limit *= 2;
            }
        } else {
            /* reset spin to 0 if wakaups are not too frequent */
            rp_ctx->thread_spin_limit = 0;
        }
        rp_ctx->last_thread_wakeup = now;
    }

    /* send signal if there is no active thread ready to process the request */
    if (0 == rp_ctx->active_threads ||
            (((sr_cbuff_items_in_queue(rp_ctx->request_queue) / rp_ctx->active_threads) > RP_REQ_PER_THREADS) &&
             rp_ctx->active_threads < RP_THREAD_COUNT)) {
        pthread_cond_signal(&rp_ctx->request_queue_cv);
    }

    pthread_mutex_unlock(&rp_ctx->request_queue_mutex);

    if (SR_ERR_OK != rc) {
        /* release the message by error */
        SR_LOG_ERR_MSG("Unable to process the message, skipping.");
        sr__msg__free_unpacked(msg, NULL);
    }

    return rc;
}
