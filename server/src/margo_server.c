/*
 * Copyright (c) 2020, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2020, UT-Battelle, LLC.
 *
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * This is the license for UnifyFS.
 * For details, see https://github.com/LLNL/UnifyFS.
 * Please read https://github.com/LLNL/UnifyFS/LICENSE for full license text.
 */

// common headers
#include "unifyfs_keyval.h"
#include "unifyfs_client_rpcs.h"
#include "unifyfs_server_rpcs.h"
#include "unifyfs_rpc_util.h"

// server headers
#include "unifyfs_global.h"
#include "margo_server.h"
#include "na_config.h" // from mercury include lib
#include "mercury_log.h"

// global variables
ServerRpcContext_t* unifyfsd_rpc_context;
bool margo_use_tcp = true;
bool margo_lazy_connect; // = false
int  margo_client_server_pool_sz = UNIFYFS_MARGO_POOL_SZ;
int  margo_server_server_pool_sz = UNIFYFS_MARGO_POOL_SZ;
double margo_client_server_timeout_msec =
    UNIFYFS_MARGO_CLIENT_SERVER_TIMEOUT_MSEC;
double margo_server_server_timeout_msec =
    UNIFYFS_MARGO_SERVER_SERVER_TIMEOUT_MSEC;
int  margo_use_progress_thread = 1;

// records pmi rank, server address string, and server address
// for each server for use in server-to-server rpcs
static server_info_t* server_infos; // array of server_info_t

#if defined(NA_HAS_SM)
static const char* PROTOCOL_MARGO_SHM = "na+sm";
#else
#error Required Mercury NA shared memory plugin not found (please enable 'SM')
#endif

#if !defined(NA_HAS_BMI) && !defined(NA_HAS_OFI)
#error No supported Mercury NA plugin found (please use one of: 'BMI', 'OFI')
#endif

#if defined(NA_HAS_BMI)
static const char* PROTOCOL_MARGO_BMI_TCP = "bmi+tcp";
#else
static const char* PROTOCOL_MARGO_BMI_TCP;
#endif

#if defined(NA_HAS_OFI)
static const char* PROTOCOL_MARGO_OFI_SOCKETS = "ofi+sockets";
static const char* PROTOCOL_MARGO_OFI_TCP = "ofi+tcp";
static const char* PROTOCOL_MARGO_OFI_RMA = "ofi+verbs";
#else
static const char* PROTOCOL_MARGO_OFI_SOCKETS;
static const char* PROTOCOL_MARGO_OFI_TCP;
static const char* PROTOCOL_MARGO_OFI_RMA;
#endif

/* Given a margo instance ID (mid), return its corresponding
 * address as a newly allocated string to be freed by caller.
 * Returns NULL on error. */
static char* get_margo_addr_str(margo_instance_id mid)
{
    /* get margo address for given instance */
    hg_addr_t addr_self;
    hg_return_t hret = margo_addr_self(mid, &addr_self);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_addr_self() failed");
        return NULL;
    }

    /* convert margo address to a string */
    char self_string[128];
    hg_size_t self_string_sz = sizeof(self_string);
    hret = margo_addr_to_string(mid,
        self_string, &self_string_sz, addr_self);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_addr_to_string() failed");
        margo_addr_free(mid, addr_self);
        return NULL;
    }
    margo_addr_free(mid, addr_self);

    /* return address in newly allocated string */
    char* addr = strdup(self_string);
    return addr;
}

/* setup_remote_target - Initializes the server-server margo target */
static margo_instance_id setup_remote_target(void)
{
    /* by default we try to use ofi */
    const char* margo_protocol = margo_use_tcp ?
                     PROTOCOL_MARGO_OFI_TCP : PROTOCOL_MARGO_OFI_RMA;
    if (!margo_protocol) {
        /* when ofi is not available, fallback to using bmi */
        LOGWARN("OFI is not available, using BMI for margo rpc");
        margo_protocol = PROTOCOL_MARGO_BMI_TCP;
    }

    /* initialize margo */
    margo_instance_id mid = margo_init(margo_protocol, MARGO_SERVER_MODE,
        margo_use_progress_thread, margo_server_server_pool_sz);
    if (mid == MARGO_INSTANCE_NULL) {
        LOGERR("margo_init(%s, SERVER_MODE, %d, %d) failed",
               margo_protocol, margo_use_progress_thread,
               margo_server_server_pool_sz);
        if (margo_protocol == PROTOCOL_MARGO_OFI_TCP) {
            /* try "ofi+sockets" instead */
            margo_protocol = PROTOCOL_MARGO_OFI_SOCKETS;
            mid = margo_init(margo_protocol, MARGO_SERVER_MODE,
                             margo_use_progress_thread,
                             margo_server_server_pool_sz);
            if (mid == MARGO_INSTANCE_NULL) {
                LOGERR("margo_init(%s, SERVER_MODE, %d, %d) failed",
                       margo_protocol, margo_use_progress_thread,
                       margo_server_server_pool_sz);
                return mid;
            }
        }
    }

    /* get our address for server-server rpcs */
    char* self_string = get_margo_addr_str(mid);
    if (NULL == self_string) {
        LOGERR("invalid value to publish server-server margo rpc address");
        margo_finalize(mid);
        return MARGO_INSTANCE_NULL;
    }
    LOGINFO("margo RPC server: %s", self_string);

    /* publish rpc address of server for remote servers */
    rpc_publish_remote_server_addr(self_string);

    free(self_string);

    return mid;
}

/* register server-server RPCs */
static void register_server_server_rpcs(margo_instance_id mid)
{
    unifyfsd_rpc_context->rpcs.bcast_progress_id =
        MARGO_REGISTER(mid, "bcast_progress_rpc",
                       bcast_progress_in_t, bcast_progress_out_t,
                       bcast_progress_rpc);

    unifyfsd_rpc_context->rpcs.chunk_read_request_id =
        MARGO_REGISTER(mid, "chunk_read_request_rpc",
                       chunk_read_request_in_t, chunk_read_request_out_t,
                       chunk_read_request_rpc);

    unifyfsd_rpc_context->rpcs.chunk_read_response_id =
        MARGO_REGISTER(mid, "chunk_read_response_rpc",
                       chunk_read_response_in_t, chunk_read_response_out_t,
                       chunk_read_response_rpc);

    unifyfsd_rpc_context->rpcs.extent_add_id =
        MARGO_REGISTER(mid, "add_extents_rpc",
                       add_extents_in_t, add_extents_out_t,
                       add_extents_rpc);

    unifyfsd_rpc_context->rpcs.extent_bcast_id =
        MARGO_REGISTER(mid, "extent_bcast_rpc",
                       extent_bcast_in_t, extent_bcast_out_t,
                       extent_bcast_rpc);

    unifyfsd_rpc_context->rpcs.extent_lookup_id =
        MARGO_REGISTER(mid, "find_extents_rpc",
                       find_extents_in_t, find_extents_out_t,
                       find_extents_rpc);

    unifyfsd_rpc_context->rpcs.fileattr_bcast_id =
        MARGO_REGISTER(mid, "fileattr_bcast_rpc",
                       fileattr_bcast_in_t, fileattr_bcast_out_t,
                       fileattr_bcast_rpc);

    unifyfsd_rpc_context->rpcs.filesize_id =
        MARGO_REGISTER(mid, "filesize_rpc",
                       filesize_in_t, filesize_out_t,
                       filesize_rpc);

    unifyfsd_rpc_context->rpcs.laminate_id =
        MARGO_REGISTER(mid, "laminate_rpc",
                       laminate_in_t, laminate_out_t,
                       laminate_rpc);

    unifyfsd_rpc_context->rpcs.laminate_bcast_id =
        MARGO_REGISTER(mid, "laminate_bcast_rpc",
                       laminate_bcast_in_t, laminate_bcast_out_t,
                       laminate_bcast_rpc);

    unifyfsd_rpc_context->rpcs.metaget_id =
        MARGO_REGISTER(mid, "metaget_rpc",
                       metaget_in_t, metaget_out_t,
                       metaget_rpc);

    unifyfsd_rpc_context->rpcs.metaset_id =
        MARGO_REGISTER(mid, "metaset_rpc",
                       metaset_in_t, metaset_out_t,
                       metaset_rpc);

    unifyfsd_rpc_context->rpcs.server_pid_id =
        MARGO_REGISTER(mid, "server_pid_rpc",
                       server_pid_in_t, server_pid_out_t,
                       server_pid_rpc);

    unifyfsd_rpc_context->rpcs.transfer_id =
        MARGO_REGISTER(mid, "transfer_rpc",
                       transfer_in_t, transfer_out_t,
                       transfer_rpc);

    unifyfsd_rpc_context->rpcs.transfer_bcast_id =
        MARGO_REGISTER(mid, "transfer_bcast_rpc",
                       transfer_bcast_in_t, transfer_bcast_out_t,
                       transfer_bcast_rpc);

    unifyfsd_rpc_context->rpcs.truncate_id =
        MARGO_REGISTER(mid, "truncate_rpc",
                       truncate_in_t, truncate_out_t,
                       truncate_rpc);

    unifyfsd_rpc_context->rpcs.truncate_bcast_id =
        MARGO_REGISTER(mid, "truncate_bcast_rpc",
                       truncate_bcast_in_t, truncate_bcast_out_t,
                       truncate_bcast_rpc);

    unifyfsd_rpc_context->rpcs.unlink_bcast_id =
        MARGO_REGISTER(mid, "unlink_bcast_rpc",
                       unlink_bcast_in_t, unlink_bcast_out_t,
                       unlink_bcast_rpc);

    unifyfsd_rpc_context->rpcs.node_local_extents_get_id =
       MARGO_REGISTER(mid, "unifyfs_node_local_extents_get_rpc",
                      unifyfs_node_local_extents_get_in_t,
                      unifyfs_node_local_extents_get_out_t,
                      unifyfs_node_local_extents_get_rpc);
    unifyfsd_rpc_context->rpcs.metaget_all_bcast_id =
        MARGO_REGISTER(mid, "metaget_all_bcast_rpc",
                       metaget_all_bcast_in_t, metaget_all_bcast_out_t,
                       metaget_all_bcast_rpc);
}

/* setup_local_target - Initializes the client-server margo target */
static margo_instance_id setup_local_target(void)
{
    /* initialize margo */
    const char* margo_protocol = PROTOCOL_MARGO_SHM;
    margo_instance_id mid = margo_init(margo_protocol, MARGO_SERVER_MODE,
                     margo_use_progress_thread, margo_client_server_pool_sz);
    if (mid == MARGO_INSTANCE_NULL) {
        LOGERR("margo_init(%s, SERVER_MODE, %d, %d) failed", margo_protocol,
               margo_use_progress_thread, margo_client_server_pool_sz);
        return mid;
    }

    /* figure out what address this server is listening on */
    char* self_string = get_margo_addr_str(mid);
    if (NULL == self_string) {
        LOGERR("margo_addr_self() failed");
        margo_finalize(mid);
        return MARGO_INSTANCE_NULL;
    }
    LOGINFO("shared-memory margo RPC server: %s", self_string);

    /* publish rpc address of server for local clients */
    rpc_publish_local_server_addr(self_string);

    free(self_string);

    return mid;
}

/* register client-server RPCs */
static void register_client_server_rpcs(margo_instance_id mid)
{
    /* register the RPC handler functions */
    MARGO_REGISTER(mid, "unifyfs_attach_rpc",
                   unifyfs_attach_in_t, unifyfs_attach_out_t,
                   unifyfs_attach_rpc);

    MARGO_REGISTER(mid, "unifyfs_mount_rpc",
                   unifyfs_mount_in_t, unifyfs_mount_out_t,
                   unifyfs_mount_rpc);

    MARGO_REGISTER(mid, "unifyfs_unmount_rpc",
                   unifyfs_unmount_in_t, unifyfs_unmount_out_t,
                   unifyfs_unmount_rpc);

    MARGO_REGISTER(mid, "unifyfs_metaget_rpc",
                   unifyfs_metaget_in_t, unifyfs_metaget_out_t,
                   unifyfs_metaget_rpc);

    MARGO_REGISTER(mid, "unifyfs_metaset_rpc",
                   unifyfs_metaset_in_t, unifyfs_metaset_out_t,
                   unifyfs_metaset_rpc);

    MARGO_REGISTER(mid, "unifyfs_fsync_rpc",
                   unifyfs_fsync_in_t, unifyfs_fsync_out_t,
                   unifyfs_fsync_rpc);

    MARGO_REGISTER(mid, "unifyfs_filesize_rpc",
                   unifyfs_filesize_in_t, unifyfs_filesize_out_t,
                   unifyfs_filesize_rpc);

    MARGO_REGISTER(mid, "unifyfs_transfer_rpc",
                   unifyfs_transfer_in_t, unifyfs_transfer_out_t,
                   unifyfs_transfer_rpc);

    MARGO_REGISTER(mid, "unifyfs_truncate_rpc",
                   unifyfs_truncate_in_t, unifyfs_truncate_out_t,
                   unifyfs_truncate_rpc);

    MARGO_REGISTER(mid, "unifyfs_unlink_rpc",
                   unifyfs_unlink_in_t, unifyfs_unlink_out_t,
                   unifyfs_unlink_rpc);

    MARGO_REGISTER(mid, "unifyfs_laminate_rpc",
                   unifyfs_laminate_in_t, unifyfs_laminate_out_t,
                   unifyfs_laminate_rpc);

    MARGO_REGISTER(mid, "unifyfs_mread_rpc",
                   unifyfs_mread_in_t, unifyfs_mread_out_t,
                   unifyfs_mread_rpc);

    MARGO_REGISTER(mid, "unifyfs_node_local_extents_get_rpc",
                   unifyfs_node_local_extents_get_in_t,
                   unifyfs_node_local_extents_get_out_t,
                   unifyfs_node_local_extents_get_rpc);

    MARGO_REGISTER(mid, "unifyfs_get_gfids_rpc",
                   unifyfs_get_gfids_in_t, unifyfs_get_gfids_out_t,
                   unifyfs_get_gfids_rpc);

    /* register the RPCs we call (and capture assigned hg_id_t) */
    unifyfsd_rpc_context->rpcs.client_heartbeat_id =
            MARGO_REGISTER(mid, "unifyfs_heartbeat_rpc",
                           unifyfs_heartbeat_in_t,
                           unifyfs_heartbeat_out_t,
                           NULL);

    unifyfsd_rpc_context->rpcs.client_mread_data_id =
        MARGO_REGISTER(mid, "unifyfs_mread_req_data_rpc",
                       unifyfs_mread_req_data_in_t,
                       unifyfs_mread_req_data_out_t,
                       NULL);

    unifyfsd_rpc_context->rpcs.client_mread_complete_id =
        MARGO_REGISTER(mid, "unifyfs_mread_req_complete_rpc",
                       unifyfs_mread_req_complete_in_t,
                       unifyfs_mread_req_complete_out_t,
                       NULL);

    unifyfsd_rpc_context->rpcs.client_transfer_complete_id =
        MARGO_REGISTER(mid, "unifyfs_transfer_complete_rpc",
                       unifyfs_transfer_complete_in_t,
                       unifyfs_transfer_complete_out_t,
                       NULL);

    unifyfsd_rpc_context->rpcs.client_unlink_callback_id =
        MARGO_REGISTER(mid, "unifyfs_unlink_callback_rpc",
                       unifyfs_unlink_callback_in_t,
                       unifyfs_unlink_callback_out_t,
                       NULL);
}

/* margo_server_rpc_init
 *
 * Initialize the server's Margo RPC functionality, for
 * both intra-node (client-server shared memory) and
 * inter-node (server-server).
 */
int margo_server_rpc_init(void)
{
    int rc = UNIFYFS_SUCCESS;

    if (NULL == unifyfsd_rpc_context) {
        /* create rpc server context */
        unifyfsd_rpc_context = calloc(1, sizeof(ServerRpcContext_t));
        if (NULL == unifyfsd_rpc_context) {
            return ENOMEM;
        }
    }

#if defined(HG_VERSION_MAJOR) && (HG_VERSION_MAJOR > 1)
    /* redirect mercury log to ours, using current log level */
    const char* mercury_log_level = NULL;
    switch (unifyfs_log_level) {
    case LOG_DBG:
        mercury_log_level = "debug";
        break;
    case LOG_ERR:
        mercury_log_level = "error";
        break;
    case LOG_WARN:
        mercury_log_level = "warning";
        break;
    default:
        break;
    }
    if (NULL != mercury_log_level) {
        HG_Set_log_level(mercury_log_level);
    }
    if (NULL != unifyfs_log_stream) {
        hg_log_set_stream_debug(unifyfs_log_stream);
        hg_log_set_stream_error(unifyfs_log_stream);
        hg_log_set_stream_warning(unifyfs_log_stream);
    }
#endif

    margo_instance_id mid;
    mid = setup_local_target();
    if (mid == MARGO_INSTANCE_NULL) {
        rc = UNIFYFS_ERROR_MARGO;
    } else {
        unifyfsd_rpc_context->shm_mid = mid;
        register_client_server_rpcs(mid);
    }

    mid = setup_remote_target();
    if (mid == MARGO_INSTANCE_NULL) {
        rc = UNIFYFS_ERROR_MARGO;
    } else {
        unifyfsd_rpc_context->svr_mid = mid;
        register_server_server_rpcs(mid);
    }

    return rc;
}

/* margo_server_rpc_finalize
 *
 * Finalize the server's Margo RPC functionality, for
 * both intra-node (client-server shared memory) and
 * inter-node (server-server).
 */
int margo_server_rpc_finalize(void)
{
    int rc = UNIFYFS_SUCCESS;

    if (NULL != unifyfsd_rpc_context) {
        /* define a temporary to refer to context */
        ServerRpcContext_t* ctx = unifyfsd_rpc_context;
        unifyfsd_rpc_context = NULL;

        rpc_clean_local_server_addr();

        /* free global server addresses */
        for (int i = 0; i < glb_num_servers; i++) {
            server_info_t* server = &server_infos[i];
            if (server->margo_svr_addr != HG_ADDR_NULL) {
                margo_addr_free(ctx->svr_mid, server->margo_svr_addr);
                server->margo_svr_addr = HG_ADDR_NULL;
            }
            if (NULL != server->margo_svr_addr_str) {
                free(server->margo_svr_addr_str);
                server->margo_svr_addr_str = NULL;
            }
        }

        free(server_infos);

        /* shut down margo */
        LOGDBG("finalizing server-server margo");
        margo_finalize(ctx->svr_mid);

        /* NOTE: 2nd call to margo_finalize() sometimes crashes - Margo bug? */
        LOGDBG("finalizing client-server margo");
        margo_finalize(ctx->shm_mid);

        /* free memory allocated for context structure */
        free(ctx);
    }

    return rc;
}

int margo_connect_server(int rank)
{
    assert(rank < glb_num_servers);

    int ret = UNIFYFS_SUCCESS;

    server_info_t* server = &server_infos[rank];

    /* lookup rpc address for this server */
    char* margo_addr_str = rpc_lookup_remote_server_addr(rank);
    if (NULL == margo_addr_str) {
        LOGERR("server index=%zu - margo server lookup failed", rank);
        return (int)UNIFYFS_ERROR_KEYVAL;
    }
    LOGDBG("server rank=%d, margo_addr=%s", rank, margo_addr_str);
    server->margo_svr_addr_str = margo_addr_str;

    hg_return_t hret = margo_addr_lookup(unifyfsd_rpc_context->svr_mid,
                                         server->margo_svr_addr_str,
                                         &(server->margo_svr_addr));
    if (hret != HG_SUCCESS) {
        LOGERR("server index=%zu - margo_addr_lookup(%s) failed",
               rank, margo_addr_str);
        ret = UNIFYFS_ERROR_MARGO;
    }

    return ret;
}

/* margo_connect_servers
 *
 * Gather pmi rank and margo address string for all servers,
 * and optionally connect to each one.
 */
int margo_connect_servers(void)
{
    int rc;

    int ret = (int)UNIFYFS_SUCCESS;

    /* block until all servers have published their address */
    rc = unifyfs_keyval_fence_remote();
    if ((int)UNIFYFS_SUCCESS != rc) {
        LOGERR("keyval fence on margo_svr key failed");
        return (int)UNIFYFS_ERROR_KEYVAL;
    }

    /* allocate array of structs to record address for each server */
    server_infos = (server_info_t*) calloc(glb_num_servers,
        sizeof(server_info_t));
    if (NULL == server_infos) {
        LOGERR("failed to allocate server_info array");
        return ENOMEM;
    }

    /* lookup address string for each server, and optionally connect */
    size_t i;
    for (i = 0; i < glb_num_servers; i++) {
        /* record values on struct for this server */
        server_info_t* server = &server_infos[i];
        server->pmi_rank           = i;
        server->margo_svr_addr     = HG_ADDR_NULL;
        server->margo_svr_addr_str = NULL;

        /* connect to each server now if not using lazy connect */
        if (!margo_lazy_connect) {
            rc = margo_connect_server(i);
            if (UNIFYFS_SUCCESS != rc) {
                ret = rc;
            }
        }
    }

    return ret;
}

hg_addr_t get_margo_server_address(int rank)
{
    assert(rank < glb_num_servers);
    server_info_t* server = &server_infos[rank];
    hg_addr_t addr = server->margo_svr_addr;
    if ((HG_ADDR_NULL == addr) && margo_lazy_connect) {
        int rc = margo_connect_server(rank);
        if (UNIFYFS_SUCCESS == rc) {
            addr = server->margo_svr_addr;
        }
    }
    return addr;
}

/* MARGO CLIENT-SERVER RPC INVOCATION FUNCTIONS */

/* create and return a margo handle for given rpc id and app-client */
static hg_handle_t create_client_handle(hg_id_t id,
                                        int app_id,
                                        int client_id)
{
    hg_handle_t handle = HG_HANDLE_NULL;

    /* lookup application client */
    app_client* client = get_app_client(app_id, client_id);
    if (NULL != client) {
        hg_addr_t client_addr = client->margo_addr;

        /* create handle for specified rpc */
        hg_return_t hret = margo_create(unifyfsd_rpc_context->shm_mid,
                                        client_addr, id, &handle);
        if (hret != HG_SUCCESS) {
            LOGERR("margo_create() failed");
        }
    } else {
        LOGERR("invalid app-client [%d:%d]", app_id, client_id);
    }
    return handle;
}

static int forward_to_client(hg_handle_t hdl, void* input_ptr)
{
    double timeout_msec = margo_client_server_timeout_msec;
    hg_return_t hret = margo_forward_timed(hdl, input_ptr, timeout_msec);
    if (hret != HG_SUCCESS) {
        LOGWARN("margo_forward_timed() failed - %s", HG_Error_to_string(hret));
        return UNIFYFS_ERROR_MARGO;
    }
    return UNIFYFS_SUCCESS;
}

/* invokes the heartbeat rpc function */
int invoke_client_heartbeat_rpc(int app_id,
                                int client_id)
{
    hg_return_t hret;

    /* check that we have initialized margo */
    if (NULL == unifyfsd_rpc_context) {
        return UNIFYFS_FAILURE;
    }

    /* fill input struct */
    unifyfs_heartbeat_in_t in;
    in.app_id    = (int32_t) app_id;
    in.client_id = (int32_t) client_id;

    /* get handle to rpc function */
    hg_id_t rpc_id = unifyfsd_rpc_context->rpcs.client_heartbeat_id;
    hg_handle_t handle = create_client_handle(rpc_id, app_id, client_id);

    /* call rpc function */
    LOGDBG("invoking the heartbeat rpc function in client[%d:%d]",
           app_id, client_id);
    int rc = forward_to_client(handle, &in);
    if (rc != UNIFYFS_SUCCESS) {
        LOGINFO("forward of heartbeat rpc to client failed");
        margo_destroy(handle);
        return rc;
    }

    /* decode response */
    int ret;
    unifyfs_heartbeat_out_t out;
    hret = margo_get_output(handle, &out);
    if (hret == HG_SUCCESS) {
        LOGDBG("Got response ret=%" PRIi32, out.ret);
        ret = (int) out.ret;
        margo_free_output(handle, &out);
    } else {
        LOGERR("margo_get_output() failed");
        ret = UNIFYFS_ERROR_MARGO;
    }

    /* free resources */
    margo_destroy(handle);

    return ret;
}

/* invokes the client mread request data response rpc function */
int invoke_client_mread_req_data_rpc(int app_id,
                                     int client_id,
                                     int mread_id,
                                     int read_index,
                                     size_t read_offset,
                                     size_t extent_size,
                                     void* extent_buffer)
{
    hg_return_t hret;

    /* check that we have initialized margo */
    if (NULL == unifyfsd_rpc_context) {
        return UNIFYFS_FAILURE;
    }

    /* fill input struct */
    unifyfs_mread_req_data_in_t in;
    in.app_id        = (int32_t) app_id;
    in.client_id     = (int32_t) client_id;
    in.mread_id      = (int32_t) mread_id;
    in.read_index    = (int32_t) read_index;
    in.read_offset   = (hg_size_t) read_offset;
    in.bulk_size     = (hg_size_t) extent_size;

    if (extent_size > 0) {
        /* initialize bulk handle for extents */
        hret = margo_bulk_create(unifyfsd_rpc_context->shm_mid,
                                 1, &extent_buffer, &extent_size,
                                 HG_BULK_READ_ONLY, &in.bulk_data);
        if (hret != HG_SUCCESS) {
            return UNIFYFS_ERROR_MARGO;
        }
    }

    /* get handle to rpc function */
    hg_id_t rpc_id = unifyfsd_rpc_context->rpcs.client_mread_data_id;
    hg_handle_t handle = create_client_handle(rpc_id, app_id, client_id);

    /* call rpc function */
    LOGDBG("invoking the mread[%d] req data (index=%d) rpc function in "
           "client[%d:%d]", mread_id, read_index, app_id, client_id);
    int rc = forward_to_client(handle, &in);
    if (rc != UNIFYFS_SUCCESS) {
        LOGERR("forward of mread-req-data rpc to client failed");
        margo_destroy(handle);
        return rc;
    }

    /* decode response */
    int ret;
    unifyfs_mread_req_data_out_t out;
    hret = margo_get_output(handle, &out);
    if (hret == HG_SUCCESS) {
        LOGDBG("Got response ret=%" PRIi32, out.ret);
        ret = (int) out.ret;
        margo_free_output(handle, &out);
    } else {
        LOGERR("margo_get_output() failed");
        ret = UNIFYFS_ERROR_MARGO;
    }

    if (extent_size > 0) {
        margo_bulk_free(in.bulk_data);
    }

    /* free resources */
    margo_destroy(handle);

    return ret;
}

/* invokes the client mread request completion rpc function */
int invoke_client_mread_req_complete_rpc(int app_id,
                                         int client_id,
                                         int mread_id,
                                         int read_index,
                                         int read_error)
{
    hg_return_t hret;

    /* check that we have initialized margo */
    if (NULL == unifyfsd_rpc_context) {
        return UNIFYFS_FAILURE;
    }

    /* fill input struct */
    unifyfs_mread_req_complete_in_t in;
    in.app_id        = (int32_t) app_id;
    in.client_id     = (int32_t) client_id;
    in.mread_id      = (int32_t) mread_id;
    in.read_index    = (int32_t) read_index;
    in.read_error    = (int32_t) read_error;

    /* get handle to rpc function */
    hg_id_t rpc_id = unifyfsd_rpc_context->rpcs.client_mread_complete_id;
    hg_handle_t handle = create_client_handle(rpc_id, app_id, client_id);

    /* call rpc function */
    LOGDBG("invoking the mread[%d] complete rpc function in client[%d:%d]",
            mread_id, app_id, client_id);
    int rc = forward_to_client(handle, &in);
    if (rc != UNIFYFS_SUCCESS) {
        LOGERR("forward of mread-complete rpc to client failed");
        margo_destroy(handle);
        return rc;
    }

    /* decode response */
    int ret;
    unifyfs_mread_req_complete_out_t out;
    hret = margo_get_output(handle, &out);
    if (hret == HG_SUCCESS) {
        LOGDBG("Got response ret=%" PRIi32, out.ret);
        ret = (int) out.ret;
        margo_free_output(handle, &out);
    } else {
        LOGERR("margo_get_output() failed");
        ret = UNIFYFS_ERROR_MARGO;
    }

    /* free resources */
    margo_destroy(handle);

    return ret;
}

/* invokes the client mread request completion rpc function */
int invoke_client_transfer_complete_rpc(int app_id,
                                        int client_id,
                                        int transfer_id,
                                        size_t transfer_sz_bytes,
                                        int transfer_time_sec,
                                        int transfer_time_usec,
                                        int error_code)
{
    hg_return_t hret;

    /* check that we have initialized margo */
    if (NULL == unifyfsd_rpc_context) {
        return UNIFYFS_FAILURE;
    }

    /* fill input struct */
    unifyfs_transfer_complete_in_t in;
    in.app_id              = (int32_t) app_id;
    in.client_id           = (int32_t) client_id;
    in.transfer_id         = (int32_t) transfer_id;
    in.transfer_size_bytes = (hg_size_t) transfer_sz_bytes;
    in.transfer_time_sec   = (uint32_t) transfer_time_sec;
    in.transfer_time_usec  = (uint32_t) transfer_time_usec;
    in.error_code          = (int32_t) error_code;

    /* get handle to rpc function */
    hg_id_t rpc_id = unifyfsd_rpc_context->rpcs.client_transfer_complete_id;
    hg_handle_t handle = create_client_handle(rpc_id, app_id, client_id);

    /* call rpc function */
    LOGDBG("invoking the transfer[%d] complete rpc function in client[%d:%d]",
           transfer_id, app_id, client_id);
    int rc = forward_to_client(handle, &in);
    if (rc != UNIFYFS_SUCCESS) {
        LOGERR("forward of transfer-complete rpc to client failed");
        margo_destroy(handle);
        return rc;
    }

    /* decode response */
    int ret;
    unifyfs_transfer_complete_out_t out;
    hret = margo_get_output(handle, &out);
    if (hret == HG_SUCCESS) {
        LOGDBG("Got response ret=%" PRIi32, out.ret);
        ret = (int) out.ret;
        margo_free_output(handle, &out);
    } else {
        LOGERR("margo_get_output() failed");
        ret = UNIFYFS_ERROR_MARGO;
    }

    /* free resources */
    margo_destroy(handle);

    return ret;
}

/* invokes the client mread request completion rpc function */
int invoke_client_unlink_callback_rpc(int app_id,
                                      int client_id,
                                      int gfid)
{
    hg_return_t hret;

    /* check that we have initialized margo */
    if (NULL == unifyfsd_rpc_context) {
        return UNIFYFS_FAILURE;
    }

    /* fill input struct */
    unifyfs_unlink_callback_in_t in;
    in.app_id    = (int32_t) app_id;
    in.client_id = (int32_t) client_id;
    in.gfid      = (int32_t) gfid;

    /* get handle to rpc function */
    hg_id_t rpc_id = unifyfsd_rpc_context->rpcs.client_unlink_callback_id;
    hg_handle_t handle = create_client_handle(rpc_id, app_id, client_id);

    /* call rpc function */
    LOGDBG("invoking the unlink (gfid=%d) callback rpc function in "
           "client[%d:%d]", gfid, app_id, client_id);
    hret = margo_forward(handle, &in);
    if (hret != HG_SUCCESS) {
        LOGERR("margo_forward() failed");
        margo_destroy(handle);
        return UNIFYFS_ERROR_MARGO;
    }

    /* decode response */
    int ret;
    unifyfs_unlink_callback_out_t out;
    hret = margo_get_output(handle, &out);
    if (hret == HG_SUCCESS) {
        LOGDBG("Got response ret=%" PRIi32, out.ret);
        ret = (int) out.ret;
        margo_free_output(handle, &out);
    } else {
        LOGERR("margo_get_output() failed");
        ret = UNIFYFS_ERROR_MARGO;
    }

    /* free resources */
    margo_destroy(handle);

    return ret;
}
