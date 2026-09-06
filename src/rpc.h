/* Exclusive JSON-RPC streams share quotas, deadlines and host-request refusal. */
#ifndef ASTOOLS_RPC_H
#define ASTOOLS_RPC_H
#include "execution.h"
#include "json.h"
typedef astools_err (*astools_rpc_note)(void *ud, const char *method, const jx_value *params,
                                        jx_value **out);
const char *astools_rpc_string(const jx_value *object, const char *key);
/* send/request own params, including on failure. Replies belong to the caller. */
astools_err astools_rpc_send(astools_ctx *c, astools_pproc *p, int id, const char *method,
                             jx_value *params, int64_t deadline, astools_task *cancel);
astools_err astools_rpc_receive(astools_ctx *c, astools_pproc *p, int id, astools_rpc_note note,
                                void *ud, int64_t deadline, astools_task *cancel, jx_value **out);
astools_err astools_rpc_request(astools_ctx *c, astools_pproc *p, const char *method,
                                jx_value *params, int64_t deadline, astools_task *cancel,
                                jx_value **out);
#endif
