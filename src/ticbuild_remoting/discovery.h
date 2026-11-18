#ifndef TICBUILD_REMOTING_DISCOVERY_H
#define TICBUILD_REMOTING_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>

/* Start the discovery session file writer.
 * port: the remoting TCP port
 * session_dir: project-local session dir (may be NULL)
 * global_disco: if true, also write to the OS/global session dir
 * instance_path: optional path to the running cart (may be NULL)
 * instance_path_len: length of instance_path (0 if NULL)
 * Returns true if at least one session file was written. */
bool tb_discovery_start(int port, const char* session_dir, bool global_disco,
                        const char* instance_path, size_t instance_path_len);

/* Stop discovery: remove any session files we created. */
void tb_discovery_stop(void);

#endif /* TICBUILD_REMOTING_DISCOVERY_H */
