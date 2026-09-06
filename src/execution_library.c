/* execution library — runtime implementation. */
#include "execution.h"

astools_err astools_exec_library(astools_ctx *c, astools_tool *t, const char *request_text,
                                 const char *invocation_id, astools_result *r) {
  astools_pproc *p;
  const astools_tool_vtable *vt = NULL;
  astools_err e = ASTOOLS_OK;
  char *resp;

  if (!c || !t || !request_text || !invocation_id || !r) return ASTOOLS_ERR_INVALID;
  memset(r, 0, sizeof *r);
  e = astools_pp_acquire(c, PP_KIND_LIB, t, 0, NULL, &p);
  if (e != ASTOOLS_OK) return e;
  if (memcmp(p->tool->content_sha256, t->content_sha256, 32)) {
    astools_pp_release(c, p);
    return astools_seterr(
        c, ASTOOLS_ERR_DENIED,
        "loaded library changed; reopen the context before using its replacement");
  }

  os_mutex_lock(&p->io_mu);
  if (!p->lib_loaded && !p->lib_failed) {
    const char *a0 = (t->entry && t->entry->argv_len > 0) ? t->entry->argv[0] : NULL;
    char *path = NULL;
    if (!a0) {
      e = astools_seterr(c, ASTOOLS_ERR_CONFIG, "library tool '%s' has no entry argv", t->m->id);
    } else if (os_path_is_abs(a0)) {
      path = astools_strdup(a0);
      if (!path) e = ASTOOLS_ERR_NOMEM;
    } else {
      path = os_path_join(t->pkg_dir, a0);
      if (!path) e = ASTOOLS_ERR_NOMEM;
    }
    if (e == ASTOOLS_OK) {
      e = os_dylib_open(path, &p->lib);
      if (e != ASTOOLS_OK) {
        e = astools_seterr(c, ASTOOLS_ERR_IO, "cannot load library '%s'", path);
      } else {
        void *sym = os_dylib_sym(&p->lib, "astools_tool_entry");
        astools_tool_entry_fn fn = NULL;
        if (sym) memcpy(&fn, &sym, sizeof fn);
        vt = fn ? fn() : NULL;
        if (!vt || vt->abi_version != ASTOOLS_TOOL_ABI || !vt->init || !vt->invoke ||
            !vt->free_result) {
          os_dylib_close(&p->lib);
          vt = NULL;
          e = astools_seterr(c, ASTOOLS_ERR_PROTOCOL,
                             "library '%s': missing or incompatible "
                             "astools_tool_entry (need ABI %d)",
                             path, ASTOOLS_TOOL_ABI);
        } else {
          char *mtext = NULL;
          e = astools_manifest_render(t->m, &mtext);
          if (e == ASTOOLS_OK) {
            int rc = vt->init(mtext);
            free(mtext);
            p->lib_loaded = true;
            p->vt = vt;
            if (rc != 0) {
              /* ABI: nonzero init fails every invocation — sticky */
              p->lib_failed = true;
              e = astools_seterr(c, ASTOOLS_ERR_TOOL, "library '%s' init failed (%d)", t->m->id,
                                 rc);
            } else {
              p->lib_init_ok = true;
            }
          } else {
            os_dylib_close(&p->lib);
            vt = NULL;
          }
        }
      }
    }
    free(path);
  }
  if (e == ASTOOLS_OK && p->lib_failed) {
    e = astools_seterr(c, ASTOOLS_ERR_TOOL, "library '%s' failed to initialize", t->m->id);
  }
  vt = (e == ASTOOLS_OK) ? p->vt : NULL;
  os_mutex_unlock(&p->io_mu);
  if (!vt) {
    astools_exec_result_set(r, "astools/tool-crashed", "library unavailable: %s",
                            astools_last_error(c) ? astools_last_error(c) : "load failed");
    astools_pp_release(c, p);
    return e != ASTOOLS_OK ? e : ASTOOLS_ERR_TOOL;
  }

  /* vtable->invoke is thread-safe by ABI contract: no lock held here.
   * The node stays pinned by the busy count until astools_pp_release. */
  resp = vt->invoke(request_text);
  if (!resp) {
    astools_exec_result_set(r, "astools/tool-crashed", "library returned no response");
    e = astools_seterr(c, ASTOOLS_ERR_TOOL, "library '%s' returned no response", t->m->id);
  } else {
    char *rid = NULL, *perr = NULL;
    astools_err pe =
        astools_proto_parse_response(resp, strlen(resp), invocation_id, &rid, r, &perr);
    if (pe != ASTOOLS_OK) {
      astools_exec_result_clear(r);
      astools_exec_result_set(r, "astools/protocol", "malformed library response: %s",
                              perr ? perr : "unparsable");
      e = astools_seterr(c, ASTOOLS_ERR_PROTOCOL, "library '%s': malformed response: %s", t->m->id,
                         perr ? perr : "unparsable");
    }
    free(rid);
    free(perr);
    vt->free_result(resp);
  }
  astools_pp_release(c, p);
  return e;
}
