/************************************************************\
 * Copyright 2019 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef HAVE_BROKER_PMIUTIL_H
#define HAVE_BROKER_PMIUTIL_H 1

struct pmi_params {
    int rank;
    int size;
    char kvsname[1024];
};

struct pmi_handle;

typedef int (*broker_pmi_kvs_commit_t) (struct pmi_handle *pmi, const char *kvsname);

typedef int (*broker_pmi_kvs_put_t) (struct pmi_handle *pmi,
                           const char *kvsname,
                           const char *key,
                           const char *value);

typedef int (*broker_pmi_kvs_get_t) (struct pmi_handle *pmi,
                           const char *kvsname,
                           const char *key,
                           char *value,
                           int len);

typedef int (*broker_pmi_barrier_t) (struct pmi_handle *pmi);

typedef int (*broker_pmi_get_params_t) (struct pmi_handle *pmi, struct pmi_params *params);

typedef int (*broker_pmi_init_t) (struct pmi_handle *pmi);

typedef int (*broker_pmi_finalize_t) (struct pmi_handle *pmi);

typedef void (*broker_pmi_destroy_t) (struct pmi_handle *pmi);

typedef struct pmi_handle* (*broker_pmi_create_t) (void);

typedef struct pmi_callbacks {
    broker_pmi_kvs_commit_t kvs_commit;
    broker_pmi_kvs_put_t kvs_put;
    broker_pmi_kvs_get_t kvs_get;
    broker_pmi_barrier_t barrier;
    broker_pmi_get_params_t get_params;
    broker_pmi_init_t init;
    broker_pmi_finalize_t finalize;
    broker_pmi_destroy_t destroy;
    broker_pmi_create_t create;
} pmi_callbacks_t;

extern pmi_callbacks_t broker_pmi_callbacks;

#endif /* !HAVE_BROKER_PMIUTIL_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
