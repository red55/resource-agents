/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004-2005 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

/*
 * midcomms.c
 *
 * This is the appallingly named "mid-level" comms layer.
 *
 * Its purpose is to take packets from the "real" comms layer,
 * split them up into packets and pass them to the interested
 * part of the locking mechanism.
 *
 * It also takes messages from the locking layer, formats them
 * into packets and sends them to the comms layer.
 */

#include "dlm_internal.h"
#include "lowcomms.h"
#include "config.h"
#include "rcom.h"
#include "lock.h"


static void copy_from_cb(void *dst, const void *base, unsigned offset,
			 unsigned len, unsigned limit)
{
	unsigned copy = len;

	if ((copy + offset) > limit)
		copy = limit - offset;
	memcpy(dst, base + offset, copy);
	len -= copy;
	if (len)
		memcpy(dst + copy, base, len);
}

/*
 * Called from the low-level comms layer to process a buffer of
 * commands.
 *
 * Only complete messages are processed here, any "spare" bytes from
 * the end of a buffer are saved and tacked onto the front of the next
 * message that comes in. I doubt this will happen very often but we
 * need to be able to cope with it and I don't want the task to be waiting
 * for packets to come in when there is useful work to be done.
 */

int dlm_process_incoming_buffer(int nodeid, const void *base,
				unsigned offset, unsigned len, unsigned limit)
{
	unsigned char __tmp[sizeof(struct dlm_rcom) + sizeof(struct rcom_lock)];
	struct dlm_header *msg = (struct dlm_header *) __tmp;
	int ret = 0;
	int err = 0;
	unsigned msglen;
	__u32 space;

	while (len > sizeof(struct dlm_header)) {
		/* Get message header and check it over */
		copy_from_cb(msg, base, offset, sizeof(struct dlm_header),
			     limit);

		msglen = le16_to_cpu(msg->h_length);
		space = msg->h_lockspace;

		/* Check message size */
		err = -EINVAL;
		if (msglen < sizeof(struct dlm_header))
			break;
		err = -E2BIG;
		if (msglen > dlm_config.buffer_size) {
			printk("dlm: message size from %d too big %d(pkt len=%d)\n", nodeid, msglen, len);
			break;
		}
		err = 0;

		/* Not enough in buffer yet? wait for some more */
		if (msglen > len)
			break;

		/* Make sure our temp buffer is large enough */
		if (msglen > sizeof(__tmp) &&
		    msg == (struct dlm_header *) __tmp) {
			msg = kmalloc(dlm_config.buffer_size, GFP_KERNEL);
			if (msg == NULL)
				return ret;
		}

		copy_from_cb(msg, base, offset, msglen, limit);
		BUG_ON(space != msg->h_lockspace);
		ret += msglen;
		offset += msglen;
		offset &= (limit - 1);
		len -= msglen;

		switch (msg->h_cmd) {
		case DLM_MSG:
			dlm_receive_message(msg, nodeid, FALSE);
			break;

		case DLM_RCOM:
			dlm_receive_rcom(msg, nodeid);
			break;

		default:
			printk("dlm: msg error cmd %u len %u\n",
			       msg->h_cmd, msglen);

			printk("dlm: comms: base=%p, offset=%u, len=%u, "
			       "ret=%u, limit=%08x newbuf=%d\n",
			       base, offset, len, ret, limit,
			       ((struct dlm_header *) __tmp == msg));
		}
	}

	if (msg != (struct dlm_header *) __tmp)
		kfree(msg);

	return err ? err : ret;
}


