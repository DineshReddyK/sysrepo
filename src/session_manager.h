/**
 * @file session_manager.h
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief API of Sysrepo Engine's Session Manager.
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

#ifndef SESSION_MANAGER_H_
#define SESSION_MANAGER_H_

#include <stdint.h>

/**
 * @defgroup sm Session Manager
 * @{
 *
 * @brief Session manager holds information about all active sysrepo sessions.
 *
 * It allows fast session lookup by provided session_id (::sm_session_t#id - see ::sm_session_find_id) or
 * by associated file desciptor (::sm_session_t#fd - see ::sm_session_find_fd).
 */

/**
 * @brief Session manager context used to identify particular instance of Session manager.
 */
typedef struct sm_ctx_s sm_ctx_t;

/**
 * @brief Session context structure, represents one particular session.
 */
typedef struct sm_session_s {
    uint32_t id;                         /**< Auto-generated unique session ID (do not modify it). */
    struct sm_connection_s *connection;  /**< Connection associated with this session. */

    const char *real_user;               /**< Real username of the other side. */
    const char *effective_user;          /**< Effective username of the other side (if different to real_user). */

    void *rp_data;                       /**< Request Processor session data, opaque to Session Manager. */
} sm_session_t;

/**
 * @brief Linked-list of sessions.
 */
typedef struct sm_session_list_s {
    sm_session_t *session;           /**< Session context. */
    struct sm_session_list_s *next;  /**< Pointer to next session context. */
} sm_session_list_t;

/**
 * @brief Connection type.
 */
typedef enum {
    CM_AF_UNIX_CLIENT,  /**< The other side is an unix-domain socket client. */
    CM_AF_UNIX_SERVER,  /**< The other side is an unix-domain socket server. */
} sm_connection_type_t;

/**
 * @brief Connection context structure, represents one particalar connection.
 * Multiple sessions can be assigned to the same connection.
 */
typedef struct sm_connection_s {
    sm_connection_type_t type;        /**< Type of the connection. */
    sm_session_list_t *session_list;  /**< List of sessions associated to the connection. */

    int fd;  /**< File descriptor of the connection. */

    /**
     * @brief Buffers used for send/receive data to/from the other side.
     */
    struct {
        char *in_buff;           /**< Input buffer. If not empty, there is some message to be processed (or part of it). */
        size_t in_buff_size;     /**< Current size of the input buffer. */
        size_t in_buff_pos;      /**< Current possition in the input buffer (new data is appended starting from this position). */

        char *out_buff;          /**< Output buffer. If not empty, there is some data to be sent when reciever is ready. */
        size_t out_buff_size;    /**< Current size of the output buffer. */
        size_t out_buff_pos;     /**< Current possition in the output buffer (new data is appended starting from this position). */
    } buffers;
} sm_connection_t;

/**
 * @brief Initializes Session Manager.
 *
 * @param[out] sm_ctx Allocated Session Manager context that can be used in subsequent SM requests.
 *
 * @return Error code (SR_ERR_OK on success).
 */
int sm_init(sm_ctx_t **sm_ctx);

/**
 * @brief Cleans up Session Manager.
 *
 * All outstanding sessions will be automatically dropped and all memory held by
 * this Session Manager instance will be freed.
 *
 * @param[in] sm_ctx Session Manager context.
 *
 * @return Error code (SR_ERR_OK on success).
 */
void sm_cleanup(sm_ctx_t *sm_ctx);

/**
 * TODO
 */
int sm_connection_start(const sm_ctx_t *sm_ctx, const sm_connection_type_t type, const int fd,
        sm_connection_t **connection);

/**
 * TODO
 */
int sm_connection_stop(const sm_ctx_t *sm_ctx,  sm_connection_t *connection);

/**
 * @brief Creates new session.
 *
 * A new unique session ID will be assigned and set to the allocated session
 * context (::sm_session_t#id). Lookup for a session by given session ID is possible
 * (see ::sm_session_find_id).
 *
 * @param[in] sm_ctx Session Manager context.
 * @param[in] type Type of the session.
 * @param[out] session Allocated session context.
 *
 * @return Error code (SR_ERR_OK on success).
 */
int sm_session_create(const sm_ctx_t *sm_ctx, sm_connection_t *connection,
        const char *real_user, const char *effective_user, sm_session_t **session);

/**
 * @brief Drops a session.
 *
 * All session-related memory held by Session Manager will be freed, session ID
 * won't be valid anymore.
 *
 * @param[in] sm_ctx Session Manager context.
 * @param[in] session Session context.
 *
 * @return Error code (SR_ERR_OK on success).
 */
int sm_session_drop(const sm_ctx_t *sm_ctx, sm_session_t *session);

/**
 * @brief Finds session context assocaiated to provided session ID.
 *
 * @param[in] sm_ctx Session Manager context.
 * @param[in] session_id ID of the session.
 * @param[out] session Session context matching with provided session_id.
 *
 * @return Error code (SR_ERR_OK on success, SR_ERR_NOT_FOUND if session
 * matching to session_id cannot be found).
 */
int sm_session_find_id(const sm_ctx_t *sm_ctx, uint32_t session_id, sm_session_t **session);

/**
 * @brief Finds session contexts associated to provided file descriptor.
 * @see sm_session_assign_fd
 * @see sm_session_list_t
 *
 * @param[in] sm_ctx Session Manager context.
 * @param[in] fd File Descriptor of sessions.
 * @param[out] connection Connection context matching with provided file descriptor.
 *
 * @return Error code (SR_ERR_OK on success, SR_ERR_NOT_FOUND if session
 * matching to fd cannot be found).
 */
int sm_connection_find_fd(const sm_ctx_t *sm_ctx, const int fd, sm_connection_t **connection);

/**@} sm */

#endif /* SESSION_MANAGER_H_ */