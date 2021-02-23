/************************************************************\
 * Copyright 2019 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/param.h>
#include <unistd.h>
#include <dlfcn.h>
#include <assert.h>
#include <czmq.h>
#include <pmix.h>

#include "src/common/libutil/log.h"
#include "src/common/libutil/iterators.h"
#include "src/common/libpmi/pmi.h"
#include "src/common/libpmi/pmi_strerror.h"
#include "src/common/libpmi/simple_client.h"

#include "pmiutil.h"
#include "pmixutil.h"
#include "liblist.h"

#define ANL_MAPPING "PMI_process_mapping"

static pmix_proc_t myproc;

typedef enum {
    PMI_MODE_SINGLETON,
    PMI_MODE_DLOPEN,
    PMI_MODE_WIRE1,
} pmi_mode_t;

struct pmix_dso {
    void *dso;
    pmix_status_t (*init) (pmix_proc_t *proc, pmix_info_t info[], size_t ninfo);
    pmix_status_t (*finalize) (void *, int);
    pmix_status_t (*get) (const pmix_proc_t *proc, const char key[], const pmix_info_t info[], size_t ninfo, pmix_value_t **val);
    pmix_status_t (*fence)(const pmix_proc_t procs[], size_t nprocs, const pmix_info_t info[], size_t ninfo);
    pmix_status_t (*put)(pmix_scope_t scope, const pmix_key_t key, pmix_value_t *val);
    pmix_status_t (*commit) ();
};

struct pmi_handle {
    struct pmix_dso *dso;
    struct pmi_simple_client *cli;
    int debug;
    pmi_mode_t mode;
    int rank;
};

static int convert_err(pmix_status_t rc)
{
    switch (rc) {
    case PMIX_ERR_INVALID_SIZE:
        return PMI_ERR_INVALID_SIZE;

    case PMIX_ERR_INVALID_KEYVALP:
        return PMI_ERR_INVALID_KEYVALP;

    case PMIX_ERR_INVALID_NUM_PARSED:
        return PMI_ERR_INVALID_NUM_PARSED;

    case PMIX_ERR_INVALID_ARGS:
        return PMI_ERR_INVALID_ARGS;

    case PMIX_ERR_INVALID_NUM_ARGS:
        return PMI_ERR_INVALID_NUM_ARGS;

    case PMIX_ERR_INVALID_LENGTH:
        return PMI_ERR_INVALID_LENGTH;

    case PMIX_ERR_INVALID_VAL_LENGTH:
        return PMI_ERR_INVALID_VAL_LENGTH;

    case PMIX_ERR_INVALID_VAL:
        return PMI_ERR_INVALID_VAL;

    case PMIX_ERR_INVALID_KEY_LENGTH:
        return PMI_ERR_INVALID_KEY_LENGTH;

    case PMIX_ERR_INVALID_KEY:
        return PMI_ERR_INVALID_KEY;

    case PMIX_ERR_INVALID_ARG:
        return PMI_ERR_INVALID_ARG;

    case PMIX_ERR_NOMEM:
        return PMI_ERR_NOMEM;

    case PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER:
    case PMIX_ERR_LOST_CONNECTION_TO_SERVER:
    case PMIX_ERR_LOST_PEER_CONNECTION:
    case PMIX_ERR_LOST_CONNECTION_TO_CLIENT:
    case PMIX_ERR_NOT_SUPPORTED:
    case PMIX_ERR_NOT_FOUND:
    case PMIX_ERR_SERVER_NOT_AVAIL:
    case PMIX_ERR_INVALID_NAMESPACE:
    case PMIX_ERR_DATA_VALUE_NOT_FOUND:
    case PMIX_ERR_OUT_OF_RESOURCE:
    case PMIX_ERR_RESOURCE_BUSY:
    case PMIX_ERR_BAD_PARAM:
    case PMIX_ERR_IN_ERRNO:
    case PMIX_ERR_UNREACH:
    case PMIX_ERR_TIMEOUT:
    case PMIX_ERR_NO_PERMISSIONS:
    case PMIX_ERR_PACK_MISMATCH:
    case PMIX_ERR_PACK_FAILURE:
    case PMIX_ERR_UNPACK_FAILURE:
    case PMIX_ERR_UNPACK_INADEQUATE_SPACE:
    case PMIX_ERR_TYPE_MISMATCH:
    case PMIX_ERR_PROC_ENTRY_NOT_FOUND:
    case PMIX_ERR_UNKNOWN_DATA_TYPE:
    case PMIX_ERR_WOULD_BLOCK:
    case PMIX_EXISTS:
    case PMIX_ERROR:
        return PMI_FAIL;

    case PMIX_ERR_INIT:
        return PMI_ERR_INIT;

    case PMIX_SUCCESS:
        return PMI_SUCCESS;
    default:
        return PMI_FAIL;
    }
}

static pmix_status_t convert_int(int *value, pmix_value_t *kv)
{
    switch (kv->type) {
    case PMIX_INT:
        *value = kv->data.integer;
        break;
    case PMIX_INT8:
        *value = kv->data.int8;
        break;
    case PMIX_INT16:
        *value = kv->data.int16;
        break;
    case PMIX_INT32:
        *value = kv->data.int32;
        break;
    case PMIX_INT64:
        *value = kv->data.int64;
        break;
    case PMIX_UINT:
        *value = kv->data.uint;
        break;
    case PMIX_UINT8:
        *value = kv->data.uint8;
        break;
    case PMIX_UINT16:
        *value = kv->data.uint16;
        break;
    case PMIX_UINT32:
        *value = kv->data.uint32;
        break;
    case PMIX_UINT64:
        *value = kv->data.uint64;
        break;
    case PMIX_BYTE:
        *value = kv->data.byte;
        break;
    case PMIX_SIZE:
        *value = kv->data.size;
        break;
    case PMIX_BOOL:
        *value = kv->data.flag;
        break;
    default:
        /* not an integer type */
        return PMIX_ERR_BAD_PARAM;
    }
    return PMIX_SUCCESS;
}
static void vdebugf (struct pmi_handle *pmix, const char *fmt, va_list ap)
{

    if (pmix->debug) {
        char buf[1024];
        (void)vsnprintf (buf, sizeof (buf), fmt, ap);
        fprintf (stderr, "pmix-debug-%s[%d]: %s\n",
                pmix->mode == PMI_MODE_SINGLETON ? "singleton" :
                pmix->mode == PMI_MODE_WIRE1 ? "wire.1" :
                pmix->mode == PMI_MODE_DLOPEN ? "dlopen" : "unknown",
                pmix->rank,
                buf);
    }
}

static void debugf (struct pmi_handle *pmi, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    vdebugf (pmi, fmt, ap);
    va_end (ap);
}

static void broker_pmix_dlclose (struct pmix_dso *dso)
{
    if (dso) {
#ifndef __SANITIZE_ADDRESS__
        if (dso->dso)
            dlclose (dso->dso);
#endif
        free (dso);
    }
}

/* Notes:
 * - Use RTLD_GLOBAL due to issue #432
 */
static struct pmix_dso *broker_pmix_dlopen (const char *pmix_library, int debug)
{
    struct pmix_dso *dso;
    zlist_t *libs = NULL;
    char *name;

    if (!(dso = calloc (1, sizeof (*dso))))
        return NULL;
    if (!pmix_library)
        pmix_library = "libpmix.so";
    if (!(libs = liblist_create (pmix_library)))
        goto error;
    FOREACH_ZLIST (libs, name) {
        dlerror ();
        if (!(dso->dso = dlopen (name, RTLD_NOW | RTLD_GLOBAL))) {
            if (debug) {
                char *errstr = dlerror ();
                if (errstr)
                    log_msg ("pmix-debug-dlopen: %s", errstr);
                else
                    log_msg ("pmix-debug-dlopen: dlopen %s failed", name);
            }
        }
        else if (dlsym (dso->dso, "flux_pmix_library")) {
            if (debug)
                log_msg ("pmix-debug-dlopen: skipping %s", name);
            dlclose (dso->dso);
            dso->dso = NULL;
        }
        else {
            if (debug)
                log_msg ("pmix-debug-dlopen: library name %s", name);
        }
    }
    liblist_destroy (libs);
    libs = NULL;
    if (!dso->dso)
        goto error;
    dso->init = dlsym (dso->dso, "PMIx_Init");
    dso->finalize = dlsym (dso->dso, "PMIx_Finalize");
    dso->get = dlsym (dso->dso, "PMIx_Get");
    dso->fence = dlsym (dso->dso, "PMIx_Fence");
    dso->put = dlsym (dso->dso, "PMIx_Put");
    dso->commit = dlsym (dso->dso, "PMIx_Commit");

    if (!dso->init || !dso->finalize || !dso->get
            || !dso->fence || !dso->put || !dso->commit ) {
        log_msg ("pmix-debug-dlopen: dlsym: %s is missing required symbols",
                 pmix_library);
        goto error;
    }
    return dso;
error:
    broker_pmix_dlclose (dso);
    if (libs)
        liblist_destroy (libs);
    return NULL;
}

int broker_pmix_kvs_commit (struct pmi_handle *pmi, const char *kvsname)
{
    int ret = PMI_SUCCESS;
    pmix_status_t rc = PMIX_SUCCESS;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            break;
        case PMI_MODE_DLOPEN:
            rc = pmi->dso->commit();
            ret = convert_err(rc);
            break;
    }
    debugf (pmi,
            "kvs_commit (kvsname=%s) = %s",
            kvsname,
            pmi_strerror (ret));
    return ret;
}

int broker_pmix_kvs_put (struct pmi_handle *pmi,
                        const char *kvsname,
                        const char *key,
                        const char *value)
{
    int ret = PMI_SUCCESS;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_value_t val;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            ret = pmi_simple_client_kvs_put (pmi->cli, kvsname, key, value);
            break;
        case PMI_MODE_DLOPEN:
            val.type = PMIX_STRING;
            val.data.string = (char*)value;
            rc = pmi->dso->put(PMIX_GLOBAL, key, &val);
            ret = convert_err(rc);
            break;
    }
    debugf (pmi,
            "kvs_put (kvsname=%s key=%s value=%s) = %s",
            kvsname,
            key,
            value,
            pmi_strerror (ret));
    return ret;
}

int broker_pmix_kvs_get (struct pmi_handle *pmi,
                               const char *kvsname,
                               const char *key,
                               char *value,
                               int len)
{
    int ret = PMI_FAIL;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_value_t *val;
    pmix_proc_t proc;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            ret = pmi_simple_client_kvs_get (pmi->cli, kvsname, key, value, len);
            break;
        case PMI_MODE_DLOPEN:
            /* PMI-1 expects resource manager to set
             * process mapping in ANL notation. */
            if (!strcmp(key, ANL_MAPPING)) {
                /* we are looking in the job-data. If there is nothing there
                 * we don't want to look in rank's data, thus set rank to widcard */
                proc = myproc;
                proc.rank = PMIX_RANK_WILDCARD;
                if (PMIX_SUCCESS == pmi->dso->get(&proc, PMIX_ANL_MAP, NULL, 0, &val) &&
                       (NULL != val) && (PMIX_STRING == val->type)) {
                    pmix_strncpy(value, val->data.string, len-1);
                    PMIX_VALUE_FREE(val, 1);
                    ret = PMI_SUCCESS;
                } else {
                    /* artpol:
                     * Some RM's (i.e. SLURM) already have ANL precomputed. The export it
                     * through PMIX_ANL_MAP variable.
                     * If we haven't found it we want to have our own packing functionality
                     * since it's common.
                     * Somebody else has to write it since I've already done that for
                     * GPL'ed SLURM :) */
                    ret = PMI_FAIL;
                }
            } else {
                /* retrieve the data from PMIx - since we don't have a rank,
                 * we indicate that by passing the UNDEF value */
                pmix_strncpy(proc.nspace, kvsname, PMIX_MAX_NSLEN);
                proc.rank = PMIX_RANK_UNDEF;

                rc = pmi->dso->get(&proc, key, NULL, 0, &val);
                if (PMIX_SUCCESS == rc && NULL != val) {
                    if (PMIX_STRING != val->type) {
                        rc = PMIX_ERROR;
                    } else if (NULL != val->data.string) {
                        pmix_strncpy(value, val->data.string, len-1);
                    }
                    PMIX_VALUE_RELEASE(val);
                }

                ret = convert_err(rc);
            }

            break;
    }
    debugf (pmi,
            "kvs_get (kvsname=%s key=%s value=%s) = %s",
            kvsname,
            key,
            ret == PMI_SUCCESS ? value : "<none>",
            pmi_strerror (ret));
    return ret;
}

int broker_pmix_barrier (struct pmi_handle *pmi)
{
    int ret = PMI_SUCCESS;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_info_t buf;
    int ninfo = 0;
    pmix_info_t *info = NULL;
    bool val = 1;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            ret = pmi_simple_client_barrier (pmi->cli);
            break;
        case PMI_MODE_DLOPEN:
            info = &buf;
            PMIX_INFO_CONSTRUCT(info);
            /* Do not PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &val, PMIX_BOOL)
             * so we do not have to link with libpmix.so */
            info[0].flags = 0;
            pmix_strncpy(info[0].key, PMIX_COLLECT_DATA, PMIX_MAX_KEYLEN);
            info[0].flags = 0;
            info[0].value.type = PMIX_BOOL;
            info[0].value.data.flag = val;
            ninfo = 1;
            rc = pmi->dso->fence(NULL, 0, info, ninfo);
            PMIX_INFO_DESTRUCT(info);
            ret = convert_err(rc);
            break;
    }
    debugf (pmi, "barrier = %s", pmi_strerror (ret));
    return ret;
}

int broker_pmix_get_params (struct pmi_handle *pmi,
                            struct pmi_params *params)
{
    int ret = PMI_SUCCESS;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_value_t *val;
    pmix_info_t info[1];
    bool  val_optinal = 1;
    pmix_proc_t proc = myproc;
    proc.rank = PMIX_RANK_WILDCARD;


    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            params->rank = 0;
            params->size = 1;
            snprintf (params->kvsname, sizeof (params->kvsname), "singleton");
            break;
        case PMI_MODE_WIRE1:
            params->rank = pmi->cli->rank;
            params->size = pmi->cli->size;
            ret = pmi_simple_client_kvs_get_my_name (pmi->cli,
                                                     params->kvsname,
                                                     sizeof (params->kvsname));
            break;
        case PMI_MODE_DLOPEN:
            params->rank = myproc.rank;

            /* set controlling parameters
             * PMIX_OPTIONAL - expect that these keys should be available on startup
             */
            PMIX_INFO_CONSTRUCT(&info[0]);
            /* Do not PMIX_INFO_LOAD(&info[0], PMIX_OPTIONAL, &val_optinal, PMIX_BOOL)
             * so we do not have to link with libpmix.so */
            info[0].flags = 0;
            pmix_strncpy(info[0].key, PMIX_OPTIONAL, PMIX_MAX_KEYLEN);
            info[0].flags = 0;
            info[0].value.type = PMIX_BOOL;
            info[0].value.data.flag = val_optinal;

            rc = pmi->dso->get(&proc, PMIX_JOB_SIZE, info, 1, &val);
            if (PMIX_SUCCESS == rc) {
                rc = convert_int(&params->size, val);
                PMIX_VALUE_RELEASE(val);
            }

            PMIX_INFO_DESTRUCT(&info[0]);
            pmix_strncpy(params->kvsname, myproc.nspace, sizeof(params->kvsname)-1);
            pmi->rank = params->rank;
    }
    if (ret == PMI_SUCCESS)
    debugf (pmi,
            "get_params (rank=%d size=%d kvsname=%s) = %s",
            ret == PMI_SUCCESS ? params->rank : -1,
            ret == PMI_SUCCESS ? params->size : -1,
            ret == PMI_SUCCESS ? params->kvsname: "<none>",
            pmi_strerror (ret));
    return ret;
}

int broker_pmix_init (struct pmi_handle *pmi)
{
    int ret = PMI_SUCCESS;
            pmix_status_t rc = PMIX_SUCCESS;
            pmix_proc_t proc;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            ret = pmi_simple_client_init (pmi->cli);
            break;
        case PMI_MODE_DLOPEN:

            if (PMIX_SUCCESS != (rc = pmi->dso->init(&myproc, NULL, 0))) {
                /* if we didn't see a PMIx server (e.g., missing envar),
                 * then allow us to run as a singleton */
                ret = PMI_ERR_INIT;
                break;
            }

            /* getting internal key requires special rank value */
            memcpy(&proc, &myproc, sizeof(myproc));
            proc.rank = PMIX_RANK_WILDCARD;

            ret = PMI_SUCCESS;
            break;
    }
    debugf (pmi, "init = %s", pmi_strerror (ret));
    return ret;
}

int broker_pmix_finalize (struct pmi_handle *pmi)
{
    int ret = PMI_SUCCESS;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            ret = pmi_simple_client_finalize (pmi->cli);
            break;
        case PMI_MODE_DLOPEN:
            (void)pmi->dso->finalize (NULL, 0);
            break;
    }
    debugf (pmi, "finalize = %s", pmi_strerror (ret));
    return PMI_SUCCESS;
}

void broker_pmix_destroy (struct pmi_handle *pmi)
{
    if (pmi) {
        int saved_errno = errno;
        switch (pmi->mode) {
            case PMI_MODE_SINGLETON:
                break;
            case PMI_MODE_WIRE1:
                pmi_simple_client_destroy (pmi->cli);
                break;
            case PMI_MODE_DLOPEN:
                broker_pmix_dlclose (pmi->dso);
                break;
        }
        free (pmi);
        errno = saved_errno;
    }
}

/* Attempt to set up PMI-1 wire protocol client.
 * If that fails, try dlopen.
 * If that fails, singleton will be used.
 */
struct pmi_handle *broker_pmix_create (void)
{
    const char *pmix_debug;
    struct pmi_handle *pmix = calloc (1, sizeof (*pmix));
    if (!pmix)
        return NULL;
    pmix->rank = -1;
    pmix_debug = getenv ("FLUX_PMIX_DEBUG");
    if (pmix_debug)
        pmix->debug = strtol (pmix_debug, NULL, 10);
    if (false && (pmix->cli = pmi_simple_client_create_fd (getenv ("PMI_FD"),
                                                           getenv ("PMI_RANK"),
                                                           getenv ("PMI_SIZE"),
                                                           NULL))) {
        /* FIXME should this be fixed or removed? */
        pmix->mode = PMI_MODE_WIRE1;
    }
    /* N.B. SLURM boldly installs its libpmix.so into the system libdir,
     * so it will be found here, even if not running in a SLURM job.
     * Fortunately it emulates singleton in that case, in lieu of failing.
     */
    else if ((pmix->dso = broker_pmix_dlopen (getenv ("PMIX_LIBRARY"),
                                              pmix->debug))) {
        pmix->mode = PMI_MODE_DLOPEN;
    }
    /* If neither pmi->cli nor pmi->dso is set, singleton is assumed.
     */
    else {
        pmix->mode = PMI_MODE_SINGLETON;
    }
    return pmix;
}

pmi_callbacks_t broker_pmix_callbacks = {
    broker_pmix_kvs_commit,
    broker_pmix_kvs_put,
    broker_pmix_kvs_get,
    broker_pmix_barrier,
    broker_pmix_get_params,
    broker_pmix_init,
    broker_pmix_finalize,
    broker_pmix_destroy,
    broker_pmix_create
};

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
