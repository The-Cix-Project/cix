#ifndef CONTAINERRELOAD_H
#define CONTAINERRELOAD_H

/*
 * ADR-0384: puts files the daemon has just rewritten inside a running
 * container into force. The one way to do it -- never a signal to the
 * container's pidfd, which is cix-init (pid 1) and discards one it has
 * no handler for (#697).
 *
 * Asks the container's cix-init to send each ready service its declared
 * reload_signal. A cix-init older than CIXINIT_VERSION_RELOAD, or one
 * whose version is unknown, gets those services restarted instead. A
 * container whose services declare no reload_signal is left alone, and
 * the log says the change takes effect at its next start.
 *
 * Implemented in main.c, beside the cix-init command channel.
 */
void container_reload(const char *name);

#endif /* CONTAINERRELOAD_H */
