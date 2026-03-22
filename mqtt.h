#ifndef MQTT_H
#define MQTT_H

/*
 * Minimal MQTT 3.1.1 client — connect, subscribe, receive.
 *
 * Designed for a single subscription on a background thread.
 * Not a general-purpose library: no QoS 2, no publish, no TLS.
 */

/*
 * Connect to an MQTT broker, subscribe to `topic`, and spawn a
 * background thread that calls `callback(payload, len)` for each
 * received PUBLISH message.
 *
 * Returns 0 on success, -1 on failure.  The connection is
 * automatically retried on disconnect.
 */
int mqtt_start(const char *broker_ip, int port,
               const char *topic,
               void (*callback)(const char *payload, int len));

/* Shut down the MQTT background thread and close the connection. */
void mqtt_stop(void);

#endif /* MQTT_H */
