#ifndef CHAT_H
#define CHAT_H

/** @file Route player chat: local, party, whisper, and global.
 *
 * Extracted from the generic packet handler, which is where recipient
 * selection, whisper parsing and fan-out all used to live -- the only
 * gameplay feature without a module of its own.
 *
 * Fan-out does not run on the network loop thread. A global message used to be
 * delivered inline: the registry read lock taken, every online player's slot
 * mutex taken in turn, and one send issued per player, all on the thread that
 * had just parsed the packet. At capacity that is a thousand locks and a
 * thousand writes charged to one player's keystroke, with every other packet on
 * that loop waiting behind it -- the largest single amplifier in the server.
 * The loop thread now validates and enqueues; a dispatch thread delivers.
 */

#include <stdint.h>
#include <sys/types.h>

/** Start the chat dispatch thread.
 *
 * @return 1 on success, or 0 when the thread or its queue cannot be created.
 */
int chat_init(void);

/** Stop the dispatch thread after delivering what is already queued. */
void chat_shutdown(void);

/** Validate one CHAT_SEND and hand it to the dispatcher.
 *
 * Runs on a network loop thread. Everything here is bounded work: parsing,
 * sanitizing, and a cooldown check. Recipient selection and sending are not.
 */
void chat_handle_send(int client_fd, uint32_t character_id,
                      uint8_t* buffer, ssize_t bytes);

/** Messages waiting to be delivered. For tests and metrics. */
size_t chat_queue_depth(void);

/** Messages dropped because the queue was over its backpressure limit. */
uint64_t chat_dropped_count(void);

#endif // CHAT_H
