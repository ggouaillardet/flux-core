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

#include "src/common/libutil/log.h"
#include "src/common/libutil/iterators.h"
#include "src/common/libpmi/pmi.h"
#include "src/common/libpmi/pmi_strerror.h"
#include "src/common/libpmi/simple_client.h"

#include "pmiutil.h"
#include "liblist.h"

typedef enum {
    PMI_MODE_SINGLETON,
    PMI_MODE_DLOPEN,
    PMI_MODE_WIRE1,
} pmi_mode_t;

struct pmi_dso {
    void *dso;
    int (*init) (int *spawned);
    int (*finalize) (void);
    int (*get_size) (int *size);
    int (*get_rank) (int *rank);
    int (*barrier) (void);
    int (*kvs_get_my_name) (char *kvsname, int length);
    int (*kvs_put) (const char *kvsname, const char *key, const char *value);
    int (*kvs_commit) (const char *kvsname);
    int (*kvs_get) (const char *kvsname, const char *key, char *value, int len);
};

struct pmi_handle {
    struct pmi_dso *dso;
    struct pmi_simple_client *cli;
    int debug;
    pmi_mode_t mode;
    int rank;
};

static void vdebugf (struct pmi_handle *pmi, const char *fmt, va_list ap)
{

    if (pmi->debug) {
        char buf[1024];
        (void)vsnprintf (buf, sizeof (buf), fmt, ap);
        fprintf (stderr, "pmi-debug-%s[%d]: %s\n",
                pmi->mode == PMI_MODE_SINGLETON ? "singleton" :
                pmi->mode == PMI_MODE_WIRE1 ? "wire.1" :
                pmi->mode == PMI_MODE_DLOPEN ? "dlopen" : "unknown",
                pmi->rank,
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

static void broker_pmi_dlclose (struct pmi_dso *dso)
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
static struct pmi_dso *broker_pmi_dlopen (const char *pmi_library, int debug)
{
    struct pmi_dso *dso;
    zlist_t *libs = NULL;
    char *name;

    if (!(dso = calloc (1, sizeof (*dso))))
        return NULL;
    if (!pmi_library)
        pmi_library = "libpmi.so";
    if (!(libs = liblist_create (pmi_library)))
        goto error;
    FOREACH_ZLIST (libs, name) {
        dlerror ();
        if (!(dso->dso = dlopen (name, RTLD_NOW | RTLD_GLOBAL))) {
            if (debug) {
                char *errstr = dlerror ();
                if (errstr)
                    log_msg ("pmi-debug-dlopen: %s", errstr);
                else
                    log_msg ("pmi-debug-dlopen: dlopen %s failed", name);
            }
        }
        else if (dlsym (dso->dso, "flux_pmi_library")) {
            if (debug)
                log_msg ("pmi-debug-dlopen: skipping %s", name);
            dlclose (dso->dso);
            dso->dso = NULL;
        }
        else {
            if (debug)
                log_msg ("pmi-debug-dlopen: library name %s", name);
        }
    }
    liblist_destroy (libs);
    libs = NULL;
    if (!dso->dso)
        goto error;
    dso->init = dlsym (dso->dso, "PMI_Init");
    dso->finalize = dlsym (dso->dso, "PMI_Finalize");
    dso->get_size = dlsym (dso->dso, "PMI_Get_size");
    dso->get_rank = dlsym (dso->dso, "PMI_Get_rank");
    dso->barrier = dlsym (dso->dso, "PMI_Barrier");
    dso->kvs_get_my_name = dlsym (dso->dso, "PMI_KVS_Get_my_name");
    dso->kvs_put = dlsym (dso->dso, "PMI_KVS_Put");
    dso->kvs_commit = dlsym (dso->dso, "PMI_KVS_Commit");
    dso->kvs_get = dlsym (dso->dso, "PMI_KVS_Get");

    if (!dso->init || !dso->finalize || !dso->get_size || !dso->get_rank
            || !dso->barrier || !dso->kvs_get_my_name
            || !dso->kvs_put || !dso->kvs_commit || !dso->kvs_get) {
        log_msg ("pmi-debug-dlopen: dlsym: %s is missing required symbols",
                 pmi_library);
        goto error;
    }
    return dso;
error:
    broker_pmi_dlclose (dso);
    if (libs)
        liblist_destroy (libs);
    return NULL;
}

int broker_pmi_kvs_commit (struct pmi_handle *pmi, const char *kvsname)
{
    int ret = PMI_SUCCESS;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            break;
        case PMI_MODE_DLOPEN:
            ret = pmi->dso->kvs_commit (kvsname);
            break;
    }
    debugf (pmi,
            "kvs_commit (kvsname=%s) = %s",
            kvsname,
            pmi_strerror (ret));
    return ret;
}

int broker_pmi_kvs_put (struct pmi_handle *pmi,
                        const char *kvsname,
                        const char *key,
                        const char *value)
{
    int ret = PMI_SUCCESS;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            ret = pmi_simple_client_kvs_put (pmi->cli, kvsname, key, value);
            break;
        case PMI_MODE_DLOPEN:
            ret = pmi->dso->kvs_put (kvsname, key, value);
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

int broker_pmi_kvs_get (struct pmi_handle *pmi,
                               const char *kvsname,
                               const char *key,
                               char *value,
                               int len)
{
    int ret = PMI_FAIL;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            ret = pmi_simple_client_kvs_get (pmi->cli, kvsname, key, value, len);
            break;
        case PMI_MODE_DLOPEN:
            ret = pmi->dso->kvs_get (kvsname, key, value, len);
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

int broker_pmi_barrier (struct pmi_handle *pmi)
{
    int ret = PMI_SUCCESS;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            ret = pmi_simple_client_barrier (pmi->cli);
            break;
        case PMI_MODE_DLOPEN:
            ret = pmi->dso->barrier();
            break;
    }
    debugf (pmi, "barrier = %s", pmi_strerror (ret));
    return ret;
}

int broker_pmi_get_params (struct pmi_handle *pmi,
                           struct pmi_params *params)
{
    int ret = PMI_SUCCESS;

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
            if ((ret = pmi->dso->get_rank (&params->rank)) != PMI_SUCCESS)
                break;
            if ((ret = pmi->dso->get_size (&params->size)) != PMI_SUCCESS)
                break;
            ret = pmi->dso->kvs_get_my_name (params->kvsname,
                                             sizeof (params->kvsname));
            break;
    }
    if (ret == PMI_SUCCESS)
        pmi->rank = params->rank;
    debugf (pmi,
            "get_params (rank=%d size=%d kvsname=%s) = %s",
            ret == PMI_SUCCESS ? params->rank : -1,
            ret == PMI_SUCCESS ? params->size : -1,
            ret == PMI_SUCCESS ? params->kvsname: "<none>",
            pmi_strerror (ret));
    return ret;
}

int broker_pmi_init (struct pmi_handle *pmi)
{
    int spawned;
    int ret = PMI_SUCCESS;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            ret = pmi_simple_client_init (pmi->cli);
            break;
        case PMI_MODE_DLOPEN:
            ret = pmi->dso->init(&spawned);
            break;
    }
    debugf (pmi, "init = %s", pmi_strerror (ret));
    return ret;
}

int broker_pmi_finalize (struct pmi_handle *pmi)
{
    int ret = PMI_SUCCESS;

    switch (pmi->mode) {
        case PMI_MODE_SINGLETON:
            break;
        case PMI_MODE_WIRE1:
            ret = pmi_simple_client_finalize (pmi->cli);
            break;
        case PMI_MODE_DLOPEN:
            ret = pmi->dso->finalize ();
            break;
    }
    debugf (pmi, "finalize = %s", pmi_strerror (ret));
    return PMI_SUCCESS;
}

void broker_pmi_destroy (struct pmi_handle *pmi)
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
                broker_pmi_dlclose (pmi->dso);
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
struct pmi_handle *broker_pmi_create (void)
{
    const char *pmi_debug;
    struct pmi_handle *pmi = calloc (1, sizeof (*pmi));
    if (!pmi)
        return NULL;
    pmi->rank = -1;
    pmi_debug = getenv ("FLUX_PMI_DEBUG");
    if (pmi_debug)
        pmi->debug = strtol (pmi_debug, NULL, 10);
    if ((pmi->cli = pmi_simple_client_create_fd (getenv ("PMI_FD"),
                                                 getenv ("PMI_RANK"),
                                                 getenv ("PMI_SIZE"),
                                                 NULL))) {
        pmi->mode = PMI_MODE_WIRE1;
    }
    /* N.B. SLURM boldly installs its libpmi.so into the system libdir,
     * so it will be found here, even if not running in a SLURM job.
     * Fortunately it emulates singleton in that case, in lieu of failing.
     */
    else if ((pmi->dso = broker_pmi_dlopen (getenv ("PMI_LIBRARY"),
                                            pmi->debug))) {
        pmi->mode = PMI_MODE_DLOPEN;
    }
    /* If neither pmi->cli nor pmi->dso is set, singleton is assumed.
     */
    else {
        pmi->mode = PMI_MODE_SINGLETON;
    }
    return pmi;
}

pmi_callbacks_t broker_pmi_callbacks = {
    broker_pmi_kvs_commit,
    broker_pmi_kvs_put,
    broker_pmi_kvs_get,
    broker_pmi_barrier,
    broker_pmi_get_params,
    broker_pmi_init,
    broker_pmi_finalize,
    broker_pmi_destroy,
    broker_pmi_create
};

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
