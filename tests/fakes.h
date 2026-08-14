/*
 * fakes.h — shared fixtures for the astools integration tests.
 *
 * fake_registry_write drops a minimal valid package (manifest only) into a
 * registry root; the runtime entry targets the CURRENT platform so the
 * package is available after a scan. fake_config_write emits a matching
 * #astools_config (watch off, pinning off, logging debug) so tests stay
 * hermetic and deterministic.
 */

#ifndef ASTOOLS_TESTS_FAKES_H
#define ASTOOLS_TESTS_FAKES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Canned #command bodies for fake_registry_write's `commands` slot. */

/* run {msg(string, required), p?(path rw), count?(int, default 7)} */
extern const char FAKE_CMD_RUN_ECHO[];
/* run {ms(integer >= 0, required)} */
extern const char FAKE_CMD_RUN_MS[];
/* run {} */
extern const char FAKE_CMD_RUN_EMPTY[];

/*
 * Write <root>/<tool_id>/manifest.xcdn (version 1.0.0, kind executable,
 * mode oneshot) whose runtime entry for the current (os, arch) is
 * argv: [tool_bin, behavior].
 *
 *   tool_bin     absolute tool_fake path (requires a "full"-trust root) or
 *                NULL for the package-relative dummy "bin/fake" (fine for
 *                tests that never spawn: catalog, policy, lockfile).
 *   behavior     argv[1] selector for tool_fake; NULL = "echo".
 *   permissions  body of the permissions object, or NULL for
 *                fs [${workspace} read-write], net/proc false, env [].
 *   commands     elements of the commands array, or NULL for
 *                FAKE_CMD_RUN_ECHO.
 *   extra_fields extra top-level manifest fields, each ending with a
 *                trailing comma, or NULL.
 *
 * Returns 1 on success, 0 on failure.
 */
int fake_registry_write(const char *root, const char *tool_id,
                        const char *tool_bin, const char *behavior,
                        const char *permissions, const char *commands,
                        const char *extra_fields);

/*
 * Write a #astools_config at path: one registry root at trust "full"
 * (trust_full != 0) or "standard", watch "off", pinning "off",
 * workspace.root = workspace, logging level "debug". extra appends
 * additional top-level config fields (each ending with a trailing comma)
 * or NULL. Returns 1 on success, 0 on failure.
 */
int fake_config_write(const char *path, const char *root_dir, int trust_full,
                      const char *workspace, const char *extra);

#ifdef __cplusplus
}
#endif

#endif /* ASTOOLS_TESTS_FAKES_H */
