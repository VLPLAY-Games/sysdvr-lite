#ifndef WATCHDOG_H
#define WATCHDOG_H

/* Reports to systemd that the client is alive, so that systemd restarts it when it hangs (a crash is already
 * handled by Restart=always). Nothing happens when not started by systemd (no NOTIFY_SOCKET).
 *
 * The client asks for the watchdog itself (WATCHDOG_USEC=...), so an old service file or a client without this
 * code never gets killed by a timeout it does not know about. Heartbeats are sent only while the pipeline makes
 * progress: the reader loop turns round (it does so at least every few seconds even without a console) and no
 * picture takes more than ten seconds to decode. */
void watchdog_start(volatile int *stop);
void watchdog_armed(void);          /* the pipeline is up: from now on heartbeats require progress */
void watchdog_beat(void);           /* the reader loop (or a wait inside the transport) is turning round */
void watchdog_decode_begin(void);
void watchdog_decode_end(void);
void watchdog_stop(void);

#endif
